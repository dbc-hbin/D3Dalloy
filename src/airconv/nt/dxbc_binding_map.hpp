#pragma once

#include "air_builder.hpp"
#include "llvm/IR/Value.h"

namespace dxmt::dxbc {

struct ConstantBufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata; // may be null
};

struct SamplerDescriptor {
  llvm::Value *SamplerHandle;
  llvm::Value *CubeSamplerHandle;
  llvm::Value *Metadata;
};

struct TextureDescirptor {
  llvm::Value *ResourceHandle;
  llvm::Value *Metadata;
  bool GlobalCoherent;
  llvm::air::Texture::ResourceKind ResourceKind;
  llvm::air::Texture::ResourceKind ResourceKindLogical;
  llvm::air::Texture::MemoryAccess MemoryAccess;
  llvm::air::Texture::SampleType SampleType;

};

struct BufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata;
  uint32_t StructureStride;
  bool GlobalCoherent;
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
  virtual uint32_t RootSignatureArgumentIndex() const { return ~0u; }
  virtual uint32_t StaticSamplerArgumentIndex() const { return ~0u; }
};

} // namespace dxmt::dxbc
