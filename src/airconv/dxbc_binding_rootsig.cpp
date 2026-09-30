/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "air_signature.hpp"
#include "dxbc_converter.hpp"
#include "dxbc_root_signature.hpp"
#include "shader_common.hpp"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/DerivedTypes.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <tuple>

namespace dxmt::dxbc {

using namespace llvm::air;

/**
 * The ranges one root signature defines, by the range id the shader declares.
 *
 * `second` is where the range's argument lives inside the signature's qword
 * entry: the element index for a descriptor table, root descriptor or root
 * constant, and the offset inside the descriptor table the range belongs to for
 * a range of a table. `~0u` marks a root descriptor and `~0u - 1` a root
 * constant, exactly like the two root signatures' entries do.
 *
 * A raytracing export has two of these: the pipeline's global root signature and
 * the export's own local root signature. They are kept apart so a range is only
 * ever read from the entry of the signature that defines it.
 */
struct RootSignatureRanges {
  std::map<RangeId, std::pair<ConstantBufferInfo, uint64_t>> ConstantBuffers;
  std::map<RangeId, std::pair<SamplerInfo, uint64_t>> Samplers;
  std::map<RangeId, std::pair<ShaderResourceViewInfo, uint64_t>> SRVs;
  std::map<RangeId, std::pair<UnorderedAccessViewInfo, uint64_t>> UAVs;
};

class RootSignatureBindingMap : public BindingMap {
public:
  /* ---------------------------------------------------------------------- */
  /* Root argument entries                                                  */
  /* ---------------------------------------------------------------------- */

  /**
   * One field of a root argument entry.
   *
   * A root argument entry is the qword array the root signature describes: one
   * qword per descriptor table or root descriptor and one qword per two root
   * constants, in parameter order (the runtime's
   * `CalculateRootSignatureQwordOffsets`, and the same order this file lays the
   * argument buffer out in).
   *
   * The command list's global root arguments and a shader record's local root
   * arguments are that same entry, so both are read with the accessors here;
   * only the base pointer differs - the global one is the root argument function
   * argument the runtime binds the command list state to, the local one is the
   * record's own entry, selected per invocation by the stage builder.
   */
  llvm::Value *
  GetEntryField(llvm::air::AIRBuilder &AIR, llvm::Value *Entry, uint32_t Index) {
    auto &B = AIR.builder;
    auto TyEntry = llvm::cast<llvm::PointerType>(Entry->getType())->getNonOpaquePointerElementType();
    auto TyStruct = llvm::cast<llvm::StructType>(TyEntry);
    return B.CreateLoad(TyStruct->getElementType(Index), B.CreateStructGEP(TyStruct, Entry, Index));
  };

  /* The root view's status qword (see setup_binding_rootsig), or null when it has none. */
  llvm::Value *
  RootViewStatusBase(llvm::air::AIRBuilder &AIR, llvm::Value *Entry, uint32_t Index) {
    auto TyStruct = llvm::cast<llvm::StructType>(
        llvm::cast<llvm::PointerType>(Entry->getType())->getNonOpaquePointerElementType()
    );
    if (Index >= TyStruct->getNumElements() || !TyStruct->getElementType(Index)->isIntegerTy(64))
      return nullptr;
    return GetEntryField(AIR, Entry, Index);
  }

  llvm::Value *
  GetEntryRootConstantPointer(llvm::air::AIRBuilder &AIR, llvm::Value *Entry, uint32_t Index) {
    auto &B = AIR.builder;
    auto TyEntry = llvm::cast<llvm::PointerType>(Entry->getType())->getNonOpaquePointerElementType();
    auto TyStruct = llvm::cast<llvm::StructType>(TyEntry);
    return B.CreatePointerCast(
        B.CreateStructGEP(TyStruct, Entry, Index),
        AIR.getIntTy(4)->getPointerTo(
            llvm::cast<llvm::PointerType>(Entry->getType())->getAddressSpace()
        )
    );
  };

  /**
   * The command list's global root arguments.
   *
   * Normally the root argument argument the runtime binds the command list state
   * to. A visible function reached through the visible function table cannot be
   * handed one - its ABI is fixed at payload, hit state, record and depth (see
   * EmitVisibleFunctionCall), so the root arguments are not part of its signature -
   * and its stage builder reads the address of the same entry out of the invocation
   * frame and hands it in with SetGlobalRootArgumentPointer instead. The entry is
   * then read through that pointer, in the pointer's own address space, and the
   * argument it would otherwise have taken is not declared at all
   * (`bind_signature_arguments` in setup_binding_rootsig).
   */
  llvm::Value *
  GlobalRootArguments(llvm::air::AIRBuilder &AIR) {
    auto &B = AIR.builder;
    if (GlobalRootArgumentPointer && GlobalRootArgumentsType) {
      auto Space =
          llvm::cast<llvm::PointerType>(GlobalRootArgumentPointer->getType())->getAddressSpace();
      return B.CreateBitCast(GlobalRootArgumentPointer, GlobalRootArgumentsType->getPointerTo(Space));
    }
    if (~RootSignatureArgumentSlot == 0)
      return nullptr;
    return B.GetInsertBlock()->getParent()->getArg(RootSignatureArgumentSlot);
  };

  /**
   * The dispatch's static sampler table.
   *
   * Static samplers are pipeline state, not root arguments: the runtime binds one
   * table for the whole dispatch (encoded from the dispatch's root signature, i.e. the
   * union of the global root signature's static samplers and the ones only a local one
   * defines - see `GetDispatchRootSignature`) at the static sampler slot, and a shader
   * that declares that argument reads it directly. A visible function cannot, for the
   * same reason it cannot take the root arguments, so its stage builder hands the same
   * table's address in with SetStaticSamplerArgumentPointer.
   */
  llvm::Value *
  StaticSamplerTable(llvm::air::AIRBuilder &AIR) {
    if (StaticSamplerPointer) {
      auto &B = AIR.builder;
      auto Space =
          llvm::cast<llvm::PointerType>(StaticSamplerPointer->getType())->getAddressSpace();
      return B.CreateBitCast(StaticSamplerPointer, SamplerTableType(AIR)->getPointerTo(Space));
    }
    if (~StaticSamplerArgumentSlot == 0)
      return nullptr;
    return AIR.builder.GetInsertBlock()->getParent()->getArg(StaticSamplerArgumentSlot);
  };

  /**
   * The shader record's local root arguments, or null when the stage builder has
   * not selected one for this invocation.
   *
   * The pointer is the entry the runtime materialized for the record being
   * dispatched (the record's local root table plus the record index times the
   * entry stride); the stage computes it and hands it in before the resource
   * lowering runs. It is an opaque byte pointer because the runtime's entry is
   * only defined by its layout, so it is cast to the entry type of the local
   * root signature here.
   */
  llvm::Value *
  LocalRootArguments(llvm::air::AIRBuilder &AIR) {
    if (LocalRootArgumentPointer && LocalRootArgumentsType) {
      auto &B = AIR.builder;
      auto Space =
          llvm::cast<llvm::PointerType>(LocalRootArgumentPointer->getType())->getAddressSpace();
      return B.CreateBitCast(LocalRootArgumentPointer, LocalRootArgumentsType->getPointerTo(Space));
    }
    /*
     * A stage that declares the local root table reads its own record's entry
     * straight out of the table the runtime binds: the ray generation shader is the
     * single record of its table, so its entry is the table's base, and an
     * intersection shader's record is selected the same way. A visible function
     * cannot declare the table (fixed ABI), so it takes the entry the caller
     * selected instead - the pointer above.
     */
    if (~LocalRootArgumentSlot == 0)
      return nullptr;
    return AIR.builder.GetInsertBlock()->getParent()->getArg(LocalRootArgumentSlot);
  };

  /**
   * The argument positions the stage builder needs to publish.
   *
   * The ray generation shader publishes the addresses of the root argument entry
   * and of the static sampler table into the invocation frame, because a visible
   * function cannot declare them as arguments; it can only do that by reading the
   * arguments it was handed, i.e. by position. `~0u` means the argument was not
   * declared.
   */
  virtual uint32_t RootSignatureArgumentIndex() const { return RootSignatureArgumentSlot; }
  virtual uint32_t StaticSamplerArgumentIndex() const { return StaticSamplerArgumentSlot; }

  virtual void SetLocalRootArgumentPointer(llvm::Value *Pointer) {
    LocalRootArgumentPointer = Pointer;
  };

  virtual void SetGlobalRootArgumentPointer(llvm::Value *Pointer) {
    GlobalRootArgumentPointer = Pointer;
  };

  virtual void SetStaticSamplerArgumentPointer(llvm::Value *Pointer) {
    StaticSamplerPointer = Pointer;
  };

  /* ---------------------------------------------------------------------- */
  /* Range resolution                                                       */
  /* ---------------------------------------------------------------------- */

