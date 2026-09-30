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

/*
 * DXIL `dx.op` resource operations -> AIR.
 *
 * The DXIL CFG/SSA is left alone: only `dx.op` calls are rewritten, so handle
 * values keep flowing through the original phi/select/dynamic index chains and
 * each handle is resolved where it is defined. Descriptor access is delegated
 * to dxbc::BindingMap, i.e. the same lookup the SM5.1 DXBC root signature path
 * uses (dxbc_binding_rootsig.cpp).
 *
 * Descriptor ABI, 32 bytes per shader visible descriptor, produced by
 * src/d3d12/d3d12_descriptor_heap.cpp:
 *
 *   +0   handle / pointer        (texture or buffer view id, device address)
 *   +8   metadata                (see below)
 *   +16  aux                     (UAV counter pointer, cube sampler handle)
 *   +24  padding
 *
 *   typed buffer SRV/UAV: low 32 = firstElement, high 32 = elementCount
 *   raw/structured buffer: low 32 = byte length
 *   texture: low 32 = min LOD clamp (float), high 32 = array length
 *   constant buffer: size in bytes
 *   sampler: low 32 = LOD bias (float)
 *
 * Both halves of a typed view are honoured: typed buffer accesses are biased by
 * firstElement and bound checked against elementCount, every other buffer
 * access is bound checked against the byte length. A UAV counter is a plain
 * device pointer and is regularly placed inside the buffer it belongs to (the
 * shared 4096-byte counter convention), so it is used verbatim.
 */

#include "dxil_converter.hpp"

#include <functional>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "nt/air_builder.hpp"
#include "nt/dxbc_binding_map.hpp"

namespace dxmt::dxil {

namespace {

/* dx.op opcodes handled by this translation unit, see DxilConstants.h. */
enum Opcode : uint32_t {
  kCreateHandle = 57,
  kCBufferLoadLegacy = 59,
  kSample = 60,
  kSampleBias = 61,
  kSampleLevel = 62,
  kSampleGrad = 63,
  kSampleCmp = 64,
  kSampleCmpLevelZero = 65,
  kTextureLoad = 66,
  kTextureStore = 67,
  kBufferLoad = 68,
  kBufferStore = 69,
  kBufferUpdateCounter = 70,
  kCheckAccessFullyMapped = 71,
  kGetDimensions = 72,
  kTextureGather = 73,
  kTextureGatherCmp = 74,
  kTexture2DMSGetSamplePosition = 75,
  kRenderTargetGetSamplePosition = 76,
  kRenderTargetGetSampleCount = 77,
  kAtomicBinOp = 78,
  kAtomicCompareExchange = 79,
  kCalculateLOD = 81,
  kRawBufferLoad = 139,
  kRawBufferStore = 140,
  kCreateHandleForLib = 160,
  kAnnotateHandle = 216,
  kCreateHandleFromBinding = 217,
  kTextureGatherRaw = 223,
  kSampleCmpLevel = 224,
  kTextureStoreSample = 225,
  kBarrierByMemoryHandle = 245,
  kSampleCmpGrad = 254,
  kSampleCmpBias = 255,
  kCreateHandleFromHeap = 218,
};

/* DxilResourceProperties dword 0 (DirectXShaderCompiler DxilResourceProperties.h). */
constexpr uint32_t kPropertyKindMask = 0xffu;
constexpr uint32_t kPropertyIsUAV = 1u << 12;
constexpr uint32_t kPropertyIsROV = 1u << 13;
constexpr uint32_t kPropertyGloballyCoherent = 1u << 14;

/* How a heap-indexed handle is used; its view shape is only known from these. */
struct HeapUsage {
  bool read = false;
  bool written = false;
  bool sampled = false;
  bool compared = false;
};

HeapUsage
HeapUsageOfOpcode(uint32_t opcode) {
  HeapUsage usage;
  switch (opcode) {
  case 60: /* Sample */
  case 61: /* SampleBias */
  case 62: /* SampleLevel */
  case 63: /* SampleGrad */
  case 73: /* TextureGather */
  case 81: /* CalculateLOD */
    usage.read = usage.sampled = true;
    break;
  case 64:  /* SampleCmp */
  case 65:  /* SampleCmpLevelZero */
  case 74:  /* TextureGatherCmp */
  case 224: /* SampleCmpLevel */
  case 254: /* SampleCmpGrad */
  case 255: /* SampleCmpBias */
    usage.read = usage.sampled = usage.compared = true;
    break;
  case 67:  /* TextureStore */
  case 69:  /* BufferStore */
  case 140: /* RawBufferStore */
  case 225: /* TextureStoreSample */
    usage.written = true;
    break;
  case 70: /* BufferUpdateCounter */
  case 78: /* AtomicBinOp */
  case 79: /* AtomicCompareExchange */
    usage.read = usage.written = true;
    break;
  default:
    usage.read = true;
    break;
  }
  return usage;
}

/* DXIL::AtomicBinOpCode. */
enum AtomicOperation : uint32_t {
  kAtomicAdd = 0,
  kAtomicAnd = 1,
  kAtomicOr = 2,
  kAtomicXor = 3,
  kAtomicIMin = 4,
  kAtomicIMax = 5,
  kAtomicUMin = 6,
  kAtomicUMax = 7,
  kAtomicExchange = 8,
};

/* DXIL resource classes, matching ResourceClass. */
enum ClassIndex : uint32_t {
  kSrvClass = 0,
  kUavClass = 1,
  kCbvClass = 2,
  kSamplerClass = 3,
};

/* Slots of a resolved handle; a binding only fills the ones it has. */
enum Slot : unsigned {
  kSlotPrimary = 0,
  kSlotAux = 1,
  kSlotMetadata = 2,
  kSlotCounter = 3,
  kSlotCount = 4,
};

using SlotArray = std::array<llvm::Value *, kSlotCount>;

bool
IsDxilCall(const llvm::CallInst *call, uint32_t &opcode) {
  auto *callee = call->getCalledFunction();
  if (!callee || !callee->isDeclaration())
    return false;
  if (!callee->getName().startswith("dx.op."))
    return false;
  if (call->arg_size() == 0)
    return false;
  auto *operation = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
  if (!operation)
    return false;
  opcode = operation->getZExtValue();
  return true;
}

bool
IsUndefOrPoison(const llvm::Value *value) {
  return llvm::isa<llvm::UndefValue>(value) || llvm::isa<llvm::PoisonValue>(value);
}

/* Resource shapes that are addressed through a texture descriptor. Typed
 * buffers ("Buffer<T>") and tbuffer are texture_buffer in AIR, matching the
 * DXBC frontend's ResourceType::TextureBuffer mapping. */
bool
IsTextureKind(ResourceKind kind) {
  switch (kind) {
  case ResourceKind::Texture1D:
  case ResourceKind::Texture2D:
  case ResourceKind::Texture2DMS:
  case ResourceKind::Texture3D:
  case ResourceKind::TextureCube:
  case ResourceKind::Texture1DArray:
  case ResourceKind::Texture2DArray:
  case ResourceKind::Texture2DMSArray:
  case ResourceKind::TextureCubeArray:
  case ResourceKind::TypedBuffer:
  case ResourceKind::TBuffer:
  case ResourceKind::FeedbackTexture2D:
  case ResourceKind::FeedbackTexture2DArray:
    return true;
  default:
    return false;
  }
}

/*
 * Device coherent memory access is only emitted for Metal 3.2 and newer
 * (dxbc_converter.cpp:SupportsMemoryCoherency). LowerStage records the
 * requested version with setup_metal_version() before resources are lowered, so
 * read it back rather than duplicating compilation argument handling. SM 6.x is
 * D3D12 only, which never sets the D3D11 sampling/FMA flags.
 */
bool
MetalVersionSupportsCoherentAccess(const llvm::Module &module) {
  auto *language = module.getNamedMetadata("air.language_version");
  if (!language || language->getNumOperands() == 0)
    return false;
  auto *tuple = llvm::dyn_cast<llvm::MDTuple>(language->getOperand(0));
  if (!tuple || tuple->getNumOperands() < 3)
    return false;
  auto major_md = llvm::dyn_cast<llvm::ConstantAsMetadata>(tuple->getOperand(1));
  auto minor_md = llvm::dyn_cast<llvm::ConstantAsMetadata>(tuple->getOperand(2));
  if (!major_md || !minor_md)
    return false;
  auto *major = llvm::dyn_cast<llvm::ConstantInt>(major_md->getValue());
  auto *minor = llvm::dyn_cast<llvm::ConstantInt>(minor_md->getValue());
  if (!major || !minor)
    return false;
  return major->getZExtValue() > 3 || (major->getZExtValue() == 3 && minor->getZExtValue() >= 2);
}

/* A handle plus everything needed to lower operations on it. */
struct ResourceHandle {
  /* Null for handles that only carry a dx.op.annotateHandle shape. */
  const ResourceBinding *binding = nullptr;
  ResourceKind kind = ResourceKind::Invalid;

  llvm::Optional<dxbc::TextureDescirptor> texture;
  llvm::Optional<dxbc::BufferDescriptor> buffer;
  llvm::Optional<dxbc::ConstantBufferDescriptor> cbuffer;
  llvm::Optional<dxbc::SamplerDescriptor> sampler;
  /* The GPU virtual address of a RaytracingAccelerationStructure's descriptor. */
  llvm::Optional<dxbc::AccelerationStructureDescriptor> acceleration_structure;
  llvm::Value *counter = nullptr;
};

/* Where a handle comes from, before its descriptors are materialised. */
struct HandleSite {
  uint32_t klass = 0;
  uint32_t space = 0;
  /* Physical register of the range, independent of its BindingMap key. */
  uint32_t lower_bound = 0;
  /* `!dx.resources` declaration ID, unique within the resource class. */
  uint32_t declaration_id = 0;
  /* Absolute register index of the handle: `createHandle`'s index operand (for
   * `createHandleForLib`, the base register of the range the object names). */
  llvm::Value *index = nullptr;
  const ResourceBinding *record = nullptr;
  ResourceKind annotated_kind = ResourceKind::Invalid;
  std::string name;
};

class ResourceLowering {
public:
  ResourceLowering(ModuleInfo &info, dxbc::BindingMap &binding) :
      info(info),
      binding(binding),
      ctx(info.module->getContext()),
      builder(ctx),
      nulldbg(),
      air(llvm::air::AIRBuilderOptions{}, builder, nulldbg),
      coherent_access(MetalVersionSupportsCoherentAccess(*info.module)) {}

  llvm::Error run();

private:
  ModuleInfo &info;
  dxbc::BindingMap &binding;
  llvm::LLVMContext &ctx;
  llvm::IRBuilder<> builder;
  llvm::raw_null_ostream nulldbg;
  llvm::air::AIRBuilder air;
  bool coherent_access;
  /* The AIR entry is a kernel: implicit derivatives come from compute quads (SM 6.6). */
  bool kernel_stage = false;

  /* Resolved handle values, keyed by the SSA value that produces them. */
  llvm::DenseMap<llvm::Value *, ResourceHandle> resolved;
  /* dx.op.annotateHandle shapes, keyed by the annotated handle value. */
  llvm::DenseMap<llvm::Value *, ResourceKind> annotations;
  /* Both dx.types.ResourceProperties dwords of every annotated handle. */
  llvm::DenseMap<llvm::Value *, std::pair<uint32_t, uint32_t>> annotation_properties;
  /* Combined usage of each createHandleFromHeap, through every phi/select it reaches. */
  llvm::DenseMap<llvm::Value *, HeapUsage> heap_usage;
  /* Handle values consumed by dx.op.bufferUpdateCounter. */
  llvm::DenseSet<llvm::Value *> counter_handles;

  /* ---- descriptor decoding ---- */

  llvm::Value *Low32(llvm::Value *metadata);
  llvm::Value *High32(llvm::Value *metadata);
  llvm::Value *Low32AsFloat(llvm::Value *metadata);
  llvm::Value *scoped3DUAVAddress(const dxbc::TextureDescirptor &descriptor, llvm::Value *address);
  llvm::Value *scoped3DUAVInBounds(const dxbc::TextureDescirptor &descriptor, llvm::Value *address);

  /* ---- handle resolution ---- */

  void collectAnnotations();
  void collectCounterHandles();
  void collectHeapUsage();
  void markCounterHandle(llvm::Value *value);

  llvm::Expected<ResourceHandle> resolve(llvm::Value *handle);
  llvm::Expected<ResourceHandle> materialise(llvm::CallInst *call);
  llvm::Expected<ResourceHandle> lookupDescriptors(const HandleSite &site, bool needs_counter);
  llvm::Expected<ResourceHandle> lookupHeapDescriptors(llvm::CallInst *call, bool needs_counter);
  llvm::Expected<HandleSite> siteFromCreateHandle(llvm::CallInst *call);
  llvm::Expected<HandleSite> siteFromCreateHandleFromBinding(llvm::CallInst *call);
  llvm::Expected<HandleSite> siteFromCreateHandleForLib(llvm::CallInst *call);

  llvm::Expected<ResourceHandle> resolvePhi(llvm::PHINode *phi);
  llvm::Expected<ResourceHandle> resolveSelect(llvm::SelectInst *select);
  llvm::Expected<ResourceHandle> combine(
      llvm::ArrayRef<ResourceHandle> parts, llvm::ArrayRef<llvm::BasicBlock *> incoming_blocks,
      llvm::Instruction *where, llvm::Value *condition
  );

  SlotArray slotsOf(const ResourceHandle &handle);
  ResourceHandle rebuild(const ResourceHandle &model, const SlotArray &slots);

  llvm::Value *resolveCounter(llvm::Value *handle);

  /* ---- operations ---- */

  llvm::Error lowerCall(llvm::CallInst *call);
  llvm::Error guardNullDescriptors(llvm::CallInst *call);
  llvm::Error lowerCBufferLoadLegacy(llvm::CallInst *call);
  llvm::Error lowerTextureLoad(llvm::CallInst *call);
  llvm::Error lowerTextureStore(llvm::CallInst *call);
  llvm::Error lowerBufferLoad(llvm::CallInst *call);
  llvm::Error lowerBufferStore(llvm::CallInst *call);
  llvm::Error lowerRawBufferLoad(llvm::CallInst *call);
  llvm::Error lowerRawBufferStore(llvm::CallInst *call);
  llvm::Error lowerSample(llvm::CallInst *call);
  llvm::Error lowerGather(llvm::CallInst *call);
  llvm::Error lowerGatherCmp(llvm::CallInst *call);
  llvm::Error lowerTexture2DMSGetSamplePosition(llvm::CallInst *call);
  llvm::Error lowerRenderTargetGetSamplePosition(llvm::CallInst *call);
  llvm::Error lowerRenderTargetGetSampleCount(llvm::CallInst *call);
  llvm::Error lowerCalculateLOD(llvm::CallInst *call);
  llvm::Error lowerSampleBias(llvm::CallInst *call);
  llvm::Error lowerSampleLevel(llvm::CallInst *call);
  llvm::Error lowerSampleGrad(llvm::CallInst *call);
  llvm::Error lowerSampleCmp(llvm::CallInst *call, bool level_zero);
  llvm::Error lowerSampleCmpExtended(llvm::CallInst *call, uint32_t opcode);
  llvm::Error lowerGatherRaw(llvm::CallInst *call);
  llvm::Error lowerGetDimensions(llvm::CallInst *call);
  llvm::Error lowerBufferUpdateCounter(llvm::CallInst *call);
  llvm::Error lowerAtomicBinOp(llvm::CallInst *call);
  llvm::Error lowerAtomicCompareExchange(llvm::CallInst *call);

  /* ---- shared helpers ---- */

  llvm::air::Texture textureOf(const dxbc::TextureDescirptor &descriptor);
  llvm::Expected<int32_t> constantOperand(llvm::Value *value, const char *what);
  llvm::Expected<dxbc::TextureDescirptor> textureDescriptor(const ResourceHandle &handle, const char *who);
  llvm::Expected<dxbc::BufferDescriptor> bufferDescriptor(const ResourceHandle &handle, const char *who);
  uint32_t usedComponents(llvm::CallInst *call);

  llvm::Value *makeVector(llvm::ArrayRef<llvm::Value *> components);
  llvm::Value *padVector(llvm::Value *component, unsigned lanes, llvm::Value *zero);
  llvm::Value *clampArrayIndex(llvm::Value *value, llvm::Value *metadata);
  void appendTexel(llvm::SmallVectorImpl<llvm::Value *> &components, llvm::Value *texel, llvm::Type *element_type);
  llvm::Value *buildResult(llvm::CallInst *call, llvm::ArrayRef<llvm::Value *> components);
  /*
   * Residency status (i32, nonzero = fully mapped) that the next buildResult stores in the status
   * member of a resource operation's result; null means fully mapped. Consumed by buildResult.
   */
  llvm::Value *pending_status_ = nullptr;
  void setStatusFromResidency(llvm::Value *residency);
  llvm::Value *bufferTileStatus(llvm::Value *status_base, llvm::Value *first_byte, llvm::Value *last_byte);
  llvm::Error lowerCheckAccessFullyMapped(llvm::CallInst *call);
  bool usesStatus(llvm::CallInst *call);
  llvm::Value *texelBufferStatus(const dxbc::TextureDescirptor &descriptor, llvm::Value *element);

  llvm::Value *
  texelBufferAddress(const dxbc::TextureDescirptor &texture, llvm::Value *element_index, llvm::Value *offset);
  llvm::Value *bufferDwordIndex(llvm::Value *c0, llvm::Value *c1, uint32_t structure_stride);
  llvm::Value *bufferInBounds(const dxbc::BufferDescriptor &buffer, llvm::Value *dword_index);
  llvm::SmallVector<llvm::Value *, 4> emitGuardedAccess(
      llvm::Value *condition, llvm::Instruction *where, llvm::ArrayRef<llvm::Type *> result_types,
      llvm::function_ref<llvm::SmallVector<llvm::Value *, 4>()> emit
  );
  llvm::Value *bufferByteOffset(llvm::Value *c0, llvm::Value *c1, uint32_t structure_stride);
  llvm::Value *bufferBytesInBounds(const dxbc::BufferDescriptor &buffer, llvm::Value *byte, unsigned bytes);
  llvm::Value *bufferBytePointer(const dxbc::BufferDescriptor &buffer, llvm::Value *byte, llvm::Type *type);
  llvm::Error loadBufferWords(
      llvm::CallInst *call, const dxbc::BufferDescriptor &buffer, llvm::Value *c0, llvm::Value *c1, uint32_t mask
  );
  llvm::Error storeBufferWords(
      llvm::CallInst *call, const dxbc::BufferDescriptor &buffer, llvm::Value *c0, llvm::Value *c1, uint32_t mask
  );
  llvm::Error
  readTexture(
      llvm::CallInst *call, const dxbc::TextureDescirptor &texture, llvm::Value *address, llvm::Value *array_index,
      llvm::Value *sample_index, llvm::Value *level
  );
  llvm::Error writeTexture(
      llvm::CallInst *call, const dxbc::TextureDescirptor &texture, llvm::Value *address, llvm::Value *array_index,
      llvm::Value *value
  );

