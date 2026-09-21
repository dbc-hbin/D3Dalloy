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
};

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

  /* Resolved handle values, keyed by the SSA value that produces them. */
  llvm::DenseMap<llvm::Value *, ResourceHandle> resolved;
  /* dx.op.annotateHandle shapes, keyed by the annotated handle value. */
  llvm::DenseMap<llvm::Value *, ResourceKind> annotations;
  /* Handle values consumed by dx.op.bufferUpdateCounter. */
  llvm::DenseSet<llvm::Value *> counter_handles;

  /* ---- descriptor decoding ---- */

  llvm::Value *Low32(llvm::Value *metadata);
  llvm::Value *High32(llvm::Value *metadata);
  llvm::Value *Low32AsFloat(llvm::Value *metadata);

  /* ---- handle resolution ---- */

  void collectAnnotations();
  void collectCounterHandles();
  void markCounterHandle(llvm::Value *value);

  llvm::Expected<ResourceHandle> resolve(llvm::Value *handle);
  llvm::Expected<ResourceHandle> materialise(llvm::CallInst *call);
  llvm::Expected<ResourceHandle> lookupDescriptors(const HandleSite &site, bool needs_counter);
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

  llvm::Value *
  texelBufferAddress(const dxbc::TextureDescirptor &texture, llvm::Value *element_index, llvm::Value *offset);
  llvm::Value *bufferDwordIndex(llvm::Value *c0, llvm::Value *c1, uint32_t structure_stride);
  llvm::Value *bufferInBounds(const dxbc::BufferDescriptor &buffer, llvm::Value *dword_index);
  llvm::SmallVector<llvm::Value *, 4> emitGuardedAccess(
      llvm::Value *condition, llvm::Instruction *where, llvm::ArrayRef<llvm::Type *> result_types,
      llvm::function_ref<llvm::SmallVector<llvm::Value *, 4>()> emit
  );
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
    /* Last component is the residency status of a resource operation. DXMT has
     * no tiled resource support, so every access is fully mapped. */
    result = builder.CreateInsertValue(result, builder.getInt32(1), {type->getNumElements() - 1});
  }
  return result;
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
  auto *then_block = then_term->getParent();
  auto *continuation = where->getParent();
  builder.SetInsertPoint(then_term);
  auto values = emit();
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