  llvm::Optional<ConstantBufferDescriptor>
  ResolveConstantBuffer(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const ConstantBufferInfo &CBuf,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (DescriptorOffset == ~0u - 1) {
      auto Pointer = GetEntryRootConstantPointer(Builder, Entry, CBuf.arg_index);
      return ConstantBufferDescriptor{Pointer, nullptr};
    }
    if (DescriptorOffset == ~0u) {
      auto Pointer = GetEntryField(Builder, Entry, CBuf.arg_index);
      return ConstantBufferDescriptor{Pointer, nullptr};
    }
    auto HeapPointer = GetEntryField(Builder, Entry, CBuf.arg_index);
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy(4)->getPointerTo(2), CBuf.range.lower_bound,
        DescriptorOffset
    );
    return ConstantBufferDescriptor{Pointer, Metadata};
  }

  /**
   * A static sampler of the pipeline.
   *
   * Static samplers are not root arguments: they are pipeline state, and the
   * runtime binds one static sampler table for the whole dispatch, encoded from the
   * dispatch's root signature (see `GetDispatchRootSignature`). That signature defines
   * the union of the global root signature's static samplers and the ones only a local
   * root signature defines, so a static sampler either of them declares resolves
   * through this one table at its index in the union - which is what D3D12 requires,
   * since every root signature of a dispatch that defines a static sampler must define
   * it identically and the union has to fit one static sampler budget.
   */
  llvm::Optional<SamplerDescriptor>
  ResolveStaticSampler(
      llvm::air::AIRBuilder &Builder, const SamplerInfo &Sampler, llvm::Value *Index
  ) {
    if (Sampler.arg_index == ~0u)
      return {};
    auto Table = StaticSamplerTable(Builder);
    if (!Table)
      return {};
    auto [SamplerH, CubeSampler, Metadata] =
        GetSamplerDescriptor(Builder, Table, Index, Sampler.range.lower_bound, Sampler.arg_index);
    return SamplerDescriptor{SamplerH, CubeSampler, Metadata};
  }

  llvm::Optional<SamplerDescriptor>
  ResolveSamplerFromEntry(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const SamplerInfo &Sampler,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    auto HeapPointer = GetEntryField(Builder, Entry, Sampler.arg_index);
    auto [SamplerH, CubeSampler, Metadata] =
        GetSamplerDescriptor(Builder, HeapPointer, Index, Sampler.range.lower_bound, DescriptorOffset);
    return SamplerDescriptor{SamplerH, CubeSampler, Metadata};
  }