  /* atomics */
  llvm::Value *atomicAddress(const ResourceHandle &handle, llvm::CallInst *call, unsigned first_coordinate);
  llvm::Value *atomicArrayIndex(const ResourceHandle &handle, llvm::CallInst *call, unsigned first_coordinate);
  llvm::Value *atomicLockTable();
  llvm::Value *emitLockedAtomic64(
      llvm::Value *pointer, llvm::function_ref<llvm::Value *(llvm::Value *old)> update
  );
  llvm::Error lowerBarrierByMemoryHandle(llvm::CallInst *call);
  llvm::Error lowerBufferAtomic64(llvm::CallInst *call, const ResourceHandle &handle, bool compare_exchange);
  llvm::Constant *offsetVector(llvm::air::Texture::ResourceKind kind, const int32_t offsets[3]);

  /* sampling */
  llvm::Value *samplerBias(const dxbc::SamplerDescriptor &sampler);
  llvm::Value *minLodClamp(const dxbc::TextureDescirptor &descriptor);
  llvm::Error sampleOffsets(llvm::CallInst *call, unsigned first, int32_t offsets[3]);
  llvm::Error sampleTarget(
      const dxbc::TextureDescirptor &descriptor, llvm::Value *cube_sampler, llvm::Value *c0, llvm::Value *c1,
      llvm::Value *c2, llvm::Value *c3, llvm::Value *&coord, llvm::Value *&array_index, llvm::Value *&sampler_handle
  );
  uint32_t gradientCount(llvm::air::Texture::ResourceKind kind);
  /* Fine quad derivatives of a sample coordinate (compute shaders, SM 6.6). */
  std::pair<llvm::Value *, llvm::Value *> quadGradients(llvm::Value *coord, uint32_t components);
  /* `sample` with the implicit LOD of a compute quad; `bias` is added to that LOD. */
  std::pair<llvm::Value *, llvm::Value *> sampleInKernel(
      const dxbc::TextureDescirptor &descriptor, llvm::Value *sampler_value, llvm::Value *coord,
      llvm::Value *array_index, const int32_t offsets[3], llvm::Value *bias, llvm::Value *min_lod,
      llvm::Value *reference
  );
};

/* ----------------------------------------------------------------------- */
/* small helpers                                                           */
/* ----------------------------------------------------------------------- */

llvm::Value *
ResourceLowering::Low32(llvm::Value *metadata) {
  return builder.CreateTrunc(metadata, air.getIntTy());
}

llvm::Value *
ResourceLowering::High32(llvm::Value *metadata) {
  return builder.CreateTrunc(builder.CreateLShr(metadata, 32uLL), air.getIntTy());
}

llvm::Value *
ResourceLowering::Low32AsFloat(llvm::Value *metadata) {
  return builder.CreateBitCast(builder.CreateTrunc(metadata, air.getIntTy()), air.getFloatTy());
}

llvm::Value *
ResourceLowering::scoped3DUAVAddress(const dxbc::TextureDescirptor &descriptor, llvm::Value *address) {
  auto *first = builder.CreateAnd(High32(descriptor.Metadata), air.getInt(0xffff));
  auto *z = builder.CreateExtractElement(address, 2);
  return builder.CreateInsertElement(address, builder.CreateAdd(z, first), 2);
}

llvm::Value *
ResourceLowering::scoped3DUAVInBounds(const dxbc::TextureDescirptor &descriptor, llvm::Value *address) {
  auto *count = builder.CreateLShr(High32(descriptor.Metadata), 16);
  return builder.CreateICmpULT(builder.CreateExtractElement(address, 2), count);
}

llvm::air::Texture
ResourceLowering::textureOf(const dxbc::TextureDescirptor &descriptor) {
  llvm::air::Texture texture;
  texture.kind = descriptor.ResourceKind;
  texture.sample_type = descriptor.SampleType;
  texture.memory_access = descriptor.MemoryAccess;
  return texture;
}

llvm::Expected<int32_t>
ResourceLowering::constantOperand(llvm::Value *value, const char *what) {
  if (IsUndefOrPoison(value))
    return 0;
  if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
    return (int32_t)constant->getSExtValue();
  return createError(llvm::Twine("dxil: ") + what + " must be a constant");
}

llvm::Expected<dxbc::TextureDescirptor>
ResourceLowering::textureDescriptor(const ResourceHandle &handle, const char *who) {
  if (!handle.texture)
    return createError(llvm::Twine("dxil: ") + who + " requires a texture handle");
  return *handle.texture;
}

llvm::Expected<dxbc::BufferDescriptor>
ResourceLowering::bufferDescriptor(const ResourceHandle &handle, const char *who) {
  if (!handle.buffer)
    return createError(llvm::Twine("dxil: ") + who + " requires a buffer handle");
  return *handle.buffer;
}

uint32_t
ResourceLowering::usedComponents(llvm::CallInst *call) {
  uint32_t mask = 0;
  for (auto *user : call->users()) {
    auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
    if (!extract)
      return 0b1111u;
    for (unsigned index : extract->getIndices()) {
      if (index < 4)
        mask |= 1u << index;
    }
  }
  return mask ? mask : 0b1u;
}

llvm::Value *
ResourceLowering::makeVector(llvm::ArrayRef<llvm::Value *> components) {
  llvm::Value *vector =
      llvm::PoisonValue::get(llvm::FixedVectorType::get(components.front()->getType(), components.size()));
  for (unsigned i = 0; i < components.size(); i++)
    vector = builder.CreateInsertElement(vector, components[i], i);
  return vector;
}

llvm::Value *
ResourceLowering::padVector(llvm::Value *component, unsigned lanes, llvm::Value *zero) {
  llvm::SmallVector<llvm::Value *, 3> components{component};
  for (unsigned i = 1; i < lanes; i++)
    components.push_back(zero);
  return makeVector(components);
}

llvm::Value *
ResourceLowering::clampArrayIndex(llvm::Value *value, llvm::Value *metadata) {
  auto *rounded = air.CreateFPUnOp(llvm::air::AIRBuilder::rint, value);
  auto *integer = air.CreateConvertToSigned(rounded);
  auto *positive = air.CreateIntBinOp(llvm::air::AIRBuilder::max, integer, air.getInt(0), true);
  auto *max_index = builder.CreateSub(High32(metadata), air.getInt(1));
  return air.CreateIntBinOp(llvm::air::AIRBuilder::min, positive, max_index, true);
}

void
ResourceLowering::appendTexel(
    llvm::SmallVectorImpl<llvm::Value *> &components, llvm::Value *texel, llvm::Type *element_type
) {
  if (texel->getType()->isVectorTy()) {
    for (unsigned i = 0; i < 4; i++)
      components.push_back(builder.CreateExtractElement(texel, i));
    return;
  }
  components.push_back(texel);
  for (unsigned i = 1; i < 4; i++)
    components.push_back(llvm::UndefValue::get(element_type));
}

llvm::Value *
ResourceLowering::buildResult(llvm::CallInst *call, llvm::ArrayRef<llvm::Value *> components) {
  auto *type = llvm::cast<llvm::StructType>(call->getType());
  llvm::Value *result = llvm::UndefValue::get(type);
  for (unsigned i = 0; i < components.size(); i++)
    result = builder.CreateInsertValue(result, components[i], {i});
  if (type->getNumElements() == components.size() + 1) {
    /* Last component is the residency status of a resource operation: nonzero when every texel or
     * byte the operation touched is mapped. Only reserved resources ever report zero. */
    llvm::Value *status = pending_status_ ? pending_status_ : builder.getInt32(1);
    result = builder.CreateInsertValue(result, status, {type->getNumElements() - 1});
  }
  pending_status_ = nullptr;
  return result;
}

/* CheckAccessFullyMapped(status): status is the i32 an access stored, nonzero when fully mapped. */
llvm::Error
ResourceLowering::lowerCheckAccessFullyMapped(llvm::CallInst *call) {
  if (call->arg_size() < 2)
    return createError("dxil: CheckAccessFullyMapped without a status operand");
  builder.SetInsertPoint(call);
  auto *status = call->getArgOperand(1);
  auto *mapped = builder.CreateICmpNE(status, llvm::ConstantInt::get(status->getType(), 0));
  call->replaceAllUsesWith(mapped);
  call->eraseFromParent();
  return llvm::Error::success();
}

/* Whether any user of a resource operation's result reads its status member. */
bool
ResourceLowering::usesStatus(llvm::CallInst *call) {
  auto *type = llvm::dyn_cast<llvm::StructType>(call->getType());
  if (!type || type->getNumElements() < 2)
    return false;
  const unsigned status = type->getNumElements() - 1;
  for (auto *user : call->users()) {
    auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
    if (!extract || (extract->getNumIndices() == 1 && extract->getIndices()[0] == status))
      return true;
  }
  return false;
}

/*
 * Typed buffer view status: the view's q2 is the reserved buffer's biased residency base and q3 its
 * first byte address << 8 | log2(texel) << 1. Bit 0 marks RGB32 views, whose status base is q0 instead.
 */
llvm::Value *
ResourceLowering::texelBufferStatus(const dxbc::TextureDescirptor &descriptor, llvm::Value *element) {
  if (!descriptor.StatusBase || !descriptor.StatusView)
    return nullptr;
  auto *view = descriptor.StatusView;
  auto *rgb32 = builder.CreateICmpNE(builder.CreateAnd(view, builder.getInt64(1)), builder.getInt64(0));
  /* Out-of-bounds elements (all ones, see texelBufferAddress) read zero and never touch a tile. */
  auto *out_of_bounds = builder.CreateICmpEQ(element, llvm::ConstantInt::getAllOnesValue(element->getType()));
  auto *base = builder.CreateSelect(builder.CreateOr(rgb32, out_of_bounds), builder.getInt64(0), descriptor.StatusBase);
  auto *log2 = builder.CreateAnd(builder.CreateLShr(view, 1), builder.getInt64(127));
  auto *first = builder.CreateAdd(builder.CreateLShr(view, 8),
                                  builder.CreateShl(builder.CreateZExt(element, builder.getInt64Ty()), log2));
  auto *last = builder.CreateSub(builder.CreateAdd(first, builder.CreateShl(builder.getInt64(1), log2)),
                                 builder.getInt64(1));
  if (descriptor.RGB32StatusBase) {
    /* RGB32 views: 12 byte elements from q2 (EmulationAddress), status base in q0; bounded by q1. */
    auto *in_bounds = builder.CreateICmpULT(element, High32(descriptor.Metadata));
    auto *rgb_first = builder.CreateAdd(descriptor.EmulationAddress,
                                        builder.CreateMul(builder.CreateZExt(element, builder.getInt64Ty()),
                                                          builder.getInt64(12)));
    first = builder.CreateSelect(rgb32, rgb_first, first);
    last = builder.CreateSelect(rgb32, builder.CreateAdd(rgb_first, builder.getInt64(11)), last);
    base = builder.CreateSelect(rgb32,
                                builder.CreateSelect(in_bounds, descriptor.RGB32StatusBase, builder.getInt64(0)), base);
  }
  return bufferTileStatus(base, first, last);
}

void
ResourceLowering::setStatusFromResidency(llvm::Value *residency) {
  /* AIR texture results carry an i8 whose bit 0 is set when a texel was NOT resident (Metal lowers
   * sparse_color::resident() to !(flag & 1)). */
  if (!residency) {
    pending_status_ = nullptr;
    return;
  }
  auto *missing = builder.CreateAnd(residency, llvm::ConstantInt::get(residency->getType(), 1));
  pending_status_ = builder.CreateZExt(builder.CreateICmpEQ(missing, llvm::ConstantInt::get(residency->getType(), 0)),
                                       builder.getInt32Ty());
}

/*
 * Status of a buffer access touching GPU addresses [first_byte, last_byte] (i64): the byte-per-tile
 * residency array a reserved buffer's descriptor points at, biased so that the tile of address `va`
 * is at status_base + (va >> 16). A zero base means an ordinary, always-mapped buffer.
 */
llvm::Value *
ResourceLowering::bufferTileStatus(llvm::Value *status_base, llvm::Value *first_byte, llvm::Value *last_byte) {
  if (!status_base)
    return nullptr;
  auto *reserved = builder.CreateICmpNE(status_base, builder.getInt64(0));
  auto *where = &*builder.GetInsertPoint();
  auto loaded = emitGuardedAccess(reserved, where, {builder.getInt32Ty()}, [&]() -> llvm::SmallVector<llvm::Value *, 4> {
    auto *byte_ptr = builder.getInt8Ty()->getPointerTo(1);
    auto tile_byte = [&](llvm::Value *address) {
      auto *pointer = builder.CreateIntToPtr(builder.CreateAdd(status_base, builder.CreateLShr(address, 16)), byte_ptr);
      return builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), pointer), builder.getInt8(0));
    };
    /* An access never spans more than two 64KB tiles (at most 16 bytes). */
    auto *mapped = builder.CreateAnd(tile_byte(first_byte), tile_byte(last_byte));
    return {builder.CreateZExt(mapped, builder.getInt32Ty())};
  });
  /* An unreserved buffer takes the other branch, whose phi value is 0: turn that into 1. */
  return builder.CreateSelect(reserved, loaded[0], builder.getInt32(1));
}

/* ----------------------------------------------------------------------- */
/* addressing                                                              */
/* ----------------------------------------------------------------------- */

llvm::Value *
ResourceLowering::texelBufferAddress(
    const dxbc::TextureDescirptor &texture, llvm::Value *element_index, llvm::Value *offset
) {
  auto *address = offset ? builder.CreateAdd(element_index, offset) : element_index;
  if (texture.Metadata) {
    /* firstElement biases the access and elementCount is the bound, exactly like
     * the DXBC texture buffer path (nt/dxbc_converter_base.cpp InstLoad). */
    auto *out_of_bounds = builder.CreateICmpUGE(address, High32(texture.Metadata));
    address = builder.CreateAdd(address, Low32(texture.Metadata));
    address = builder.CreateSelect(out_of_bounds, llvm::ConstantInt::getAllOnesValue(address->getType()), address);
  }
  return address;
}

llvm::Value *
ResourceLowering::bufferDwordIndex(llvm::Value *c0, llvm::Value *c1, uint32_t structure_stride) {
  llvm::Value *byte_offset = c0;
  if (structure_stride) {
    byte_offset = builder.CreateAdd(builder.CreateMul(c0, air.getInt(structure_stride)), c1);
  }
  return builder.CreateLShr(byte_offset, 2);
}

/* Bounds condition of one buffer access. A descriptor without metadata (root
 * descriptor, root constants) carries no size, so D3D12 defines no bound for it
 * and the access stays unconditional. A descriptor with metadata - including a
 * zero filled null descriptor, whose length is zero - is bound checked. */
llvm::Value *
ResourceLowering::bufferInBounds(const dxbc::BufferDescriptor &buffer, llvm::Value *dword_index) {
  if (!buffer.Metadata)
    return nullptr;
  return builder.CreateICmpULT(dword_index, builder.CreateLShr(Low32(buffer.Metadata), 2));
}

/*
 * Runs `emit` - the memory access - inside a real branch taken only while
 * `condition` holds, and merges the results in the continuation block with zero
 * for every path that did not perform the access.
 *
 * D3D12 defines an out-of-range or unbound descriptor access as reading zero and
 * writing nothing, which means the access must not happen at all. Selecting a
 * null pointer *after* the address has been formed still issues the load or
 * store against address zero, so the guard is expressed as control flow instead:
 * the access only exists on the path where the descriptor covers it.
 *
 * A null `condition` means the descriptor has no bound (root descriptor); `emit`
 * then runs unconditionally, exactly as before.
 */
llvm::SmallVector<llvm::Value *, 4>
ResourceLowering::emitGuardedAccess(
    llvm::Value *condition, llvm::Instruction *where, llvm::ArrayRef<llvm::Type *> result_types,
    llvm::function_ref<llvm::SmallVector<llvm::Value *, 4>()> emit
) {
  if (!condition)
    return emit();
  /* Head keeps everything up to `where` and ends with the guard; `where` and the
   * rest move to the continuation block, so the access is emitted between them. */
  auto *head = where->getParent();
  auto *then_term = llvm::SplitBlockAndInsertIfThen(condition, where, /*Unreachable=*/false);
  auto *continuation = where->getParent();
  builder.SetInsertPoint(then_term);
  auto values = emit();
  /* `emit` may create its own control flow (the 64 bit atomic lock loop); the
   * values reach the continuation from whichever block it finished in. */
  auto *then_block = builder.GetInsertBlock();
  builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
  llvm::SmallVector<llvm::Value *, 4> merged;
  for (unsigned index = 0; index < values.size(); index++) {
    auto *phi = builder.CreatePHI(result_types[index], 2);
    phi->addIncoming(values[index], then_block);
    phi->addIncoming(llvm::Constant::getNullValue(result_types[index]), head);
    merged.push_back(phi);
  }
  return merged;
}

/*
 * Raw and structured buffer element access at the lane width of the dx.op overload
 * (SM 6.2 16 bit and SM 6.3 64 bit raw buffer access). The element is addressed in
 * bytes: DXIL guarantees 16 bit lanes are 2 byte aligned and 64 bit lanes only 4
 * byte aligned, so a 64 bit lane is moved as two dwords. An element that does not
 * lie entirely inside the view is an out-of-range access: it reads zero and is not
 * written.
 */
llvm::Value *
ResourceLowering::bufferByteOffset(llvm::Value *c0, llvm::Value *c1, uint32_t structure_stride) {
  if (!structure_stride)
    return c0;
  return builder.CreateAdd(builder.CreateMul(c0, air.getInt(structure_stride)), c1);
}

llvm::Value *
ResourceLowering::bufferBytesInBounds(const dxbc::BufferDescriptor &buffer, llvm::Value *byte, unsigned bytes) {
  if (!buffer.Metadata)
    return nullptr;
  auto *end = builder.CreateAdd(builder.CreateZExt(byte, builder.getInt64Ty()), builder.getInt64(bytes));
  return builder.CreateICmpULE(end, builder.CreateZExt(Low32(buffer.Metadata), builder.getInt64Ty()));
}