llvm::Error
ResourceLowering::loadBufferWords(
    llvm::CallInst *call, const dxbc::BufferDescriptor &buffer, llvm::Value *c0, llvm::Value *c1, uint32_t mask
) {
  bool coherent = buffer.GlobalCoherent && coherent_access;
  auto *lane_type = llvm::cast<llvm::StructType>(call->getType())->getElementType(0);
  if (lane_type->getPrimitiveSizeInBits() != 32)
    return createError("dxil: non-32-bit raw buffer load is not supported");
  auto *dword_index = bufferDwordIndex(c0, c1, buffer.StructureStride);
  /* The index and its bound condition are emitted before any block split so they
   * dominate the guarded access; only the access itself moves behind the branch. */
  llvm::SmallVector<unsigned, 4> lanes;
  llvm::SmallVector<llvm::Value *, 4> indices;
  llvm::SmallVector<llvm::Value *, 4> conditions;
  for (unsigned i = 0; i < 4; i++) {
    if (!(mask & (1u << i)))
      continue;
    auto *index = builder.CreateAdd(dword_index, air.getInt(i));
    lanes.push_back(i);
    indices.push_back(index);
    conditions.push_back(bufferInBounds(buffer, index));
  }
  llvm::SmallVector<llvm::Value *, 4> components(4, llvm::UndefValue::get(lane_type));
  for (unsigned lane = 0; lane < lanes.size(); lane++) {
    auto *index = indices[lane];
    auto guarded = emitGuardedAccess(
        conditions[lane], call, {air.getIntTy()},
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          auto *pointer = builder.CreateGEP(air.getIntTy(), buffer.Pointer, {index});
          llvm::Value *value = coherent ? (llvm::Value *)air.CreateDeviceCoherentLoad(air.getIntTy(), pointer)
                                        : (llvm::Value *)builder.CreateLoad(air.getIntTy(), pointer);
          return {value};
        }
    );
    components[lanes[lane]] = builder.CreateBitCast(guarded[0], lane_type);
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
  if (call->getArgOperand(4)->getType()->getPrimitiveSizeInBits() != 32)
    return createError("dxil: non-32-bit raw buffer store is not supported");
  auto *dword_index = bufferDwordIndex(c0, c1, buffer.StructureStride);
  llvm::SmallVector<llvm::Value *, 4> indices;
  llvm::SmallVector<llvm::Value *, 4> conditions;
  llvm::SmallVector<llvm::Value *, 4> values;
  for (unsigned i = 0; i < 4; i++) {
    if (!(mask & (1u << i)))
      continue;
    auto *index = builder.CreateAdd(dword_index, air.getInt(i));
    indices.push_back(index);
    conditions.push_back(bufferInBounds(buffer, index));
    values.push_back(builder.CreateBitCast(call->getArgOperand(4 + i), air.getIntTy()));
  }
  for (unsigned lane = 0; lane < indices.size(); lane++) {
    auto *index = indices[lane];
    auto *value = values[lane];
    /* An out-of-range or unbound store writes nothing, so the store itself must
     * not be issued; a select over a null pointer would still store to zero. */
    emitGuardedAccess(
        conditions[lane], call, {},
        [&]() -> llvm::SmallVector<llvm::Value *, 4> {
          auto *pointer = builder.CreateGEP(air.getIntTy(), buffer.Pointer, {index});
          if (coherent)
            air.CreateDeviceCoherentStore(value, pointer);
          else
            builder.CreateStore(value, pointer);
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
  auto [texel, residency] = air.CreateRead(
      texture, descriptor.ResourceHandle, address, array_index, sample_index, level,
      descriptor.GlobalCoherent && coherent_access
  );
  if (!texel)
    return createError("dxil: AIR texture read failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
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
  air.CreateWrite(
      texture, descriptor.ResourceHandle, address, array_index, nullptr, air.getInt(0), value,
      descriptor.GlobalCoherent && coherent_access
  );
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
    case kCreateHandleForLib: {
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
  auto *bind = llvm::dyn_cast<llvm::ConstantAggregate>(call->getArgOperand(1));
  if (!bind || bind->getNumOperands() < 4)
    return createError("dxil: dx.op.createHandleFromBinding needs a constant %dx.types.ResBind");
  auto *space = llvm::dyn_cast<llvm::ConstantInt>(bind->getOperand(0));
  auto *lower = llvm::dyn_cast<llvm::ConstantInt>(bind->getOperand(1));
  auto *klass = llvm::dyn_cast<llvm::ConstantInt>(bind->getOperand(3));
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

llvm::Expected<ResourceHandle>
ResourceLowering::materialise(llvm::CallInst *call) {
  uint32_t opcode;
  if (!IsDxilCall(call, opcode))
    return createError("dxil: internal error, not a dx.op call");

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
  if (buffer->StructureStride % 4)
    return createError("dxil: structured buffer stride must be a multiple of 4 bytes");
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
  if (buffer->StructureStride % 4)
    return createError("dxil: structured buffer stride must be a multiple of 4 bytes");
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
  if (buffer->StructureStride % 4)
    return createError("dxil: structured buffer stride must be a multiple of 4 bytes");
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
  if (buffer->StructureStride % 4)
    return createError("dxil: structured buffer stride must be a multiple of 4 bytes");
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
        z = air.CreateTextureQuery(texture, descriptor.ResourceHandle, llvm::air::Texture::depth, level);
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
    return makeVector({c[0], c[1], c[2]});
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
    result = air.CreateAtomicRMW(
        textureOf(*handle->texture), handle->texture->ResourceHandle, binop, address, value_vector,
        atomicArrayIndex(*handle, call, 3)
    );
    if (!result)
      return createError("dxil: AIR texture atomic failed");
  }
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
  auto result = air.CreateAtomicCmpXchg(
      textureOf(*handle->texture), handle->texture->ResourceHandle, address, builder.CreateVectorSplat(4, comparison),
      builder.CreateVectorSplat(4, value), atomicArrayIndex(*handle, call, 2)
  );
  if (!result.first)
    return createError("dxil: AIR texture compare exchange failed");
  call->replaceAllUsesWith(result.first);
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
    coord = padVector(c0, 2, air.getFloat(0));
    break;
  case llvm::air::Texture::texture1d_array:
    coord = padVector(c0, 2, air.getFloat(0));
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

  auto [texel, residency] = air.CreateSample(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
      llvm::air::sample_bias{samplerBias(*sampler_handle->sampler)}, llvm::air::sample_min_lod_clamp{min_lod}
  );
  if (!texel)
    return createError("dxil: AIR sample failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
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
  auto lod = air.CreateCalculateLOD(textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord);
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

  auto [texel, residency] = air.CreateSample(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
      llvm::air::sample_bias{bias}, llvm::air::sample_min_lod_clamp{min_lod}
  );
  if (!texel)
    return createError("dxil: AIR sampleBias failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
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
  auto [texel, residency] = air.CreateSample(
      textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, offsets,
      llvm::air::sample_level{lod}
  );
  if (!texel)
    return createError("dxil: AIR sampleLevel failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, texel, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
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
        llvm::air::sample_level{air.getFloat(0)}
    );
  } else {
    auto *min_lod = minLodClamp(*descriptor);
    auto *clamp = call->getArgOperand(11);
    if (!IsUndefOrPoison(clamp))
      min_lod = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, min_lod, clamp);
    result = air.CreateSampleCmp(
        textureOf(*descriptor), descriptor->ResourceHandle, sampler_value, coord, array_index, reference, offsets,
        llvm::air::sample_bias{samplerBias(*sampler_handle->sampler)}, llvm::air::sample_min_lod_clamp{min_lod}
    );
  }
  if (!result.first)
    return createError("dxil: AIR sampleCmp failed");
  llvm::SmallVector<llvm::Value *, 4> components;
  appendTexel(components, result.first, llvm::cast<llvm::StructType>(call->getType())->getElementType(0));
  call->replaceAllUsesWith(buildResult(call, components));
  call->eraseFromParent();
  return llvm::Error::success();
}

/* ----------------------------------------------------------------------- */
/* driver                                                                  */
/* ----------------------------------------------------------------------- */

llvm::Error
ResourceLowering::lowerCall(llvm::CallInst *call) {
  uint32_t opcode;
  if (!IsDxilCall(call, opcode))
    return createError("dxil: internal error, not a dx.op call");
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
  case kGetDimensions:
    return lowerGetDimensions(call);
  case kBufferUpdateCounter:
    return lowerBufferUpdateCounter(call);
  case kAtomicBinOp:
    return lowerAtomicBinOp(call);
  case kAtomicCompareExchange:
    return lowerAtomicCompareExchange(call);
  default:
    return createError(llvm::Twine("dxil: unsupported resource operation ") + call->getCalledFunction()->getName());
  }
}

llvm::Error
ResourceLowering::run() {
  collectAnnotations();
  collectCounterHandles();

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
        case kGetDimensions:
        case kBufferUpdateCounter:
        case kAtomicBinOp:
        case kAtomicCompareExchange:
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
            opcode == kCreateHandleFromBinding)
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
