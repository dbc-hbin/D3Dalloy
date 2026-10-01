#pragma once

#include "air_builder.hpp"
#include "llvm/IR/Value.h"

namespace dxmt::dxbc {

struct ShaderResourceViewInfo;
struct UnorderedAccessViewInfo;

struct ConstantBufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata; // may be null
};

struct SamplerDescriptor {
  llvm::Value *SamplerHandle;
  llvm::Value *CubeSamplerHandle;
  llvm::Value *Metadata;
  // D3D12 sampler descriptor q3 (MinLOD | MaxLOD << 32); only sampler feedback reads it.
  llvm::Value *LodRange = nullptr;
};

struct TextureDescirptor {
  llvm::Value *ResourceHandle;
  llvm::Value *Metadata;
  bool GlobalCoherent;
  llvm::air::Texture::ResourceKind ResourceKind;
  llvm::air::Texture::ResourceKind ResourceKindLogical;
  llvm::air::Texture::MemoryAccess MemoryAccess;
  llvm::air::Texture::SampleType SampleType;
  // Only D3D12 texture3D UAVs pack FirstWSlice/WSize in the high metadata word.
  bool Scoped3DUAV = false;
  // D3D12-only RGB32 typed-buffer descriptor slots q2/q3. Legacy SM50 binding
  // tables do not carry this metadata and leave the fields null.
  llvm::Value *EmulationAddress = nullptr;
  llvm::Value *EmulationTag = nullptr;
  // Typed buffer views of reserved buffers: the same q2/q3 as residency status (StatusBase = the
  // buffer's biased residency base, StatusView = first byte address << 8 | log2(texel) << 1, bit 0
  // clear; bit 0 set means q2/q3 are the RGB32 slots above). Only status queries read them.
  llvm::Value *StatusBase = nullptr;
  llvm::Value *StatusView = nullptr;
  // RGB32 SRVs: q0 (unused as a texture handle) is the reserved buffer's biased residency base.
  llvm::Value *RGB32StatusBase = nullptr;
  // D3D12 texture SRVs: q2 view range for sampler feedback (bit 63 valid; mips/slices packed).
  llvm::Value *ViewRange = nullptr;

};

struct BufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata;
  uint32_t StructureStride;
  bool GlobalCoherent;
  /* i64 biased tile-residency base of a reserved buffer (descriptor qword 3), 0 when always mapped;
     null for root descriptors, which are always reported fully mapped. Read only by status queries. */
  llvm::Value *StatusBase = nullptr;
};

/**
 * An acceleration structure bound as a root-signature SRV.
 *
 * `RaytracingAccelerationStructure` is an ordinary SRV whose descriptor holds the
 * D3D12 GPU virtual address of the structure; `GpuVA` is that address as an i64
 * (the descriptor-table slot's first qword, or the root SRV argument itself). The
 * compiler loads the structure's 64 byte header from it and takes the native
 * structure out of the header's resource id, so the descriptor index stays dynamic
 * and no descriptor-to-structure bookkeeping happens on the CPU.
 */
struct AccelerationStructureDescriptor {
  llvm::Value *GpuVA;
};

struct CounterDescriptor {
  llvm::Value *Pointer;
};

using RangeId = uint32_t;

class BindingMap {
public:
  virtual ~BindingMap() {};
  virtual llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;