llvm::Value *
ResourceLowering::bufferBytePointer(const dxbc::BufferDescriptor &buffer, llvm::Value *byte, llvm::Type *type) {
  auto *bytes = builder.CreateBitCast(buffer.Pointer, builder.getInt8PtrTy(1));
  auto *address = builder.CreateGEP(builder.getInt8Ty(), bytes, {byte});
  return builder.CreateBitCast(address, type->getPointerTo(1));
}

llvm::Error
ResourceLowering::loadBufferWords(
    llvm::CallInst *call, const dxbc::BufferDescriptor &buffer, llvm::Value *c0, llvm::Value *c1, uint32_t mask
) {
  bool coherent = buffer.GlobalCoherent && coherent_access;
  auto *lane_type = llvm::cast<llvm::StructType>(call->getType())->getElementType(0);
  const unsigned width = lane_type->getPrimitiveSizeInBits();
  if (width != 16 && width != 32 && width != 64)
    return createError("dxil: raw buffer load lane must be 16, 32 or 64 bits wide");
  auto *lane_int = builder.getIntNTy(width);
  auto *byte_offset = bufferByteOffset(c0, c1, buffer.StructureStride);
  /* The addresses and their bound conditions are emitted before any block split so
   * they dominate the guarded access; only the access itself moves behind the branch. */
  llvm::SmallVector<unsigned, 4> lanes;
  llvm::SmallVector<llvm::Value *, 4> bytes;
  llvm::SmallVector<llvm::Value *, 4> conditions;
  for (unsigned i = 0; i < 4; i++) {
    if (!(mask & (1u << i)))
      continue;
    auto *byte = builder.CreateAdd(byte_offset, air.getInt(i * width / 8));
    lanes.push_back(i);
    bytes.push_back(byte);
    conditions.push_back(bufferBytesInBounds(buffer, byte, width / 8));
  }
  auto load = [&](llvm::Type *type, llvm::Value *pointer) -> llvm::Value * {
    return coherent ? (llvm::Value *)air.CreateDeviceCoherentLoad(type, pointer)
                    : (llvm::Value *)builder.CreateLoad(type, pointer);
  };
  llvm::SmallVector<llvm::Value *, 4> components(4, llvm::UndefValue::get(lane_type));
  for (unsigned lane = 0; lane < lanes.size(); lane++) {
    auto *byte = bytes[lane];
    auto guarded = emitGuardedAccess(
        conditions[lane], call, {lane_int},
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          if (width != 64)
            return {load(lane_int, bufferBytePointer(buffer, byte, lane_int))};
          auto *low = load(air.getIntTy(), bufferBytePointer(buffer, byte, air.getIntTy()));
          auto *high = load(air.getIntTy(),
                            bufferBytePointer(buffer, builder.CreateAdd(byte, air.getInt(4)), air.getIntTy()));
          return {builder.CreateOr(builder.CreateZExt(low, lane_int),
                                   builder.CreateShl(builder.CreateZExt(high, lane_int), 32))};
        }
    );
    components[lanes[lane]] = builder.CreateBitCast(guarded[0], lane_type);
  }
  if (!bytes.empty() && buffer.StatusBase && usesStatus(call)) {
    /* The descriptor pointer is the view's GPU address; the lanes span [first, last] bytes past it. */
    /* Out-of-bounds lanes read zero without touching memory, so they must not index the tile array. */
    llvm::Value *in_bounds = conditions.front();
    for (auto *condition : conditions)
      in_bounds = builder.CreateAnd(in_bounds, condition);
    auto *status_base = builder.CreateSelect(in_bounds, buffer.StatusBase, builder.getInt64(0));
    auto *base = builder.CreatePtrToInt(buffer.Pointer, builder.getInt64Ty());
    auto *first = builder.CreateAdd(base, builder.CreateZExt(bytes.front(), builder.getInt64Ty()));
    auto *last = builder.CreateAdd(base, builder.CreateZExt(builder.CreateAdd(bytes.back(), air.getInt(width / 8 - 1)),
                                                           builder.getInt64Ty()));
    pending_status_ = bufferTileStatus(status_base, first, last);
  }
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::storeBufferWords(
    llvm::CallInst *call, const dxbc::BufferDescriptor &buffer, llvm::Value *c0, llvm::Value *c1, uint32_t mask
) {
  bool coherent = buffer.GlobalCoherent && coherent_access;
  const unsigned width = call->getArgOperand(4)->getType()->getPrimitiveSizeInBits();
  if (width != 16 && width != 32 && width != 64)
    return createError("dxil: raw buffer store lane must be 16, 32 or 64 bits wide");
  auto *lane_int = builder.getIntNTy(width);
  auto *byte_offset = bufferByteOffset(c0, c1, buffer.StructureStride);
  llvm::SmallVector<llvm::Value *, 4> bytes;
  llvm::SmallVector<llvm::Value *, 4> conditions;
  llvm::SmallVector<llvm::Value *, 4> values;
  for (unsigned i = 0; i < 4; i++) {
    if (!(mask & (1u << i)))
      continue;
    auto *byte = builder.CreateAdd(byte_offset, air.getInt(i * width / 8));
    bytes.push_back(byte);
    conditions.push_back(bufferBytesInBounds(buffer, byte, width / 8));
    values.push_back(builder.CreateBitCast(call->getArgOperand(4 + i), lane_int));
  }
  auto store = [&](llvm::Value *value, llvm::Value *pointer) {
    if (coherent)
      air.CreateDeviceCoherentStore(value, pointer);
    else
      builder.CreateStore(value, pointer);
  };
  for (unsigned lane = 0; lane < bytes.size(); lane++) {
    auto *byte = bytes[lane];
    auto *value = values[lane];
    /* An out-of-range or unbound store writes nothing, so the store itself must
     * not be issued; a select over a null pointer would still store to zero. */
    emitGuardedAccess(
        conditions[lane], call, {},
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          if (width != 64) {
            store(value, bufferBytePointer(buffer, byte, lane_int));
            return {};
          }
          store(builder.CreateTrunc(value, air.getIntTy()), bufferBytePointer(buffer, byte, air.getIntTy()));
          store(builder.CreateTrunc(builder.CreateLShr(value, 32), air.getIntTy()),
                bufferBytePointer(buffer, builder.CreateAdd(byte, air.getInt(4)), air.getIntTy()));
          return {};
        }
    );
  }
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::readTexture(
    llvm::CallInst *call, const dxbc::TextureDescirptor &descriptor, llvm::Value *address, llvm::Value *array_index,
    llvm::Value *sample_index, llvm::Value *level
) {
  auto texture = textureOf(descriptor);
  if (descriptor.ResourceKindLogical == llvm::air::Texture::texture_buffer && descriptor.EmulationTag) {
    auto *rgb32 = builder.CreateICmpNE(builder.CreateAnd(descriptor.EmulationTag, builder.getInt64(1)),
                                      builder.getInt64(0));
    auto *in_bounds = builder.CreateICmpULT(address, High32(descriptor.Metadata));
    auto *non_null = builder.CreateICmpNE(descriptor.EmulationAddress,
                                         llvm::ConstantInt::getNullValue(descriptor.EmulationAddress->getType()));
    auto *valid = builder.CreateAnd(rgb32, builder.CreateAnd(in_bounds, non_null));
    auto *lane_type = llvm::cast<llvm::StructType>(call->getType())->getElementType(0);
    llvm::SmallVector<llvm::Type *, 4> word_types(4, air.getIntTy());
    auto words = emitGuardedAccess(valid, call, word_types,
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          auto *pointer = builder.CreateIntToPtr(descriptor.EmulationAddress, air.getIntTy()->getPointerTo(1));
          auto *base = builder.CreateMul(builder.CreateZExt(address, builder.getInt64Ty()), builder.getInt64(3));
          llvm::SmallVector<llvm::Value *, 4> result;
          for (unsigned channel = 0; channel < 3; ++channel)
            result.push_back(builder.CreateLoad(air.getIntTy(),
                builder.CreateGEP(air.getIntTy(), pointer, builder.CreateAdd(base, builder.getInt64(channel)))));
          result.push_back(air.getInt(lane_type->isFloatingPointTy() ? 0x3f800000u : 1u));
          auto *mapping = builder.CreateTrunc(builder.CreateLShr(descriptor.EmulationTag, 32), air.getIntTy());
          llvm::SmallVector<llvm::Value *, 4> mapped;
          for (unsigned channel = 0; channel < 4; ++channel) {
            auto *selector = builder.CreateAnd(builder.CreateLShr(mapping, channel * 3), air.getInt(7));
            llvm::Value *word = air.getInt(0);
            for (unsigned component = 0; component < 4; ++component)
              word = builder.CreateSelect(builder.CreateICmpEQ(selector, air.getInt(component)),
                                          result[component], word);
            mapped.push_back(builder.CreateSelect(builder.CreateICmpEQ(selector, air.getInt(5)),
                                                  result[3], word));
          }
          return mapped;
        });
    llvm::SmallVector<llvm::Type *, 4> lane_types(4, lane_type);
    auto native = emitGuardedAccess(builder.CreateNot(rgb32), call, lane_types,
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          auto [texel, residency] = air.CreateRead(texture, descriptor.ResourceHandle, address, array_index,
                                                   sample_index, level, false);
          llvm::SmallVector<llvm::Value *, 4> values;
          appendTexel(values, texel, lane_type);
          return values;
        });
    llvm::SmallVector<llvm::Value *, 4> components;
    for (unsigned channel = 0; channel < 4; ++channel)
      components.push_back(builder.CreateSelect(rgb32, builder.CreateBitCast(words[channel], lane_type), native[channel]));
    if (usesStatus(call))
      pending_status_ = texelBufferStatus(descriptor, address);
    call->replaceAllUsesWith(buildResult(call, components));
    call->eraseFromParent();
    return llvm::Error::success();
  }
  llvm::Value *texel = nullptr;
  if (texture.memory_access == llvm::air::Texture::acesss_readwrite)
    air.CreateTextureFence(texture, descriptor.ResourceHandle);
  if (descriptor.Scoped3DUAV) {
    auto *valid = scoped3DUAVInBounds(descriptor, address);
    address = scoped3DUAVAddress(descriptor, address);
    auto *type = llvm::FixedVectorType::get(llvm::cast<llvm::StructType>(call->getType())->getElementType(0), 4);
    bool read_supported = true;
    texel = emitGuardedAccess(valid, call, {type}, [&]() -> llvm::SmallVector<llvm::Value *, 4> {
      auto *value = air.CreateRead(texture, descriptor.ResourceHandle, address, array_index, sample_index, level,
                                   descriptor.GlobalCoherent && coherent_access).first;
      if (!value) {
        read_supported = false;
        return {llvm::PoisonValue::get(type)};
      }
      return {value};
    })[0];
    if (!read_supported)
      return createError("dxil: AIR texture read failed");
  } else {
    auto read = air.CreateRead(texture, descriptor.ResourceHandle, address, array_index, sample_index, level,
                               descriptor.GlobalCoherent && coherent_access);
    texel = read.first;
    /* Buffer views read through texture_buffer have no native residency: use the tile bitmap. */
    if (descriptor.ResourceKindLogical != llvm::air::Texture::texture_buffer)
      setStatusFromResidency(read.second);
  }
  if (!texel)
    return createError("dxil: AIR texture read failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  if (descriptor.ResourceKindLogical == llvm::air::Texture::texture_buffer && usesStatus(call))
    pending_status_ = texelBufferStatus(descriptor, address);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::writeTexture(
    llvm::CallInst *call, const dxbc::TextureDescirptor &descriptor, llvm::Value *address, llvm::Value *array_index,
    llvm::Value *value
) {
  auto texture = textureOf(descriptor);
  if (descriptor.Scoped3DUAV) {
    auto *valid = scoped3DUAVInBounds(descriptor, address);
    address = scoped3DUAVAddress(descriptor, address);
    emitGuardedAccess(valid, call, {}, [&]() -> llvm::SmallVector<llvm::Value *, 4> {
      air.CreateWrite(texture, descriptor.ResourceHandle, address, array_index, nullptr, air.getInt(0), value,
                      descriptor.GlobalCoherent && coherent_access);
      return {};
    });
  } else {
    air.CreateWrite(texture, descriptor.ResourceHandle, address, array_index, nullptr, air.getInt(0), value,
                    descriptor.GlobalCoherent && coherent_access);
  }
  return llvm::Error::success();
}

/* ----------------------------------------------------------------------- */
/* handle collection                                                       */
/* ----------------------------------------------------------------------- */

void
ResourceLowering::collectAnnotations() {
  for (auto &function : *info.module) {
    for (auto &block : function) {
      for (auto &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        uint32_t opcode;
        if (!call || !IsDxilCall(call, opcode) || opcode != kAnnotateHandle)
          continue;
        auto *properties = llvm::dyn_cast<llvm::ConstantAggregate>(call->getArgOperand(2));
        if (!properties || properties->getNumOperands() < 1)
          continue;
        auto *shape = llvm::dyn_cast<llvm::ConstantInt>(properties->getOperand(0));
        if (!shape)
          continue;
        /* ResourceKind lives in the low byte of the first property word, the
         * upper bits are sampling/coherence/ROV flags. */
        uint32_t kind = shape->getZExtValue() & 0xffu;
        if (kind == 0 || kind >= 19)
          continue;
        auto *handle = call->getArgOperand(1);
        uint32_t dword1 = 0;
        if (properties->getNumOperands() >= 2)
          if (auto *second = llvm::dyn_cast<llvm::ConstantInt>(properties->getOperand(1)))
            dword1 = second->getZExtValue();
        annotation_properties.try_emplace(handle, uint32_t(shape->getZExtValue()), dword1);
        auto entry = annotations.find(handle);
        if (entry != annotations.end() && entry->second != (ResourceKind)kind)
          continue; /* the validator guarantees one shape; keep the first */
        annotations[handle] = (ResourceKind)kind;
      }
    }
  }
}

void
ResourceLowering::markCounterHandle(llvm::Value *value) {
  if (!counter_handles.insert(value).second)
    return;
  if (auto *call = llvm::dyn_cast<llvm::CallInst>(value)) {
    uint32_t opcode;
    if (IsDxilCall(call, opcode) && opcode == kAnnotateHandle)
      markCounterHandle(call->getArgOperand(1));
    return;
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    for (auto &incoming : phi->incoming_values())
      markCounterHandle(incoming.get());
    return;
  }
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    markCounterHandle(select->getTrueValue());
    markCounterHandle(select->getFalseValue());
  }
}

void
ResourceLowering::collectCounterHandles() {
  for (auto &function : *info.module) {
    for (auto &block : function) {
      for (auto &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        uint32_t opcode;
        if (!call || !IsDxilCall(call, opcode) || opcode != kBufferUpdateCounter)
          continue;
        markCounterHandle(call->getArgOperand(1));
      }
    }
  }
}

/* ----------------------------------------------------------------------- */
/* handle resolution                                                       */
/* ----------------------------------------------------------------------- */

llvm::Expected<ResourceHandle>
ResourceLowering::resolve(llvm::Value *handle) {
  auto cached = resolved.find(handle);
  if (cached != resolved.end())
    return cached->second;

  if (auto *call = llvm::dyn_cast<llvm::CallInst>(handle)) {
    uint32_t opcode;
    if (!IsDxilCall(call, opcode))
      return createError("dxil: a resource handle is produced by a non dx.op instruction");
    switch (opcode) {
    case kAnnotateHandle: {
      auto inner = resolve(call->getArgOperand(1));
      if (!inner)
        return inner.takeError();
      resolved[handle] = *inner;
      return *inner;
    }
    case kCreateHandle:
    case kCreateHandleFromBinding:
    case kCreateHandleForLib:
    case kCreateHandleFromHeap: {
      auto result = materialise(call);
      if (!result)
        return result.takeError();
      resolved[handle] = *result;
      return *result;
    }
    default:
      return createError(
          llvm::Twine("dxil: ") + call->getCalledFunction()->getName() + " does not produce a resource handle"
      );
    }
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(handle))
    return resolvePhi(phi);
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(handle))
    return resolveSelect(select);
  return createError("dxil: a resource handle is neither a dx.op result, a phi nor a select");
}

SlotArray
ResourceLowering::slotsOf(const ResourceHandle &handle) {
  SlotArray slots{};
  if (handle.texture) {
    slots[kSlotPrimary] = handle.texture->ResourceHandle;
    slots[kSlotMetadata] = handle.texture->Metadata;
  } else if (handle.buffer) {
    slots[kSlotPrimary] = handle.buffer->Pointer;
    slots[kSlotMetadata] = handle.buffer->Metadata;
  } else if (handle.cbuffer) {
    slots[kSlotPrimary] = handle.cbuffer->Pointer;
    slots[kSlotMetadata] = handle.cbuffer->Metadata;
  } else if (handle.sampler) {
    slots[kSlotPrimary] = handle.sampler->SamplerHandle;
    slots[kSlotAux] = handle.sampler->CubeSamplerHandle;
    slots[kSlotMetadata] = handle.sampler->Metadata;
  }
  slots[kSlotCounter] = handle.counter;
  return slots;
}

ResourceHandle
ResourceLowering::rebuild(const ResourceHandle &model, const SlotArray &slots) {
  ResourceHandle result = model;
  if (result.texture) {
    result.texture->ResourceHandle = slots[kSlotPrimary];
    result.texture->Metadata = slots[kSlotMetadata];
  } else if (result.buffer) {
    result.buffer->Pointer = slots[kSlotPrimary];
    result.buffer->Metadata = slots[kSlotMetadata];
  } else if (result.cbuffer) {
    result.cbuffer->Pointer = slots[kSlotPrimary];
    result.cbuffer->Metadata = slots[kSlotMetadata];
  } else if (result.sampler) {
    result.sampler->SamplerHandle = slots[kSlotPrimary];
    result.sampler->CubeSamplerHandle = slots[kSlotAux];
    result.sampler->Metadata = slots[kSlotMetadata];
  }
  result.counter = slots[kSlotCounter];
  return result;
}

