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
#include "llvm/IR/DerivedTypes.h"
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
    auto HeapPointer = GetEntryField(Builder, Entry, SRV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto ResourceKindLogical = air::to_air_resource_type(SRV.resource_type, SRV.compared);
    auto ResourceKind = air::lowering_texture_1d_to_2d(ResourceKindLogical);
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(SRV.scaler_type)
    );
    auto MemoryAccess = SRV.sampled ? Texture::MemoryAccess::access_sample : Texture::MemoryAccess::access_read;

    auto [Handle, Metadata] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, SRV.range.lower_bound, DescriptorOffset);
    return TextureDescirptor{Handle, Metadata, false, ResourceKind, ResourceKindLogical, MemoryAccess, SampleType};
  }

  llvm::Optional<TextureDescirptor>
  ResolveUAVTexture(
      llvm::air::AIRBuilder &Builder, llvm::Value *Entry, const UnorderedAccessViewInfo &UAV,
      uint64_t DescriptorOffset, llvm::Value *Index
  ) {
    if (UAV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    auto HeapPointer = GetEntryField(Builder, Entry, UAV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
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
    auto [Handle, Metadata] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, UAV.range.lower_bound, DescriptorOffset);
    return TextureDescirptor{Handle,       Metadata,  UAV.global_coherent, ResourceKind, ResourceKindLogical,
                             MemoryAccess, SampleType};
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
      return BufferDescriptor{HeapPointer, Builder.builder.getInt64(0xffffffff), SRV.structure_stride, false};
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), SRV.range.lower_bound,
        DescriptorOffset
    );

    return BufferDescriptor{Pointer, Metadata, SRV.structure_stride, false};
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
      return BufferDescriptor{
          HeapPointer, Builder.builder.getInt64(0xffffffff), UAV.structure_stride, UAV.global_coherent
      };
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), UAV.range.lower_bound,
        DescriptorOffset
    );

    return BufferDescriptor{Pointer, Metadata, UAV.structure_stride, UAV.global_coherent};
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

  std::pair<llvm::Value *, llvm::Value *>
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
      (version != D3D_ROOT_SIGNATURE_VERSION_1 && version != D3D_ROOT_SIGNATURE_VERSION_1_1))
    return nullptr;
  if (FAILED(deserializer.Deserialize(bytecode, bytecode_length)))
    return nullptr;
  return &deserializer.desc_1_1_.Desc_1_1;
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

  auto range_contained = [=](const ResourceRange &range, const D3D12_DESCRIPTOR_RANGE1 &range_rootsig) -> bool {
    if (range.lower_bound < range_rootsig.BaseShaderRegister)
      return false;
    if (range_rootsig.NumDescriptors == ~0u)
      return true;
    if (range.size == ~0u)
      return true;
    if (range.lower_bound + range.size <= range_rootsig.BaseShaderRegister + range_rootsig.NumDescriptors)
      return true;
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
        auto table_index = entry_builder.DefineBuffer(
            "table" + std::to_string(i), AddressSpace::constant, MemoryAccess::read, msl_int,
            attribute_index++
        );
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
          }
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_UAV: {
        auto &descriptor = Parameter.Descriptor;
        auto descriptor_index = entry_builder.DefineBuffer(
            "u" + std::to_string(i), AddressSpace::device, MemoryAccess::read_write /* FIXME */, msl_uint,
            attribute_index++, std::nullopt /* FIXME: */
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