  /*
   * SM 6.6 dynamic resources: descriptors fetched straight out of the bound
   * CBV_SRV_UAV / sampler heap by absolute heap index (`dx.op.createHandleFromHeap`).
   *
   * The shape of a heap descriptor is not declared anywhere but at the use site, so the
   * caller hands the view info in (`range` is ignored). Only a root-signature map whose
   * signature sets D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED /
   * SAMPLER_HEAP_DIRECTLY_INDEXED can resolve them; every other map returns none, which
   * the caller reports as a shader / root signature mismatch.
   *
   * An index at or past the bound heap's size, or no heap bound at all, reads the all-zero
   * null descriptor, so a stray index is a null access instead of a GPU fault.
   */
  virtual llvm::Optional<ConstantBufferDescriptor>
  GetHeapConstantBuffer(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    (void)Builder;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<SamplerDescriptor>
  GetHeapSampler(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    (void)Builder;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<TextureDescirptor>
  GetHeapSRVTexture(llvm::air::AIRBuilder &Builder, const ShaderResourceViewInfo &Info, llvm::Value *Index) {
    (void)Builder;
    (void)Info;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<TextureDescirptor>
  GetHeapUAVTexture(llvm::air::AIRBuilder &Builder, const UnorderedAccessViewInfo &Info, llvm::Value *Index) {
    (void)Builder;
    (void)Info;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<BufferDescriptor>
  GetHeapSRVBuffer(llvm::air::AIRBuilder &Builder, const ShaderResourceViewInfo &Info, llvm::Value *Index) {
    (void)Builder;
    (void)Info;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<BufferDescriptor>
  GetHeapUAVBuffer(llvm::air::AIRBuilder &Builder, const UnorderedAccessViewInfo &Info, llvm::Value *Index) {
    (void)Builder;
    (void)Info;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<CounterDescriptor>
  GetHeapUAVCounter(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    (void)Builder;
    (void)Index;
    return llvm::None;
  }
  virtual llvm::Optional<AccelerationStructureDescriptor>
  GetHeapAccelerationStructure(llvm::air::AIRBuilder &Builder, llvm::Value *Index) {
    (void)Builder;
    (void)Index;
    return llvm::None;
  }
  /**
   * The acceleration structure a raytracing export traces against. Only a
   * raytracing binding map implements it; the SM 5.x table2 map has no raytracing
   * resources and returns nullopt.
   */
  virtual llvm::Optional<AccelerationStructureDescriptor>
  GetAccelerationStructure(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    (void)Builder;
    (void)Range;
    (void)Index;
    return llvm::None;
  }
  /**
   * The local root arguments of the shader record the invocation runs as.
   *
   * A raytracing export's root signature can define some of its ranges in a local
   * root signature, whose arguments live in the record of the shader table being
   * dispatched instead of in the command list state. The stage builder knows that
   * address (the record's local root table plus the record index times the entry
   * stride) and hands it in before the resource lowering runs, so the map resolves
   * those ranges through it exactly like the global root arguments.
   *
   * The default does nothing because only a root-signature binding map can have
   * local ranges: a map built for the SM 5.x argument tables has no root signature
   * at all, so it has nothing to resolve locally either.
   */
  virtual void SetLocalRootArgumentPointer(llvm::Value *Pointer) { (void)Pointer; }
  /**
   * The command list's global root arguments, for a shader that cannot take them
   * as an argument.
   *
   * The runtime binds them at the root argument slot, and every shader it binds the
   * command list state to takes them from there. A visible function called through
   * the visible function table has a fixed ABI - the traversal hands it its payload,
   * its hit state, its record and its depth, and nothing else - so it cannot, and
   * its stage builder reads their address out of the invocation frame and hands it
   * in here before the resource lowering runs. The address space of the value handed
   * in is the one used, so the stage may hand a device or a constant pointer.
   *
   * The default does nothing because only a root-signature binding map has root
   * arguments to resolve: a map built for the SM 5.x argument tables has none.
   */
  virtual void SetGlobalRootArgumentPointer(llvm::Value *Pointer) { (void)Pointer; }
  /**
   * The static sampler table of the dispatch, for a shader that cannot take it as an
   * argument, exactly like the global root arguments above.
   */
  virtual void SetStaticSamplerArgumentPointer(llvm::Value *Pointer) { (void)Pointer; }
  /**
   * Argument positions of the root argument entry and of the static sampler table,
   * or `~0u` when the shader does not declare them (a visible function cannot; see
   * the two setters above).
   *
   * A stage that has to publish their addresses into the invocation frame - the ray
   * generation shader, whose visible callees cannot declare them either - reads the
   * arguments it was handed by these positions. Only a root-signature binding map
   * declares them, so the default is "none".
   */
  /**
   * SV_ViewID (view instancing): the view-id qword the runtime appends last to a non-local root signature's
   * entry (MTLD3D12RootSignature::ViewIdQwordOffset), truncated to 32 bits. Null for a map without root
   * arguments; the caller then reads 0, which is what a PSO without view instancing requires.
   */
  virtual llvm::Value *ViewInstanceId(llvm::air::AIRBuilder &Builder) {
    (void)Builder;
    return nullptr;
  }
  virtual uint32_t RootSignatureArgumentIndex() const { return ~0u; }
  virtual uint32_t StaticSamplerArgumentIndex() const { return ~0u; }
};

} // namespace dxmt::dxbc