llvm::Expected<ResourceHandle>
ResourceLowering::combine(
    llvm::ArrayRef<ResourceHandle> parts, llvm::ArrayRef<llvm::BasicBlock *> incoming_blocks, llvm::Instruction *where,
    llvm::Value *condition
) {
  const ResourceHandle &model = parts.front();
  for (const auto &part : parts) {
    if (part.kind != model.kind)
      return createError(
          "dxil: a resource handle merges two different resource shapes (" + llvm::Twine((uint32_t)part.kind)
          + " and " + llvm::Twine((uint32_t)model.kind) + ")"
      );
    if (part.texture.has_value() != model.texture.has_value())
      return createError("dxil: a resource handle merges a texture with a non-texture");
  }

  llvm::SmallVector<SlotArray, 4> part_slots;
  for (const auto &part : parts)
    part_slots.push_back(slotsOf(part));

  SlotArray merged{};
  for (unsigned slot = 0; slot < kSlotCount; slot++) {
    bool any = false;
    bool all = true;
    for (const auto &slots : part_slots) {
      any |= slots[slot] != nullptr;
      all &= slots[slot] != nullptr;
    }
    if (!any)
      continue;
    if (!all)
      return createError("dxil: a resource handle merges descriptors with different layouts");

    llvm::Value *value = nullptr;
    if (incoming_blocks.empty()) {
      builder.SetInsertPoint(where);
      value = builder.CreateSelect(condition, part_slots[0][slot], part_slots[1][slot]);
    } else {
      /* New phis are appended after the existing ones, which keeps every phi at
       * the top of the block where it belongs. */
      builder.SetInsertPoint(where->getParent(), where->getParent()->getFirstInsertionPt());
      auto *phi = builder.CreatePHI(part_slots[0][slot]->getType(), incoming_blocks.size());
      for (unsigned i = 0; i < incoming_blocks.size(); i++)
        phi->addIncoming(part_slots[i][slot], incoming_blocks[i]);
      value = phi;
    }
    merged[slot] = value;
  }
  return rebuild(model, merged);
}

llvm::Expected<ResourceHandle>
ResourceLowering::resolvePhi(llvm::PHINode *phi) {
  llvm::SmallVector<ResourceHandle, 4> parts;
  llvm::SmallVector<llvm::BasicBlock *, 4> incoming_blocks;
  for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
    auto part = resolve(phi->getIncomingValue(i));
    if (!part)
      return part.takeError();
    parts.push_back(*part);
    incoming_blocks.push_back(phi->getIncomingBlock(i));
  }
  auto result = combine(parts, incoming_blocks, phi, nullptr);
  if (!result)
    return result.takeError();
  resolved[phi] = *result;
  return *result;
}

llvm::Expected<ResourceHandle>
ResourceLowering::resolveSelect(llvm::SelectInst *select) {
  auto true_part = resolve(select->getTrueValue());
  if (!true_part)
    return true_part.takeError();
  auto false_part = resolve(select->getFalseValue());
  if (!false_part)
    return false_part.takeError();
  llvm::SmallVector<ResourceHandle, 2> parts{*true_part, *false_part};
  auto result = combine(parts, {}, select, select->getCondition());
  if (!result)
    return result.takeError();
  resolved[select] = *result;
  return *result;
}

llvm::Expected<HandleSite>
ResourceLowering::siteFromCreateHandle(llvm::CallInst *call) {
  auto *klass = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
  auto *range = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
  if (!klass || !range)
    return createError("dxil: dx.op.createHandle needs a constant resource class and range id");
  uint32_t klass_value = klass->getZExtValue();
  if (klass_value > kSamplerClass)
    return createError("dxil: dx.op.createHandle with an invalid resource class");
  auto *record = info.find_resource((ResourceClass)klass_value, range->getZExtValue());
  if (!record)
    return createError(
        "dxil: dx.op.createHandle references range id " + llvm::Twine(range->getZExtValue())
        + " which is not declared in !dx.resources"
    );
  HandleSite site;
  site.klass = klass_value;
  site.space = record->space;
  site.lower_bound = record->lower_bound;
  site.declaration_id = record->declaration_id;
  site.index = call->getArgOperand(3);
  site.record = record;
  site.name = record->name;
  return site;
}

llvm::Expected<HandleSite>
ResourceLowering::siteFromCreateHandleFromBinding(llvm::CallInst *call) {
  // %dx.types.ResBind = { i32 rangeLowerBound, i32 rangeUpperBound, i32 spaceID, i8 resourceClass }. DXC folds
  // t0 space0 (every field zero) to zeroinitializer, a ConstantAggregateZero.
  auto *bind = llvm::dyn_cast<llvm::Constant>(call->getArgOperand(1));
  auto *bind_type = bind ? llvm::dyn_cast<llvm::StructType>(bind->getType()) : nullptr;
  if (!bind_type || bind_type->getNumElements() < 4 ||
      !(llvm::isa<llvm::ConstantAggregate>(bind) || llvm::isa<llvm::ConstantAggregateZero>(bind)))
    return createError("dxil: dx.op.createHandleFromBinding needs a constant %dx.types.ResBind");
  auto *lower = llvm::dyn_cast_or_null<llvm::ConstantInt>(bind->getAggregateElement(0u));
  auto *space = llvm::dyn_cast_or_null<llvm::ConstantInt>(bind->getAggregateElement(2u));
  auto *klass = llvm::dyn_cast_or_null<llvm::ConstantInt>(bind->getAggregateElement(3u));
  if (!space || !lower || !klass)
    return createError("dxil: dx.op.createHandleFromBinding needs constant binding fields");
  HandleSite site;
  site.klass = klass->getZExtValue();
  site.space = space->getZExtValue();
  site.lower_bound = lower->getZExtValue();
  site.declaration_id = site.lower_bound;
  site.index = call->getArgOperand(2);
  site.record = info.find_resource_at((ResourceClass)site.klass, site.space, site.lower_bound);
  if (site.record) {
    site.declaration_id = site.record->declaration_id;
    site.name = site.record->name;
  }
  return site;
}

llvm::Expected<HandleSite>
ResourceLowering::siteFromCreateHandleForLib(llvm::CallInst *call) {
  llvm::Value *symbol = call->getArgOperand(1);
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(symbol))
    symbol = load->getPointerOperand();
  else if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(symbol))
    symbol = gep->getPointerOperand();
  symbol = symbol->stripPointerCasts();

  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(symbol);
  if (!global)
    return createError(
        "dxil: dx.op.createHandleForLib operand is not a resource global (" + llvm::Twine(symbol->getName()) + ")"
    );

  const ResourceBinding *match = nullptr;
  for (const auto &record : info.resources) {
    if (!record.symbol)
      continue;
    llvm::Value *record_symbol = record.symbol;
    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(record_symbol))
      record_symbol = gep->getPointerOperand();
    record_symbol = record_symbol->stripPointerCasts();
    if (record_symbol == global) {
      match = &record;
      break;
    }
  }
  if (!match) {
    /* Reflection may be stripped, in which case the record only carries the
     * mangled global name ("?Name@@..."). */
    for (const auto &record : info.resources) {
      if (record.name.empty())
        continue;
      std::string mangled = "?" + record.name + "@@";
      if (!global->getName().contains(mangled))
        continue;
      if (match)
        return createError(
            "dxil: dx.op.createHandleForLib cannot uniquely resolve " + global->getName()
            + " (several declarations match)"
        );
      match = &record;
    }
  }
  if (!match)
    return createError(
        "dxil: dx.op.createHandleForLib cannot resolve " + global->getName() + " to a resource declaration"
    );

  HandleSite site;
  site.klass = (uint32_t)match->klass;
  site.space = match->space;
  site.lower_bound = match->lower_bound;
  site.declaration_id = match->declaration_id;
  /* A library resource object refers to the base of its range. */
  site.index = builder.getInt32(match->lower_bound);
  site.record = match;
  site.name = match->name;
  return site;
}

llvm::Expected<ResourceHandle>
ResourceLowering::lookupDescriptors(const HandleSite &site, bool needs_counter) {
  ResourceHandle handle;
  handle.binding = site.record;
  handle.kind = site.record ? site.record->kind : site.annotated_kind;
  if (handle.kind == ResourceKind::Invalid)
    return createError("dxil: resource " + site.name + " has no resource shape");
  if (site.record && site.annotated_kind != ResourceKind::Invalid && site.annotated_kind != handle.kind)
    return createError(
        "dxil: resource " + site.name + " is annotated with a shape that does not match its declaration"
    );

  /* BindingMap uses declaration IDs; the physical register and space remain
   * in the range used by root-signature matching. */
  const uint32_t key = site.declaration_id;

  /*
   * `dx.op.createHandle`'s index operand is the *absolute* register, not a
   * range-relative ordinal: DXC emits `t2` as index 2 and `arr[4..8]` as
   * `threadId + 4`. BindingMap subtracts the declaration's lower bound and adds
   * the range's descriptor offset, so the raw register is exactly what it wants.
   * Adding the lower bound again would shift every declaration whose lower bound
   * is not zero by that amount and read a neighbouring descriptor.
   */
  llvm::Value *absolute_index = site.index;

  switch (site.klass) {
  case kCbvClass: {
    auto descriptor = binding.GetConstantBuffer(air, key, absolute_index);
    if (!descriptor)
      return createError("dxil: constant buffer " + site.name + " is not bound by the root signature");
    handle.cbuffer = *descriptor;
    return handle;
  }
  case kSamplerClass: {
    auto descriptor = binding.GetSampler(air, key, absolute_index);
    if (!descriptor)
      return createError("dxil: sampler " + site.name + " is not bound by the root signature");
    handle.sampler = *descriptor;
    return handle;
  }
  case kSrvClass:
  case kUavClass: {
    /*
     * A RaytracingAccelerationStructure is an SRV whose descriptor holds the D3D12
     * GPU virtual address of the structure rather than a resource id. It is not a
     * buffer, so it is resolved through its own binding-map entry and the address
     * is what the traversal lowering turns into a Metal structure.
     */
    if (handle.kind == ResourceKind::RTAccelerationStructure) {
      if (site.klass != kSrvClass)
        return createError("dxil: a RaytracingAccelerationStructure must be a shader resource");
      auto descriptor = binding.GetAccelerationStructure(air, key, absolute_index);
      if (!descriptor)
        return createError(
            "dxil: the acceleration structure " + site.name + " is not bound by the root signature"
        );
      handle.acceleration_structure = *descriptor;
      return handle;
    }
    if (IsTextureKind(handle.kind)) {
      auto descriptor = site.klass == kSrvClass ? binding.GetSRVTexture(air, key, absolute_index)
                                                : binding.GetUAVTexture(air, key, absolute_index);
      if (!descriptor)
        return createError(
            "dxil: " + llvm::Twine(site.klass == kSrvClass ? "shader resource " : "unordered access view ") + site.name
            + " is not bound by the root signature"
        );
      handle.texture = *descriptor;
      return handle;
    }
    auto descriptor = site.klass == kSrvClass ? binding.GetSRVBuffer(air, key, absolute_index)
                                              : binding.GetUAVBuffer(air, key, absolute_index);
    if (!descriptor)
      return createError(
          "dxil: " + llvm::Twine(site.klass == kSrvClass ? "shader resource " : "unordered access view ") + site.name
          + " is not bound by the root signature"
      );
    handle.buffer = *descriptor;
    if (needs_counter && site.klass == kUavClass) {
      auto counter = binding.GetUAVCounter(air, key, absolute_index);
      if (!counter)
        return createError("dxil: UAV " + site.name + " has no counter bound");
      handle.counter = counter->Pointer;
    }
    return handle;
  }
  default:
    return createError("dxil: invalid resource class in a handle");
  }
}

/*
 * The usage of every heap-indexed handle.
 *
 * A `ResourceDescriptorHeap[i]` object has no `!dx.resources` declaration, so the view
 * facts the SM 5.x maps carry (read/written/sampled/compared) come from its uses. Handles
 * that meet in a phi or select become one SSA value, and their descriptors must then have
 * one AIR type, so usage is merged across each connected handle graph.
 */
void
ResourceLowering::collectHeapUsage() {
  llvm::DenseMap<llvm::Value *, llvm::Value *> parent;
  std::function<llvm::Value *(llvm::Value *)> find = [&](llvm::Value *value) -> llvm::Value * {
    auto entry = parent.find(value);
    if (entry == parent.end() || entry->second == value)
      return value;
    auto *root = find(entry->second);
    parent[value] = root;
    return root;
  };
  auto unite = [&](llvm::Value *a, llvm::Value *b) {
    parent.try_emplace(a, a);
    parent.try_emplace(b, b);
    auto *ra = find(a);
    auto *rb = find(b);
    if (ra != rb)
      parent[ra] = rb;
  };
  llvm::SmallVector<llvm::CallInst *, 16> heap_calls;
  llvm::SmallVector<std::pair<llvm::Value *, uint32_t>, 32> consumers;
  for (auto &function : *info.module) {
    for (auto &block : function) {
      for (auto &instruction : block) {
        auto *type = llvm::dyn_cast<llvm::StructType>(instruction.getType());
        const bool handle_type = type && type->hasName() && type->getName() == "dx.types.Handle";
        if (handle_type) {
          if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction))
            for (auto &incoming : phi->incoming_values())
              unite(phi, incoming.get());
          else if (auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
            unite(select, select->getTrueValue());
            unite(select, select->getFalseValue());
          }
        }
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        uint32_t opcode;
        if (!call || !IsDxilCall(call, opcode))
          continue;
        if (opcode == kCreateHandleFromHeap) {
          heap_calls.push_back(call);
          parent.try_emplace(call, call);
        } else if (opcode == kAnnotateHandle) {
          unite(call, call->getArgOperand(1));
        } else if (call->arg_size() >= 2) {
          auto *operand = call->getArgOperand(1);
          auto *operand_type = llvm::dyn_cast<llvm::StructType>(operand->getType());
          if (operand_type && operand_type->hasName() && operand_type->getName() == "dx.types.Handle")
            consumers.emplace_back(operand, opcode);
        }
      }
    }
  }
  if (heap_calls.empty())
    return;
  llvm::DenseMap<llvm::Value *, HeapUsage> by_root;
  for (auto [handle, opcode] : consumers) {
    auto usage = HeapUsageOfOpcode(opcode);
    auto &merged = by_root[find(handle)];
    merged.read |= usage.read;
    merged.written |= usage.written;
    merged.sampled |= usage.sampled;
    merged.compared |= usage.compared;
  }
  for (auto *call : heap_calls)
    heap_usage[call] = by_root.lookup(find(call));
}

/*
 * `dx.op.createHandleFromHeap(i32 218, i32 index, i1 samplerHeap, i1 nonUniform)`.
 *
 * The view shape is the `dx.op.annotateHandle` DXC always wraps the result in (the
 * validator requires it). Every lane loads its own 32 byte descriptor from the bound
 * heap, so a non-uniform index needs no waterfall: NonUniformResourceIndex is honoured
 * by construction and its marker is ignored.
 */
llvm::Expected<ResourceHandle>
ResourceLowering::lookupHeapDescriptors(llvm::CallInst *call, bool needs_counter) {
  auto *index = call->getArgOperand(1);
  auto *sampler_heap = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
  if (!sampler_heap)
    return createError("dxil: dx.op.createHandleFromHeap needs a constant heap selector");
  auto properties = annotation_properties.find(call);
  if (properties == annotation_properties.end())
    return createError("dxil: dx.op.createHandleFromHeap result is not annotated with its resource properties");
  const uint32_t dword0 = properties->second.first;
  const uint32_t dword1 = properties->second.second;
  ResourceHandle handle;
  handle.kind = static_cast<ResourceKind>(dword0 & kPropertyKindMask);
  const auto usage = heap_usage.lookup(call);
  const char *heap_name = sampler_heap->isOne() ? "SamplerDescriptorHeap" : "ResourceDescriptorHeap";
  auto unbound = [&]() {
    return createError(
        llvm::Twine("dxil: ") + heap_name +
        " is indexed, but the root signature does not set the matching HEAP_DIRECTLY_INDEXED flag"
    );
  };

  if (sampler_heap->isOne()) {
    if (handle.kind != ResourceKind::Sampler)
      return createError("dxil: SamplerDescriptorHeap can only produce samplers");
    auto descriptor = binding.GetHeapSampler(air, index);
    if (!descriptor)
      return unbound();
    handle.sampler = *descriptor;
    return handle;
  }

  if (handle.kind == ResourceKind::Sampler || handle.kind == ResourceKind::Invalid)
    return createError("dxil: ResourceDescriptorHeap cannot produce this resource shape");
  if (handle.kind == ResourceKind::CBuffer) {
    auto descriptor = binding.GetHeapConstantBuffer(air, index);
    if (!descriptor)
      return unbound();
    handle.cbuffer = *descriptor;
    return handle;
  }
  if (handle.kind == ResourceKind::RTAccelerationStructure) {
    auto descriptor = binding.GetHeapAccelerationStructure(air, index);
    if (!descriptor)
      return unbound();
    handle.acceleration_structure = *descriptor;
    return handle;
  }

  const bool uav = (dword0 & kPropertyIsUAV) != 0;
  const bool typed = IsTextureKind(handle.kind);
  const auto component = typed ? static_cast<ComponentType>(dword1 & 0xffu) : ComponentType::Invalid;
  const uint32_t stride = handle.kind == ResourceKind::StructuredBuffer ? dword1 : 0;
  const dxbc::ResourceRange range{0, 0, ~0u, 0};
  if (uav) {
    dxbc::UnorderedAccessViewInfo view{};
    view.range = range;
    view.resource_type = to_resource_type(handle.kind);
    view.scaler_type = to_scaler_type(component);
    view.read = usage.read;
    view.written = usage.written;
    view.global_coherent = (dword0 & kPropertyGloballyCoherent) != 0;
    view.rasterizer_order = (dword0 & kPropertyIsROV) != 0;
    view.structure_stride = stride;
    view.with_counter = needs_counter;
    if (view.rasterizer_order && info.stage != Stage::Pixel)
      return createError("dxil: a rasterizer ordered view is only valid in a pixel shader");
    if (typed) {
      auto descriptor = binding.GetHeapUAVTexture(air, view, index);
      if (!descriptor)
        return unbound();
      handle.texture = *descriptor;
      return handle;
    }
    auto descriptor = binding.GetHeapUAVBuffer(air, view, index);
    if (!descriptor)
      return unbound();
    handle.buffer = *descriptor;
    if (needs_counter) {
      auto counter = binding.GetHeapUAVCounter(air, index);
      if (!counter)
        return unbound();
      handle.counter = counter->Pointer;
    }
    return handle;
  }
  dxbc::ShaderResourceViewInfo view{};
  view.range = range;
  view.resource_type = to_resource_type(handle.kind);
  view.scaler_type = to_scaler_type(component);
  view.read = usage.read;
  view.sampled = usage.sampled;
  view.compared = usage.compared;
  view.structure_stride = stride;
  if (typed) {
    auto descriptor = binding.GetHeapSRVTexture(air, view, index);
    if (!descriptor)
      return unbound();
    handle.texture = *descriptor;
    return handle;
  }
  auto descriptor = binding.GetHeapSRVBuffer(air, view, index);
  if (!descriptor)
    return unbound();
  handle.buffer = *descriptor;
  return handle;
}