  llvm::Optional<TextureDescirptor>
  ResolveSRVTexture(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const ShaderResourceViewInfo &SRV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (SRV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto HeapPointer = GetEntryField(Builder, Entry, SRV.arg_index);
    return FetchSRVTexture(Builder, HeapPointer, SRV, SRV.range.lower_bound, DescriptorOffset, Index);
  }

  /** The SRV texture descriptor at `Index - First + DescriptorOffset` of the heap at `HeapPointer`. */
  TextureDescirptor
  FetchSRVTexture(
      llvm::air::AIRBuilder &Builder, llvm::Value *HeapPointer, const ShaderResourceViewInfo &SRV, uint32_t First,
      uint32_t DescriptorOffset, llvm::Value *Index
  ) {
    auto ResourceKindLogical = air::to_air_resource_type(SRV.resource_type, SRV.compared);
    auto ResourceKind = air::lowering_texture_1d_to_2d(ResourceKindLogical);
    // A D3D12 1D depth resource is stored as a one-row Metal depth2d
    // texture. Comparison sampling needs the depth texture intrinsic, while
    // the logical shader coordinates remain one-dimensional.
    if (SRV.compared && ResourceKindLogical == Texture::texture1d)
      ResourceKind = Texture::depth2d;
    else if (SRV.compared && ResourceKindLogical == Texture::texture1d_array)
      ResourceKind = Texture::depth2d_array;
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(SRV.scaler_type)
    );
    auto MemoryAccess = SRV.sampled ? Texture::MemoryAccess::access_sample : Texture::MemoryAccess::access_read;

    auto [Handle, Metadata, EmulationAddress, EmulationTag] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, First, DescriptorOffset);
    auto result = TextureDescirptor{Handle, Metadata, false, ResourceKind, ResourceKindLogical, MemoryAccess, SampleType};
    if (ResourceKindLogical == Texture::texture_buffer) {
      result.EmulationAddress = EmulationAddress;
      result.EmulationTag = EmulationTag;
      result.StatusBase = EmulationAddress;
      result.StatusView = EmulationTag;
      result.RGB32StatusBase = GetDescriptorQword0(Builder, HeapPointer, Index, First, DescriptorOffset);
    }
    return result;
  }

  llvm::Optional<TextureDescirptor>
  ResolveUAVTexture(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const UnorderedAccessViewInfo &UAV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (UAV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto HeapPointer = GetEntryField(Builder, Entry, UAV.arg_index);
    return FetchUAVTexture(Builder, HeapPointer, UAV, UAV.range.lower_bound, DescriptorOffset, Index);
  }

  TextureDescirptor
  FetchUAVTexture(
      llvm::air::AIRBuilder &Builder, llvm::Value *HeapPointer, const UnorderedAccessViewInfo &UAV, uint32_t First,
      uint32_t DescriptorOffset, llvm::Value *Index
  ) {
    auto ResourceKindLogical = air::to_air_resource_type(UAV.resource_type);
    auto ResourceKind = air::lowering_texture_1d_to_2d(ResourceKindLogical);
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(UAV.scaler_type)
    );
    auto MemoryAccess = UAV.written
                            ? (UAV.read ? Texture::MemoryAccess::acesss_readwrite : Texture::MemoryAccess::access_write)
                            : Texture::MemoryAccess::access_read;
    auto [Handle, Metadata, EmulationAddress, EmulationTag] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, First, DescriptorOffset);
    auto result = TextureDescirptor{Handle,       Metadata,  UAV.global_coherent, ResourceKind, ResourceKindLogical,
                                    MemoryAccess, SampleType, ResourceKindLogical == Texture::texture3d};
    if (ResourceKindLogical == Texture::texture_buffer) {
      result.StatusBase = EmulationAddress;
      result.StatusView = EmulationTag;
    }
    return result;
  }

  llvm::Optional<BufferDescriptor>
  ResolveSRVBuffer(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const ShaderResourceViewInfo &SRV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (SRV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto HeapPointer = GetEntryField(Builder, Entry, SRV.arg_index);
    if (DescriptorOffset == ~0u) {
      auto result = BufferDescriptor{HeapPointer, Builder.builder.getInt64(0xffffffff), SRV.structure_stride, false};
      result.StatusBase = RootViewStatusBase(Builder, Entry, SRV.arg_metadata_index);
      return result;
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), SRV.range.lower_bound,
        DescriptorOffset
    );

    auto result = BufferDescriptor{Pointer, Metadata, SRV.structure_stride, false};
    result.StatusBase = GetBufferStatusBase(Builder, HeapPointer, Index, SRV.range.lower_bound, DescriptorOffset);
    return result;
  }

  llvm::Optional<BufferDescriptor>
  ResolveUAVBuffer(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const UnorderedAccessViewInfo &UAV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (UAV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto HeapPointer = GetEntryField(Builder, Entry, UAV.arg_index);
    if (DescriptorOffset == ~0u) {
      auto result = BufferDescriptor{
          HeapPointer, Builder.builder.getInt64(0xffffffff), UAV.structure_stride, UAV.global_coherent
      };
      if (!UAV.rasterizer_order)
        result.StatusBase = RootViewStatusBase(Builder, Entry, UAV.arg_metadata_index);
      return result;
    }
    auto [Pointer, Metadata] = UAV.rasterizer_order
                                   ? GetRasterOrderedBufferDescriptor(
                                         Builder, HeapPointer, Index, UAV.range.lower_bound, DescriptorOffset
                                     )
                                   : GetBufferDescriptor(
                                         Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1),
                                         UAV.range.lower_bound, DescriptorOffset
                                     );

    auto result = BufferDescriptor{Pointer, Metadata, UAV.structure_stride, UAV.global_coherent};
    if (!UAV.rasterizer_order)
      result.StatusBase = GetBufferStatusBase(Builder, HeapPointer, Index, UAV.range.lower_bound, DescriptorOffset);
    return result;
  }

  llvm::Optional<CounterDescriptor>
  ResolveUAVCounter(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const UnorderedAccessViewInfo &UAV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (UAV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto HeapPointer = GetEntryField(Builder, Entry, UAV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can not have counters associated
    auto Pointer = GetUAVCounterDescriptor(Builder, HeapPointer, Index, UAV.range.lower_bound, DescriptorOffset);

    return CounterDescriptor{Pointer};
  }

  /**
   * The acceleration structure of a raytracing export.
   *
   * A RaytracingAccelerationStructure is an ordinary SRV: the descriptor holds the
   * D3D12 GPU virtual address of the structure, so this returns that address as an
   * i64 and the traversal lowering turns it into a Metal structure through the
   * structure's own header. Both encodings are handled: a root SRV argument is
   * itself the address, and a descriptor-table slot holds it in the slot's first
   * qword, so the index stays dynamic exactly like every other descriptor.
   */
  llvm::Optional<AccelerationStructureDescriptor>
  ResolveAccelerationStructure(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const ShaderResourceViewInfo &SRV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (SRV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto HeapPointer = GetEntryField(Builder, Entry, SRV.arg_index);
    auto &B = Builder.builder;
    auto *i64 = llvm::Type::getInt64Ty(Builder.getContext());
    if (DescriptorOffset == ~0u)
      return AccelerationStructureDescriptor{B.CreatePtrToInt(HeapPointer, i64)};
    auto *TyBufferDescriptor = llvm::StructType::get(
        Builder.getContext(), {i64, i64, i64, i64}
    );
    auto IdxDescriptor =
        B.CreateAdd(B.CreateSub(Index, Builder.getInt(SRV.range.lower_bound)), Builder.getInt(DescriptorOffset));
    return AccelerationStructureDescriptor{B.CreateLoad(
        i64, B.CreateGEP(
                 TyBufferDescriptor, B.CreatePointerCast(HeapPointer, TyBufferDescriptor->getPointerTo(2)),
                 {IdxDescriptor, Builder.getInt(0) /* address */}
             )
    )};
  }

  std::pair<llvm::Value *, llvm::Value *>
  GetRasterOrderedBufferDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *table, llvm::Value *index, RangeId range_id,
      uint32_t descriptor_offset
  ) {
    auto *table_type = llvm::cast<llvm::StructType>(
        llvm::cast<llvm::PointerType>(table->getType())->getNonOpaquePointerElementType()
    );
    uint32_t first = 0;
    for (unsigned field = 0; field < table_type->getNumElements(); ++field) {
      auto *array = llvm::cast<llvm::ArrayType>(
          llvm::cast<llvm::StructType>(table_type->getElementType(field))->getElementType(0)
      );
      auto *element = array->getElementType();
      const uint32_t count = element->isStructTy() ? array->getNumElements() : array->getNumElements() / 4;
      if (element->isStructTy() && descriptor_offset >= first && descriptor_offset - first < count) {
        auto &B = AIR.builder;
        auto *descriptor = llvm::cast<llvm::StructType>(element);
        auto local = B.CreateAdd(B.CreateSub(index, AIR.getInt(range_id)),
                                 AIR.getInt(descriptor_offset - first));
        auto *address = B.CreateGEP(table_type, table,
                                    {AIR.getInt(0), AIR.getInt(field), AIR.getInt(0), local, AIR.getInt(0)});
        auto *metadata = B.CreateGEP(table_type, table,
                                     {AIR.getInt(0), AIR.getInt(field), AIR.getInt(0), local, AIR.getInt(1)});
        return {B.CreateLoad(descriptor->getElementType(0), address),
                B.CreateLoad(descriptor->getElementType(1), metadata)};
      }
      first += count;
    }
    llvm_unreachable("raster-ordered descriptor must have a matching table field");
  }

  std::pair<llvm::Value *, llvm::Value *>
  GetBufferDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, llvm::Type *HandleType, RangeId RangeId,
      uint32_t DescriptorOffset
  ) {
    auto TyBufferDescriptor = llvm::StructType::get(
        AIR.getContext(), {HandleType, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return {
        B.CreateLoad(
            HandleType, B.CreateGEP(
                            TyBufferDescriptor, B.CreatePointerCast(IntPtr, TyBufferDescriptor->getPointerTo(2)),
                            {IdxDescriptor, AIR.getInt(0) /* pointer */}
                        )
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(
                TyBufferDescriptor, B.CreatePointerCast(IntPtr, TyBufferDescriptor->getPointerTo(2)),
                {IdxDescriptor, AIR.getInt(1) /* metadata */}
            )
        )
    };
  }

  /* Qword 3 of a raw/structured buffer descriptor: the reserved buffer's biased residency base. */
  llvm::Value *
  GetBufferStatusBase(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset
  ) {
    auto *i64 = llvm::Type::getInt64Ty(AIR.getContext());
    auto TyBufferDescriptor = llvm::StructType::get(AIR.getContext(), {i64, i64, i64, i64});
    auto &B = AIR.builder;
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return B.CreateLoad(
        i64, B.CreateGEP(
                 TyBufferDescriptor, B.CreatePointerCast(IntPtr, TyBufferDescriptor->getPointerTo(2)),
                 {IdxDescriptor, AIR.getInt(3) /* residency status base */}
             )
    );
  }

  llvm::Value *
  GetUAVCounterDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset
  ) {
    auto TyCounter = AIR.getIntTy()->getPointerTo(1);
    auto TyBufferDescriptor = llvm::StructType::get(
        AIR.getContext(), {llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext()),
                           TyCounter, llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return B.CreateLoad(
        TyCounter, B.CreateGEP(
                       TyBufferDescriptor, B.CreatePointerCast(IntPtr, TyBufferDescriptor->getPointerTo(2)),
                       {IdxDescriptor, AIR.getInt(2) /* metadata */}
                   )
    );
  }

  /** The sampler descriptor table: `{ sampler, cube_sampler, metadata, ... }`. */
  llvm::StructType *
  SamplerTableType(llvm::air::AIRBuilder &AIR) {
    auto TySampleHandler = AIR.getSamplerHandleType();
    return llvm::StructType::get(
        AIR.getContext(), {TySampleHandler, TySampleHandler, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext())}
    );
  }

  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
  GetSamplerDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset
  ) {
    auto TySampleHandler = AIR.getSamplerHandleType();
    auto TySamplerDescriptor = SamplerTableType(AIR);
    auto &B = AIR.builder;
    /* The table is read in the address space of the pointer it is reached
     * through: the argument the runtime binds for a shader that declares it, or
     * the address a visible stage hands in. */
    auto Space = llvm::cast<llvm::PointerType>(IntPtr->getType())->getAddressSpace();
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return {
        B.CreateLoad(
            TySampleHandler, //
            B.CreateGEP(
                TySamplerDescriptor, B.CreatePointerCast(IntPtr, TySamplerDescriptor->getPointerTo(Space)),
                {IdxDescriptor, AIR.getInt(0) /*sampler*/}
            )
        ),
        B.CreateLoad(
            TySampleHandler, //
            B.CreateGEP(
                TySamplerDescriptor, B.CreatePointerCast(IntPtr, TySamplerDescriptor->getPointerTo(Space)),
                {IdxDescriptor, AIR.getInt(1) /* cube_sampler */}
            )
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(
                TySamplerDescriptor, B.CreatePointerCast(IntPtr, TySamplerDescriptor->getPointerTo(Space)),
                {IdxDescriptor, AIR.getInt(2) /* metadata*/}
            )
        )
    };
  }

  /* The first qword of a 32 byte heap descriptor as an integer (for RGB32 SRVs, their status base). */
  llvm::Value *
  GetDescriptorQword0(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset
  ) {
    auto &B = AIR.builder;
    auto *I64 = llvm::Type::getInt64Ty(AIR.getContext());
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    auto *Qwords = B.CreatePointerCast(IntPtr, I64->getPointerTo(2));
    return B.CreateLoad(I64, B.CreateGEP(I64, Qwords, B.CreateMul(B.CreateZExt(IdxDescriptor, I64), B.getInt64(4))));
  }

  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *, llvm::Value *>
  GetTextureDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, Texture::ResourceKind Kind, RangeId RangeId,
      uint32_t DescriptorOffset
  ) {
    auto TyTextureHandle = AIR.getTextureHandleType(Kind);
    auto TyTextureDescriptor = llvm::StructType::get(
        AIR.getContext(), {TyTextureHandle, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto IdxDescriptor = B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset));
    return {
        B.CreateLoad(
            TyTextureHandle, //
            B.CreateGEP(
                TyTextureDescriptor, B.CreatePointerCast(IntPtr, TyTextureDescriptor->getPointerTo(2)),
                {IdxDescriptor, AIR.getInt(0) /* pointer */}
            )
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(
                TyTextureDescriptor, B.CreatePointerCast(IntPtr, TyTextureDescriptor->getPointerTo(2)),
                {IdxDescriptor, AIR.getInt(1) /* metadata */}
            )
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(
                TyTextureDescriptor, B.CreatePointerCast(IntPtr, TyTextureDescriptor->getPointerTo(2)),
                {IdxDescriptor, AIR.getInt(2) /* RGB32 buffer address */}
            )
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(
                TyTextureDescriptor, B.CreatePointerCast(IntPtr, TyTextureDescriptor->getPointerTo(2)),
                {IdxDescriptor, AIR.getInt(3) /* RGB32 emulation tag */}
            )
        )
    };
  }

  /* ---------------------------------------------------------------------- */
  /* BindingMap                                                             */
  /* ---------------------------------------------------------------------- */

  virtual llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.ConstantBuffers.find(Range);
    if (LocalIter != Local.ConstantBuffers.end()) {
      /*
       * A range the export's local root signature defines is read from the root
       * arguments of the record this invocation runs as. Without a selected
       * record there is nothing to read, and the global root arguments must not
       * be used instead.
       */
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveConstantBuffer(
          Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index
      );
    }
    auto Iter = Global.ConstantBuffers.find(Range);
    if (Iter == Global.ConstantBuffers.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveConstantBuffer(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  virtual llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.Samplers.find(Range);
    if (LocalIter != Local.Samplers.end()) {
      auto &Sampler = LocalIter->second.first;
      auto DescriptorOffset = LocalIter->second.second;
      if (DescriptorOffset == ~0u)
        return ResolveStaticSampler(Builder, Sampler, Index);
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveSamplerFromEntry(Builder, Entry, Sampler, DescriptorOffset, Index);
    }
    auto Iter = Global.Samplers.find(Range);
    if (Iter == Global.Samplers.end())
      return {};
    auto &Sampler = Iter->second.first;
    auto DescriptorOffset = Iter->second.second;
    if (DescriptorOffset == ~0u)
      return ResolveStaticSampler(Builder, Sampler, Index);
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveSamplerFromEntry(Builder, Entry, Sampler, DescriptorOffset, Index);
  }

  virtual llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.SRVs.find(Range);
    if (LocalIter != Local.SRVs.end()) {
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveSRVTexture(Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index);
    }
    auto Iter = Global.SRVs.find(Range);
    if (Iter == Global.SRVs.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveSRVTexture(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  virtual llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.UAVs.find(Range);
    if (LocalIter != Local.UAVs.end()) {
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveUAVTexture(Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index);
    }
    auto Iter = Global.UAVs.find(Range);
    if (Iter == Global.UAVs.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveUAVTexture(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  virtual llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.SRVs.find(Range);
    if (LocalIter != Local.SRVs.end()) {
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveSRVBuffer(Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index);
    }
    auto Iter = Global.SRVs.find(Range);
    if (Iter == Global.SRVs.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveSRVBuffer(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  virtual llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.UAVs.find(Range);
    if (LocalIter != Local.UAVs.end()) {
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveUAVBuffer(Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index);
    }
    auto Iter = Global.UAVs.find(Range);
    if (Iter == Global.UAVs.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveUAVBuffer(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  virtual llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.UAVs.find(Range);
    if (LocalIter != Local.UAVs.end()) {
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveUAVCounter(Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index);
    }
    auto Iter = Global.UAVs.find(Range);
    if (Iter == Global.UAVs.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveUAVCounter(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  virtual llvm::Optional<AccelerationStructureDescriptor>
  GetAccelerationStructure(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto LocalIter = Local.SRVs.find(Range);
    if (LocalIter != Local.SRVs.end()) {
      auto Entry = LocalRootArguments(Builder);
      if (!Entry)
        return {};
      return ResolveAccelerationStructure(
          Builder, Entry, LocalIter->second.first, LocalIter->second.second, Index
      );
    }
    auto Iter = Global.SRVs.find(Range);
    if (Iter == Global.SRVs.end())
      return {};
    auto Entry = GlobalRootArguments(Builder);
    if (!Entry)
      return {};
    return ResolveAccelerationStructure(Builder, Entry, Iter->second.first, Iter->second.second, Index);
  }

  /*
   * Argument positions of the two entries a shader that declares signature
   * arguments reads directly, and of the local root table a ray generation or
   * intersection stage reads its own record's entry from. `~0u` means the
   * argument was not declared (the shader resolves the entry through a pointer the
   * stage builder handed in instead, or has no such entry at all).
   */
  uint32_t RootSignatureArgumentSlot = ~0u;
  uint32_t StaticSamplerArgumentSlot = ~0u;
  uint32_t LocalRootArgumentSlot = ~0u;

  /**
   * Entry type of the global root signature, i.e. of the command list's root
   * argument buffer.
   */
  llvm::StructType *GlobalRootArgumentsType = nullptr;
  /** The ranges the pipeline's global root signature defines. */
  RootSignatureRanges Global;
  /** The ranges this export's local root signature defines. */
  RootSignatureRanges Local;
  /** Entry type of the local root signature, i.e. of a materialized shader record entry. */
  llvm::StructType *LocalRootArgumentsType = nullptr;
  /**
   * Root arguments of the record this invocation runs as, as the stage builder
   * computed them (the record's local root table plus the record index times the
   * entry stride). Null until it is handed in, and null for a stage that has no
   * record of its own.
   */
  llvm::Value *LocalRootArgumentPointer = nullptr;
  /**
   * The command list's root arguments and the dispatch's static sampler table, as
   * the stage builder found them for a shader that cannot take them as arguments
   * (a visible function: see GlobalRootArguments / StaticSamplerTable). Null when
   * the shader takes them as arguments, which is every raster and compute shader.
   */
  llvm::Value *GlobalRootArgumentPointer = nullptr;
  llvm::Value *StaticSamplerPointer = nullptr;

  /* ---------------------------------------------------------------------- */
  /* SM 6.6 directly indexed heaps                                          */
  /* ---------------------------------------------------------------------- */

  /** First of the four heap qwords in the global root entry, or ~0u without DIRECTLY_INDEXED. */
  uint32_t HeapFieldIndex = ~0u;
  bool ResourceHeapRasterOrdered = false;
  bool ResourceHeapIndexed = false;
  bool SamplerHeapIndexed = false;

  /**
   * The 32 byte all-zero descriptor a heap index outside the bound heap resolves to.
   *
   * D3D12 leaves an access through an index past the heap undefined; resolving it to
   * the null descriptor keeps it a null access (reads zero, writes and atomics dropped
   * by the descriptor bound checks) instead of a load from arbitrary memory. The same
   * applies when no heap of that type is bound: base and count are then both zero.
   */
  llvm::Value *
  NullDescriptor(llvm::air::AIRBuilder &AIR) {
    auto &module = *AIR.builder.GetInsertBlock()->getModule();
    constexpr const char *name = "dxmt.heap.null_descriptor";
    auto *type = llvm::ArrayType::get(llvm::Type::getInt64Ty(AIR.getContext()), 4);
    auto *global = module.getGlobalVariable(name, true);
    if (!global) {
      global = new llvm::GlobalVariable(
          module, type, true, llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(type), name,
          nullptr, llvm::GlobalValue::NotThreadLocal, 2
      );
      global->setAlignment(llvm::Align(32));
    }
    return global;
  }

  /**
   * The heap and in-heap index a heap access reads its descriptor from: the bound heap
   * and `Index` while `Index` is inside it, the null descriptor at index 0 otherwise.
   * The comparison is unsigned, so any index a shader can form is covered.
   */
  std::pair<llvm::Value *, llvm::Value *>
  HeapSlot(llvm::air::AIRBuilder &AIR, bool Sampler, llvm::Value *Index) {
    auto &B = AIR.builder;
    auto Entry = GlobalRootArguments(AIR);
    if (!Entry || HeapFieldIndex == ~0u)
      return {nullptr, nullptr};
    const uint32_t field = HeapFieldIndex + (Sampler ? 2 : 0);
    auto *base = GetEntryField(AIR, Entry, field);
    auto *count = GetEntryField(AIR, Entry, field + 1);
    auto *index64 = B.CreateZExt(Index, B.getInt64Ty());
    auto *inside = B.CreateICmpULT(index64, count);
    auto *pointer_type = AIR.getIntTy()->getPointerTo(2);
    // A pixel shader with heap ROVs declares the resource heap as a raster-ordered indirect buffer.
    auto *heap = base->getType()->isPointerTy() ? B.CreatePointerCast(base, pointer_type)
                                                : B.CreateIntToPtr(base, pointer_type);
    auto *null = B.CreatePointerCast(NullDescriptor(AIR), pointer_type);
    return {B.CreateSelect(inside, heap, null), B.CreateSelect(inside, Index, AIR.getInt(0))};
  }

  virtual llvm::Optional<ConstantBufferDescriptor>
  GetHeapConstantBuffer(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    if (!ResourceHeapIndexed)
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    auto [Pointer, Metadata] =
        GetBufferDescriptor(Builder, Heap, Slot, Builder.getIntTy(4)->getPointerTo(2), 0, 0);
    return ConstantBufferDescriptor{Pointer, Metadata};
  }

  virtual llvm::Optional<SamplerDescriptor>
  GetHeapSampler(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    if (!SamplerHeapIndexed)
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, true, Index);
    if (!Heap)
      return {};
    auto [SamplerH, CubeSampler, Metadata] = GetSamplerDescriptor(Builder, Heap, Slot, 0, 0);
    return SamplerDescriptor{SamplerH, CubeSampler, Metadata};
  }

  virtual llvm::Optional<TextureDescirptor>
  GetHeapSRVTexture(llvm::air::AIRBuilder &Builder, const ShaderResourceViewInfo &Info, llvm::Value *Index) {
    if (!ResourceHeapIndexed || Info.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    return FetchSRVTexture(Builder, Heap, Info, 0, 0, Slot);
  }

  virtual llvm::Optional<TextureDescirptor>
  GetHeapUAVTexture(llvm::air::AIRBuilder &Builder, const UnorderedAccessViewInfo &Info, llvm::Value *Index) {
    if (!ResourceHeapIndexed || Info.resource_type == shader::common::ResourceType::NonApplicable ||
        (Info.rasterizer_order && !ResourceHeapRasterOrdered))
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    return FetchUAVTexture(Builder, Heap, Info, 0, 0, Slot);
  }

  virtual llvm::Optional<BufferDescriptor>
  GetHeapSRVBuffer(llvm::air::AIRBuilder &Builder, const ShaderResourceViewInfo &Info, llvm::Value *Index) {
    if (!ResourceHeapIndexed || Info.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    auto [Pointer, Metadata] = GetBufferDescriptor(Builder, Heap, Slot, Builder.getIntTy()->getPointerTo(1), 0, 0);
    auto result = BufferDescriptor{Pointer, Metadata, Info.structure_stride, false};
    result.StatusBase = GetBufferStatusBase(Builder, Heap, Slot, 0, 0);
    return result;
  }

  virtual llvm::Optional<BufferDescriptor>
  GetHeapUAVBuffer(llvm::air::AIRBuilder &Builder, const UnorderedAccessViewInfo &Info, llvm::Value *Index) {
    if (!ResourceHeapIndexed || Info.resource_type != shader::common::ResourceType::NonApplicable ||
        (Info.rasterizer_order && !ResourceHeapRasterOrdered))
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    auto [Pointer, Metadata] = GetBufferDescriptor(Builder, Heap, Slot, Builder.getIntTy()->getPointerTo(1), 0, 0);
    auto result = BufferDescriptor{Pointer, Metadata, Info.structure_stride, Info.global_coherent};
    result.StatusBase = GetBufferStatusBase(Builder, Heap, Slot, 0, 0);
    return result;
  }

  virtual llvm::Optional<CounterDescriptor>
  GetHeapUAVCounter(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    if (!ResourceHeapIndexed)
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    return CounterDescriptor{GetUAVCounterDescriptor(Builder, Heap, Slot, 0, 0)};
  }

  virtual llvm::Optional<AccelerationStructureDescriptor>
  GetHeapAccelerationStructure(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    if (!ResourceHeapIndexed)
      return {};
    auto [Heap, Slot] = HeapSlot(Builder, false, Index);
    if (!Heap)
      return {};
    auto &B = Builder.builder;
    auto *i64 = llvm::Type::getInt64Ty(Builder.getContext());
    auto *TyBufferDescriptor = llvm::StructType::get(Builder.getContext(), {i64, i64, i64, i64});
    return AccelerationStructureDescriptor{B.CreateLoad(
        i64, B.CreateGEP(
                 TyBufferDescriptor, B.CreatePointerCast(Heap, TyBufferDescriptor->getPointerTo(2)),
                 {Slot, Builder.getInt(0) /* address */}
             )
    )};
  }
};

/**
 * Deserialize a root signature blob.
 *
 * A D3D12 root signature reaches the compiler as the DXBC container holding an
 * RTS0 chunk - that is what a pipeline state passes, and what the raytracing
 * pipeline passes for the pipeline's global signature. An already unwrapped
 * RTS0 descriptor (what the raytracing pipeline keeps for a per-export local
 * signature) is accepted as well, so the two callers do not have to agree on the
 * wrapper: the same `RootSignatureDeserializer` reads either, and the raw form is
 * recognized by the version field every serialized descriptor starts with.
 */
static const D3D12_ROOT_SIGNATURE_DESC1 *
deserialize_root_signature(
    const void *bytecode, size_t bytecode_length, RootSignatureDeserializer &deserializer
) {
  if (!bytecode || bytecode_length < sizeof(uint32_t))
    return nullptr;

  const auto *bytes = static_cast<const uint8_t *>(bytecode);
  if (std::memcmp(bytes, "DXBC", 4) == 0) {
    /* A DXBC container: the signature is its RTS0 chunk. */
    const void *pRawRootSig;
    UINT RawRootSigSize;
    if (FAILED(microsoft::DXBCGetRootSignature(bytecode, &pRawRootSig, &RawRootSigSize))) {
      assert(0 && "invalid root signature blob");
      return nullptr;
    }
    if (FAILED(deserializer.Deserialize(pRawRootSig, RawRootSigSize))) {
      assert(0 && "invalid root signature");
      return nullptr;
    }
    return &deserializer.desc_1_1_.Desc_1_1;
  }

  /*
   * The RTS0 chunk itself: `RawRootSignatureDesc` starts with the
   * D3D_ROOT_SIGNATURE_VERSION the descriptor was serialized as, and the
   * raytracing pipeline hands a per-export local signature over in exactly that
   * form.
   */
  uint32_t version = 0;
  std::memcpy(&version, bytes, sizeof(version));
  if (bytecode_length < sizeof(RawRootSignatureDesc) ||
      (version != D3D_ROOT_SIGNATURE_VERSION_1 && version != D3D_ROOT_SIGNATURE_VERSION_1_1 &&
       version != kRootSignatureVersion_1_2))
    return nullptr;
  if (FAILED(deserializer.Deserialize(bytecode, bytecode_length)))
    return nullptr;
  return &deserializer.desc_1_1_.Desc_1_1;
}

static bool range_contained(const ResourceRange &range, const D3D12_DESCRIPTOR_RANGE1 &range_rootsig) {
  if (range.lower_bound < range_rootsig.BaseShaderRegister)
    return false;
  if (range_rootsig.NumDescriptors == ~0u)
    return true;
  if (range.size == ~0u)
    return true;
  if (range.lower_bound + range.size <= range_rootsig.BaseShaderRegister + range_rootsig.NumDescriptors)
    return true;
  return false;
}

static bool ordered_uav_in_range(const UnorderedAccessViewInfo &uav,
                                 const D3D12_DESCRIPTOR_RANGE1 &range) {
  return range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && uav.rasterizer_order &&
         uav.range.space == range.RegisterSpace && range_contained(uav.range, range);
}

static bool ordered_buffer_in_table(const ShaderInfo &shader_info,
                                    const D3D12_DESCRIPTOR_RANGE1 &range) {
  if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
    return false;
  for (const auto &[_, uav] : shader_info.uavMap)
    if (ordered_uav_in_range(uav, range) &&
        (uav.resource_type == shader::common::ResourceType::NonApplicable ||
         uav.resource_type == shader::common::ResourceType::TextureBuffer))
      return true;
  return false;
}

static bool needs_ordered_buffer_marker(const ShaderInfo &shader_info,
                                        const D3D12_ROOT_SIGNATURE_DESC1 &root_sig) {
  for (unsigned i = 0; i < root_sig.NumParameters; ++i) {
    const auto &parameter = root_sig.pParameters[i];
    if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
        (parameter.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL &&
         parameter.ShaderVisibility != D3D12_SHADER_VISIBILITY_PIXEL))
      continue;
    for (unsigned j = 0; j < parameter.DescriptorTable.NumDescriptorRanges; ++j)
      if (ordered_buffer_in_table(shader_info, parameter.DescriptorTable.pDescriptorRanges[j]))
        return true;
  }
  return false;
}

/**
 * Whether a DXIL module annotates a `dx.op.createHandleFromHeap` result (possibly through
 * phi/select) as a rasterizer ordered view: ResourceProperties dword0 bit 13 (IsROV).
 */
static bool module_has_heap_rov(llvm::Module &module) {
  constexpr uint32_t kCreateHandleFromHeap = 218;
  constexpr uint32_t kAnnotateHandle = 216;
  constexpr uint32_t kPropertyIsROV = 1u << 13;
  auto opcode_of = [](const llvm::Value *value, uint32_t &opcode) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(value);
    auto *callee = call ? call->getCalledFunction() : nullptr;
    if (!callee || !callee->getName().startswith("dx.op.") || call->arg_size() < 1)
      return false;
    auto *op = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
    if (!op)
      return false;
    opcode = uint32_t(op->getZExtValue());
    return true;
  };
  auto from_heap = [&](llvm::Value *root) {
    llvm::SmallVector<llvm::Value *, 8> pending{root};
    llvm::SmallPtrSet<llvm::Value *, 8> seen;
    while (!pending.empty()) {
      auto *value = pending.pop_back_val();
      if (!seen.insert(value).second)
        continue;
      uint32_t opcode;
      if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value))
        pending.append(phi->incoming_values().begin(), phi->incoming_values().end());
      else if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value))
        pending.append({select->getTrueValue(), select->getFalseValue()});
      else if (opcode_of(value, opcode) && opcode == kCreateHandleFromHeap)
        return true;
    }
    return false;
  };
  for (auto &function : module)
    for (auto &block : function)
      for (auto &instruction : block) {
        uint32_t opcode;
        if (!opcode_of(&instruction, opcode) || opcode != kAnnotateHandle)
          continue;
        auto *call = llvm::cast<llvm::CallInst>(&instruction);
        auto *properties = call->arg_size() >= 3 ? llvm::dyn_cast<llvm::ConstantAggregate>(call->getArgOperand(2))
                                                 : nullptr;
        auto *dword0 = properties && properties->getNumOperands() >= 1
                           ? llvm::dyn_cast<llvm::ConstantInt>(properties->getOperand(0))
                           : nullptr;
        if (dword0 && (dword0->getZExtValue() & kPropertyIsROV) && from_heap(call->getArgOperand(1)))
          return true;
      }
  return false;
}

std::unique_ptr<BindingMap>
setup_binding_rootsig(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type, const void *bytecode, size_t bytecode_length,
    uint32_t root_sig_slot, uint32_t static_sampler_slot,
    const void *local_bytecode, size_t local_bytecode_length,
    bool bind_signature_arguments, uint32_t local_root_slot
) {
  using namespace microsoft;
  using namespace dxmt::dxbc;
  using namespace dxmt::air;
  using namespace dxmt::shader::common;

  auto binding_map = std::make_unique<RootSignatureBindingMap>();

  /*
   * The pipeline's global root signature, or null when the pipeline has none.
   *
   * A raytracing pipeline is not required to declare one, and a graphics/compute
   * pipeline always does; every export then still has to resolve its resources
   * through something, which is why at least one of the two signatures must be
   * present (the caller rejects a pipeline with neither).
   */
  RootSignatureDeserializer deserializer;
  const D3D12_ROOT_SIGNATURE_DESC1 *root_sig =
      (bytecode && bytecode_length) ? deserialize_root_signature(bytecode, bytecode_length, deserializer)
                                    : nullptr;

  air::ArgumentBufferBuilder builder;
  unsigned inc_attribute_index = 0;

  auto check_visibility = [=](D3D12_SHADER_VISIBILITY visibility) -> bool {
    if (visibility == D3D12_SHADER_VISIBILITY_ALL)
      return true;
    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_VERTEX:
      if (shader_type == D3D10_SB_VERTEX_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_HULL:
      if (shader_type == D3D11_SB_HULL_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_DOMAIN:
      if (shader_type == D3D11_SB_DOMAIN_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_GEOMETRY:
      if (shader_type == D3D10_SB_GEOMETRY_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_PIXEL:
      if (shader_type == D3D10_SB_PIXEL_SHADER)
        return true;
      break;
    default:
      break;
    }
    return false;
  };

  /**
   * Whether two shader-visible register ranges overlap.
   *
   * D3D12 forbids the register bindings of a local root signature from
   * overlapping the global root signature's for the same shader, and the
   * bindings it compares are the register ranges themselves: the space and the
   * interval of registers a range covers. An unbounded range (`~0u`) reaches to
   * the end of its space, i.e. its exclusive end is 2^32.
   */
  auto registers_overlap = [](const ResourceRange &a, const ResourceRange &b) -> bool {
    if (a.space != b.space)
      return false;
    uint64_t a_end = a.size == ~0u ? (uint64_t)~0u + 1 : (uint64_t)a.lower_bound + a.size;
    uint64_t b_end = b.size == ~0u ? (uint64_t)~0u + 1 : (uint64_t)b.lower_bound + b.size;
    return (uint64_t)a.lower_bound < b_end && (uint64_t)b.lower_bound < a_end;
  };

  /**
   * Claim every range the shader declares that `desc` defines into `ranges`, and
   * lay the qword entry of `desc` out in `entry_builder`.
   *
   * One walk serves both root signatures of a dispatch. They have the same qword
   * entry layout - one qword per descriptor table or root descriptor, one qword
   * per two root constants, in parameter order - so the element index of a
   * parameter is its qword offset in either entry, and the ranges a signature
   * defines are read from that signature's own entry.
   *
   * `other` is the other signature's ranges. The register range of every range
   * claimed here is checked against it, so the two signatures together never
   * define one register twice: a register both would serve is a signature pair
   * D3D12 rejects, and it fails the build here instead of one of them winning by
   * precedence.
   */
  auto build_ranges = [&](const D3D12_ROOT_SIGNATURE_DESC1 &desc, air::ArgumentBufferBuilder &entry_builder,
                          RootSignatureRanges &ranges, const RootSignatureRanges &other,
                          unsigned &attribute_index) -> bool {
    auto claim = [&](const auto &other_map, const ResourceRange &range) -> bool {
      for (auto &[_, other_claim] : other_map) {
        if (!registers_overlap(range, other_claim.first.range))
          continue;
        /* A register both signatures would serve is a signature pair D3D12
         * rejects. It is reported through the same "invalid root signature" path
         * every other malformed signature uses, never by aborting the compiler. */
        return false;
      }
      return true;
    };

    for (unsigned i = 0; i < desc.NumParameters; i++) {
      auto &Parameter = desc.pParameters[i];

      switch (Parameter.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        auto &table = Parameter.DescriptorTable;
        unsigned descriptor_offset = 0;
        std::vector<air::RasterOrderedDescriptorRange> ordered_ranges;
        if (check_visibility(Parameter.ShaderVisibility)) {
          constexpr uint32_t MaxShaderVisibleResourceDescriptors = 1'000'000;
          uint64_t next_offset = 0;
          for (unsigned range_index = 0; range_index < table.NumDescriptorRanges; ++range_index) {
            const auto &range = table.pDescriptorRanges[range_index];
            uint64_t first = range.OffsetInDescriptorsFromTableStart == ~0u
                               ? next_offset : range.OffsetInDescriptorsFromTableStart;
            next_offset = first + range.NumDescriptors;
            if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
              continue;
            for (const auto &[_, uav] : shader_info->uavMap) {
              if (!ordered_uav_in_range(uav, range))
                continue;
              uint64_t offset = first + uav.range.lower_bound - range.BaseShaderRegister;
              /* No shader-visible resource heap can contain more than one million
               * descriptors. This also supplies the finite AIR type of a D3D
               * unbounded range without allocating or copying that many entries. */
              if (offset >= MaxShaderVisibleResourceDescriptors)
                return false;
              uint64_t count = std::min<uint64_t>(
                  uav.range.size,
                  MaxShaderVisibleResourceDescriptors - offset
              );
              if (range.NumDescriptors != ~0u)
                count = std::min<uint64_t>(count, range.NumDescriptors -
                                              (uav.range.lower_bound - range.BaseShaderRegister));
              if (!count)
                return false;
              air::RasterOrderedDescriptorRange ordered{
                  static_cast<uint32_t>(offset), static_cast<uint32_t>(count), std::nullopt, uav.with_counter
              };
              if (uav.resource_type != shader::common::ResourceType::NonApplicable) {
                auto kind = air::to_air_resource_type(uav.resource_type);
                ordered.texture = air::MSLTexture{
                    air::to_air_scaler_type(uav.scaler_type), MemoryAccess::read_write,
                    air::lowering_texture_1d_to_2d(kind), kind
                };
              }
              ordered_ranges.push_back(ordered);
            }
          }
        }
        uint32_t table_index;
        if (ordered_ranges.empty()) {
          table_index = entry_builder.DefineBuffer(
              "table" + std::to_string(i), AddressSpace::constant, MemoryAccess::read, msl_int,
              attribute_index++
          );
        } else {
          std::sort(ordered_ranges.begin(), ordered_ranges.end(), [](const auto &a, const auto &b) {
            return a.descriptor_offset < b.descriptor_offset;
          });
          for (size_t j = 1; j < ordered_ranges.size(); ++j)
            if (static_cast<uint64_t>(ordered_ranges[j - 1].descriptor_offset) +
                    ordered_ranges[j - 1].descriptor_count > ordered_ranges[j].descriptor_offset)
              return false;
          auto [table_type, table_info] = air::BuildRasterOrderedDescriptorTable(
              ordered_ranges, module.getContext(), module.getDataLayout()
          );
          table_index = entry_builder.DefineIndirectBuffer(
              "table" + std::to_string(i), table_type, table_info, attribute_index++
          );
        }
        if (!check_visibility(Parameter.ShaderVisibility))
          break;
        for (unsigned i = 0; i < table.NumDescriptorRanges; i++) {
          auto &range = table.pDescriptorRanges[i];
          auto calculated_descriptor_offset = range.OffsetInDescriptorsFromTableStart == ~0u
                                                  ? descriptor_offset
                                                  : range.OffsetInDescriptorsFromTableStart;
          descriptor_offset = calculated_descriptor_offset + range.NumDescriptors;
          switch (range.RangeType) {
          case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: {
            for (auto &[_, srv] : shader_info->srvMap) {
              if (srv.range.space != range.RegisterSpace)
                continue;
              if (range_contained(srv.range, range)) {
                auto range_id = srv.range.range_id;
                if (!claim(other.SRVs, srv.range))
                  return false;
                ranges.SRVs[range_id] = {
                    srv, calculated_descriptor_offset + (srv.range.lower_bound - range.BaseShaderRegister)
                };
                ranges.SRVs[range_id].first.arg_index = table_index;
              }
            }
            break;
          }
          case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
            for (auto &[_, uav] : shader_info->uavMap) {
              if (uav.range.space != range.RegisterSpace)
                continue;
              if (range_contained(uav.range, range)) {
                auto range_id = uav.range.range_id;
                if (!claim(other.UAVs, uav.range))
                  return false;
                ranges.UAVs[range_id] = {
                    uav, calculated_descriptor_offset + (uav.range.lower_bound - range.BaseShaderRegister)
                };
                ranges.UAVs[range_id].first.arg_index = table_index;
              }
            }
            break;
          case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
            for (auto &[_, cbuf] : shader_info->cbufferMap) {
              if (cbuf.range.space != range.RegisterSpace)
                continue;
              if (range_contained(cbuf.range, range)) {
                auto range_id = cbuf.range.range_id;
                if (!claim(other.ConstantBuffers, cbuf.range))
                  return false;
                ranges.ConstantBuffers[range_id] = {
                    cbuf, calculated_descriptor_offset + (cbuf.range.lower_bound - range.BaseShaderRegister)
                };
                ranges.ConstantBuffers[range_id].first.arg_index = table_index;
              }
            }
            break;
          case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
            for (auto &[_, sampler] : shader_info->samplerMap) {
              if (sampler.range.space != range.RegisterSpace)
                continue;
              if (range_contained(sampler.range, range)) {
                auto range_id = sampler.range.range_id;
                if (!claim(other.Samplers, sampler.range))
                  return false;
                ranges.Samplers[range_id] = {
                    sampler, calculated_descriptor_offset + (sampler.range.lower_bound - range.BaseShaderRegister)
                };
                ranges.Samplers[range_id].first.arg_index = table_index;
              }
            }
            break;
          default:
            assert(0 && "unknown range type");
            return false;
          }
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
        auto &constants = Parameter.Constants;
        auto qwords = (constants.Num32BitValues >> 1) + (constants.Num32BitValues & 1);
        auto first = entry_builder.DefineInteger64("root_constant" + std::to_string(i), attribute_index++);
        for (unsigned x = 1; x < qwords; x++) {
          entry_builder.DefineInteger64(
              "root_constant" + std::to_string(i) + "_" + std::to_string(x), attribute_index++
          );
        }
        if (!check_visibility(Parameter.ShaderVisibility))
          break;
        for (auto &[_, cbuf] : shader_info->cbufferMap) {
          if (cbuf.range.lower_bound == constants.ShaderRegister && cbuf.range.space == constants.RegisterSpace) {
            auto range_id = cbuf.range.range_id;
            if (!claim(other.ConstantBuffers, cbuf.range))
              return false;
            ranges.ConstantBuffers[range_id] = {cbuf, (~0u - 1)};
            ranges.ConstantBuffers[range_id].first.arg_index = first;
          }
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_CBV: {
        auto &descriptor = Parameter.Descriptor;
        auto descriptor_index = entry_builder.DefineBuffer(
            "cb" + std::to_string(i), AddressSpace::constant, MemoryAccess::read, msl_uint4,
            attribute_index++
        );
        if (!check_visibility(Parameter.ShaderVisibility))
          break;
        for (auto &[_, cbuf] : shader_info->cbufferMap) {
          if (cbuf.range.lower_bound == descriptor.ShaderRegister && cbuf.range.space == descriptor.RegisterSpace) {
            auto range_id = cbuf.range.range_id;
            if (!claim(other.ConstantBuffers, cbuf.range))
              return false;
            ranges.ConstantBuffers[range_id] = {cbuf, ~0u};
            ranges.ConstantBuffers[range_id].first.arg_index = descriptor_index;
          }
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_SRV: {
        auto &descriptor = Parameter.Descriptor;
        auto descriptor_index = entry_builder.DefineBuffer(
            "t" + std::to_string(i), AddressSpace::device, MemoryAccess::read, msl_uint, attribute_index++
        );
        if (!check_visibility(Parameter.ShaderVisibility))
          break;
        for (auto &[_, srv] : shader_info->srvMap) {
          if (srv.range.lower_bound == descriptor.ShaderRegister && srv.range.space == descriptor.RegisterSpace) {
            assert(srv.resource_type == shader::common::ResourceType::NonApplicable);
            auto range_id = srv.range.range_id;
            if (!claim(other.SRVs, srv.range))
              return false;
            ranges.SRVs[range_id] = {srv, ~0u};
            ranges.SRVs[range_id].first.arg_index = descriptor_index;
            ranges.SRVs[range_id].first.arg_metadata_index = ~0u; // status qword, set after the heap qwords
          }
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_UAV: {
        auto &descriptor = Parameter.Descriptor;
        bool raster_order = false;
        if (check_visibility(Parameter.ShaderVisibility)) {
          for (const auto &[_, uav] : shader_info->uavMap)
            if (uav.range.lower_bound == descriptor.ShaderRegister &&
                uav.range.space == descriptor.RegisterSpace && uav.rasterizer_order)
              raster_order = true;
        }
        auto descriptor_index = entry_builder.DefineBuffer(
            "u" + std::to_string(i), AddressSpace::device, MemoryAccess::read_write, msl_uint,
            attribute_index++, raster_order ? std::optional<uint32_t>(1) : std::nullopt
        );
        if (!check_visibility(Parameter.ShaderVisibility))
          break;
        for (auto &[_, uav] : shader_info->uavMap) {
          if (uav.range.lower_bound == descriptor.ShaderRegister && uav.range.space == descriptor.RegisterSpace) {
            assert(uav.resource_type == shader::common::ResourceType::NonApplicable);
            auto range_id = uav.range.range_id;
            if (!claim(other.UAVs, uav.range))
              return false;
            ranges.UAVs[range_id] = {uav, ~0u};
            ranges.UAVs[range_id].first.arg_index = descriptor_index;
            ranges.UAVs[range_id].first.arg_metadata_index = ~0u; // status qword, set after the heap qwords
          }
        }
        break;
      }
      }
    }
    return true;
  };

  if (root_sig &&
      !build_ranges(*root_sig, builder, binding_map->Global, binding_map->Local, inc_attribute_index))
    return {};

  /*
   * SM 6.6 directly indexed heaps. A signature that sets either DIRECTLY_INDEXED flag
   * gets four qwords after its parameters - resource heap base, resource heap
   * descriptor count, sampler heap base, sampler heap descriptor count - which the
   * command list fills from its SetDescriptorHeaps state whenever it uploads the root
   * arguments (MTLD3D12RootSignature::HeapQwordOffset). Every field of the entry is
   * one qword, so the field index is the qword offset.
   */
  if (root_sig && (static_cast<UINT>(root_sig->Flags) & kRootSignatureFlagsDirectlyIndexed)) {
    binding_map->ResourceHeapIndexed =
        (static_cast<UINT>(root_sig->Flags) & kRootSignatureFlagCbvSrvUavHeapDirectlyIndexed) != 0;
    /*
     * A rasterizer ordered view taken from ResourceDescriptorHeap. Metal orders the
     * fragment accesses of memory reached through an argument member carrying
     * `raster_order_group`, including a member of a dynamically indexed indirect buffer,
     * so the resource heap qword becomes a pointer to 32 byte descriptors whose first
     * member is a raster-ordered device buffer. Every heap access keeps casting that
     * pointer to its own descriptor shape (a texture handle occupies the same qword);
     * the ordering covers them all, which is correct and only serializes more.
     */
    if (binding_map->ResourceHeapIndexed && shader_type == D3D10_SB_PIXEL_SHADER && module_has_heap_rov(module)) {
      air::ArgumentBufferBuilder descriptor;
      descriptor.DefineBuffer("resource", AddressSpace::device, MemoryAccess::read_write, msl_uint, 0, 1);
      descriptor.DefineInteger64("metadata", 1);
      descriptor.DefineInteger64("aux", 2);
      descriptor.DefineInteger64("pad", 3);
      auto [descriptor_type, descriptor_info] = descriptor.Build(module.getContext(), module.getDataLayout());
      assert(module.getDataLayout().getTypeAllocSize(descriptor_type) == 32);
      binding_map->HeapFieldIndex =
          builder.DefineIndirectBuffer("resource_heap", descriptor_type, descriptor_info, inc_attribute_index++);
      binding_map->ResourceHeapRasterOrdered = true;
    } else {
      binding_map->HeapFieldIndex = builder.DefineInteger64("resource_heap", inc_attribute_index++);
    }
    builder.DefineInteger64("resource_heap_count", inc_attribute_index++);
    builder.DefineInteger64("sampler_heap", inc_attribute_index++);
    builder.DefineInteger64("sampler_heap_count", inc_attribute_index++);
    binding_map->SamplerHeapIndexed =
        (static_cast<UINT>(root_sig->Flags) & kRootSignatureFlagSamplerHeapDirectlyIndexed) != 0;
  }

  /*
   * Residency status bases of root SRV/UAV descriptors (TILED): one qword per root SRV/UAV
   * parameter, in parameter order, after the parameters and the heap qwords
   * (MTLD3D12RootSignature::RootViewStatusQwordOffset). The runtime writes the reserved buffer's
   * biased status base (BufferAllocation::residencyStatusBase) or 0 there on SetRoot*View. Local
   * root signatures have no such qwords; their root views report fully mapped.
   */
  if (root_sig && !(static_cast<UINT>(root_sig->Flags) & 0x80u /* LOCAL_ROOT_SIGNATURE, see IsLocal */)) {
    for (unsigned i = 0; i < root_sig->NumParameters; i++) {
      const auto &Parameter = root_sig->pParameters[i];
      if (Parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_SRV &&
          Parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_UAV)
        continue;
      auto status_index = builder.DefineInteger64("root_status" + std::to_string(i), inc_attribute_index++);
      const auto &descriptor = Parameter.Descriptor;
      auto matches = [&](const ResourceRange &range) {
        return range.lower_bound == descriptor.ShaderRegister && range.space == descriptor.RegisterSpace;
      };
      if (Parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV) {
        for (auto &[_, srv] : binding_map->Global.SRVs)
          if (srv.second == ~0u && matches(srv.first.range))
            srv.first.arg_metadata_index = status_index;
      } else {
        for (auto &[_, uav] : binding_map->Global.UAVs)
          if (uav.second == ~0u && matches(uav.first.range) && !uav.first.rasterizer_order)
            uav.first.arg_metadata_index = status_index;
      }
    }
  }

  auto [type, metadata] = builder.Build(module.getContext(), module.getDataLayout());
  binding_map->GlobalRootArgumentsType = type;

  /*
   * The root argument argument, and the static sampler one below.
   *
   * Every shader the runtime binds the command list state to declares them and reads
   * them from there. A visible function reached through the visible function table
   * cannot: its ABI is fixed at payload, hit state, record and depth, so appending
   * these two would make the indirect call type mismatch the callee and the callee
   * read garbage. Such a stage hands the addresses of the same entries in with
   * SetGlobalRootArgumentPointer / SetStaticSamplerArgumentPointer instead, and asks
   * for the arguments not to be declared (`bind_signature_arguments` false); the map
   * then resolves through the handed pointers and reports an unbound range until they
   * are handed in, exactly like the local root arguments.
   *
   * A signature with no parameters declares no root argument entry at all: the entry
   * holds nothing, and the runtime binds nothing for it (there is no qword to stage),
   * so declaring it would leave an argument the pipeline never binds. That is the
   * signature a raytracing pipeline with no global root signature gets.
   */
  if (bind_signature_arguments && !builder.Empty())
    binding_map->RootSignatureArgumentSlot = func_signature.DefineInput(air::ArgumentBindingIndirectBuffer{
        .location_index = root_sig_slot, // SM50_BINDING_INDEX_ROOT_ARGUMENTS
        .array_size = 1,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::constant,
        .struct_type = type,
        .struct_type_info = metadata,
        .arg_name = "rootsig",
    });

  if (root_sig && root_sig->NumStaticSamplers) {
    for (unsigned i = 0; i < root_sig->NumStaticSamplers; i++) {
      auto &State = root_sig->pStaticSamplers[i];

      for (auto &[_, sampler] : shader_info->samplerMap) {
        if (sampler.range.space != State.RegisterSpace)
          continue;
        if (sampler.range.lower_bound != State.ShaderRegister)
          continue;
        if (!check_visibility(State.ShaderVisibility))
          break;
        auto range_id = sampler.range.range_id;
        binding_map->Global.Samplers[range_id] = {sampler, ~0u};
        binding_map->Global.Samplers[range_id].first.arg_index = i;
      }
    }
  }

  /*
   * The export's local root signature, when it has one.
   *
   * Its ranges are the record's own root arguments and are kept apart from the
   * global ones, so a range is only ever read from the entry of the signature
   * that defines it. The entry's layout is the local signature's own qword
   * layout, which is what the runtime materializes per shader record
   * (`MTLD3D12RootSignature`'s local root copies: root constants 4 byte packed,
   * root descriptors and descriptor tables 8 byte aligned, each placed at the
   * qword offset of its parameter).
   */
  if (local_bytecode && local_bytecode_length) {
    RootSignatureDeserializer local_deserializer;
    auto *local_sig = deserialize_root_signature(local_bytecode, local_bytecode_length, local_deserializer);
    if (!local_sig)
      return {};

    air::ArgumentBufferBuilder local_builder;
    unsigned local_attribute_index = 0;
    if (!build_ranges(
            *local_sig, local_builder, binding_map->Local, binding_map->Global, local_attribute_index
        ))
      return {};

    auto [local_type, local_type_info] = local_builder.Build(module.getContext(), module.getDataLayout());
    binding_map->LocalRootArgumentsType = local_type;

    /*
     * The local root table itself, for the stages that take signature arguments.
     *
     * The runtime binds the buffer holding every record's materialized local root
     * entry at `local_root_slot` for every DispatchRays, and nothing declared it
     * before, so the binding was dead. Declaring it is what lets a ray generation
     * shader (the single record of its own table, i.e. the table base) read its own
     * local root arguments; a visible function cannot declare it - its ABI is fixed -
     * so it takes the entry its caller selected, through the pointer above.
     */
    if (bind_signature_arguments && ~local_root_slot)
      binding_map->LocalRootArgumentSlot = func_signature.DefineInput(air::ArgumentBindingIndirectBuffer{
          .location_index = local_root_slot, // SM50_RT_BINDING_INDEX_LOCAL_ROOT_TABLE
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .struct_type = binding_map->LocalRootArgumentsType,
          .struct_type_info = local_type_info,
          .arg_name = "local_roots",
      });

    /*
     * A static sampler of the local signature is pipeline state, not a record
     * argument, so it is not read from the local root entry: the runtime binds one
     * static sampler table for the whole dispatch and every root signature the
     * dispatch can use has to define a static sampler of a given register identically.
     * The pipeline builds that table's signature from the union of the global root
     * signature's static samplers and the ones only a local one defines, and hands it
     * in as the primary signature, so a local static sampler resolves through the
     * primary's identical definition, at that definition's index in the table - the
     * same slot the runtime encodes the sampler into and the same slot every other root
     * signature of the dispatch uses for it.
     */
    for (unsigned i = 0; i < local_sig->NumStaticSamplers; i++) {
      auto &State = local_sig->pStaticSamplers[i];

      for (auto &[_, sampler] : shader_info->samplerMap) {
        if (sampler.range.space != State.RegisterSpace)
          continue;
        if (sampler.range.lower_bound != State.ShaderRegister)
          continue;
        if (!check_visibility(State.ShaderVisibility))
          break;
        auto range_id = sampler.range.range_id;
        /*
         * The primary signature is the union of every root signature the dispatch can
         * use, so it defines this sampler: it is claimed as a static sampler there
         * (the only claim whose second is `~0u`). The check is defensive - if the
         * primary ever served this register with a descriptor table instead, a table
         * slot would be an unrelated sampler and the build has to fail rather than
         * sample through it.
         */
        auto GlobalClaim = binding_map->Global.Samplers.find(range_id);
        if (GlobalClaim == binding_map->Global.Samplers.end() || GlobalClaim->second.second != ~0u)
          return {};
        binding_map->Local.Samplers[range_id] = {sampler, ~0u};
        binding_map->Local.Samplers[range_id].first.arg_index = GlobalClaim->second.first.arg_index;
      }
    }
  }

  if (!root_sig || root_sig->NumStaticSamplers == 0)
    return binding_map;

  if (bind_signature_arguments)
    binding_map->StaticSamplerArgumentSlot = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = static_sampler_slot, // SM50_BINDING_INDEX_STATIC_SAMPLERS
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::constant,
        .type = air::msl_ulong,
        .arg_name = "static_samplers",
        .raster_order_group = {},
    });

  return binding_map;
}

} // namespace dxmt::dxbc

AIRCONV_API int SM50NeedsRasterOrderedBufferMarker(
    sm50_shader_t pShader, const void *pRootSignature, size_t RootSignatureLength
) {
  if (!pShader || !pRootSignature || !RootSignatureLength)
    return 0;
  const auto *shader = static_cast<const dxmt::dxbc::SM50ShaderInternal *>(pShader);
  if (shader->shader_type != microsoft::D3D10_SB_PIXEL_SHADER)
    return 0;
  dxmt::RootSignatureDeserializer deserializer;
  const auto *root = dxmt::dxbc::deserialize_root_signature(
      pRootSignature, RootSignatureLength, deserializer);
  return root && dxmt::dxbc::needs_ordered_buffer_marker(shader->shader_info, *root);
}