llvm::Expected<ResourceHandle>
ResourceLowering::materialise(llvm::CallInst *call) {
  uint32_t opcode;
  if (!IsDxilCall(call, opcode))
    return createError("dxil: internal error, not a dx.op call");

  if (opcode == kCreateHandleFromHeap) {
    builder.SetInsertPoint(call);
    return lookupHeapDescriptors(call, counter_handles.contains(call));
  }

  llvm::Expected<HandleSite> site = [&]() -> llvm::Expected<HandleSite> {
    switch (opcode) {
    case kCreateHandle:
      return siteFromCreateHandle(call);
    case kCreateHandleFromBinding:
      return siteFromCreateHandleFromBinding(call);
    case kCreateHandleForLib:
      return siteFromCreateHandleForLib(call);
    default:
      return createError("dxil: internal error, not a handle producing operation");
    }
  }();
  if (!site)
    return site.takeError();

  auto annotation = annotations.find(call);
  if (annotation != annotations.end())
    site->annotated_kind = annotation->second;

  /* Descriptors are loaded where the handle is created, which dominates every
   * use of it. */
  builder.SetInsertPoint(call);
  return lookupDescriptors(*site, counter_handles.contains(call));
}

llvm::Value *
ResourceLowering::resolveCounter(llvm::Value *handle) {
  auto cached = resolved.find(handle);
  if (cached != resolved.end() && cached->second.counter)
    return cached->second.counter;

  auto remember = [&](llvm::Value *counter) {
    auto entry = resolved.find(handle);
    if (entry != resolved.end())
      entry->second.counter = counter;
    return counter;
  };

  if (auto *call = llvm::dyn_cast<llvm::CallInst>(handle)) {
    uint32_t opcode;
    if (!IsDxilCall(call, opcode))
      return nullptr;
    if (opcode == kAnnotateHandle)
      return remember(resolveCounter(call->getArgOperand(1)));
    if (opcode == kCreateHandleFromHeap) {
      builder.SetInsertPoint(call);
      auto counter = binding.GetHeapUAVCounter(air, call->getArgOperand(1));
      return counter ? remember(counter->Pointer) : nullptr;
    }
    if (opcode != kCreateHandle && opcode != kCreateHandleFromBinding && opcode != kCreateHandleForLib)
      return nullptr;
    auto site = opcode == kCreateHandle
                    ? siteFromCreateHandle(call)
                    : (opcode == kCreateHandleFromBinding ? siteFromCreateHandleFromBinding(call)
                                                          : siteFromCreateHandleForLib(call));
    if (!site) {
      llvm::consumeError(site.takeError());
      return nullptr;
    }
    if (site->klass != kUavClass)
      return nullptr;
    auto *absolute_index = site->index;
    auto counter = binding.GetUAVCounter(air, site->declaration_id, absolute_index);
    return counter ? remember(counter->Pointer) : nullptr;
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(handle)) {
    builder.SetInsertPoint(phi->getParent(), phi->getParent()->getFirstInsertionPt());
    auto *merged = builder.CreatePHI(air.getIntTy()->getPointerTo(1), phi->getNumIncomingValues());
    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
      llvm::Value *incoming = resolveCounter(phi->getIncomingValue(i));
      if (!incoming)
        return nullptr;
      merged->addIncoming(incoming, phi->getIncomingBlock(i));
    }
    return remember(merged);
  }
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(handle)) {
    llvm::Value *true_counter = resolveCounter(select->getTrueValue());
    llvm::Value *false_counter = resolveCounter(select->getFalseValue());
    if (!true_counter || !false_counter)
      return nullptr;
    builder.SetInsertPoint(select);
    return remember(builder.CreateSelect(select->getCondition(), true_counter, false_counter));
  }
  return nullptr;
}

/* ----------------------------------------------------------------------- */
/* cbuffer                                                                 */
/* ----------------------------------------------------------------------- */

llvm::Error
ResourceLowering::lowerCBufferLoadLegacy(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  if (!handle->cbuffer)
    return createError("dxil: cbufferLoadLegacy requires a constant buffer handle");
  builder.SetInsertPoint(call);

  const auto &buffer = *handle->cbuffer;
  auto *row = call->getArgOperand(2);
  auto *row_type = air.getIntTy(4);
  /* SM5.1 constant buffer OOB reads return zero. A descriptor table CBV carries
   * its size, and a null descriptor carries zero, so the row is guarded by real
   * control flow: the row load must not be issued at all when the descriptor does
   * not cover it. Root CBVs carry no size, which is exactly the information a root
   * CBV is bound with, so their access stays unconditional. */
  auto *condition = buffer.Metadata
                        ? builder.CreateICmpULT(row, builder.CreateLShr(Low32(buffer.Metadata), 4))
                        : nullptr;
  auto guarded = emitGuardedAccess(
      condition, call, {row_type},
      [&]() -> llvm::SmallVector<llvm::Value *, 4> {
        auto *pointer = builder.CreateGEP(row_type, buffer.Pointer, {row});
        return {builder.CreateLoad(row_type, pointer)};
      }
  );
  llvm::Value *value = guarded[0];
  auto *result_type = llvm::cast<llvm::StructType>(call->getType());
  auto *lane_type = result_type->getElementType(0);
  auto *vector_type = llvm::FixedVectorType::get(lane_type, result_type->getNumElements());
  if (vector_type->getPrimitiveSizeInBits() != value->getType()->getPrimitiveSizeInBits())
    return createError("dxil: cbufferLoadLegacy result must describe one 128-bit row");
  value = builder.CreateBitCast(value, vector_type);
  llvm::SmallVector<llvm::Value *, 4> components;
  for (unsigned i = 0; i < result_type->getNumElements(); i++)
    components.push_back(builder.CreateExtractElement(value, i));
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

/* ----------------------------------------------------------------------- */
/* texture operations                                                      */
/* ----------------------------------------------------------------------- */

llvm::Error
ResourceLowering::lowerTextureLoad(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  auto descriptor = textureDescriptor(*handle, "textureLoad");
  if (!descriptor)
    return descriptor.takeError();
  builder.SetInsertPoint(call);

  auto logical = descriptor->ResourceKindLogical;
  auto *level = call->getArgOperand(2);
  llvm::Value *c[3] = {call->getArgOperand(3), call->getArgOperand(4), call->getArgOperand(5)};
  int32_t offsets[3];
  for (unsigned i = 0; i < 3; i++) {
    auto offset = constantOperand(call->getArgOperand(6 + i), "textureLoad offset");
    if (!offset)
      return offset.takeError();
    offsets[i] = *offset;
  }

  llvm::Value *address = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sample_index = nullptr;
  int32_t offset_values[3] = {offsets[0], offsets[1], offsets[2]};

  switch (logical) {
  case llvm::air::Texture::texture_buffer:
    /* Load on a typed buffer: the view offset takes part in the bound check. */
    return readTexture(
        call, *descriptor,
        texelBufferAddress(*descriptor, c[0], offsetVector(logical, offset_values)), nullptr, nullptr, air.getInt(0)
    );
  case llvm::air::Texture::texture1d:
    address = padVector(c[0], 2, air.getInt(0));
    offset_values[1] = 0;
    break;
  case llvm::air::Texture::texture1d_array:
    address = padVector(c[0], 2, air.getInt(0));
    array_index = c[1];
    offset_values[1] = 0;
    break;
  case llvm::air::Texture::texture2d:
  case llvm::air::Texture::depth2d:
    address = makeVector({c[0], c[1]});
    break;
  case llvm::air::Texture::texture2d_array:
  case llvm::air::Texture::depth2d_array:
    address = makeVector({c[0], c[1]});
    array_index = c[2];
    break;
  case llvm::air::Texture::texture3d:
    address = makeVector({c[0], c[1], c[2]});
    break;
  case llvm::air::Texture::texture2d_ms:
  case llvm::air::Texture::depth_2d_ms:
    address = makeVector({c[0], c[1]});
    sample_index = level;
    level = air.getInt(0);
    break;
  case llvm::air::Texture::texture2d_ms_array:
  case llvm::air::Texture::depth_2d_ms_array:
    address = makeVector({c[0], c[1]});
    array_index = c[2];
    sample_index = level;
    level = air.getInt(0);
    break;
  default:
    return createError("dxil: textureLoad on an unsupported resource shape");
  }

  auto *offset = offsetVector(descriptor->ResourceKind, offset_values);
  if (offset && !offset->isNullValue())
    address = builder.CreateAdd(address, offset);
  return readTexture(call, *descriptor, address, array_index, sample_index, level);
}

llvm::Error
ResourceLowering::lowerTextureStore(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  auto descriptor = textureDescriptor(*handle, "textureStore");
  if (!descriptor)
    return descriptor.takeError();
  builder.SetInsertPoint(call);

  auto logical = descriptor->ResourceKindLogical;
  llvm::Value *c[3] = {call->getArgOperand(2), call->getArgOperand(3), call->getArgOperand(4)};
  llvm::Value *address = nullptr;
  llvm::Value *array_index = nullptr;
  switch (logical) {
  case llvm::air::Texture::texture1d:
    address = padVector(c[0], 2, air.getInt(0));
    break;
  case llvm::air::Texture::texture1d_array:
    address = padVector(c[0], 2, air.getInt(0));
    array_index = c[1];
    break;
  case llvm::air::Texture::texture2d:
    address = makeVector({c[0], c[1]});
    break;
  case llvm::air::Texture::texture2d_array:
    address = makeVector({c[0], c[1]});
    array_index = c[2];
    break;
  case llvm::air::Texture::texture3d:
    address = makeVector({c[0], c[1], c[2]});
    break;
  default:
    return createError("dxil: textureStore on an unsupported resource shape");
  }

  llvm::SmallVector<llvm::Value *, 4> components;
  for (unsigned i = 0; i < 4; i++)
    components.push_back(call->getArgOperand(5 + i));
  auto error = writeTexture(call, *descriptor, address, array_index, makeVector(components));
  call->eraseFromParent();
  return error;
}

/* ----------------------------------------------------------------------- */
/* buffer operations                                                       */
/* ----------------------------------------------------------------------- */

llvm::Error
ResourceLowering::lowerBufferLoad(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  builder.SetInsertPoint(call);
  auto *c0 = call->getArgOperand(2);
  auto *c1 = call->getArgOperand(3);

  if (handle->kind == ResourceKind::TypedBuffer || handle->kind == ResourceKind::TBuffer) {
    auto descriptor = textureDescriptor(*handle, "bufferLoad");
    if (!descriptor)
      return descriptor.takeError();
    return readTexture(call, *descriptor, texelBufferAddress(*descriptor, c0, nullptr), nullptr, nullptr, air.getInt(0));
  }
  auto buffer = bufferDescriptor(*handle, "bufferLoad");
  if (!buffer)
    return buffer.takeError();
  if (buffer->StructureStride % 2)
    return createError("dxil: structured buffer stride must be a multiple of 2 bytes");
  return loadBufferWords(call, *buffer, c0, c1, usedComponents(call));
}

llvm::Error
ResourceLowering::lowerBufferStore(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  auto *mask_value = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(8));
  if (!mask_value)
    return createError("dxil: bufferStore needs a constant write mask");
  uint32_t mask = mask_value->getZExtValue();
  builder.SetInsertPoint(call);
  auto *c0 = call->getArgOperand(2);
  auto *c1 = call->getArgOperand(3);

  if (handle->kind == ResourceKind::TypedBuffer || handle->kind == ResourceKind::TBuffer) {
    auto descriptor = textureDescriptor(*handle, "bufferStore");
    if (!descriptor)
      return descriptor.takeError();
    llvm::SmallVector<llvm::Value *, 4> components;
    for (unsigned i = 0; i < 4; i++)
      components.push_back(call->getArgOperand(4 + i));
    auto error =
        writeTexture(call, *descriptor, texelBufferAddress(*descriptor, c0, nullptr), nullptr, makeVector(components));
    call->eraseFromParent();
    return error;
  }
  auto buffer = bufferDescriptor(*handle, "bufferStore");
  if (!buffer)
    return buffer.takeError();
  if (buffer->StructureStride % 2)
    return createError("dxil: structured buffer stride must be a multiple of 2 bytes");
  if (mask != 0b1 && mask != 0b11 && mask != 0b111 && mask != 0b1111)
    return createError("dxil: buffer store mask must be contiguous starting at x");
  return storeBufferWords(call, *buffer, c0, c1, mask);
}

llvm::Error
ResourceLowering::lowerRawBufferLoad(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  auto buffer = bufferDescriptor(*handle, "rawBufferLoad");
  if (!buffer)
    return buffer.takeError();
  auto *mask_value = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(4));
  if (!mask_value)
    return createError("dxil: rawBufferLoad needs a constant read mask");
  if (buffer->StructureStride % 2)
    return createError("dxil: structured buffer stride must be a multiple of 2 bytes");
  builder.SetInsertPoint(call);
  auto *c1 = buffer->StructureStride ? call->getArgOperand(3) : nullptr;
  return loadBufferWords(call, *buffer, call->getArgOperand(2), c1, mask_value->getZExtValue());
}

llvm::Error
ResourceLowering::lowerRawBufferStore(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  auto buffer = bufferDescriptor(*handle, "rawBufferStore");
  if (!buffer)
    return buffer.takeError();
  auto *mask_value = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(8));
  if (!mask_value)
    return createError("dxil: rawBufferStore needs a constant write mask");
  uint32_t mask = mask_value->getZExtValue();
  if (mask != 0b1 && mask != 0b11 && mask != 0b111 && mask != 0b1111)
    return createError("dxil: raw buffer store mask must be contiguous starting at x");
  if (buffer->StructureStride % 2)
    return createError("dxil: structured buffer stride must be a multiple of 2 bytes");
  builder.SetInsertPoint(call);
  auto *c1 = buffer->StructureStride ? call->getArgOperand(3) : nullptr;
  return storeBufferWords(call, *buffer, call->getArgOperand(2), c1, mask);
}

/* ----------------------------------------------------------------------- */
/* queries, counters and atomics                                           */
/* ----------------------------------------------------------------------- */

llvm::Error
ResourceLowering::lowerGetDimensions(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  builder.SetInsertPoint(call);
  auto *level = call->getArgOperand(2);
  llvm::Value *x = air.getInt(0);
  llvm::Value *y = air.getInt(0);
  llvm::Value *z = air.getInt(0);
  llvm::Value *w = air.getInt(0);

  if (handle->buffer) {
    /* Raw and structured buffers size themselves in bytes, structured buffers
     * in elements. */
    if (!handle->buffer->Metadata)
      return createError("dxil: getDimensions on a buffer without a known size");
    x = Low32(handle->buffer->Metadata);
    if (handle->buffer->StructureStride)
      x = builder.CreateUDiv(x, air.getInt(handle->buffer->StructureStride));
  } else if (handle->texture) {
    const auto &descriptor = *handle->texture;
    auto texture = textureOf(descriptor);
    auto logical = descriptor.ResourceKindLogical;
    if (logical == llvm::air::Texture::texture_buffer) {
      if (!descriptor.Metadata)
        return createError("dxil: getDimensions on a typed buffer without metadata");
      x = High32(descriptor.Metadata);
    } else {
      x = air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::width, level);
      switch (logical) {
      case llvm::air::Texture::texture1d:
        break;
      case llvm::air::Texture::texture1d_array:
        y = air.CreateTextureQuery(
            texture, descriptor.ResourceHandle, llvm::air::Texture::array_length, air.getInt(0)
        );
        break;
      case llvm::air::Texture::texture2d:
      case llvm::air::Texture::depth2d:
      case llvm::air::Texture::texturecube:
      case llvm::air::Texture::depthcube:
      case llvm::air::Texture::texture2d_ms:
      case llvm::air::Texture::depth_2d_ms:
        y = air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::height, level);
        break;
      case llvm::air::Texture::texture2d_array:
      case llvm::air::Texture::depth2d_array:
      case llvm::air::Texture::texturecube_array:
      case llvm::air::Texture::depthcube_array:
      case llvm::air::Texture::texture2d_ms_array:
      case llvm::air::Texture::depth_2d_ms_array:
        y = air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::height, level);
        z = air.CreateTextureQuery(
            texture, descriptor.ResourceHandle, llvm::air::Texture::array_length, air.getInt(0)
        );
        break;
      case llvm::air::Texture::texture3d:
        y = air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::height, level);
        z = descriptor.Scoped3DUAV
                ? builder.CreateLShr(High32(descriptor.Metadata), 16)
                : air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::depth, level);
        break;
      default:
        return createError("dxil: getDimensions on an unsupported resource shape");
      }
      switch (logical) {
      case llvm::air::Texture::texture2d_ms:
      case llvm::air::Texture::texture2d_ms_array:
      case llvm::air::Texture::depth_2d_ms:
      case llvm::air::Texture::depth_2d_ms_array:
        w = air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::num_samples, air.getInt(0));
        break;
      default:
        w = air.CreateTextureQuery(
            texture, descriptor.ResourceHandle, llvm::air::Texture::num_mip_levels, air.getInt(0)
        );
        break;
      }
    }
  } else {
    return createError("dxil: getDimensions requires a buffer or a texture handle");
  }

  call->replaceAllUsesWith(buildResult(call, {x, y, z, w}));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerBufferUpdateCounter(llvm::CallInst *call) {
  auto *direction = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
  if (!direction)
    return createError("dxil: bufferUpdateCounter needs a constant direction");
  int64_t delta = direction->getSExtValue();
  if (delta != 1 && delta != -1)
    return createError("dxil: bufferUpdateCounter direction must be 1 or -1");
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  if (!handle->counter)
    return createError("dxil: bufferUpdateCounter on a handle without a bound counter");
  builder.SetInsertPoint(call);
  /* A null descriptor carries no counter pointer, so the update must not be
   * issued at all; the atomic is guarded by control flow and reports the zero it
   * would have read. */
  auto *counter_type = llvm::cast<llvm::PointerType>(handle->counter->getType());
  auto guarded = emitGuardedAccess(
      builder.CreateICmpNE(handle->counter, llvm::ConstantPointerNull::get(counter_type)), call, {air.getIntTy()},
      [&]() -> llvm::SmallVector<llvm::Value *, 4> {
        auto *old = air.CreateAtomicRMW(
            delta > 0 ? llvm::AtomicRMWInst::Add : llvm::AtomicRMWInst::Sub, handle->counter, air.getInt(1)
        );
        /* Increment reports the value before the update, decrement the value after
         * it, like the DXBC imm_atomic_inc / imm_atomic_dec operations. */
        return {delta > 0 ? (llvm::Value *)old : (llvm::Value *)builder.CreateSub(old, air.getInt(1))};
      }
  );
  call->replaceAllUsesWith(guarded[0]);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Constant *
ResourceLowering::offsetVector(llvm::air::Texture::ResourceKind kind, const int32_t offsets[3]) {
  switch (kind) {
  case llvm::air::Texture::texture_buffer:
  case llvm::air::Texture::texture1d:
  case llvm::air::Texture::texture1d_array:
    return air.getInt(offsets[0]);
  case llvm::air::Texture::texture2d:
  case llvm::air::Texture::texture2d_array:
  case llvm::air::Texture::depth2d:
  case llvm::air::Texture::depth2d_array:
    return air.getInt2(offsets[0], offsets[1]);
  case llvm::air::Texture::texture3d:
    return air.getInt3(offsets[0], offsets[1], offsets[2]);
  default:
    break;
  }
  return nullptr;
}

/* Address of an atomic on a buffer handle: the dword index for buffer
 * descriptors, the AIR address vector for texture descriptors. Buffer atomics
 * are guarded against the descriptor bound by their caller, so the raw index is
 * returned instead of a pointer that a select could mask after the fact. */
llvm::Value *
ResourceLowering::atomicAddress(const ResourceHandle &handle, llvm::CallInst *call, unsigned c0_index) {
  if (handle.buffer) {
    auto *c1 = handle.buffer->StructureStride ? call->getArgOperand(c0_index + 1) : nullptr;
    return bufferDwordIndex(call->getArgOperand(c0_index), c1, handle.buffer->StructureStride);
  }
  const auto &descriptor = *handle.texture;
  if (descriptor.ResourceKindLogical == llvm::air::Texture::texture_buffer)
    return texelBufferAddress(descriptor, call->getArgOperand(c0_index), nullptr);
  llvm::Value *c[3] = {call->getArgOperand(c0_index), call->getArgOperand(c0_index + 1),
                       call->getArgOperand(c0_index + 2)};
  switch (descriptor.ResourceKindLogical) {
  case llvm::air::Texture::texture1d:
    return padVector(c[0], 2, air.getInt(0));
  case llvm::air::Texture::texture2d:
    return makeVector({c[0], c[1]});
  case llvm::air::Texture::texture3d:
    return descriptor.Scoped3DUAV
               ? scoped3DUAVAddress(descriptor, makeVector({c[0], c[1], c[2]}))
               : makeVector({c[0], c[1], c[2]});
  default:
    return nullptr;
  }
}

llvm::Value *
ResourceLowering::atomicArrayIndex(const ResourceHandle &handle, llvm::CallInst *call, unsigned c0_index) {
  if (!handle.texture)
    return nullptr;
  switch (handle.texture->ResourceKindLogical) {
  case llvm::air::Texture::texture1d_array:
    return call->getArgOperand(c0_index + 1);
  case llvm::air::Texture::texture2d_array:
    return call->getArgOperand(c0_index + 2);
  default:
    return nullptr;
  }
}

/*
 * 64 bit atomics on raw and structured UAVs (required by Shader Model 6.6).
 *
 * Apple GPUs only provide non-returning 64 bit min/max, so every returning 64 bit
 * operation - and, for mutual atomicity, min/max too - takes a spin lock from a
 * device-global table of SM50_ATOMIC_LOCK_TABLE_ENTRIES 32 bit words, indexed by a
 * hash of the target's device address. Aliasing views, concurrent dispatches and
 * queues on the same MTLDevice therefore serialize on the same lock; a hash
 * collision only costs time.
 *
 * The lane-safe loop acquires, updates and releases inside the same iteration:
 *
 *   do { if (cas32(lock, 0, 1)) { fence; old = load64; store64(f(old)); fence;
 *        store32(lock, 0); done = true; } } while (!done);
 *
 * so a lane that owns the lock never waits on a lane of its own SIMD group that is
 * spinning. The 64 bit value moves as two relaxed 32 bit atomic accesses bracketed
 * by device-scope fences, which keeps the compiler from moving them out of the
 * critical section.
 */
llvm::Value *
ResourceLowering::atomicLockTable() {
  auto &module = *info.module;
  auto *global = module.getNamedGlobal(kAtomicLockTableGlobal);
  if (!global) {
    global = new llvm::GlobalVariable(module, builder.getInt64Ty(), /*isConstant=*/true,
                                      llvm::GlobalValue::InternalLinkage, builder.getInt64(0),
                                      kAtomicLockTableGlobal, nullptr, llvm::GlobalValue::NotThreadLocal, 2);
    /* The runtime patches the table address in; its initializer must never be folded. */
    global->setExternallyInitialized(true);
    global->setAlignment(llvm::Align(8));
  }
  auto *address = builder.CreateLoad(builder.getInt64Ty(), global);
  return builder.CreateIntToPtr(address, air.getIntTy()->getPointerTo(1));
}

/*
 * SM 6.8 barrierByMemoryHandle(handle, SemanticFlags): orders the memory of one UAV.
 * Metal fences whole address spaces, so the handle selects device + texture memory
 * (a superset of the one resource); semantics 1 group sync, 2 group scope, 4 device
 * scope, 8 reorder scope (no memory effect).
 */
llvm::Error
ResourceLowering::lowerBarrierByMemoryHandle(llvm::CallInst *call) {
  /* The DXIL validator guarantees a UAV handle; the fence does not need its descriptor. */
  const auto *semantics = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
  if (!semantics)
    return createError("dxil: barrierByMemoryHandle semantic flags must be constant");
  const uint32_t bits = static_cast<uint32_t>(semantics->getZExtValue());
  if (bits & ~0xfu)
    return createError("dxil: barrierByMemoryHandle has unknown semantic flag bits");
  builder.SetInsertPoint(call);
  const auto memory = static_cast<llvm::air::MemFlags>(static_cast<uint32_t>(llvm::air::MemFlags::Device) |
                                                       static_cast<uint32_t>(llvm::air::MemFlags::Texture));
  auto scope = static_cast<uint32_t>(llvm::air::ThreadScope::Threadgroup);
  if (bits & 4u)
    scope |= static_cast<uint32_t>(llvm::air::ThreadScope::Device);
  if (!(bits & 1u) || (bits & 4u))
    air.CreateAtomicFence(memory, llvm::air::ThreadScope(scope));
  if (bits & 1u)
    air.CreateBarrier(memory, /*SimdGroup=*/false);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Value *
ResourceLowering::emitLockedAtomic64(llvm::Value *pointer, llvm::function_ref<llvm::Value *(llvm::Value *old)> update) {
  auto *i1 = builder.getInt1Ty();
  auto *i32 = air.getIntTy();
  auto *i64 = builder.getInt64Ty();
  auto *pointer32 = i32->getPointerTo(1);
  /* Every access in and out of the critical section is a device-coherent read-modify-write
   * (`or 0` reads, `xchg` writes): the AIR 2.7 air.atomic.global.{load,store}.i32 intrinsics
   * crash the Metal compiler service (XPC_ERROR_CONNECTION_INTERRUPTED, M5 Pro / macOS 27),
   * and generic atomic loads/stores are not coherent across threadgroups, which left the lock
   * release invisible and hung contended dispatches. */
  auto load32 = [&](llvm::Value *address) {
    auto *rmw = builder.CreateAtomicRMW(llvm::AtomicRMWInst::Or, address, air.getInt(0), llvm::MaybeAlign(4),
                                        llvm::AtomicOrdering::Monotonic);
    rmw->setVolatile(true);
    return rmw;
  };
  auto store32 = [&](llvm::Value *address, llvm::Value *value) {
    auto *rmw = builder.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, address, value, llvm::MaybeAlign(4),
                                        llvm::AtomicOrdering::Monotonic);
    rmw->setVolatile(true);
  };
  auto fence = [&]() {
    air.CreateAtomicFence(llvm::air::MemFlags::Device, llvm::air::ThreadScope::Device);
  };

  auto *low = builder.CreateBitCast(pointer, pointer32);
  auto *high = builder.CreateGEP(i32, low, {air.getInt(1)});
  auto *address = builder.CreatePtrToInt(pointer, i64);
  auto *hash = builder.CreateXor(builder.CreateLShr(address, 3), builder.CreateLShr(address, 19));
  auto *slot = builder.CreateAnd(builder.CreateTrunc(hash, i32), air.getInt(SM50_ATOMIC_LOCK_TABLE_ENTRIES - 1));
  auto *lock = builder.CreateGEP(i32, atomicLockTable(), {slot});

  /* One lane of a simdgroup at a time tries the lock, and it tries once per round: every
   * round the pending lanes vote, the lowest pending lane makes a single compare-exchange,
   * and on success updates and releases before the group votes again. No lane ever waits
   * inside divergent control flow, so progress never depends on the scheduler running a
   * diverged lane. Per-lane spinning (or an elected lane spinning in an inner loop) hangs
   * Apple fragment functions (kIOGPUCommandBufferCallbackErrorHang, M5 Pro / macOS 27). */
  auto attributes = llvm::AttributeList::get(
      ctx, {{~0U, llvm::Attribute::get(ctx, llvm::Attribute::Convergent)},
            {~0U, llvm::Attribute::get(ctx, llvm::Attribute::NoUnwind)},
            {~0U, llvm::Attribute::get(ctx, llvm::Attribute::WillReturn)}});
  auto ballot = info.module->getOrInsertFunction("air.simd_ballot.i64", llvm::FunctionType::get(i64, {i1}, false),
                                                 attributes);
  auto lane_index = info.module->getOrInsertFunction(
      "air.get_thread_index_in_simdgroup.i32", llvm::FunctionType::get(i32, false), attributes);

  auto *function = builder.GetInsertBlock()->getParent();
  auto *head = builder.GetInsertBlock();
  auto *tail = head->splitBasicBlock(builder.GetInsertPoint(), "atomic64.done");
  head->getTerminator()->eraseFromParent();
  auto *vote = llvm::BasicBlock::Create(ctx, "atomic64.vote", function, tail);
  auto *elected = llvm::BasicBlock::Create(ctx, "atomic64.elected", function, tail);
  auto *attempt = llvm::BasicBlock::Create(ctx, "atomic64.try", function, tail);
  auto *critical = llvm::BasicBlock::Create(ctx, "atomic64.locked", function, tail);
  auto *next_round = llvm::BasicBlock::Create(ctx, "atomic64.next", function, tail);
  builder.SetInsertPoint(head);
  auto *lane = builder.CreateCall(lane_index);
  builder.CreateBr(vote);

  builder.SetInsertPoint(vote);
  auto *pending = builder.CreatePHI(i1, 2);
  auto *result = builder.CreatePHI(i64, 2);
  pending->addIncoming(builder.getTrue(), head);
  result->addIncoming(llvm::UndefValue::get(i64), head);
  auto *mask = builder.CreateTrunc(builder.CreateCall(ballot, {pending}), i32);
  auto *leader = air.CreateCountZero(mask, /*TrailingZero=*/true);
  auto *mine = builder.CreateAnd(pending, builder.CreateICmpEQ(lane, leader));
  builder.CreateCondBr(builder.CreateICmpNE(mask, air.getInt(0)), elected, tail);

  builder.SetInsertPoint(elected);
  builder.CreateCondBr(mine, attempt, next_round);

  builder.SetInsertPoint(attempt);
  auto *claim = builder.CreateAtomicCmpXchg(lock, air.getInt(0), air.getInt(1), llvm::MaybeAlign(4),
                                            llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic);
  claim->setVolatile(true);
  builder.CreateCondBr(builder.CreateExtractValue(claim, 1), critical, next_round);

  builder.SetInsertPoint(critical);
  fence();
  auto *old = builder.CreateOr(builder.CreateZExt(load32(low), i64),
                               builder.CreateShl(builder.CreateZExt(load32(high), i64), 32));
  auto *next = update(old);
  store32(low, builder.CreateTrunc(next, i32));
  store32(high, builder.CreateTrunc(builder.CreateLShr(next, 32), i32));
  fence();
  store32(lock, air.getInt(0));
  builder.CreateBr(next_round);

  builder.SetInsertPoint(next_round);
  auto *still_pending = builder.CreatePHI(i1, 3);
  still_pending->addIncoming(builder.getFalse(), critical);
  still_pending->addIncoming(pending, attempt);
  still_pending->addIncoming(pending, elected);
  auto *value = builder.CreatePHI(i64, 3);
  value->addIncoming(old, critical);
  value->addIncoming(result, attempt);
  value->addIncoming(result, elected);
  pending->addIncoming(still_pending, next_round);
  result->addIncoming(value, next_round);
  builder.CreateBr(vote);

  builder.SetInsertPoint(tail, tail->getFirstInsertionPt());
  return result;
}

llvm::Error
ResourceLowering::lowerBufferAtomic64(llvm::CallInst *call, const ResourceHandle &handle, bool compare_exchange) {
  const auto &buffer = *handle.buffer;
  /* atomicBinOp (handle, op, c0, c1, c2, value); atomicCompareExchange (handle, c0,
   * c1, c2, compare, value). */
  const unsigned c0_index = compare_exchange ? 2 : 3;
  auto *c1 = buffer.StructureStride ? call->getArgOperand(c0_index + 1) : nullptr;
  auto *byte = bufferByteOffset(call->getArgOperand(c0_index), c1, buffer.StructureStride);
  auto *value = call->getArgOperand(6);
  auto *comparison = compare_exchange ? call->getArgOperand(5) : nullptr;
  uint64_t operation = 0;
  if (!compare_exchange) {
    auto *constant = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
    if (!constant || constant->getZExtValue() > kAtomicExchange)
      return createError("dxil: unknown 64 bit atomicBinOp operation");
    operation = constant->getZExtValue();
  }
  auto update = [&](llvm::Value *old) -> llvm::Value * {
    if (compare_exchange)
      return builder.CreateSelect(builder.CreateICmpEQ(old, comparison), value, old);
    switch (operation) {
    case kAtomicAdd:
      return builder.CreateAdd(old, value);
    case kAtomicAnd:
      return builder.CreateAnd(old, value);
    case kAtomicOr:
      return builder.CreateOr(old, value);
    case kAtomicXor:
      return builder.CreateXor(old, value);
    case kAtomicIMin:
      return builder.CreateSelect(builder.CreateICmpSLT(value, old), value, old);
    case kAtomicIMax:
      return builder.CreateSelect(builder.CreateICmpSGT(value, old), value, old);
    case kAtomicUMin:
      return builder.CreateSelect(builder.CreateICmpULT(value, old), value, old);
    case kAtomicUMax:
      return builder.CreateSelect(builder.CreateICmpUGT(value, old), value, old);
    default:
      return value; /* exchange */
    }
  };
  /* The lock table is only safe where threadgroups are guaranteed forward progress. In
   * fragment functions a contended spin lock hangs the GPU intermittently
   * (kIOGPUCommandBufferCallbackErrorHang: 46/200 and 23/200 runs of native MSL fragment spin
   * locks, 3/200 of this lowering, M5 Pro / macOS 27), and Metal's only native 64 bit
   * buffer atomics (non-returning u64 min/max) need AIR 2.9. Graphics stages therefore
   * reject 64 bit atomics and SM 6.6 is not reported. */
  if (!kernel_stage)
    return createError("dxil: 64 bit atomics are only supported in compute shaders "
                       "(a graphics-stage lock cannot guarantee forward progress)");
  /* An atomic on an out-of-range or unbound descriptor does nothing and reads zero. */
  auto guarded = emitGuardedAccess(
      bufferBytesInBounds(buffer, byte, 8), call, {builder.getInt64Ty()},
      [&]() -> llvm::SmallVector<llvm::Value *, 4> {
        return {emitLockedAtomic64(bufferBytePointer(buffer, byte, builder.getInt64Ty()), update)};
      }
  );
  call->replaceAllUsesWith(guarded[0]);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerAtomicBinOp(llvm::CallInst *call) {
  auto *operation = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
  if (!operation)
    return createError("dxil: atomicBinOp needs a constant operation");
  llvm::AtomicRMWInst::BinOp binop;
  switch (operation->getZExtValue()) {
  case kAtomicAdd:
    binop = llvm::AtomicRMWInst::Add;
    break;
  case kAtomicAnd:
    binop = llvm::AtomicRMWInst::And;
    break;
  case kAtomicOr:
    binop = llvm::AtomicRMWInst::Or;
    break;
  case kAtomicXor:
    binop = llvm::AtomicRMWInst::Xor;
    break;
  case kAtomicIMin:
    binop = llvm::AtomicRMWInst::Min;
    break;
  case kAtomicIMax:
    binop = llvm::AtomicRMWInst::Max;
    break;
  case kAtomicUMin:
    binop = llvm::AtomicRMWInst::UMin;
    break;
  case kAtomicUMax:
    binop = llvm::AtomicRMWInst::UMax;
    break;
  case kAtomicExchange:
    binop = llvm::AtomicRMWInst::Xchg;
    break;
  default:
    return createError("dxil: unknown atomicBinOp operation");
  }

  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  if (!handle->buffer && !handle->texture)
    return createError("dxil: atomicBinOp requires a UAV handle");
  builder.SetInsertPoint(call);
  auto *value = call->getArgOperand(6);
  if (value->getType()->isIntegerTy(64)) {
    if (!handle->buffer)
      return unsupported("64 bit atomic on a typed resource (AtomicInt64OnTypedResourceSupported is not reported)");
    return lowerBufferAtomic64(call, *handle, false);
  }

  llvm::Value *result = nullptr;
  if (handle->buffer) {
    auto *index = atomicAddress(*handle, call, 3);
    /* An atomic on an out-of-range or unbound descriptor does nothing and reads
     * zero; issuing it against address zero instead would be a real access. */
    auto guarded = emitGuardedAccess(
        bufferInBounds(*handle->buffer, index), call, {air.getIntTy()},
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          auto *pointer = builder.CreateGEP(air.getIntTy(), handle->buffer->Pointer, {index});
          return {air.CreateAtomicRMW(binop, pointer, value)};
        }
    );
    result = guarded[0];
  } else {
    auto *address = atomicAddress(*handle, call, 3);
    if (!address)
      return createError("dxil: atomicBinOp on an unsupported resource shape");
    auto *value_vector = builder.CreateVectorSplat(4, value);
    if (handle->texture->Scoped3DUAV) {
      auto *count = builder.CreateLShr(High32(handle->texture->Metadata), 16);
      auto *valid = builder.CreateICmpULT(call->getArgOperand(5), count);
      result = emitGuardedAccess(valid, call, {air.getIntTy(4)}, [&]() -> llvm::SmallVector<llvm::Value *, 4> {
        return {air.CreateAtomicRMW(textureOf(*handle->texture), handle->texture->ResourceHandle, binop,
                                    address, value_vector, nullptr)};
      })[0];
    } else {
      result = air.CreateAtomicRMW(textureOf(*handle->texture), handle->texture->ResourceHandle, binop,
                                   address, value_vector, atomicArrayIndex(*handle, call, 3));
    }
    if (!result)
      return createError("dxil: AIR texture atomic failed");
  }
  if (handle->texture && result->getType()->isVectorTy())
    result = builder.CreateExtractElement(result, 0ull);
  call->replaceAllUsesWith(result);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerAtomicCompareExchange(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  if (!handle->buffer && !handle->texture)
    return createError("dxil: atomicCompareExchange requires a UAV handle");
  builder.SetInsertPoint(call);
  auto *comparison = call->getArgOperand(5);
  auto *value = call->getArgOperand(6);
  if (value->getType()->isIntegerTy(64)) {
    if (!handle->buffer)
      return unsupported("64 bit atomic on a typed resource (AtomicInt64OnTypedResourceSupported is not reported)");
    return lowerBufferAtomic64(call, *handle, true);
  }

  if (handle->buffer) {
    auto *index = atomicAddress(*handle, call, 2);
    /* Compare exchange on an out-of-range or unbound descriptor leaves the
     * destination untouched and reports the zero it would have read. */
    auto guarded = emitGuardedAccess(
        bufferInBounds(*handle->buffer, index), call, {air.getIntTy()},
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          auto *pointer = builder.CreateGEP(air.getIntTy(), handle->buffer->Pointer, {index});
          auto *result = builder.CreateAtomicCmpXchg(
              pointer, comparison, value, llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic,
              llvm::AtomicOrdering::Monotonic
          );
          return {builder.CreateExtractValue(result, 0)};
        }
    );
    call->replaceAllUsesWith(guarded[0]);
    call->eraseFromParent();
    return llvm::Error::success();
  }
  auto *address = atomicAddress(*handle, call, 2);
  if (!address)
    return createError("dxil: atomicCompareExchange on an unsupported resource shape");
  llvm::Value *result = nullptr;
  if (handle->texture->Scoped3DUAV) {
    auto *count = builder.CreateLShr(High32(handle->texture->Metadata), 16);
    auto *valid = builder.CreateICmpULT(call->getArgOperand(4), count);
    result = emitGuardedAccess(valid, call, {air.getIntTy(4)}, [&]() -> llvm::SmallVector<llvm::Value *, 4> {
      auto [old, exchanged] = air.CreateAtomicCmpXchg(
          textureOf(*handle->texture), handle->texture->ResourceHandle, address,
          builder.CreateVectorSplat(4, comparison), builder.CreateVectorSplat(4, value), nullptr);
      return {old};
    })[0];
  } else {
    result = air.CreateAtomicCmpXchg(
        textureOf(*handle->texture), handle->texture->ResourceHandle, address,
        builder.CreateVectorSplat(4, comparison), builder.CreateVectorSplat(4, value),
        atomicArrayIndex(*handle, call, 2)).first;
  }
  if (!result)
    return createError("dxil: AIR texture compare exchange failed");
  if (result->getType()->isVectorTy())
    result = builder.CreateExtractElement(result, 0ull);
  call->replaceAllUsesWith(result);
  call->eraseFromParent();
  return llvm::Error::success();
}

/* ----------------------------------------------------------------------- */
/* sampling                                                                */
/* ----------------------------------------------------------------------- */

llvm::Value *
ResourceLowering::samplerBias(const dxbc::SamplerDescriptor &sampler) {
  if (!sampler.Metadata)
    return air.getFloat(0);
  return Low32AsFloat(sampler.Metadata);
}

llvm::Value *
ResourceLowering::minLodClamp(const dxbc::TextureDescirptor &descriptor) {
  if (!descriptor.Metadata)
    return air.getFloat(0);
  return Low32AsFloat(descriptor.Metadata);
}

llvm::Error
ResourceLowering::sampleOffsets(llvm::CallInst *call, unsigned first, int32_t offsets[3]) {
  for (unsigned i = 0; i < 3; i++) {
    auto offset = constantOperand(call->getArgOperand(first + i), "sample offset");
    if (!offset)
      return offset.takeError();
    offsets[i] = *offset;
  }
  return llvm::Error::success();
}

uint32_t
ResourceLowering::gradientCount(llvm::air::Texture::ResourceKind kind) {
  switch (kind) {
  case llvm::air::Texture::texture1d:
  case llvm::air::Texture::texture1d_array:
    return 1;
  case llvm::air::Texture::texture2d:
  case llvm::air::Texture::texture2d_array:
  case llvm::air::Texture::depth2d:
  case llvm::air::Texture::depth2d_array:
    return 2;
  default:
    return 3;
  }
}

llvm::Error
ResourceLowering::sampleTarget(
    const dxbc::TextureDescirptor &descriptor, llvm::Value *cube_sampler, llvm::Value *c0, llvm::Value *c1,
    llvm::Value *c2, llvm::Value *c3, llvm::Value *&coord, llvm::Value *&array_index, llvm::Value *&sampler_handle
) {
  switch (descriptor.ResourceKindLogical) {
  case llvm::air::Texture::texture1d:
    coord = padVector(c0, 2, air.getFloat(0.5f));
    break;
  case llvm::air::Texture::texture1d_array:
    coord = padVector(c0, 2, air.getFloat(0.5f));
    array_index = clampArrayIndex(c1, descriptor.Metadata);
    break;
  case llvm::air::Texture::texture2d:
  case llvm::air::Texture::depth2d:
    coord = makeVector({c0, c1});
    break;
  case llvm::air::Texture::texture2d_array:
  case llvm::air::Texture::depth2d_array:
    coord = makeVector({c0, c1});
    array_index = clampArrayIndex(c2, descriptor.Metadata);
    break;
  case llvm::air::Texture::texture3d:
    coord = makeVector({c0, c1, c2});
    break;
  case llvm::air::Texture::texturecube:
  case llvm::air::Texture::depthcube:
    coord = makeVector({c0, c1, c2});
    sampler_handle = cube_sampler;
    break;
  case llvm::air::Texture::texturecube_array:
  case llvm::air::Texture::depthcube_array:
    coord = makeVector({c0, c1, c2});
    array_index = clampArrayIndex(c3, descriptor.Metadata);
    sampler_handle = cube_sampler;
    break;
  default:
    return createError("dxil: sampling an unsupported resource shape");
  }
  return llvm::Error::success();
}

/*
 * SM 6.6 compute derivatives for implicit-LOD sampling. Metal only computes implicit
 * derivatives in fragment functions, so a compute shader samples with explicit
 * gradients formed from the quad (Z ordered: lane q = 2*y + x, laid out by the
 * stage signature): d/dx = value(q|1) - value(q&~1), d/dy = value(q|2) - value(q&~2).
 * Bias is folded in by scaling the gradients by 2^bias, which raises the LOD by
 * exactly `bias` for Metal's isotropic LOD computation.
 */
std::pair<llvm::Value *, llvm::Value *>
ResourceLowering::quadGradients(llvm::Value *coord, uint32_t components) {
  auto *type = coord->getType();
  auto *lane_type = type->getScalarType();
  std::string suffix = air.getTypeOverloadSuffix(type, llvm::air::Signedness::DontCare);
  auto attributes = llvm::AttributeList::get(
      ctx, {{~0U, llvm::Attribute::get(ctx, llvm::Attribute::Convergent)},
            {~0U, llvm::Attribute::get(ctx, llvm::Attribute::NoUnwind)},
            {~0U, llvm::Attribute::get(ctx, llvm::Attribute::WillReturn)}});
  auto shuffle = info.module->getOrInsertFunction(
      "air.quad_shuffle" + suffix, llvm::FunctionType::get(type, {type, builder.getInt16Ty()}, false), attributes);
  auto *lane = builder.CreateCall(
      info.module->getOrInsertFunction("air.get_thread_index_in_simdgroup.i32", llvm::FunctionType::get(air.getIntTy(), false),
                                       attributes));
  auto *quad_lane = builder.CreateTrunc(builder.CreateAnd(lane, air.getInt(3)), builder.getInt16Ty());
  auto read = [&](llvm::Value *index) { return builder.CreateCall(shuffle, {coord, index}); };
  auto *dx = builder.CreateFSub(read(builder.CreateOr(quad_lane, builder.getInt16(1))),
                                read(builder.CreateAnd(quad_lane, builder.getInt16(2))));
  auto *dy = builder.CreateFSub(read(builder.CreateOr(quad_lane, builder.getInt16(2))),
                                read(builder.CreateAnd(quad_lane, builder.getInt16(1))));
  if (components == 1 && type->isVectorTy()) {
    /* 1D textures carry a padded 2 component coordinate. */
    dx = builder.CreateExtractElement(dx, uint64_t(0));
    dy = builder.CreateExtractElement(dy, uint64_t(0));
  } else if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type); vector && vector->getNumElements() != components) {
    llvm::SmallVector<int, 3> mask;
    for (unsigned i = 0; i < components; ++i)
      mask.push_back(i);
    dx = builder.CreateShuffleVector(dx, dx, mask);
    dy = builder.CreateShuffleVector(dy, dy, mask);
  }
  (void)lane_type;
  return {dx, dy};
}

std::pair<llvm::Value *, llvm::Value *>
ResourceLowering::sampleInKernel(
    const dxbc::TextureDescirptor &descriptor, llvm::Value *sampler_value, llvm::Value *coord,
    llvm::Value *array_index, const int32_t offsets[3], llvm::Value *bias, llvm::Value *min_lod, llvm::Value *reference
) {
  auto [dx, dy] = quadGradients(coord, gradientCount(descriptor.ResourceKindLogical));
  if (bias) {
    auto *scale = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, bias);
    if (dx->getType()->isVectorTy())
      scale = builder.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(dx->getType())->getNumElements(), scale);
    dx = builder.CreateFMul(dx, scale);
    dy = builder.CreateFMul(dy, scale);
  }
  return air.CreateSampleGrad(textureOf(descriptor), descriptor.ResourceHandle, sampler_value, coord, array_index, dx,
                              dy, min_lod, offsets, reference);
}

llvm::Error
ResourceLowering::lowerSample(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "sample");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: sample requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3];
  auto offsets_error = sampleOffsets(call, 7, offsets);
  if (offsets_error)
    return offsets_error;

  auto *min_lod = minLodClamp(*descriptor);
  auto *clamp = call->getArgOperand(10);
  if (!IsUndefOrPoison(clamp))
    min_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, min_lod, clamp);

  auto [texel, residency] =
      kernel_stage
          ? sampleInKernel(*descriptor, sampler_value, coord, array_index, offsets,
                           samplerBias(*sampler_handle->sampler), min_lod, nullptr)
          : air.CreateSample(
                textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
                llvm::air::sample_bias{samplerBias(*sampler_handle->sampler)}, llvm::air::sample_min_lod_clamp{min_lod}
            );
  if (!texel)
    return createError("dxil: AIR sample failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(residency);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerGather(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "textureGather");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: textureGather requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3] = {};
  for (unsigned i = 0; i < 2; ++i) {
    auto offset = constantOperand(call->getArgOperand(7 + i), "gather offset");
    if (!offset)
      return offset.takeError();
    offsets[i] = *offset;
  }

  auto [texel, residency] = air.CreateGather(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
      call->getArgOperand(9)
  );
  if (!texel)
    return createError("dxil: AIR gather failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(residency);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerGatherCmp(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "textureGatherCmp");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: textureGatherCmp requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3] = {};
  for (unsigned i = 0; i < 2; ++i) {
    auto offset = constantOperand(call->getArgOperand(7 + i), "gather compare offset");
    if (!offset)
      return offset.takeError();
    offsets[i] = *offset;
  }

  auto result = air.CreateGatherCompare(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, call->getArgOperand(10),
      offsets
  );
  if (!result.first)
    return createError("dxil: AIR gather compare failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, result.first, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(result.second);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerTexture2DMSGetSamplePosition(llvm::CallInst *call) {
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  auto descriptor = textureDescriptor(*handle, "texture2DMSGetSamplePosition");
  if (!descriptor)
    return descriptor.takeError();
  if (descriptor->ResourceKindLogical != llvm::air::Texture::texture2d_ms &&
      descriptor->ResourceKindLogical != llvm::air::Texture::texture2d_ms_array)
    return createError("dxil: texture2DMSGetSamplePosition requires a multisampled 2D texture");
  builder.SetInsertPoint(call);
  auto *count = air.CreateTextureQuery(
      textureOf(*descriptor), descriptor->ResourceHandle, llvm::air::Texture::num_samples, air.getInt(0)
  );
  auto *position = air.CreateGetSamplePosition(count, call->getArgOperand(2));
  auto *center = builder.CreateICmpEQ(builder.CreateTrunc(descriptor->Metadata, builder.getInt32Ty()),
                                      builder.getInt32(1));
  position = builder.CreateSelect(center, llvm::ConstantAggregateZero::get(air.getFloatTy(2)), position);
  llvm::Value *result = llvm::UndefValue::get(call->getType());
  result = builder.CreateInsertValue(result, builder.CreateExtractElement(position, (uint64_t)0), 0);
  result = builder.CreateInsertValue(result, builder.CreateExtractElement(position, 1), 1);
  call->replaceAllUsesWith(result);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerRenderTargetGetSamplePosition(llvm::CallInst *call) {
  builder.SetInsertPoint(call);
  auto *position = air.CreateGetSamplePosition(air.CreateGetNumSamples(), call->getArgOperand(1));
  if (auto *sample_call = llvm::dyn_cast<llvm::CallInst>(position))
    sample_call->setMetadata("dxmt.rt_sample_pos", llvm::MDNode::get(builder.getContext(), {}));
  llvm::Value *result = llvm::UndefValue::get(call->getType());
  result = builder.CreateInsertValue(result, builder.CreateExtractElement(position, (uint64_t)0), 0);
  result = builder.CreateInsertValue(result, builder.CreateExtractElement(position, 1), 1);
  call->replaceAllUsesWith(result);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerRenderTargetGetSampleCount(llvm::CallInst *call) {
  builder.SetInsertPoint(call);
  call->replaceAllUsesWith(air.CreateGetNumSamples());
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerCalculateLOD(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "calculateLOD");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: calculateLOD requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), llvm::UndefValue::get(air.getFloatTy()), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  std::pair<llvm::Value *, llvm::Value *> lod;
  if (!kernel_stage) {
    lod = air.CreateCalculateLOD(textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord);
  } else {
    /* Metal's calculate_*_lod returns 0 in a kernel (measured on the GPU): derive the
     * LOD from the compute quad, log2 of the largest texel-space gradient length, and
     * clamp it to the view's mip range for the clamped form (sampler clamps are not
     * visible to the shader and are applied by the sample itself). */
    const uint32_t components = gradientCount(descriptor->ResourceKindLogical);
    auto [dx, dy] = quadGradients(coord, components);
    auto texture = textureOf(*descriptor);
    llvm::Value *dx_length = air.getFloat(0), *dy_length = air.getFloat(0);
    static constexpr llvm::air::Texture::Query queries[] = {
        llvm::air::Texture::width, llvm::air::Texture::height, llvm::air::Texture::depth};
    for (uint32_t i = 0; i < components && i < 3; ++i) {
      auto *extent = builder.CreateUIToFP(
          air.CreateTextureQuery(texture, descriptor->ResourceHandle, queries[i], air.getInt(0)), air.getFloatTy());
      auto *x = builder.CreateFMul(components == 1 ? dx : builder.CreateExtractElement(dx, i), extent);
      auto *y = builder.CreateFMul(components == 1 ? dy : builder.CreateExtractElement(dy, i), extent);
      dx_length = builder.CreateFAdd(dx_length, builder.CreateFMul(x, x));
      dy_length = builder.CreateFAdd(dy_length, builder.CreateFMul(y, y));
    }
    auto *rho2 = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, dx_length, dy_length);
    auto *unclamped = builder.CreateFMul(air.CreateFPUnOp(llvm::air::AIRBuilder::log2, rho2), air.getFloat(0.5f));
    auto *levels = builder.CreateUIToFP(
        air.CreateTextureQuery(texture, descriptor->ResourceHandle, llvm::air::Texture::num_mip_levels, air.getInt(0)),
        air.getFloatTy());
    auto *clamped_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, unclamped, air.getFloat(0));
    clamped_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, clamped_lod,
                                    builder.CreateFSub(levels, air.getFloat(1)));
    lod = {clamped_lod, unclamped};
  }
  lod.first = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, minLodClamp(*descriptor), lod.first);
  auto *clamped = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(6));
  if (!clamped)
    return createError("dxil: calculateLOD clamped operand must be constant");
  call->replaceAllUsesWith(clamped->isZero() ? lod.second : lod.first);
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerSampleBias(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "sampleBias");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: sampleBias requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3];
  auto offsets_error = sampleOffsets(call, 7, offsets);
  if (offsets_error)
    return offsets_error;

  auto *bias = builder.CreateFAdd(samplerBias(*sampler_handle->sampler), call->getArgOperand(10));
  auto *min_lod = minLodClamp(*descriptor);
  auto *clamp = call->getArgOperand(11);
  if (!IsUndefOrPoison(clamp))
    min_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, min_lod, clamp);

  auto [texel, residency] =
      kernel_stage ? sampleInKernel(*descriptor, sampler_value, coord, array_index, offsets, bias, min_lod, nullptr)
                   : air.CreateSample(
                         textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index,
                         offsets, llvm::air::sample_bias{bias}, llvm::air::sample_min_lod_clamp{min_lod}
                     );
  if (!texel)
    return createError("dxil: AIR sampleBias failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(residency);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerSampleLevel(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "sampleLevel");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: sampleLevel requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3];
  auto offsets_error = sampleOffsets(call, 7, offsets);
  if (offsets_error)
    return offsets_error;

  /* The sampler LOD bias applies exactly once, on top of the explicit LOD. */
  auto *lod = builder.CreateFAdd(call->getArgOperand(10), samplerBias(*sampler_handle->sampler));
  lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, lod, minLodClamp(*descriptor));
  auto [texel, residency] = air.CreateSample(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
      llvm::air::sample_level{lod}
  );
  if (!texel)
    return createError("dxil: AIR sampleLevel failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(residency);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerSampleGrad(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "sampleGrad");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: sampleGrad requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3];
  auto offsets_error = sampleOffsets(call, 7, offsets);
  if (offsets_error)
    return offsets_error;

  uint32_t gradients = gradientCount(descriptor->ResourceKindLogical);
  llvm::SmallVector<llvm::Value *, 3> derivative_x;
  llvm::SmallVector<llvm::Value *, 3> derivative_y;
  for (unsigned i = 0; i < gradients; i++) {
    derivative_x.push_back(call->getArgOperand(10 + i));
    derivative_y.push_back(call->getArgOperand(13 + i));
  }
  auto *min_lod = minLodClamp(*descriptor);
  auto *clamp = call->getArgOperand(16);
  if (!IsUndefOrPoison(clamp))
    min_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, min_lod, clamp);

  auto [texel, residency] = air.CreateSampleGrad(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, makeVector(derivative_x),
      makeVector(derivative_y), min_lod, offsets
  );
  if (!texel)
    return createError("dxil: AIR sampleGrad failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(residency);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerSampleCmp(llvm::CallInst *call, bool level_zero) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "sampleCmp");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: sampleCmp requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  auto target_error = sampleTarget(
      *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
      call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
  );
  if (target_error)
    return target_error;
  int32_t offsets[3];
  auto offsets_error = sampleOffsets(call, 7, offsets);
  if (offsets_error)
    return offsets_error;

  auto *reference = call->getArgOperand(10);
  std::pair<llvm::Value *, llvm::Value *> result;
  if (level_zero) {
    result = air.CreateSampleCmp(
        textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, reference, offsets,
        llvm::air::sample_level{air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, air.getFloat(0), minLodClamp(*descriptor))}
    );
  } else {
    auto *min_lod = minLodClamp(*descriptor);
    auto *clamp = call->getArgOperand(11);
    if (!IsUndefOrPoison(clamp))
      min_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, min_lod, clamp);
    result = kernel_stage
                 ? sampleInKernel(*descriptor, sampler_value, coord, array_index, offsets,
                                  samplerBias(*sampler_handle->sampler), min_lod, reference)
                 : air.CreateSampleCmp(
                       textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index,
                       reference, offsets, llvm::air::sample_bias{samplerBias(*sampler_handle->sampler)},
                       llvm::air::sample_min_lod_clamp{min_lod}
                   );
  }
  if (!result.first)
    return createError("dxil: AIR sampleCmp failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, result.first, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(result.second);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

/*
 * SM 6.7 SampleCmpLevel (224) and SM 6.8 SampleCmpBias (255) / SampleCmpGrad (254).
 * Operands after the shared (srv, sampler, c0..c3, o0..o2, compare) prefix:
 *   level: lod;  bias: bias, clamp;  grad: ddx0..2, ddy0..2, clamp.
 * The sampler LOD bias is applied exactly as for the non-compare forms.
 */
llvm::Error
ResourceLowering::lowerSampleCmpExtended(llvm::CallInst *call, uint32_t opcode) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "sampleCmp");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: sampleCmp requires a sampler handle");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  if (auto error = sampleTarget(
          *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
          call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
      ))
    return error;
  int32_t offsets[3];
  if (auto error = sampleOffsets(call, 7, offsets))
    return error;
  auto *reference = call->getArgOperand(10);
  auto *min_lod = minLodClamp(*descriptor);
  auto clamped = [&](unsigned operand) {
    auto *clamp = call->getArgOperand(operand);
    return IsUndefOrPoison(clamp) ? min_lod : air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, min_lod, clamp);
  };

  std::pair<llvm::Value *, llvm::Value *> result;
  switch (opcode) {
  case kSampleCmpLevel: {
    auto *lod = builder.CreateFAdd(call->getArgOperand(11), samplerBias(*sampler_handle->sampler));
    lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, lod, min_lod);
    result = air.CreateSampleCmp(textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord,
                                 array_index, reference, offsets, llvm::air::sample_level{lod});
    break;
  }
  case kSampleCmpBias: {
    auto *bias = builder.CreateFAdd(samplerBias(*sampler_handle->sampler), call->getArgOperand(11));
    result = kernel_stage ? sampleInKernel(*descriptor, sampler_value, coord, array_index, offsets, bias,
                                           clamped(12), reference)
                          : air.CreateSampleCmp(textureOf(*descriptor), descriptor->ResourceHandle, sampler_value,
                                                coord, array_index, reference, offsets, llvm::air::sample_bias{bias},
                                                llvm::air::sample_min_lod_clamp{clamped(12)});
    break;
  }
  default: {
    const uint32_t gradients = gradientCount(descriptor->ResourceKindLogical);
    llvm::SmallVector<llvm::Value *, 3> derivative_x;
    llvm::SmallVector<llvm::Value *, 3> derivative_y;
    for (unsigned i = 0; i < gradients; i++) {
      derivative_x.push_back(call->getArgOperand(11 + i));
      derivative_y.push_back(call->getArgOperand(14 + i));
    }
    result = air.CreateSampleGrad(textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord,
                                  array_index, makeVector(derivative_x), makeVector(derivative_y), clamped(17),
                                  offsets, reference);
    break;
  }
  }
  if (!result.first)
    return createError("dxil: AIR sample compare failed (the resource must be a depth-compatible 2D or cube view)");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, result.first, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  setStatusFromResidency(result.second);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

/*
 * SM 6.7 TextureGatherRaw (223): the four texels' red channels as raw unsigned bits.
 * The view must be an R16_UINT / R32_UINT / R32G32_UINT view (D3D validates that), so
 * gathering component 0 of the integer view already is the raw texel; a 64 bit view
 * is gathered as two 32 bit channels and recombined.
 */
llvm::Error
ResourceLowering::lowerGatherRaw(llvm::CallInst *call) {
  auto texture_handle = resolve(call->getArgOperand(1));
  if (!texture_handle)
    return texture_handle.takeError();
  auto descriptor = textureDescriptor(*texture_handle, "textureGatherRaw");
  if (!descriptor)
    return descriptor.takeError();
  auto sampler_handle = resolve(call->getArgOperand(2));
  if (!sampler_handle)
    return sampler_handle.takeError();
  if (!sampler_handle->sampler)
    return createError("dxil: textureGatherRaw requires a sampler handle");
  auto texture = textureOf(*descriptor);
  if (texture.sample_type != llvm::air::Texture::sample_uint)
    return createError("dxil: textureGatherRaw requires an unsigned integer view");
  builder.SetInsertPoint(call);

  llvm::Value *coord = nullptr;
  llvm::Value *array_index = nullptr;
  llvm::Value *sampler_value = sampler_handle->sampler->SamplerHandle;
  if (auto error = sampleTarget(
          *descriptor, sampler_handle->sampler->CubeSamplerHandle, call->getArgOperand(3), call->getArgOperand(4),
          call->getArgOperand(5), call->getArgOperand(6), coord, array_index, sampler_value
      ))
    return error;
  int32_t offsets[3] = {};
  for (unsigned i = 0; i < 2; ++i) {
    auto offset = constantOperand(call->getArgOperand(7 + i), "gather raw offset");
    if (!offset)
      return offset.takeError();
    offsets[i] = *offset;
  }
  auto *lane_type = llvm::cast<llvm::StructType>(call->getType())->getElementType(0);
  llvm::Value *residency = nullptr;
  auto gather = [&](unsigned component) {
    auto result = air.CreateGather(texture, descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
                                   air.getInt(component));
    if (!residency)
      residency = result.second;
    return result.first;
  };
  auto *red = gather(0);
  if (!red)
    return createError("dxil: AIR gather failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  for (unsigned i = 0; i < 4; ++i) {
    llvm::Value *lane = builder.CreateExtractElement(red, i);
    if (lane_type->isIntegerTy(64)) {
      auto *green = gather(1);
      if (!green)
        return createError("dxil: AIR gather failed");
      lane = builder.CreateOr(builder.CreateZExt(lane, lane_type),
                              builder.CreateShl(builder.CreateZExt(builder.CreateExtractElement(green, i), lane_type),
                                                32));
    } else if (lane_type != lane->getType()) {
      lane = builder.CreateTrunc(lane, lane_type);
    }
    components.push_back(lane);
  }
  setStatusFromResidency(residency);
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

/* ----------------------------------------------------------------------- */
/* driver                                                                  */
/* ----------------------------------------------------------------------- */

/*
 * A null texture or sampler descriptor must not reach Metal.
 *
 * D3D12 defines a read through a null SRV/UAV as zero and a write as dropped; with
 * Resource Binding Tier 3 and SM 6.6 heap indexing the descriptor can be null (created
 * with a null resource, or an index past the bound heap resolving to the null
 * descriptor) wherever a texture is used. Metal forbids touching a null texture handle
 * ("Null texture access" under shader validation), so the whole operation runs only
 * while its texture - and its sampler, if it takes one - is non-null, and every value it
 * produces is zero otherwise. An RGB32 typed-buffer view has no native texture by design
 * (its data is read through the emulation address), so it counts as present.
 */
llvm::Error
ResourceLowering::guardNullDescriptors(llvm::CallInst *call) {
  auto is_handle = [](llvm::Value *value) {
    auto *type = llvm::dyn_cast<llvm::StructType>(value->getType());
    return type && type->hasName() && type->getName() == "dx.types.Handle";
  };
  if (call->arg_size() < 2 || !is_handle(call->getArgOperand(1)))
    return llvm::Error::success();
  auto handle = resolve(call->getArgOperand(1));
  if (!handle)
    return handle.takeError();
  if (!handle->texture)
    return llvm::Error::success();
  builder.SetInsertPoint(call);
  llvm::Value *present = builder.CreateIsNotNull(handle->texture->ResourceHandle);
  if (handle->texture->EmulationTag)
    present = builder.CreateOr(
        present, builder.CreateTrunc(handle->texture->EmulationTag, builder.getInt1Ty())
    );
  if (call->arg_size() > 2 && is_handle(call->getArgOperand(2))) {
    auto sampler = resolve(call->getArgOperand(2));
    if (!sampler)
      return sampler.takeError();
    builder.SetInsertPoint(call);
    if (sampler->sampler)
      present = builder.CreateAnd(present, builder.CreateIsNotNull(sampler->sampler->SamplerHandle));
  }
  auto *head = call->getParent();
  auto *then_term = llvm::SplitBlockAndInsertIfThen(present, call, /*Unreachable=*/false);
  auto *then_block = then_term->getParent();
  auto *continuation = call->getParent();
  call->moveBefore(then_term);
  if (!call->getType()->isVoidTy()) {
    builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
    auto *merged = builder.CreatePHI(call->getType(), 2);
    call->replaceUsesWithIf(merged, [&](llvm::Use &use) { return use.getUser() != merged; });
    merged->addIncoming(call, then_block);
    merged->addIncoming(llvm::Constant::getNullValue(call->getType()), head);
  }
  return llvm::Error::success();
}

llvm::Error
ResourceLowering::lowerCall(llvm::CallInst *call) {
  uint32_t opcode;
  if (!IsDxilCall(call, opcode))
    return createError("dxil: internal error, not a dx.op call");
  if (opcode != kBarrierByMemoryHandle && opcode != kTextureStoreSample)
    if (auto error = guardNullDescriptors(call))
      return error;
  switch (opcode) {
  case kCBufferLoadLegacy:
    return lowerCBufferLoadLegacy(call);
  case kTextureLoad:
    return lowerTextureLoad(call);
  case kTextureStore:
    return lowerTextureStore(call);
  case kBufferLoad:
    return lowerBufferLoad(call);
  case kBufferStore:
    return lowerBufferStore(call);
  case kRawBufferLoad:
    return lowerRawBufferLoad(call);
  case kRawBufferStore:
    return lowerRawBufferStore(call);
  case kSample:
    return lowerSample(call);
  case kTextureGather:
    return lowerGather(call);
  case kTextureGatherCmp:
    return lowerGatherCmp(call);
  case kTexture2DMSGetSamplePosition:
    return lowerTexture2DMSGetSamplePosition(call);
  case kRenderTargetGetSamplePosition:
    return lowerRenderTargetGetSamplePosition(call);
  case kRenderTargetGetSampleCount:
    return lowerRenderTargetGetSampleCount(call);
  case kCalculateLOD:
    return lowerCalculateLOD(call);
  case kSampleBias:
    return lowerSampleBias(call);
  case kSampleLevel:
    return lowerSampleLevel(call);
  case kSampleGrad:
    return lowerSampleGrad(call);
  case kSampleCmp:
    return lowerSampleCmp(call, false);
  case kSampleCmpLevelZero:
    return lowerSampleCmp(call, true);
  case kSampleCmpLevel:
  case kSampleCmpBias:
  case kSampleCmpGrad:
    return lowerSampleCmpExtended(call, opcode);
  case kTextureGatherRaw:
    return lowerGatherRaw(call);
  case kGetDimensions:
    return lowerGetDimensions(call);
  case kBufferUpdateCounter:
    return lowerBufferUpdateCounter(call);
  case kAtomicBinOp:
    return lowerAtomicBinOp(call);
  case kAtomicCompareExchange:
    return lowerAtomicCompareExchange(call);
  case kBarrierByMemoryHandle:
    return lowerBarrierByMemoryHandle(call);
  case kTextureStoreSample:
    /* Metal's texture2d_ms only has access::read (metal_texture: texture2d_ms static_asserts
     * access::read), so WriteableMSAATexturesSupported is not reported. */
    return createError("dxil: textureStoreSample requires WriteableMSAATexturesSupported, which is not reported");
  default:
    return createError(llvm::Twine("dxil: unsupported resource operation ") + call->getCalledFunction()->getName());
  }
}

llvm::Error
ResourceLowering::run() {
  if (auto *kernels = info.module->getNamedMetadata("air.kernel"))
    kernel_stage = kernels->getNumOperands() != 0;
  collectAnnotations();
  collectCounterHandles();
  collectHeapUsage();

  llvm::SmallVector<llvm::CallInst *, 64> worklist;
  for (auto &function : *info.module) {
    if (function.isDeclaration())
      continue;
    for (auto &block : function) {
      for (auto &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call)
          continue;
        uint32_t opcode;
        if (!IsDxilCall(call, opcode))
          continue;
        switch (opcode) {
        case kCBufferLoadLegacy:
        case kTextureLoad:
        case kTextureStore:
        case kBufferLoad:
        case kBufferStore:
        case kRawBufferLoad:
        case kRawBufferStore:
        case kSample:
        case kTextureGather:
        case kTextureGatherCmp:
        case kTexture2DMSGetSamplePosition:
        case kRenderTargetGetSamplePosition:
        case kRenderTargetGetSampleCount:
        case kCalculateLOD:
        case kSampleBias:
        case kSampleLevel:
        case kSampleGrad:
        case kSampleCmp:
        case kSampleCmpLevelZero:
        case kSampleCmpLevel:
        case kSampleCmpBias:
        case kSampleCmpGrad:
        case kTextureGatherRaw:
        case kGetDimensions:
        case kBufferUpdateCounter:
        case kAtomicBinOp:
        case kAtomicCompareExchange:
        case kBarrierByMemoryHandle:
        case kTextureStoreSample:
          worklist.push_back(call);
          break;
        default:
          break;
        }
      }
    }
  }

  for (auto *call : worklist) {
    if (auto error = lowerCall(call))
      return error;
  }

  /* CheckAccessFullyMapped consumes the status member the resource operations above just built. */
  llvm::SmallVector<llvm::CallInst *, 8> status_checks;
  for (auto &function : *info.module) {
    if (function.isDeclaration())
      continue;
    for (auto &block : function)
      for (auto &instruction : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          uint32_t opcode;
          if (IsDxilCall(call, opcode) && opcode == kCheckAccessFullyMapped)
            status_checks.push_back(call);
        }
  }
  for (auto *call : status_checks)
    if (auto error = lowerCheckAccessFullyMapped(call))
      return error;

  /* Resource consumers now use native descriptors. Remove the whole obsolete
   * handle graph, including phi/select and annotation chains, but never erase
   * a graph with an unlowered consumer. */
  llvm::SmallPtrSet<llvm::Instruction *, 32> handle_nodes;
  for (auto &function : *info.module) {
    if (function.isDeclaration())
      continue;
    for (auto &block : function) {
      for (auto &instruction : block) {
        auto *type = llvm::dyn_cast<llvm::StructType>(instruction.getType());
        if (type && type->hasName() && type->getName() == "dx.types.Handle" &&
            (llvm::isa<llvm::PHINode>(instruction) || llvm::isa<llvm::SelectInst>(instruction))) {
          handle_nodes.insert(&instruction);
          continue;
        }
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call)
          continue;
        uint32_t opcode;
        if (!IsDxilCall(call, opcode))
          continue;
        if (opcode == kCreateHandle || opcode == kCreateHandleForLib || opcode == kAnnotateHandle ||
            opcode == kCreateHandleFromBinding || opcode == kCreateHandleFromHeap)
          handle_nodes.insert(call);
      }
    }
  }
  for (auto *node : handle_nodes)
    for (auto *user : node->users())
      if (!handle_nodes.contains(llvm::dyn_cast<llvm::Instruction>(user))) {
        std::string diagnostic;
        llvm::raw_string_ostream stream(diagnostic);
        user->print(stream);
        return createError("dxil: resource handle still has an unlowered consumer: " + stream.str());
      }
  for (auto *node : handle_nodes)
    node->dropAllReferences();
  for (auto *node : handle_nodes)
    node->eraseFromParent();

  llvm::SmallVector<llvm::Function *, 32> dead_declarations;
  for (auto &function : *info.module) {
    if (!function.isDeclaration() || !function.getName().startswith("dx.op."))
      continue;
    if (function.use_empty())
      dead_declarations.push_back(&function);
  }
  for (auto *declaration : dead_declarations)
    declaration->eraseFromParent();

  return llvm::Error::success();
}

} // namespace

llvm::Error
LowerResources(ModuleInfo &module, dxbc::BindingMap &binding) {
  ResourceLowering lowering(module, binding);
  return lowering.run();
}

} // namespace dxmt::dxil
