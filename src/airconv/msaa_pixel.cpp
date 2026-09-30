#include "msaa_pixel.hpp"

#include <algorithm>
#include <array>

#include "air_signature.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Error.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace dxmt::msaa {
namespace {

Error invalid(const char *message) {
  return createStringError(inconvertibleErrorCode(), "MSAA pixel: %s", message);
}

bool sampleExternalEffect(Instruction &instruction) {
  if (auto *store = dyn_cast<StoreInst>(&instruction))
    return store->getPointerAddressSpace() != 0 || store->isVolatile();
  if (auto *load = dyn_cast<LoadInst>(&instruction))
    return load->isVolatile();
  if (isa<AtomicRMWInst>(instruction) || isa<AtomicCmpXchgInst>(instruction) ||
      isa<FenceInst>(instruction))
    return true;
  auto *call = dyn_cast<CallInst>(&instruction);
  if (!call) return false;
  auto *callee = call->getCalledFunction();
  if (!callee) return call->mayWriteToMemory();
  StringRef name = callee->getName();
  if (name.startswith("air.atomic") || name.startswith("air.write_") ||
      name.startswith("air.store.") || name.startswith("air.fence_")) return true;
  if (!call->mayWriteToMemory()) return false;
  // AIR's derivative/interpolation declarations often omit LLVM's readonly
  // attributes, although these instructions only query fragment state.
  return !(name.startswith("air.dfdx.") || name.startswith("air.dfdy.") ||
           name.startswith("air.interpolate_") || name.startswith("dxmt.sample_pos.") ||
           name.startswith("air.sample_") || name.startswith("air.read_") ||
           name.startswith("air.gather_") || name.startswith("air.load.") ||
           name.startswith("air.get_") || name.startswith("air.calculate_clamped_lod_") ||
           name.startswith("air.fast_") || name.startswith("air.convert") ||
           name.startswith("air.fma.") || name.startswith("air.dot.") ||
           name.startswith("air.fabs.") || name.startswith("air.fmin.") ||
           name.startswith("air.fmax."));
}

void guardSampleEffects(Function &body, Value *active) {
  SmallVector<Instruction *, 16> effects;
  for (auto &instruction : instructions(body))
    if (sampleExternalEffect(instruction)) effects.push_back(&instruction);
  for (auto *instruction : effects) {
    auto *block = instruction->getParent();
    auto *continuation = block->splitBasicBlock(instruction->getNextNode(), "sample.effect.cont");
    block->getTerminator()->eraseFromParent();
    auto *write = BasicBlock::Create(body.getContext(), "sample.effect", &body, continuation);
    IRBuilder<> writeBuilder(write);
    auto *end = writeBuilder.CreateBr(continuation);
    instruction->moveBefore(end);
    IRBuilder<>(block).CreateCondBr(active, write, continuation);
    if (!instruction->getType()->isVoidTy() && !instruction->use_empty()) {
      auto *result = PHINode::Create(instruction->getType(), 2, "sample.effect.value",
                                     &continuation->front());
      instruction->replaceAllUsesWith(result);
      result->addIncoming(instruction, write);
      // Inactive lanes still follow users of an effect result (notably the
      // pipeline-statistics low-word carry branch). Poison here makes that
      // branch undefined and lets LLVM elide the active guard altogether.
      result->addIncoming(Constant::getNullValue(instruction->getType()), block);
    }
  }
}

StringRef kind(const MDNode &node) {
  auto *value = dyn_cast<MDString>(node.getOperand(1));
  return value ? value->getString() : StringRef{};
}

StringRef outputKind(const MDNode &node) {
  auto *value = dyn_cast<MDString>(node.getOperand(0));
  return value ? value->getString() : StringRef{};
}

MDNode *indexed(MDNode &node, unsigned index, LLVMContext &context) {
  SmallVector<Metadata *, 16> fields(node.op_begin(), node.op_end());
  fields[0] = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index));
  return MDTuple::get(context, fields);
}

MDNode *builtin(unsigned index, StringRef attribute, StringRef type, StringRef name, LLVMContext &context) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, attribute), MDString::get(context, "air.arg_type_name"),
    MDString::get(context, type), MDString::get(context, "air.arg_name"), MDString::get(context, name)
  });
}

MDNode *positionBuiltin(unsigned index, LLVMContext &context) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, "air.position"), MDString::get(context, "air.center"),
    MDString::get(context, "air.no_perspective"), MDString::get(context, "air.arg_type_name"),
    MDString::get(context, "float4"), MDString::get(context, "air.arg_name"),
    MDString::get(context, "mtl_position")
  });
}

MDNode *clipPositionInput(unsigned index, LLVMContext &context) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, "air.fragment_input"), MDString::get(context, "user(msaa_clip_position)"),
    MDString::get(context, "air.interpolation_function"), MDString::get(context, "air.perspective"),
    MDString::get(context, "air.arg_type_name"), MDString::get(context, "float4"),
    MDString::get(context, "air.arg_name"), MDString::get(context, "msaa_clip_position")
  });
}

MDNode *reciprocalWInput(unsigned index, LLVMContext &context) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, "air.fragment_input"), MDString::get(context, "user(msaa_reciprocal_w)"),
    MDString::get(context, "air.interpolation_function"), MDString::get(context, "air.no_perspective"),
    MDString::get(context, "air.arg_type_name"), MDString::get(context, "float"),
    MDString::get(context, "air.arg_name"), MDString::get(context, "msaa_reciprocal_w")
  });
}

MDNode *lineEndpointInput(unsigned index, StringRef name, LLVMContext &context) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, "air.fragment_input"),
    MDString::get(context, ("user(" + name + ")").str()),
    MDString::get(context, "air.flat"), MDString::get(context, "air.arg_type_name"),
    MDString::get(context, "float2"), MDString::get(context, "air.arg_name"),
    MDString::get(context, name)
  });
}

MDNode *textureMetadata(unsigned index, LLVMContext &context, bool multisampled) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, "air.texture"), MDString::get(context, "air.location_index"),
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), SM50_BINDING_INDEX_MSAA_COVERAGE)),
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), 1)),
    MDString::get(context, "air.read"), MDString::get(context, "air.arg_type_name"),
    MDString::get(context, multisampled ? "texture2d_ms<uint, read>" : "texture2d<uint, read>"),
    MDString::get(context, "air.arg_name"), MDString::get(context, "msaa_coverage")
  });
}

MDNode *outputMetadata(LLVMContext &context, bool mask) {
  if (mask)
    return MDTuple::get(context, {MDString::get(context, "air.sample_mask"),
      MDString::get(context, "air.arg_type_name"), MDString::get(context, "uint"),
      MDString::get(context, "air.arg_name"), MDString::get(context, "mtl_coverage_mask")});
  return MDTuple::get(context, {MDString::get(context, "air.render_target"),
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), 0)),
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), 0)),
    MDString::get(context, "air.arg_type_name"), MDString::get(context, "uint"),
    MDString::get(context, "air.arg_name"), MDString::get(context, "msaa_pre_depth_coverage")});
}

MDNode *ownedDepthOutput(LLVMContext &context) {
  return MDTuple::get(context, {MDString::get(context, "air.depth"),
    MDString::get(context, "air.depth_qualifier"), MDString::get(context, "air.any"),
    MDString::get(context, "air.arg_type_name"), MDString::get(context, "float"),
    MDString::get(context, "air.arg_name"), MDString::get(context, "mtl_depth")});
}

MDNode *entryMetadata(Function &entry, MDNode &original, ArrayRef<Metadata *> inputs,
                      unsigned variant, bool defaultDepth) {
  auto &context = entry.getContext();
  SmallVector<Metadata *, 6> fields(original.op_begin(), original.op_end());
  fields[0] = ValueAsMetadata::get(&entry);
  if (variant == SM50_MSAA_COVERAGE)
    fields[1] = MDTuple::get(context, {outputMetadata(context, false), outputMetadata(context, true)});
  else if (variant == SM50_MSAA_PIXEL)
    fields[1] = MDTuple::get(context, {});
  else {
    auto *oldOutputs = cast<MDTuple>(fields[1]);
    SmallVector<Metadata *, 16> outputs(oldOutputs->op_begin(), oldOutputs->op_end());
    for (auto &output : outputs)
      if (outputKind(*cast<MDNode>(output)) == "air.depth")
        output = ownedDepthOutput(context);
    if (defaultDepth) outputs.push_back(ownedDepthOutput(context));
    fields[1] = MDTuple::get(context, outputs);
  }
  fields[2] = MDTuple::get(context, inputs);
  // The dedicated internal early pass consumes the pre-DS sample mask before
  // native depth/stencil. Application and capture variants must not pretest it.
  for (unsigned i = 3; i < fields.size();) {
    if (auto *tag = dyn_cast<MDString>(fields[i]); tag && tag->getString() == "early_fragment_tests")
      fields.erase(fields.begin() + i);
    else
      ++i;
  }
  return MDTuple::get(context, fields);
}

air::ArgumentBindingBuffer scratchBinding(unsigned location, bool writable, StringRef name) {
  return {.buffer_size = {}, .location_index = location, .array_size = 0,
    .memory_access = writable ? air::MemoryAccess::read_write : air::MemoryAccess::read,
    .address_space = air::AddressSpace::device, .type = air::MSLUint{},
    .arg_name = name.str(), .raster_order_group = {}};
}

air::ArgumentBindingBuffer controlBinding() {
  return {.buffer_size = sizeof(SM50_MSAA_TILE_CONTROL),
    .location_index = SM50_BINDING_INDEX_MSAA_TILE_CONTROL, .array_size = 1,
    .memory_access = air::MemoryAccess::read, .address_space = air::AddressSpace::constant,
    .type = air::MSLUint{}, .arg_name = "msaa_tile", .raster_order_group = {}};
}

bool resourceInput(StringRef name) {
  return name == "air.buffer" || name == "air.indirect_buffer" ||
    name == "air.texture" || name == "air.sampler" || name == "air.buffer_array" ||
    name == "air.texture_array" || name == "air.sampler_array";
}

unsigned dwords(Type *type, const DataLayout &layout) {
  auto bytes = layout.getTypeAllocSize(type).getFixedValue();
  return (bytes + 3) / 4;
}

enum PullLocation : unsigned { PullCenter, PullCentroid, PullSample, PullOffset };

struct PulledInput {
  unsigned argument;
  bool perspective;
  unsigned offset[4] = {~0u, ~0u, ~0u, ~0u};
  unsigned anchor[4] = {~0u, ~0u, ~0u, ~0u};
};

struct LineInput {
  unsigned argument;
  unsigned vertex_offset;
  unsigned sample_offset;
  bool no_perspective;
  bool flat;
};

struct DistanceInput {
  unsigned argument;
  unsigned vertex_offset;
  unsigned sample_offset;
  bool no_perspective;
  bool flat;
  bool centroid;
};

bool distanceSemantic(StringRef semantic) {
  return semantic.startswith("user(semantic_sv_clipdistance_") ||
         semantic.startswith("user(semantic_sv_culldistance_");
}

uint64_t userHash(StringRef semantic) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : semantic) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

Value *lineBlend(IRBuilder<> &b, Value *a, Value *z, Value *fraction) {
  Type *type = a->getType();
  if (type->isFloatingPointTy() ||
      (type->isVectorTy() && type->getScalarType()->isFloatingPointTy())) {
    Value *t = b.CreateFPCast(fraction, type->getScalarType());
    if (auto *vector = dyn_cast<FixedVectorType>(type))
      t = b.CreateVectorSplat(vector->getNumElements(), t);
    return b.CreateFAdd(a, b.CreateFMul(b.CreateFSub(z, a), t));
  }
  if (auto *array = dyn_cast<ArrayType>(type)) {
    Value *value = UndefValue::get(type);
    for (unsigned i = 0; i < array->getNumElements(); ++i)
      value = b.CreateInsertValue(value, lineBlend(b, b.CreateExtractValue(a, i),
        b.CreateExtractValue(z, i), fraction), i);
    return value;
  }
  if (auto *structure = dyn_cast<StructType>(type)) {
    Value *value = UndefValue::get(type);
    for (unsigned i = 0; i < structure->getNumElements(); ++i)
      value = b.CreateInsertValue(value, lineBlend(b, b.CreateExtractValue(a, i),
        b.CreateExtractValue(z, i), fraction), i);
    return value;
  }
  return a; // Integral components are flat.
}

Value *lineFraction(IRBuilder<> &b, Value *start, Value *end, Value *xy) {
  auto coord = [&](Value *v, unsigned i) { return b.CreateExtractElement(v, b.getInt32(i)); };
  Value *dx = b.CreateFSub(coord(end, 0), coord(start, 0));
  Value *dy = b.CreateFSub(coord(end, 1), coord(start, 1));
  Value *length2 = b.CreateFAdd(b.CreateFMul(dx, dx), b.CreateFMul(dy, dy));
  Value *numerator = b.CreateFAdd(b.CreateFMul(b.CreateFSub(coord(xy, 0), coord(start, 0)), dx),
                                   b.CreateFMul(b.CreateFSub(coord(xy, 1), coord(start, 1)), dy));
  // A zero-length line uses its leading endpoint. Nonzero projected lines
  // extrapolate at pixel centers beyond the endpoints (including cap pixels).
  Value *q = b.CreateFDiv(numerator, b.CreateSelect(
    b.CreateFCmpOGT(length2, ConstantFP::get(b.getFloatTy(), 0)),
    length2, ConstantFP::get(b.getFloatTy(), 1)));
  q = b.CreateSelect(b.CreateFCmpOGT(length2, ConstantFP::get(b.getFloatTy(), 0)),
    q, ConstantFP::get(b.getFloatTy(), 0));
  return q;
}

Value *lineValue(IRBuilder<> &b, Value *a, Value *z, Value *start, Value *end,
                 Value *interval, Value *xy, Value *w0, Value *w1,
                 bool noPerspective, bool flat) {
  if (flat) return a;
  Value *q = lineFraction(b, start, end, xy);
  auto coord = [&](Value *v, unsigned i) { return b.CreateExtractElement(v, b.getInt32(i)); };
  Value *lo = coord(interval, 0), *hi = coord(interval, 1);
  if (noPerspective) {
    auto screenT = [&](Value *t) {
      Value *w = b.CreateFAdd(w0, b.CreateFMul(b.CreateFSub(w1, w0), t));
      return b.CreateFDiv(b.CreateFMul(t, w1), w);
    };
    Value *t = b.CreateFAdd(screenT(lo),
      b.CreateFMul(q, b.CreateFSub(screenT(hi), screenT(lo))));
    return lineBlend(b, a, z, t);
  }
  Value *from = lineBlend(b, a, z, lo), *to = lineBlend(b, a, z, hi);
  Value *startW = b.CreateFAdd(w0, b.CreateFMul(b.CreateFSub(w1, w0), lo));
  Value *endW = b.CreateFAdd(w0, b.CreateFMul(b.CreateFSub(w1, w0), hi));
  Value *left = b.CreateFDiv(b.CreateFSub(ConstantFP::get(b.getFloatTy(), 1), q), startW);
  Value *right = b.CreateFDiv(q, endW);
  return lineBlend(b, from, to, b.CreateFDiv(right, b.CreateFAdd(left, right)));
}

// Screen coordinates as homogeneous numerators. Projective weights derived
// from these remain defined when an original vertex has clip W == 0.
std::array<Value *, 3> distanceScreenVertex(IRBuilder<> &b, Value *clip,
                                             Value *originX, Value *originY,
                                             Value *width, Value *height) {
  auto *x = b.CreateExtractElement(clip, b.getInt32(0));
  auto *y = b.CreateExtractElement(clip, b.getInt32(1));
  auto *w = b.CreateExtractElement(clip, b.getInt32(3));
  auto *half = ConstantFP::get(b.getFloatTy(), .5);
  return {b.CreateFAdd(b.CreateFMul(originX, w),
            b.CreateFMul(b.CreateFMul(width, half), b.CreateFAdd(x, w))),
          b.CreateFAdd(b.CreateFMul(originY, w),
            b.CreateFMul(b.CreateFMul(height, half), b.CreateFSub(w, y))), w};
}

Value *distanceInterpolate(IRBuilder<> &b, ArrayRef<Value *> values,
                            ArrayRef<std::array<Value *, 3>> vertices,
                            Value *xy, bool noPerspective, bool flat) {
  if (flat || values.size() == 1) return values[0];
  auto *px = b.CreateExtractElement(xy, b.getInt32(0));
  auto *py = b.CreateExtractElement(xy, b.getInt32(1));
  std::array<Value *, 3> dx{}, dy{};
  for (unsigned i = 0; i < vertices.size(); ++i) {
    dx[i] = b.CreateFSub(vertices[i][0], b.CreateFMul(px, vertices[i][2]));
    dy[i] = b.CreateFSub(vertices[i][1], b.CreateFMul(py, vertices[i][2]));
  }
  if (values.size() == 2) {
    auto *xDenominator = b.CreateFSub(dx[1], dx[0]);
    auto *yDenominator = b.CreateFSub(dy[1], dy[0]);
    auto *useX = b.CreateFCmpOGE(b.CreateUnaryIntrinsic(Intrinsic::fabs, xDenominator),
                                b.CreateUnaryIntrinsic(Intrinsic::fabs, yDenominator));
    auto *denominator = b.CreateSelect(useX, xDenominator, yDenominator);
    auto *nondegenerate = b.CreateFCmpONE(denominator, ConstantFP::get(b.getFloatTy(), 0));
    auto *t = b.CreateSelect(nondegenerate,
      b.CreateFDiv(b.CreateSelect(useX, b.CreateFNeg(dx[0]), b.CreateFNeg(dy[0])),
        b.CreateSelect(nondegenerate, denominator, ConstantFP::get(b.getFloatTy(), 1))),
      ConstantFP::get(b.getFloatTy(), 0));
    if (noPerspective)
      t = b.CreateFDiv(b.CreateFMul(t, vertices[1][2]),
                      b.CreateFAdd(vertices[0][2], b.CreateFMul(t,
                        b.CreateFSub(vertices[1][2], vertices[0][2]))));
    return lineBlend(b, values[0], values[1], t);
  }
  std::array<Value *, 3> weights{};
  for (unsigned i = 0; i < 3; ++i) {
    unsigned j = (i + 1) % 3, k = (i + 2) % 3;
    weights[i] = b.CreateFSub(b.CreateFMul(dx[j], dy[k]), b.CreateFMul(dy[j], dx[k]));
    if (noPerspective) weights[i] = b.CreateFMul(weights[i], vertices[i][2]);
  }
  auto *denominator = b.CreateFAdd(b.CreateFAdd(weights[0], weights[1]), weights[2]);
  Value *result = nullptr;
  for (unsigned i = 0; i < 3; ++i) {
    auto *weight = b.CreateFDiv(weights[i], denominator);
    auto *v = b.CreateFMul(values[i], b.CreateVectorSplat(4, weight));
    result = result ? b.CreateFAdd(result, v) : v;
  }
  return result;
}

Value *lineCenterXY(IRBuilder<> &b, Value *position) {
  Value *xy = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  for (unsigned axis = 0; axis < 2; ++axis) {
    Value *component = b.CreateExtractElement(position, b.getInt32(axis));
    Value *pixel = b.CreateUIToFP(b.CreateFPToUI(component, b.getInt32Ty()), b.getFloatTy());
    xy = b.CreateInsertElement(xy, b.CreateFAdd(pixel,
      ConstantFP::get(b.getFloatTy(), .5)), b.getInt32(axis));
  }
  return xy;
}

Value *lineSampleXY(IRBuilder<> &b, Value *position, Value *sample, unsigned count, bool centerPattern) {
  constexpr int8_t x1[] = {0}, y1[] = {0};
  constexpr int8_t x2[] = {4, -4}, y2[] = {4, -4};
  constexpr int8_t x4[] = {-2, 6, -6, 2}, y4[] = {-6, -2, 2, 6};
  constexpr int8_t x8[] = {1, -1, 5, -3, -5, -7, 3, 7};
  constexpr int8_t y8[] = {-3, 3, 1, -5, 5, -1, 7, -7};
  const int8_t *sx = count == 1 ? x1 : count == 2 ? x2 : count == 4 ? x4 : x8;
  const int8_t *sy = count == 1 ? y1 : count == 2 ? y2 : count == 4 ? y4 : y8;
  Value *xy = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  for (unsigned axis = 0; axis < 2; ++axis) {
    Value *coordinate = b.CreateExtractElement(position, b.getInt32(axis));
    Value *pixel = b.CreateUIToFP(b.CreateFPToUI(coordinate, b.getInt32Ty()), b.getFloatTy());
    Value *delta = ConstantFP::get(b.getFloatTy(), 0);
    for (unsigned i = 0; i < count; ++i)
      delta = b.CreateSelect(b.CreateICmpEQ(sample, b.getInt32(i)),
        ConstantFP::get(b.getFloatTy(), centerPattern ? 0.0 :
          double(axis ? sy[i] : sx[i]) / 16.0), delta);
    xy = b.CreateInsertElement(xy, b.CreateFAdd(b.CreateFAdd(pixel,
      ConstantFP::get(b.getFloatTy(), .5)), delta), b.getInt32(axis));
  }
  return xy;
}

unsigned pullLocation(StringRef name) {
  if (name.startswith("air.interpolate_center_")) return PullCenter;
  if (name.startswith("air.interpolate_centroid_")) return PullCentroid;
  if (name.startswith("air.interpolate_sample_")) return PullSample;
  if (name.startswith("air.interpolate_offset_")) return PullOffset;
  return ~0u;
}

Value *centroidSlot(IRBuilder<> &builder, Value *primitiveCoverage, Value *psoMask,
                    unsigned sampleCount) {
  auto *fullMask = builder.getInt32((1u << sampleCount) - 1);
  auto *remaining = builder.CreateAnd(primitiveCoverage, psoMask);
  auto *candidates = builder.CreateSelect(builder.CreateICmpEQ(remaining, builder.getInt32(0)),
    psoMask, remaining);
  Value *firstSample = builder.getInt32(0);
  for (unsigned sample = sampleCount; sample-- > 0;)
    firstSample = builder.CreateSelect(builder.CreateICmpNE(builder.CreateAnd(candidates,
      builder.getInt32(1u << sample)), builder.getInt32(0)), builder.getInt32(sample), firstSample);
  // Primitive coverage takes precedence over the PSO mask; an off-primitive
  // helper uses the first enabled PSO sample, or center for a full PSO mask.
  auto *center = builder.CreateOr(builder.CreateICmpEQ(primitiveCoverage, fullMask),
                                  builder.CreateICmpEQ(candidates, fullMask));
  return builder.CreateSelect(center, builder.getInt32(0),
                              builder.CreateAdd(firstSample, builder.getInt32(1)));
}

Value *recordPointer(IRBuilder<> &builder, Value *scratch, Value *pixel, unsigned stride, unsigned offset);

Value *pullIndex(IRBuilder<> &builder, CallInst &call, unsigned location, unsigned sampleCount,
                 Value *scratch, Value *pixel, Value *control, unsigned inputWords) {
  switch (location) {
  case PullCenter: return builder.getInt32(0);
  case PullCentroid: {
    auto *primitiveCoverage = builder.CreateLoad(builder.getInt32Ty(),
      recordPointer(builder, scratch, pixel, inputWords, 1));
    auto *psoMask = builder.CreateLoad(builder.getInt32Ty(),
      builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 3));
    return centroidSlot(builder, primitiveCoverage, psoMask, sampleCount);
  }
  case PullSample: return builder.CreateAnd(call.getArgOperand(1), builder.getInt32(sampleCount - 1));
  case PullOffset: {
    // Slot zero represents D3D -8/16, or 0/16 relative to Metal's pixel corner.
    auto *offset = call.getArgOperand(1);
    Value *components[2];
    for (unsigned i = 0; i < 2; ++i) {
      auto *floatValue = builder.CreateExtractElement(offset, builder.getInt32(i));
      components[i] = builder.CreateAnd(builder.CreateFPToSI(
        builder.CreateFMul(floatValue, ConstantFP::get(builder.getFloatTy(), 16.0)), builder.getInt32Ty()),
        builder.getInt32(15));
    }
    return builder.CreateOr(components[0], builder.CreateShl(components[1], 4));
  }
  }
  llvm_unreachable("invalid pull location");
}

Value *recordPointer(IRBuilder<> &builder, Value *scratch, Value *pixel, unsigned stride, unsigned offset) {
  auto *index = builder.CreateAdd(builder.CreateMul(builder.CreateZExt(pixel, builder.getInt64Ty()),
                                                   builder.getInt64(stride)), builder.getInt64(offset));
  return builder.CreateGEP(builder.getInt32Ty(), scratch, index);
}

Value *pixelIndex(IRBuilder<> &builder, Value *position, Value *control) {
  auto *x = builder.CreateFPToUI(builder.CreateExtractElement(position, builder.getInt32(0)), builder.getInt32Ty());
  auto *y = builder.CreateFPToUI(builder.CreateExtractElement(position, builder.getInt32(1)), builder.getInt32Ty());
  auto *originX = builder.CreateLoad(builder.getInt32Ty(), builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 0));
  auto *originY = builder.CreateLoad(builder.getInt32Ty(), builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 1));
  auto *tileX = builder.CreateSub(x, originX);
  auto *tileY = builder.CreateSub(y, originY);
  return builder.CreateAdd(tileX, builder.CreateMul(tileY, builder.getInt32(8)));
}

Value *inTile(IRBuilder<> &builder, Value *position, Value *control) {
  auto *x = builder.CreateFPToUI(builder.CreateExtractElement(position, builder.getInt32(0)), builder.getInt32Ty());
  auto *y = builder.CreateFPToUI(builder.CreateExtractElement(position, builder.getInt32(1)), builder.getInt32Ty());
  auto *originX = builder.CreateLoad(builder.getInt32Ty(), builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 0));
  auto *originY = builder.CreateLoad(builder.getInt32Ty(), builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 1));
  return builder.CreateAnd(builder.CreateICmpULT(builder.CreateSub(x, originX), builder.getInt32(8)),
                           builder.CreateICmpULT(builder.CreateSub(y, originY), builder.getInt32(8)));
}

Value *quadShuffle(IRBuilder<> &builder, Module &module, Value *value, unsigned lane) {
  auto *type = value->getType();
  if (auto *structure = dyn_cast<StructType>(type)) {
    Value *result = UndefValue::get(type);
    for (unsigned i = 0; i < structure->getNumElements(); ++i)
      result = builder.CreateInsertValue(result, quadShuffle(builder, module,
        builder.CreateExtractValue(value, i), lane), i);
    return result;
  }
  if (auto *array = dyn_cast<ArrayType>(type)) {
    Value *result = UndefValue::get(type);
    for (unsigned i = 0; i < array->getNumElements(); ++i)
      result = builder.CreateInsertValue(result, quadShuffle(builder, module,
        builder.CreateExtractValue(value, i), lane), i);
    return result;
  }
  unsigned bits = type->getScalarSizeInBits();
  if (auto *vector = dyn_cast<FixedVectorType>(type); vector && bits != 32) {
    Value *result = UndefValue::get(type);
    for (unsigned i = 0; i < vector->getNumElements(); ++i)
      result = builder.CreateInsertElement(result, quadShuffle(builder, module,
        builder.CreateExtractElement(value, builder.getInt32(i)), lane), builder.getInt32(i));
    return result;
  }
  if (bits == 16) {
    Value *word = type->isIntegerTy() ? value : builder.CreateBitCast(value, builder.getInt16Ty());
    Value *shuffled = quadShuffle(builder, module, builder.CreateZExt(word, builder.getInt32Ty()), lane);
    Value *part = builder.CreateTrunc(shuffled, builder.getInt16Ty());
    return type->isIntegerTy() ? part : builder.CreateBitCast(part, type);
  }
  if (bits == 64) {
    Value *words = builder.CreateBitCast(value, FixedVectorType::get(builder.getInt32Ty(), 2));
    return builder.CreateBitCast(quadShuffle(builder, module, words, lane), type);
  }
  if (type->isIntegerTy(1)) value = builder.CreateZExt(value, builder.getInt32Ty());
  auto *storage = value->getType();
  const unsigned count = storage->isVectorTy()
    ? cast<FixedVectorType>(storage)->getNumElements() : 1;
  auto fn = module.getOrInsertFunction("air.quad_shuffle.u.i32",
    FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty(), builder.getInt16Ty()}, false));
  Value *words = count == 1 ? (storage == builder.getInt32Ty() ? value :
                               builder.CreateBitCast(value, builder.getInt32Ty()))
                            : builder.CreateBitCast(value, FixedVectorType::get(builder.getInt32Ty(), count));
  Value *result = count == 1 ? nullptr : UndefValue::get(words->getType());
  for (unsigned i = 0; i < count; ++i) {
    auto *part = count == 1 ? words : builder.CreateExtractElement(words, builder.getInt32(i));
    auto *shuffled = builder.CreateCall(fn, {part, builder.getInt16(lane)});
    result = count == 1 ? shuffled : builder.CreateInsertElement(result, shuffled, builder.getInt32(i));
  }
  if (type->isIntegerTy(1)) return builder.CreateTrunc(result, type);
  return result->getType() == type ? result : builder.CreateBitCast(result, type);
}

void conditionalStore(IRBuilder<> &builder, Function &entry, Value *predicate, Value *pointer, Value *value) {
  auto *store = BasicBlock::Create(entry.getContext(), "capture_record", &entry);
  auto *join = BasicBlock::Create(entry.getContext(), "next_capture", &entry);
  builder.CreateCondBr(predicate, store, join);
  builder.SetInsertPoint(store);
  builder.CreateAlignedStore(value, pointer, Align(4));
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
}

Value *postMask(IRBuilder<> &builder, Module &module, Value *texture, Value *position, unsigned sampleCount) {
  Value *xy = UndefValue::get(FixedVectorType::get(builder.getInt32Ty(), 2));
  for (unsigned i = 0; i < 2; ++i)
    xy = builder.CreateInsertElement(xy, builder.CreateFPToUI(
      builder.CreateExtractElement(position, builder.getInt32(i)), builder.getInt32Ty()), builder.getInt32(i));
  Value *mask = builder.getInt32(0);
  auto *sampler = StructType::getTypeByName(module.getContext(), "struct._sampler_t");
  if (!sampler) sampler = StructType::create(module.getContext(), "struct._sampler_t");
  auto samplerGetter = module.getOrInsertFunction("air.get_read_sampler",
    FunctionType::get(sampler->getPointerTo(2), {}, false));
  auto *response = StructType::get(module.getContext(), {FixedVectorType::get(builder.getInt32Ty(), 4), builder.getInt8Ty()});
  if (sampleCount == 1) {
    auto read = module.getOrInsertFunction("air.read_texture_2d.u.v4i32",
      FunctionType::get(response, {texture->getType(), sampler->getPointerTo(2), xy->getType(),
                                   xy->getType(), builder.getInt32Ty(), builder.getInt32Ty()}, false));
    auto *value = builder.CreateCall(read, {texture, builder.CreateCall(samplerGetter), xy,
      Constant::getNullValue(xy->getType()), builder.getInt32(0), builder.getInt32(1)});
    return builder.CreateSelect(builder.CreateICmpNE(builder.CreateExtractElement(
      builder.CreateExtractValue(value, 0), builder.getInt32(0)), builder.getInt32(0)),
      builder.getInt32(1), builder.getInt32(0));
  }
  auto read = module.getOrInsertFunction("air.read_texture_2d_ms.u.v4i32",
    FunctionType::get(response, {texture->getType(), sampler->getPointerTo(2), xy->getType(),
                                 builder.getInt32Ty(), builder.getInt32Ty()}, false));
  for (unsigned sample = 0; sample < sampleCount; ++sample) {
    auto *value = builder.CreateCall(read, {texture, builder.CreateCall(samplerGetter),
                                             xy, builder.getInt32(sample), builder.getInt32(1)});
    auto *covered = builder.CreateICmpNE(builder.CreateExtractElement(builder.CreateExtractValue(value, 0),
                                             builder.getInt32(0)), builder.getInt32(0));
    mask = builder.CreateOr(mask, builder.CreateSelect(covered, builder.getInt32(1u << sample), builder.getInt32(0)));
  }
  return mask;
}

Value *clampDepth(IRBuilder<> &builder, Module &module, Value *control, Value *depth) {
  auto *floatControl = builder.CreateBitCast(control,
    PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::constant)));
  auto *minDepth = builder.CreateLoad(builder.getFloatTy(),
    builder.CreateConstGEP1_32(builder.getFloatTy(), floatControl, 4));
  auto *maxDepth = builder.CreateLoad(builder.getFloatTy(),
    builder.CreateConstGEP1_32(builder.getFloatTy(), floatControl, 5));
  auto binary = FunctionType::get(builder.getFloatTy(), {builder.getFloatTy(), builder.getFloatTy()}, false);
  auto max = module.getOrInsertFunction("air.fmax.f32", binary);
  auto min = module.getOrInsertFunction("air.fmin.f32", binary);
  return builder.CreateCall(min, {builder.CreateCall(max, {depth, minDepth}), maxDepth});
}

} // namespace

Error maskSampleFrequency(Module &module, uint32_t sampleMask) {
  if (sampleMask == 0xffffffffu) return Error::success();
  auto *fragments = module.getNamedMetadata("air.fragment");
  if (!fragments || fragments->getNumOperands() != 1)
    return invalid("sample-frequency shader has no native fragment entry");
  auto *signature = fragments->getOperand(0);
  auto *entry = dyn_cast<Function>(cast<ValueAsMetadata>(signature->getOperand(0))->getValue());
  auto *inputs = dyn_cast<MDTuple>(signature->getOperand(2));
  if (!entry || !inputs || inputs->getNumOperands() != entry->arg_size())
    return invalid("sample-frequency shader has an invalid native signature");
  unsigned sampleArg = ~0u;
  for (unsigned i = 0; i < inputs->getNumOperands(); ++i)
    if (kind(*cast<MDNode>(inputs->getOperand(i))) == "air.sample_id") {
      sampleArg = i;
      break;
    }
  if (sampleArg == ~0u) {
    // Sample-qualified interpolation can make a shader sample-frequency
    // without an explicit SV_SampleIndex argument. Add the native builtin
    // to a wrapper so the mask can demote unselected samples first.
    SmallVector<Type *, 16> parameters(entry->getFunctionType()->param_begin(),
                                        entry->getFunctionType()->param_end());
    sampleArg = parameters.size();
    parameters.push_back(Type::getInt32Ty(module.getContext()));
    auto name = entry->getName().str();
    entry->setName(name + ".masked_body");
    auto *wrapper = Function::Create(FunctionType::get(entry->getReturnType(), parameters, false),
      Function::ExternalLinkage, name, module);
    wrapper->setCallingConv(entry->getCallingConv());
    auto *start = BasicBlock::Create(module.getContext(), "entry", wrapper);
    IRBuilder<> call(start);
    SmallVector<Value *, 16> args;
    for (unsigned i = 0; i < entry->arg_size(); ++i) args.push_back(wrapper->getArg(i));
    auto *result = call.CreateCall(entry, args);
    if (entry->getReturnType()->isVoidTy()) call.CreateRetVoid();
    else call.CreateRet(result);
    entry->setLinkage(Function::InternalLinkage);
    SmallVector<Metadata *, 16> fields(signature->op_begin(), signature->op_end());
    fields[0] = ValueAsMetadata::get(wrapper);
    SmallVector<Metadata *, 16> attributes(inputs->op_begin(), inputs->op_end());
    attributes.push_back(builtin(sampleArg, "air.sample_id", "uint", "mtl_sample_index",
      module.getContext()));
    fields[2] = MDTuple::get(module.getContext(), attributes);
    fragments->setOperand(0, MDTuple::get(module.getContext(), fields));
    entry = wrapper;
  }

  auto &first = entry->getEntryBlock();
  auto *body = first.splitBasicBlock(first.getFirstInsertionPt(), "masked_sample_body");
  first.getTerminator()->eraseFromParent();
  IRBuilder<> builder(&first);
  auto *allowed = builder.CreateICmpNE(builder.CreateAnd(
    builder.CreateShl(builder.getInt32(1), entry->getArg(sampleArg)),
    builder.getInt32(sampleMask)), builder.getInt32(0));
  auto *masked = BasicBlock::Create(module.getContext(), "masked_sample", entry, body);
  builder.CreateCondBr(allowed, body, masked);
  builder.SetInsertPoint(masked);
  builder.CreateCall(module.getOrInsertFunction("air.discard_fragment",
    FunctionType::get(builder.getVoidTy(), {}, false)));
  // A PSO sample-mask bit is uniform across pixels: every lane for this
  // sample index is masked, so no surviving same-sample quad needs its
  // derivatives. Do not execute the application body after demotion: Metal
  // may still issue its UAV atomics for a discarded lane.
  if (entry->getReturnType()->isVoidTy()) builder.CreateRetVoid();
  else builder.CreateRet(Constant::getNullValue(entry->getReturnType()));
  return Error::success();
}

Error transformPixel(Module &module, const SM50_SHADER_D3D12_MSAA_PIXEL_DATA &options) {
  if (options.sample_count != 1 && options.sample_count != 2 &&
      options.sample_count != 4 && options.sample_count != 8)
    return invalid("sample count must be 1, 2, 4 or 8");
  if (options.variant < SM50_MSAA_COVERAGE ||
      (options.variant > SM50_MSAA_REPLAY && options.variant != SM50_MSAA_SAMPLE_APP))
    return invalid("invalid fragment variant");
  auto *fragments = module.getNamedMetadata("air.fragment");
  if (!fragments || fragments->getNumOperands() != 1)
    return invalid("expected one lowered native fragment entry");
  auto *oldEntry = fragments->getOperand(0);
  auto *original = dyn_cast<Function>(cast<ValueAsMetadata>(oldEntry->getOperand(0))->getValue());
  auto *oldInputs = dyn_cast<MDTuple>(oldEntry->getOperand(2));
  auto *oldOutputs = dyn_cast<MDTuple>(oldEntry->getOperand(1));
  if (!original || !oldInputs || !oldOutputs || oldInputs->getNumOperands() != original->arg_size())
    return invalid("inconsistent AIR fragment signature");
  if (options.center_pattern)
    for (auto &instruction : make_early_inc_range(instructions(*original))) {
      auto *call = dyn_cast<CallInst>(&instruction);
      if (!call || !call->getMetadata("dxmt.rt_sample_pos") ||
          !call->getCalledFunction() ||
          call->getCalledFunction()->getName() != "dxmt.sample_pos.i32") continue;
      call->replaceAllUsesWith(Constant::getNullValue(call->getType()));
      call->eraseFromParent();
    }
  if (options.interpolation_out && options.variant == SM50_MSAA_COVERAGE) {
    auto &interpolation = *options.interpolation_out;
    interpolation = {};
    for (auto &operand : oldInputs->operands()) {
      auto *input = cast<MDNode>(operand);
      if (kind(*input) != "air.fragment_input" || input->getNumOperands() < 3)
        continue;
      auto *semantic = dyn_cast<MDString>(input->getOperand(2));
      if (!semantic || !semantic->getString().startswith("user(")) continue;
      bool noPerspective = false;
      bool flat = false;
      for (unsigned j = 3; j < input->getNumOperands(); ++j)
        if (auto *tag = dyn_cast<MDString>(input->getOperand(j));
            tag && tag->getString() == "air.no_perspective") noPerspective = true;
      for (unsigned j = 3; j < input->getNumOperands(); ++j)
        if (auto *tag = dyn_cast<MDString>(input->getOperand(j));
            tag && tag->getString() == "air.flat") flat = true;
      if (!noPerspective && !flat) continue;
      uint64_t hash = 14695981039346656037ull;
      for (char byte : semantic->getString()) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
      }
      if (noPerspective) {
        if (interpolation.no_perspective_count == 64)
          return invalid("too many no-perspective line varyings");
        interpolation.no_perspective_user_hashes[interpolation.no_perspective_count++] = hash;
      }
      if (flat) {
        if (interpolation.flat_count == 64)
          return invalid("too many flat line varyings");
        interpolation.flat_user_hashes[interpolation.flat_count++] = hash;
      }
    }
  }
  auto &context = module.getContext();
  const auto &layout = module.getDataLayout();
  unsigned positionOld = ~0u, sampleOld = ~0u, coverageOld = ~0u, primitiveOld = ~0u;
  bool centroidPosition = false;
  bool samplePosition = false;
  SmallVector<bool, 24> centroidInput(original->arg_size(), false);
  SmallVector<unsigned, 24> offsets(original->arg_size(), ~0u);
  unsigned inputWords = 2;
  unsigned flags = 0;
  for (unsigned i = 0; i < original->arg_size(); ++i) {
    auto *md = cast<MDNode>(oldInputs->getOperand(i));
    auto type = kind(*md);
    if (type == "air.position") {
      positionOld = i;
      for (unsigned j = 2; j < md->getNumOperands(); ++j)
        if (auto *name = dyn_cast<MDString>(md->getOperand(j)); name) {
          if (name->getString() == "air.centroid") centroidPosition = true;
          if (name->getString() == "air.sample") {
            samplePosition = true;
            flags |= SM50_MSAA_PIXEL_SAMPLE_FREQUENCY;
          }
        }
    }
    if (type == "air.sample_id") { sampleOld = i; flags |= SM50_MSAA_PIXEL_SAMPLE_FREQUENCY; }
    if (type == "air.sample_mask_in") coverageOld = i;
    if (type == "air.primitive_id") primitiveOld = i;
    if (type == "air.fragment_input") {
      bool distance = false;
      if (options.distance_vertex_count && md->getNumOperands() >= 3)
        if (auto *name = dyn_cast<MDString>(md->getOperand(2)))
          distance = distanceSemantic(name->getString());
      for (unsigned j = 2; j < md->getNumOperands(); ++j)
        if (auto *name = dyn_cast<MDString>(md->getOperand(j)); name) {
          if (name->getString() == "air.sample") flags |= SM50_MSAA_PIXEL_SAMPLE_FREQUENCY;
          if (name->getString() == "air.centroid") centroidInput[i] = true;
        }
      if (original->getArg(i)->getType()->isPointerTy())
        flags |= SM50_MSAA_PIXEL_PULL_INPUT;
      else if (centroidInput[i] && !distance)
        return invalid("MSAA centroid input must use native pull interpolation");
    }
    if (!resourceInput(type) && type != "air.primitive_id" &&
        !original->getArg(i)->getType()->isPointerTy()) {
      offsets[i] = inputWords;
      inputWords += dwords(original->getArg(i)->getType(), layout);
    }
  }
  for (unsigned i = 3; i < oldEntry->getNumOperands(); ++i)
    if (auto *tag = dyn_cast<MDString>(oldEntry->getOperand(i)); tag && tag->getString() == "early_fragment_tests")
      flags |= SM50_MSAA_PIXEL_FORCE_EARLY;
  for (auto &item : oldOutputs->operands()) {
    auto type = outputKind(*cast<MDNode>(item));
    if (type == "air.depth") flags |= SM50_MSAA_PIXEL_DEPTH;
    if (type == "air.stencil") flags |= SM50_MSAA_PIXEL_STENCIL;
    if (type == "air.sample_mask") flags |= SM50_MSAA_PIXEL_COVERAGE;
  }
  SmallVector<PulledInput, 8> pulled;
  for (unsigned i = 0; i < original->arg_size(); ++i) {
    auto *md = cast<MDNode>(oldInputs->getOperand(i));
    if (kind(*md) != "air.fragment_input" || !original->getArg(i)->getType()->isPointerTy())
      continue;
    bool perspective = true;
    for (unsigned j = 2; j < md->getNumOperands(); ++j)
      if (auto *name = dyn_cast<MDString>(md->getOperand(j)); name &&
          name->getString() == "air.no_perspective") perspective = false;
    pulled.push_back({.argument = i, .perspective = perspective});
    auto &plan = pulled.back();
    for (auto &instruction : instructions(*original)) {
      auto *call = dyn_cast<CallInst>(&instruction);
      if (!call || call->arg_size() == 0 ||
          call->getArgOperand(0)->stripPointerCasts() != original->getArg(i)) continue;
      auto *callee = call->getCalledFunction();
      if (!callee) return invalid("indirect call on a native pull interpolant");
      auto location = pullLocation(callee->getName());
      if (location == ~0u) return invalid("opaque interpolant escapes native interpolation");
      if (callee->getName().contains("no_perspective") == perspective)
        return invalid("native pull perspective mode differs from AIR metadata");
      if (plan.offset[location] == ~0u) {
        plan.offset[location] = inputWords;
        unsigned count = location == PullOffset ? 256 : location == PullSample ? options.sample_count :
                         location == PullCentroid ? options.sample_count + 1 : 1;
        inputWords += 4 * count;
        plan.anchor[location] = inputWords;
        inputWords += 8 * count;
      }
    }
  }
  SmallVector<DistanceInput, 8> distanceInputs;
  if (options.distance_vertex_count) {
    auto *vertices = options.distance_vertex_layout;
    if (options.distance_vertex_count > 3 || !vertices || !vertices->record_dwords ||
        vertices->position_offset_dwords == ~0u || vertices->user_count > 64)
      return invalid("distance interpolation requires captured primitive vertex output");
    for (unsigned i = 0; i < original->arg_size(); ++i) {
      auto *md = cast<MDNode>(oldInputs->getOperand(i));
      if (kind(*md) != "air.fragment_input" || md->getNumOperands() < 3) continue;
      auto *semantic = dyn_cast<MDString>(md->getOperand(2));
      if (!semantic || !distanceSemantic(semantic->getString())) continue;
      auto *type = original->getArg(i)->getType();
      if (type != FixedVectorType::get(Type::getFloatTy(context), 4) || offsets[i] == ~0u)
        return invalid("distance input must be a plain float4 varying");
      uint64_t hash = userHash(semantic->getString());
      auto *matched = std::find_if(vertices->users, vertices->users + vertices->user_count,
        [hash](const auto &user) { return user.hash == hash; });
      if (matched == vertices->users + vertices->user_count || matched->size_bytes < 16 ||
          matched->offset_dwords + 4 > vertices->record_dwords ||
          vertices->position_offset_dwords + 4 > vertices->record_dwords)
        return invalid("distance input has no matching captured vertex field");
      bool noPerspective = false, flat = false;
      for (unsigned j = 3; j < md->getNumOperands(); ++j)
        if (auto *tag = dyn_cast<MDString>(md->getOperand(j))) {
          noPerspective |= tag->getString() == "air.no_perspective";
          flat |= tag->getString() == "air.flat";
        }
      unsigned sampleOffset = inputWords;
      inputWords += 4 * options.sample_count;
      distanceInputs.push_back({i, matched->offset_dwords, sampleOffset,
        noPerspective, flat, centroidInput[i]});
    }
    if (distanceInputs.empty())
      return invalid("distance interpolation requested without a distance fragment input");
    flags |= SM50_MSAA_PIXEL_DISTANCE_INPUTS;
  }
  SmallVector<LineInput, 16> lineInputs;
  const bool line = options.line_mode != SM50_MSAA_LINE_NONE;
  if (line) {
    auto *vertices = options.line_vertex_layout;
    if (!vertices || vertices->record_dwords == 0 ||
        vertices->position_offset_dwords == ~0u || vertices->user_count > 64)
      return invalid("line pixel shader requires captured vertex output layout");
    for (unsigned i = 0; i < original->arg_size(); ++i) {
      if (std::any_of(distanceInputs.begin(), distanceInputs.end(),
          [i](const DistanceInput &input) { return input.argument == i; })) continue;
      auto *md = cast<MDNode>(oldInputs->getOperand(i));
      if (kind(*md) != "air.fragment_input" || md->getNumOperands() < 3) continue;
      auto *semantic = dyn_cast<MDString>(md->getOperand(2));
      if (!semantic || !semantic->getString().startswith("user("))
        return invalid("line varying has no user semantic");
      uint64_t hash = userHash(semantic->getString());
      auto *matched = std::find_if(vertices->users, vertices->users + vertices->user_count,
        [hash](const auto &user) { return user.hash == hash; });
      if (matched == vertices->users + vertices->user_count)
        return invalid("line pixel varying has no captured vertex output");
      Type *type = original->getArg(i)->getType()->isPointerTy()
        ? static_cast<Type *>(FixedVectorType::get(Type::getFloatTy(module.getContext()), 4))
        : original->getArg(i)->getType();
      if (layout.getTypeStoreSize(type).getFixedValue() > matched->size_bytes ||
          matched->offset_dwords + dwords(type, layout) > vertices->record_dwords)
        return invalid("line pixel varying exceeds captured vertex output");
      bool noPerspective = false, flat = false;
      for (unsigned j = 3; j < md->getNumOperands(); ++j)
        if (auto *tag = dyn_cast<MDString>(md->getOperand(j))) {
          noPerspective |= tag->getString() == "air.no_perspective";
          flat |= tag->getString() == "air.flat";
        }
      unsigned sampleOffset = ~0u;
      if (offsets[i] != ~0u) {
        sampleOffset = inputWords;
        inputWords += dwords(type, layout) * options.sample_count;
      }
      lineInputs.push_back({i, matched->offset_dwords, sampleOffset, noPerspective, flat});
    }
  }
  const bool positionPull = centroidPosition || samplePosition;
  const unsigned clipPositionOffset = positionPull && !line ? inputWords : ~0u;
  if (positionPull && !line) inputWords += 4 * (options.sample_count + 1);
  const unsigned clipAnchorOffset = positionPull && !line ? inputWords : ~0u;
  if (positionPull && !line) inputWords += 8 * (options.sample_count + 1);
  const unsigned reciprocalWOffset = positionPull || line ? inputWords : ~0u;
  if (positionPull || line) inputWords += options.sample_count + 1;
  const unsigned reciprocalWAnchorOffset = positionPull && !line ? inputWords : ~0u;
  if (positionPull && !line) inputWords += 2 * (options.sample_count + 1);
  const unsigned centerZOffset = inputWords++;
  const unsigned sampleZOffset = inputWords;
  inputWords += options.sample_count;
  const unsigned zClaimOffset = inputWords++;
  const unsigned zAnchorOffset = inputWords;
  inputWords += 2 * (options.sample_count + 1);
  const unsigned lineMaskOffset = options.line_mode == SM50_MSAA_LINE_ALIASED ||
      options.line_mode == SM50_MSAA_LINE_ALPHA_AA ? inputWords++ : ~0u;
  const unsigned lineAlphaOffset = options.line_mode == SM50_MSAA_LINE_ALPHA_AA ? inputWords++ : ~0u;
  unsigned outputWords = 2;
  if (!original->getReturnType()->isVoidTy())
    outputWords += dwords(original->getReturnType(), layout);
  if (centroidPosition) flags |= SM50_MSAA_PIXEL_CENTROID_POSITION;
  if (options.layout_out)
    *options.layout_out = {inputWords, outputWords, 0, 1, 0, 1, flags, 8, 128, 64,
                           centerZOffset, sampleZOffset, zClaimOffset, zAnchorOffset,
                           options.line_mode, lineMaskOffset, lineAlphaOffset};

  const unsigned variant = options.variant;
  SmallVector<Type *, 32> parameters;
  SmallVector<Metadata *, 32> metadata;
  SmallVector<unsigned, 32> originalArgs;
  for (unsigned i = 0; i < original->arg_size(); ++i) {
    auto type = kind(*cast<MDNode>(oldInputs->getOperand(i)));
    if (std::any_of(distanceInputs.begin(), distanceInputs.end(),
        [i](const DistanceInput &input) { return input.argument == i; })) continue;
    if (variant != SM50_MSAA_COVERAGE &&
        !(variant == SM50_MSAA_SAMPLE_APP && type == "air.fragment_input" && !centroidInput[i]) &&
        !((variant == SM50_MSAA_SAMPLE_APP || variant == SM50_MSAA_PIXEL) && resourceInput(type)))
      continue;

    originalArgs.push_back(i);
    parameters.push_back(original->getArg(i)->getType());
    metadata.push_back(indexed(*cast<MDNode>(oldInputs->getOperand(i)), metadata.size(), context));
  }
  auto addBuffer = [&](unsigned binding, bool writable, StringRef name, unsigned addressSpace) {
    unsigned index = parameters.size();
    parameters.push_back(PointerType::get(Type::getInt32Ty(context), addressSpace));
    metadata.push_back(air::BuildBufferArgumentMetadata(index, scratchBinding(binding, writable, name),
                                                        context, layout));
    return index;
  };
  unsigned inputArg = ~0u, outputArg = ~0u;
  inputArg = addBuffer(SM50_BINDING_INDEX_MSAA_PIXEL_INPUTS, variant == SM50_MSAA_COVERAGE,
                       "msaa_input_records", unsigned(air::AddressSpace::device));
  if (variant == SM50_MSAA_PIXEL || variant == SM50_MSAA_REPLAY)
    outputArg = addBuffer(SM50_BINDING_INDEX_MSAA_PIXEL_OUTPUTS, variant == SM50_MSAA_PIXEL,
                          "msaa_output_records", unsigned(air::AddressSpace::device));
  unsigned lineRecordsArg = ~0u, lineIndicesArg = ~0u;
  if ((line || !distanceInputs.empty()) && variant == SM50_MSAA_COVERAGE) {
    lineRecordsArg = addBuffer(SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS, false,
      "msaa_line_original_vertices", unsigned(air::AddressSpace::device));
    lineIndicesArg = addBuffer(24, false, "msaa_line_original_indices",
      unsigned(air::AddressSpace::device));
  }
  unsigned controlArg = parameters.size();
  parameters.push_back(PointerType::get(Type::getInt32Ty(context), unsigned(air::AddressSpace::constant)));
  metadata.push_back(air::BuildBufferArgumentMetadata(controlArg, controlBinding(), context, layout));
  unsigned textureArg = ~0u;
  if (variant != SM50_MSAA_COVERAGE) {
    textureArg = parameters.size();
    StringRef textureName = options.sample_count == 1 ? "struct._texture_2d_t" : "struct._texture_2d_ms_t";
    auto *textureType = StructType::getTypeByName(context, textureName);
    if (!textureType) textureType = StructType::create(context, textureName);
    parameters.push_back(textureType->getPointerTo(1));
    metadata.push_back(textureMetadata(textureArg, context, options.sample_count != 1));
  }
  unsigned positionArg = ~0u, sampleArg = ~0u, coverageArg = ~0u;
  if (variant != SM50_MSAA_COVERAGE || positionOld == ~0u) {
    positionArg = parameters.size();
    parameters.push_back(FixedVectorType::get(Type::getFloatTy(context), 4));
    metadata.push_back(positionBuiltin(positionArg, context));
  } else {
    for (unsigned i = 0; i < originalArgs.size(); ++i)
      if (originalArgs[i] == positionOld) positionArg = i;
  }
  if (variant != SM50_MSAA_PIXEL && variant != SM50_MSAA_COVERAGE) {
    sampleArg = parameters.size();
    parameters.push_back(Type::getInt32Ty(context));
    metadata.push_back(builtin(sampleArg, "air.sample_id", "uint", "mtl_sample_index", context));
  } else if (variant == SM50_MSAA_COVERAGE) {
    for (unsigned i = 0; i < originalArgs.size(); ++i)
      if (originalArgs[i] == sampleOld) sampleArg = i;
  }
  if (variant == SM50_MSAA_COVERAGE && coverageOld == ~0u) {
    coverageArg = parameters.size();
    parameters.push_back(Type::getInt32Ty(context));
    metadata.push_back(builtin(coverageArg, "air.sample_mask_in", "uint", "mtl_sample_mask", context));
  } else if (variant == SM50_MSAA_COVERAGE) {
    for (unsigned i = 0; i < originalArgs.size(); ++i)
      if (originalArgs[i] == coverageOld) coverageArg = i;
  }
  unsigned clipArg = ~0u;
  unsigned reciprocalWArg = ~0u;
  if (variant == SM50_MSAA_COVERAGE && positionPull && !line) {
    clipArg = parameters.size();
    auto *interpolant = StructType::getTypeByName(context, "struct._interpolant_t");
    if (!interpolant) interpolant = StructType::create(context, "struct._interpolant_t");
    parameters.push_back(interpolant->getPointerTo(1));
    metadata.push_back(clipPositionInput(clipArg, context));
    reciprocalWArg = parameters.size();
    parameters.push_back(interpolant->getPointerTo(1));
    metadata.push_back(reciprocalWInput(reciprocalWArg, context));
  }
  unsigned lineStartArg = ~0u, lineEndArg = ~0u, lineIntervalArg = ~0u;
  if (line && variant == SM50_MSAA_COVERAGE) {
    auto addLineInput = [&](StringRef name) {
      unsigned index = parameters.size();
      parameters.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
      metadata.push_back(lineEndpointInput(index, name, context));
      return index;
    };
    lineStartArg = addLineInput("msaa_line_start");
    lineEndArg = addLineInput("msaa_line_end");
    lineIntervalArg = addLineInput("msaa_line_clip_interval");
  }
  const bool defaultDepth = options.depth_attachment &&
                            (variant == SM50_MSAA_REPLAY || variant == SM50_MSAA_SAMPLE_APP) &&
                            !(flags & SM50_MSAA_PIXEL_DEPTH);
  auto *returnType = variant == SM50_MSAA_COVERAGE
    ? static_cast<Type *>(StructType::get(context, {Type::getInt32Ty(context), Type::getInt32Ty(context)}, true))
    : variant == SM50_MSAA_PIXEL ? Type::getVoidTy(context) : original->getReturnType();
  if (defaultDepth) {
    SmallVector<Type *, 16> fields;
    if (!returnType->isVoidTy())
      for (auto *field : cast<StructType>(returnType)->elements()) fields.push_back(field);
    fields.push_back(Type::getFloatTy(context));
    returnType = StructType::get(context, fields, true);
  }
  auto originalName = original->getName().str();
  original->setName(originalName + ".msaa_body");

  auto *entry = Function::Create(FunctionType::get(returnType, parameters, false),
                                 Function::ExternalLinkage, originalName, module);
  entry->setCallingConv(original->getCallingConv());
  auto *start = BasicBlock::Create(context, "entry", entry);
  IRBuilder<> builder(start);
  auto *position = entry->getArg(positionArg);
  auto *control = entry->getArg(controlArg);
  auto *tile = inTile(builder, position, control);
  auto *valid = BasicBlock::Create(context, "valid_tile_pixel", entry);
  auto *done = BasicBlock::Create(context, "outside_tile", entry);
  builder.CreateCondBr(tile, valid, done);
  builder.SetInsertPoint(done);
  if (variant == SM50_MSAA_PIXEL) {
    auto discard = module.getOrInsertFunction("air.discard_fragment", FunctionType::get(builder.getVoidTy(), {}, false));
    builder.CreateCall(discard);
    builder.CreateRetVoid();
  } else if (variant == SM50_MSAA_COVERAGE)
    builder.CreateRet(ConstantAggregateZero::get(returnType));
  else if (returnType->isVoidTy())
    builder.CreateRetVoid();
  else
    builder.CreateRet(Constant::getNullValue(returnType));
  builder.SetInsertPoint(valid);
  auto *pixel = pixelIndex(builder, position, control);
  auto lineAlpha = [&](Value *value) -> Value * {
    if (lineAlphaOffset == ~0u || !value || value->getType()->isVoidTy()) return value;
    auto *word = recordPointer(builder, entry->getArg(inputArg), pixel, inputWords, lineAlphaOffset);
    auto *alpha = builder.CreateLoad(builder.getFloatTy(), builder.CreateBitCast(word,
      PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device))));
    for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i) {
      auto *output = cast<MDNode>(oldOutputs->getOperand(i));
      if (outputKind(*output) != "air.render_target" || output->getNumOperands() < 2)
        continue;
      auto *location = dyn_cast<ConstantAsMetadata>(output->getOperand(1));
      if (!location || cast<ConstantInt>(location->getValue())->getZExtValue() != 0) continue;
      auto *color = builder.CreateExtractValue(value, {i});
      auto *type = dyn_cast<FixedVectorType>(color->getType());
      if (!type || type->getNumElements() < 4 || !type->getElementType()->isFloatingPointTy())
        continue;
      auto *factor = builder.CreateFPCast(alpha, type->getElementType());
      auto *component = builder.CreateExtractElement(color, builder.getInt32(3));
      color = builder.CreateInsertElement(color, builder.CreateFMul(component, factor), builder.getInt32(3));
      value = builder.CreateInsertValue(value, color, {i});
      break;
    }
    return value;
  };
  if (variant == SM50_MSAA_COVERAGE) {
    auto *scratch = entry->getArg(inputArg);
    Value *geometricCoverage = entry->getArg(coverageArg);
    if (lineMaskOffset != ~0u)
      geometricCoverage = builder.CreateLoad(builder.getInt32Ty(),
        recordPointer(builder, scratch, pixel, inputWords, lineMaskOffset));
    auto *claim = recordPointer(builder, scratch, pixel, inputWords, 0);
    auto *won = builder.CreateAtomicCmpXchg(claim, builder.getInt32(0), builder.getInt32(1),
      MaybeAlign(), AtomicOrdering::Monotonic, AtomicOrdering::Monotonic);
    auto *realWon = builder.CreateICmpEQ(builder.CreateExtractValue(won, 0), builder.getInt32(0));
    SmallVector<Value *, 4> helperPixels;
    SmallVector<Value *, 4> helperWon;
    for (unsigned lane = 0; lane < 4; ++lane) {
      auto *sourcePosition = quadShuffle(builder, module, position, lane);
      auto *sourcePixel = builder.CreateAdd(pixelIndex(builder, sourcePosition, control), builder.getInt32(64));
      auto *helperClaim = recordPointer(builder, scratch, sourcePixel, inputWords, 0);
      auto *helperTicket = builder.CreateAtomicCmpXchg(helperClaim, builder.getInt32(0),
        builder.getInt32(1), MaybeAlign(), AtomicOrdering::Monotonic, AtomicOrdering::Monotonic);
      helperPixels.push_back(sourcePixel);
      helperWon.push_back(builder.CreateICmpEQ(builder.CreateExtractValue(helperTicket, 0), builder.getInt32(0)));
    }
    // The native rasterizer may invoke a fragment multiple times for one
    // primitive pixel, reporting disjoint sample bits to each invocation.
    // Input interpolants use the first claimant, but D3D centroid selection
    // requires the complete geometric union after this pass finishes.
    builder.CreateAtomicRMW(AtomicRMWInst::Or,
      recordPointer(builder, scratch, pixel, inputWords, 1), geometricCoverage,
      MaybeAlign(), AtomicOrdering::Monotonic);
    for (unsigned lane = 0; lane < 4; ++lane)
      builder.CreateAtomicRMW(AtomicRMWInst::Or,
        recordPointer(builder, scratch, helperPixels[lane], inputWords, 1),
        quadShuffle(builder, module, geometricCoverage, lane),
        MaybeAlign(), AtomicOrdering::Monotonic);
    Value *lineA = nullptr, *lineZ = nullptr, *lineW0 = nullptr, *lineW1 = nullptr;
    if (line) {
      auto *slot = builder.CreateLoad(builder.getInt32Ty(),
        builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 7));
      auto *base = builder.CreateMul(builder.CreateZExt(slot, builder.getInt64Ty()), builder.getInt64(20));
      auto *indices = entry->getArg(lineIndicesArg);
      auto *records = entry->getArg(lineRecordsArg);
      auto record = [&](unsigned ordinalOffset) {
        Value *ordinal = builder.CreateLoad(builder.getInt32Ty(), builder.CreateGEP(
          builder.getInt32Ty(), indices, builder.CreateAdd(base, builder.getInt64(ordinalOffset))));
        return builder.CreateGEP(builder.getInt32Ty(), records, builder.CreateMul(
          builder.CreateZExt(ordinal, builder.getInt64Ty()),
          builder.getInt64(options.line_vertex_layout->record_dwords)));
      };
      lineA = record(18); lineZ = record(19);
      auto capturedW = [&](Value *record) {
        auto *word = builder.CreateConstGEP1_32(builder.getInt32Ty(), record,
          options.line_vertex_layout->position_offset_dwords + 3);
        return builder.CreateLoad(builder.getFloatTy(), builder.CreateBitCast(word,
          PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device))));
      };
      lineW0 = capturedW(lineA); lineW1 = capturedW(lineZ);
    }
    auto lineInputValue = [&](const LineInput &input, Type *type, Value *xy) {
      auto captured = [&](Value *record) {
        auto *word = builder.CreateConstGEP1_32(builder.getInt32Ty(), record, input.vertex_offset);
        return builder.CreateAlignedLoad(type, builder.CreateBitCast(word,
          PointerType::get(type, unsigned(air::AddressSpace::device))), Align(4));
      };
      return lineValue(builder, captured(lineA), captured(lineZ),
        entry->getArg(lineStartArg), entry->getArg(lineEndArg),
        entry->getArg(lineIntervalArg), xy, lineW0, lineW1,
        input.no_perspective, input.flat);
    };
    SmallVector<Value *, 3> distanceRecords;
    SmallVector<std::array<Value *, 3>, 3> distanceVertices;
    if (!distanceInputs.empty()) {
      auto *vertices = options.distance_vertex_layout;
      if (line) {
        if (options.distance_vertex_count != 2 ||
            options.line_vertex_layout->record_dwords != vertices->record_dwords)
          return invalid("line distance capture layout differs from replay layout");
        distanceRecords.append({lineA, lineZ});
      } else {
        auto *slot = builder.CreateLoad(builder.getInt32Ty(),
          builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 7));
        auto *base = builder.CreateMul(builder.CreateZExt(slot, builder.getInt64Ty()), builder.getInt64(3));
        auto *indices = entry->getArg(lineIndicesArg);
        auto *records = entry->getArg(lineRecordsArg);
        for (unsigned i = 0; i < options.distance_vertex_count; ++i) {
          Value *ordinal = builder.CreateLoad(builder.getInt32Ty(), builder.CreateGEP(
            builder.getInt32Ty(), indices, builder.CreateAdd(base, builder.getInt64(i))));
          distanceRecords.push_back(builder.CreateGEP(builder.getInt32Ty(), records, builder.CreateMul(
            builder.CreateZExt(ordinal, builder.getInt64Ty()), builder.getInt64(vertices->record_dwords))));
        }
      }
      if (!line && options.distance_vertex_count > 1) {
        auto *viewport = builder.CreateBitCast(control,
          PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::constant)));
        auto *originX = builder.CreateLoad(builder.getFloatTy(), builder.CreateConstGEP1_32(
          builder.getFloatTy(), viewport, 8));
        auto *originY = builder.CreateLoad(builder.getFloatTy(), builder.CreateConstGEP1_32(
          builder.getFloatTy(), viewport, 9));
        auto *width = builder.CreateLoad(builder.getFloatTy(), builder.CreateConstGEP1_32(
          builder.getFloatTy(), viewport, 10));
        auto *height = builder.CreateLoad(builder.getFloatTy(), builder.CreateConstGEP1_32(
          builder.getFloatTy(), viewport, 11));
        for (auto *record : distanceRecords) {
          auto *positionWord = builder.CreateConstGEP1_32(builder.getInt32Ty(), record,
            vertices->position_offset_dwords);
          auto *clip = builder.CreateAlignedLoad(FixedVectorType::get(builder.getFloatTy(), 4),
            builder.CreateBitCast(positionWord, PointerType::get(FixedVectorType::get(builder.getFloatTy(), 4),
              unsigned(air::AddressSpace::device))), Align(4));
          distanceVertices.push_back(distanceScreenVertex(builder, clip, originX, originY, width, height));
        }
      }
    }
    auto distanceInputValue = [&](const DistanceInput &input, Value *xy) {
      SmallVector<Value *, 3> values;
      for (auto *record : distanceRecords) {
        auto *word = builder.CreateConstGEP1_32(builder.getInt32Ty(), record, input.vertex_offset);
        values.push_back(builder.CreateAlignedLoad(FixedVectorType::get(builder.getFloatTy(), 4),
          builder.CreateBitCast(word, PointerType::get(FixedVectorType::get(builder.getFloatTy(), 4),
            unsigned(air::AddressSpace::device))), Align(4)));
      }
      if (line)
        return lineValue(builder, values[0], values[1], entry->getArg(lineStartArg),
          entry->getArg(lineEndArg), entry->getArg(lineIntervalArg), xy, lineW0, lineW1,
          input.no_perspective, input.flat);
      return distanceInterpolate(builder, values, distanceVertices, xy,
        input.no_perspective, input.flat);
    };
    if (positionPull && !line) {
      auto *clip = entry->getArg(clipArg);
      auto *clipType = FixedVectorType::get(builder.getFloatTy(), 4);
      auto center = module.getOrInsertFunction("air.interpolate_center_perspective.v4f32",
        FunctionType::get(clipType, {clip->getType()}, false));
      auto sample = module.getOrInsertFunction("air.interpolate_sample_perspective.v4f32",
        FunctionType::get(clipType, {clip->getType(), builder.getInt32Ty()}, false));
      auto dx = module.getOrInsertFunction("air.dfdx.v4f32",
        FunctionType::get(clipType, {clipType}, false));
      auto dy = module.getOrInsertFunction("air.dfdy.v4f32",
        FunctionType::get(clipType, {clipType}, false));
      for (unsigned location = 0; location <= options.sample_count; ++location) {
        auto *value = location ? builder.CreateCall(sample, {clip, builder.getInt32(location - 1)}) :
                                 builder.CreateCall(center, {clip});
        auto *gradientX = builder.CreateCall(dx, {value});
        auto *gradientY = builder.CreateCall(dy, {value});
        auto *anchor = builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords,
          clipAnchorOffset + location * 8),
          PointerType::get(clipType, unsigned(air::AddressSpace::device)));
        conditionalStore(builder, *entry, realWon, anchor, gradientX);
        conditionalStore(builder, *entry, realWon, builder.CreateGEP(clipType, anchor,
          builder.getInt32(1)), gradientY);
        for (unsigned lane = 0; lane < 4; ++lane) {
          auto *neighbor = quadShuffle(builder, module, value, lane);
          auto *helper = builder.CreateBitCast(recordPointer(builder, scratch, helperPixels[lane],
            inputWords, clipPositionOffset + location * 4),
            PointerType::get(clipType, unsigned(air::AddressSpace::device)));
          conditionalStore(builder, *entry, helperWon[lane], helper, neighbor);
        }
        auto *pointer = builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords,
          clipPositionOffset + location * 4),
          PointerType::get(clipType, unsigned(air::AddressSpace::device)));
        conditionalStore(builder, *entry, realWon, pointer, value);

        auto *reciprocalW = entry->getArg(reciprocalWArg);
        auto reciprocalCenter = module.getOrInsertFunction("air.interpolate_center_no_perspective.f32",
          FunctionType::get(builder.getFloatTy(), {reciprocalW->getType()}, false));
        auto reciprocalSample = module.getOrInsertFunction("air.interpolate_sample_no_perspective.f32",
          FunctionType::get(builder.getFloatTy(), {reciprocalW->getType(), builder.getInt32Ty()}, false));
        auto *q = location ? builder.CreateCall(reciprocalSample,
          {reciprocalW, builder.getInt32(location - 1)}) :
          builder.CreateCall(reciprocalCenter, {reciprocalW});
        auto scalarDx = module.getOrInsertFunction("air.dfdx.f32",
          FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));
        auto scalarDy = module.getOrInsertFunction("air.dfdy.f32",
          FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));
        auto *qGradientX = builder.CreateCall(scalarDx, {q});
        auto *qGradientY = builder.CreateCall(scalarDy, {q});
        auto *qType = PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device));
        conditionalStore(builder, *entry, realWon,
          builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords,
            reciprocalWAnchorOffset + location * 2), qType),
          qGradientX);
        conditionalStore(builder, *entry, realWon,
          builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords,
            reciprocalWAnchorOffset + location * 2 + 1), qType),
          qGradientY);
        for (unsigned lane = 0; lane < 4; ++lane)
          conditionalStore(builder, *entry, helperWon[lane],
            builder.CreateBitCast(recordPointer(builder, scratch, helperPixels[lane],
              inputWords, reciprocalWOffset + location), qType),
            quadShuffle(builder, module, q, lane));
        conditionalStore(builder, *entry, realWon,
          builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords,
            reciprocalWOffset + location), qType), q);
      }
    }
    if (line) {
      Value *lo = builder.CreateExtractElement(entry->getArg(lineIntervalArg), builder.getInt32(0));
      Value *hi = builder.CreateExtractElement(entry->getArg(lineIntervalArg), builder.getInt32(1));
      Value *fromW = builder.CreateFAdd(lineW0, builder.CreateFMul(
        builder.CreateFSub(lineW1, lineW0), lo));
      Value *toW = builder.CreateFAdd(lineW0, builder.CreateFMul(
        builder.CreateFSub(lineW1, lineW0), hi));
      for (unsigned location = 0; location <= options.sample_count; ++location) {
        Value *xy = location ? lineSampleXY(builder, position, builder.getInt32(location - 1),
          options.sample_count, options.center_pattern) : lineCenterXY(builder, position);
        Value *q = lineFraction(builder, entry->getArg(lineStartArg),
          entry->getArg(lineEndArg), xy);
        Value *reciprocal = builder.CreateFAdd(builder.CreateFDiv(
          builder.CreateFSub(ConstantFP::get(builder.getFloatTy(), 1), q), fromW),
          builder.CreateFDiv(q, toW));
        auto *pointer = PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device));
        for (unsigned lane = 0; lane < 4; ++lane)
          conditionalStore(builder, *entry, helperWon[lane],
            builder.CreateBitCast(recordPointer(builder, scratch, helperPixels[lane],
              inputWords, reciprocalWOffset + location), pointer),
            quadShuffle(builder, module, reciprocal, lane));
        conditionalStore(builder, *entry, realWon, builder.CreateBitCast(
          recordPointer(builder, scratch, pixel, inputWords, reciprocalWOffset + location),
          pointer), reciprocal);
      }
    }
    for (unsigned i = 0; i < originalArgs.size(); ++i) {
      unsigned old = originalArgs[i];
      if (offsets[old] == ~0u) continue;
      auto *type = original->getArg(old)->getType();
      auto lineInput = std::find_if(lineInputs.begin(), lineInputs.end(),
        [old](const LineInput &input) { return input.argument == old; });
      Value *center = lineInput == lineInputs.end() ? entry->getArg(i) :
        lineInputValue(*lineInput, type, lineCenterXY(builder, position));
      for (unsigned lane = 0; lane < 4; ++lane) {
        auto *value = quadShuffle(builder, module, center, lane);
        auto *pointer = builder.CreateBitCast(recordPointer(builder, scratch, helperPixels[lane], inputWords, offsets[old]),
                           PointerType::get(type, unsigned(air::AddressSpace::device)));
        conditionalStore(builder, *entry, helperWon[lane], pointer, value);
      }
      auto *pointer = builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords, offsets[old]),
                         PointerType::get(type, unsigned(air::AddressSpace::device)));
      conditionalStore(builder, *entry, realWon, pointer, center);
      if (lineInput != lineInputs.end())
        for (unsigned sample = 0; sample < options.sample_count; ++sample) {
          Value *value = lineInputValue(*lineInput, type, lineSampleXY(builder, position,
            builder.getInt32(sample), options.sample_count, options.center_pattern));
          unsigned offset = lineInput->sample_offset + sample * dwords(type, layout);
          for (unsigned lane = 0; lane < 4; ++lane) {
            auto *helper = builder.CreateBitCast(recordPointer(builder, scratch,
              helperPixels[lane], inputWords, offset),
              PointerType::get(type, unsigned(air::AddressSpace::device)));
            conditionalStore(builder, *entry, helperWon[lane], helper,
              quadShuffle(builder, module, value, lane));
          }
          auto *samplePointer = builder.CreateBitCast(recordPointer(builder, scratch,
            pixel, inputWords, offset),
            PointerType::get(type, unsigned(air::AddressSpace::device)));
          conditionalStore(builder, *entry, realWon, samplePointer, value);
        }
    }

    for (const auto &input : distanceInputs) {
      for (unsigned location = 0; location <= options.sample_count; ++location) {
        Value *xy = location ? lineSampleXY(builder, position, builder.getInt32(location - 1),
          options.sample_count, options.center_pattern) : lineCenterXY(builder, position);
        auto *value = distanceInputValue(input, xy);
        unsigned offset = location ? input.sample_offset + (location - 1) * 4 : offsets[input.argument];
        for (unsigned lane = 0; lane < 4; ++lane) {
          auto *helper = builder.CreateBitCast(recordPointer(builder, scratch,
            helperPixels[lane], inputWords, offset),
            PointerType::get(value->getType(), unsigned(air::AddressSpace::device)));
          conditionalStore(builder, *entry, helperWon[lane], helper,
            quadShuffle(builder, module, value, lane));
        }
        auto *pointer = builder.CreateBitCast(recordPointer(builder, scratch,
          pixel, inputWords, offset),
          PointerType::get(value->getType(), unsigned(air::AddressSpace::device)));
        conditionalStore(builder, *entry, realWon, pointer, value);
      }
    }

    for (const auto &plan : pulled) {
      unsigned argument = ~0u;
      for (unsigned i = 0; i < originalArgs.size(); ++i)
        if (originalArgs[i] == plan.argument) argument = i;
      if (argument == ~0u) return invalid("coverage variant lost its native interpolant");
      auto lineInput = std::find_if(lineInputs.begin(), lineInputs.end(),
        [&plan](const LineInput &input) { return input.argument == plan.argument; });
      for (unsigned location = 0; location < 4; ++location) {
        if (plan.offset[location] == ~0u) continue;
        unsigned count = location == PullOffset ? 256 : location == PullSample ? options.sample_count :
                         location == PullCentroid ? options.sample_count + 1 : 1;
        auto *loop = BasicBlock::Create(context, "capture_interpolation", entry);
        auto *after = BasicBlock::Create(context, "interpolation_captured", entry);
        auto *before = builder.GetInsertBlock();
        builder.CreateBr(loop);
        builder.SetInsertPoint(loop);
        auto *index = builder.CreatePHI(builder.getInt32Ty(), 2);
        index->addIncoming(builder.getInt32(0), before);
        StringRef modes[] = {"center", "centroid", "sample", "offset"};
        std::string symbol = ("air.interpolate_" + modes[location] +
          (plan.perspective ? "_perspective.v4f32" : "_no_perspective.v4f32")).str();
        SmallVector<Type *, 2> types = {entry->getArg(argument)->getType()};
        SmallVector<Value *, 2> args = {entry->getArg(argument)};
        if (location == PullSample) {types.push_back(builder.getInt32Ty()); args.push_back(index);}
        if (location == PullOffset) {
          auto *offset = UndefValue::get(FixedVectorType::get(builder.getFloatTy(), 2));
          auto *x = builder.CreateAnd(index, builder.getInt32(15));
          auto *y = builder.CreateLShr(index, 4);
          Value *xy = builder.CreateInsertElement(offset,
            builder.CreateUIToFP(x, builder.getFloatTy()), builder.getInt32(0));
          xy = builder.CreateInsertElement(xy,
            builder.CreateUIToFP(y, builder.getFloatTy()), builder.getInt32(1));
          xy = builder.CreateFMul(xy, ConstantVector::getSplat(ElementCount::getFixed(2),
                         ConstantFP::get(builder.getFloatTy(), 1.0 / 16.0)));
          types.push_back(xy->getType()); args.push_back(xy);
        }
        auto *pullType = FixedVectorType::get(builder.getFloatTy(), 4);
        Value *value = PoisonValue::get(pullType);
        if (line) {
          if (lineInput == lineInputs.end()) return invalid("line pull input has no captured output");
          Value *xy = lineCenterXY(builder, position);
          if (location == PullSample || location == PullCentroid) {
            Value *sample = location == PullSample ? static_cast<Value *>(index) :
              builder.CreateSub(index, builder.getInt32(1));
            Value *sampleXY = lineSampleXY(builder, position, sample,
              options.sample_count, options.center_pattern);
            xy = location == PullCentroid ? builder.CreateSelect(
              builder.CreateICmpEQ(index, builder.getInt32(0)), xy, sampleXY) : sampleXY;
          } else if (location == PullOffset) {
            Value *offset = args.back();
            Value *corner = UndefValue::get(xy->getType());
            for (unsigned axis = 0; axis < 2; ++axis) {
              Value *component = builder.CreateExtractElement(position, builder.getInt32(axis));
              Value *pixelBase = builder.CreateUIToFP(builder.CreateFPToUI(
                component, builder.getInt32Ty()), builder.getFloatTy());
              corner = builder.CreateInsertElement(corner, builder.CreateFAdd(pixelBase,
                builder.CreateExtractElement(offset, builder.getInt32(axis))), builder.getInt32(axis));
            }
            xy = corner;
          }
          value = lineInputValue(*lineInput, pullType, xy);
        } else if (location != PullCentroid) {
          auto fn = module.getOrInsertFunction(symbol, FunctionType::get(pullType, types, false));
          value = builder.CreateCall(fn, args);
        }
        if (!line && location == PullCentroid) {
          // Capture every finite native value now; choose only after all
          // native invocations have contributed their geometric coverage.
          auto sampleFn = module.getOrInsertFunction(
            plan.perspective ? "air.interpolate_sample_perspective.v4f32" :
                               "air.interpolate_sample_no_perspective.v4f32",
            FunctionType::get(value->getType(),
              {entry->getArg(argument)->getType(), builder.getInt32Ty()}, false));
          auto centerFn = module.getOrInsertFunction(
            plan.perspective ? "air.interpolate_center_perspective.v4f32" :
                               "air.interpolate_center_no_perspective.v4f32",
            FunctionType::get(value->getType(), {entry->getArg(argument)->getType()}, false));
          auto *atCenterBlock = BasicBlock::Create(context, "capture_centroid_center", entry);
          auto *atSampleBlock = BasicBlock::Create(context, "capture_centroid_sample", entry);
          auto *chosenBlock = BasicBlock::Create(context, "centroid_value_captured", entry);
          builder.CreateCondBr(builder.CreateICmpEQ(index, builder.getInt32(0)),
            atCenterBlock, atSampleBlock);
          builder.SetInsertPoint(atCenterBlock);
          auto *atCenter = builder.CreateCall(centerFn, {entry->getArg(argument)});
          builder.CreateBr(chosenBlock);
          builder.SetInsertPoint(atSampleBlock);
          auto *atSample = builder.CreateCall(sampleFn,
            {entry->getArg(argument), builder.CreateSub(index, builder.getInt32(1))});
          builder.CreateBr(chosenBlock);
          builder.SetInsertPoint(chosenBlock);
          auto *chosen = builder.CreatePHI(pullType, 2);
          chosen->addIncoming(atCenter, atCenterBlock);
          chosen->addIncoming(atSample, atSampleBlock);
          value = chosen;
        }
        auto dx = module.getOrInsertFunction("air.dfdx.v4f32", FunctionType::get(pullType, {pullType}, false));
        auto dy = module.getOrInsertFunction("air.dfdy.v4f32", FunctionType::get(pullType, {pullType}, false));
        auto *anchor = builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords,
          plan.anchor[location]), PointerType::get(pullType, unsigned(air::AddressSpace::device)));
        auto *anchorPair = builder.CreateGEP(pullType, anchor, builder.CreateMul(index, builder.getInt32(2)));
        conditionalStore(builder, *entry, realWon, anchorPair, builder.CreateCall(dx, {value}));
        conditionalStore(builder, *entry, realWon, builder.CreateGEP(pullType, anchorPair, builder.getInt32(1)),
                         builder.CreateCall(dy, {value}));
        for (unsigned lane = 0; lane < 4; ++lane) {
          auto *neighbor = quadShuffle(builder, module, value, lane);
          auto *helper = builder.CreateBitCast(recordPointer(builder, scratch, helperPixels[lane],
            inputWords, plan.offset[location]),
            PointerType::get(value->getType(), unsigned(air::AddressSpace::device)));
          conditionalStore(builder, *entry, helperWon[lane],
                           builder.CreateGEP(value->getType(), helper, index), neighbor);
        }
        auto *ptr = builder.CreateBitCast(recordPointer(builder, scratch, pixel, inputWords, plan.offset[location] + 4 * 0),
                        PointerType::get(value->getType(), unsigned(air::AddressSpace::device)));
        ptr = builder.CreateGEP(value->getType(), ptr, index);
        conditionalStore(builder, *entry, realWon, ptr, value);
        auto *next = builder.CreateAdd(index, builder.getInt32(1));
        builder.CreateCondBr(builder.CreateICmpULT(next, builder.getInt32(count)), loop, after);
        index->addIncoming(next, builder.GetInsertBlock());
        builder.SetInsertPoint(after);
      }
    }
    auto *sampleMask = builder.CreateLoad(builder.getInt32Ty(), builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 3));
    auto *covered = builder.CreateAnd(geometricCoverage, sampleMask);
    auto *result = builder.CreateInsertValue(PoisonValue::get(returnType), builder.getInt32(1), {0});
    result = builder.CreateInsertValue(result, covered, {1});
    builder.CreateRet(result);
  } else if (variant == SM50_MSAA_PIXEL) {
    auto *scratch = entry->getArg(inputArg);
    auto *mask = postMask(builder, module, entry->getArg(textureArg), position, options.sample_count);
    auto *covered = builder.CreateICmpNE(mask, builder.getInt32(0));
    auto *shade = BasicBlock::Create(context, "shade_once", entry);
    auto *discard = BasicBlock::Create(context, "no_covered_samples", entry);
    builder.CreateCondBr(covered, shade, discard);
    builder.SetInsertPoint(discard);
    builder.CreateCall(module.getOrInsertFunction("air.discard_fragment", FunctionType::get(builder.getVoidTy(), {}, false)));
    builder.CreateBr(shade);
    builder.SetInsertPoint(shade);
    auto *inputRecord = builder.CreateSelect(covered, pixel, builder.CreateAdd(pixel, builder.getInt32(64)));
    SmallVector<Value *, 32> callArgs(original->arg_size());
    for (unsigned i = 0; i < originalArgs.size(); ++i)
      callArgs[originalArgs[i]] = entry->getArg(i);
    for (unsigned i = 0; i < original->arg_size(); ++i) {
      if (callArgs[i]) continue;
      if (i == coverageOld) {callArgs[i] = mask; continue;}
      if (i == sampleOld) {callArgs[i] = builder.getInt32(0); continue;}
      if (i == primitiveOld) {
        callArgs[i] = builder.CreateLoad(builder.getInt32Ty(),
          builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 6));
        continue;
      }
      if (offsets[i] == ~0u) {
        auto found = std::find_if(pulled.begin(), pulled.end(), [&](const PulledInput &item) {
          return item.argument == i;
        });
        if (found == pulled.end()) return invalid("uncaptured original pixel input");
        callArgs[i] = UndefValue::get(original->getArg(i)->getType());
        continue;
      }
      auto *ptr = builder.CreateBitCast(recordPointer(builder, scratch, inputRecord, inputWords, offsets[i]),
                             PointerType::get(original->getArg(i)->getType(), unsigned(air::AddressSpace::device)));
      callArgs[i] = builder.CreateAlignedLoad(original->getArg(i)->getType(), ptr, Align(4));
      auto distance = std::find_if(distanceInputs.begin(), distanceInputs.end(),
        [i](const DistanceInput &input) { return input.argument == i; });
      if (distance != distanceInputs.end() && distance->centroid) {
        auto *coverage = builder.CreateLoad(builder.getInt32Ty(),
          recordPointer(builder, scratch, inputRecord, inputWords, 1));
        auto *mask = builder.CreateLoad(builder.getInt32Ty(),
          builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 3));
        auto *slot = centroidSlot(builder, coverage, mask, options.sample_count);
        auto *sampleIndex = builder.CreateSelect(builder.CreateICmpEQ(slot, builder.getInt32(0)),
          builder.getInt32(0), builder.CreateSub(slot, builder.getInt32(1)));
        auto *word = recordPointer(builder, scratch, inputRecord, inputWords,
          distance->sample_offset);
        auto *sample = builder.CreateAlignedLoad(original->getArg(i)->getType(),
          builder.CreateBitCast(builder.CreateGEP(builder.getInt32Ty(), word,
            builder.CreateMul(sampleIndex, builder.getInt32(4))),
            PointerType::get(original->getArg(i)->getType(), unsigned(air::AddressSpace::device))), Align(4));
        callArgs[i] = builder.CreateSelect(builder.CreateICmpEQ(slot, builder.getInt32(0)), callArgs[i], sample);
      }
      if (i == positionOld && !centroidPosition) {
        auto *raw = recordPointer(builder, scratch, inputRecord, inputWords, centerZOffset);
        auto *centerZ = builder.CreateLoad(builder.getFloatTy(), builder.CreateBitCast(raw,
          PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device))));
        callArgs[i] = builder.CreateInsertElement(callArgs[i], centerZ, builder.getInt32(2));
        if (line) {
          auto *q = builder.CreateLoad(builder.getFloatTy(), builder.CreateBitCast(
            recordPointer(builder, scratch, inputRecord, inputWords, reciprocalWOffset),
            PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device))));
          callArgs[i] = builder.CreateInsertElement(callArgs[i], q, builder.getInt32(3));
        }
      }
      if (i == positionOld && centroidPosition) {
        auto *preCoverage = builder.CreateLoad(builder.getInt32Ty(),
          recordPointer(builder, scratch, inputRecord, inputWords, 1));
        auto *psoMask = builder.CreateLoad(builder.getInt32Ty(),
          builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 3));
        auto *clipIndex = centroidSlot(builder, preCoverage, psoMask, options.sample_count);
        auto *center = builder.CreateICmpEQ(clipIndex, builder.getInt32(0));
        auto *firstSample = builder.CreateSelect(center, builder.getInt32(0),
          builder.CreateSub(clipIndex, builder.getInt32(1)));
        const int8_t sampleX1[] = {0}, sampleY1[] = {0};
        const int8_t sampleX2[] = {4, -4}, sampleY2[] = {4, -4};
        const int8_t sampleX4[] = {-2, 6, -6, 2}, sampleY4[] = {-6, -2, 2, 6};
        const int8_t sampleX8[] = {1, -1, 5, -3, -5, -7, 3, 7};
        const int8_t sampleY8[] = {-3, 3, 1, -5, 5, -1, 7, -7};
        const int8_t *sampleX = options.sample_count == 1 ? sampleX1 : options.sample_count == 2 ? sampleX2 :
          options.sample_count == 4 ? sampleX4 : sampleX8;
        const int8_t *sampleY = options.sample_count == 1 ? sampleY1 : options.sample_count == 2 ? sampleY2 :
          options.sample_count == 4 ? sampleY4 : sampleY8;
        auto *originalPos = callArgs[i];
        Value *fixedPos = originalPos;
        for (unsigned axis = 0; axis < 2; ++axis) {
          Value *delta = ConstantFP::get(builder.getFloatTy(), 0);
          for (unsigned sample = 0; sample < options.sample_count; ++sample)
            delta = builder.CreateSelect(builder.CreateICmpEQ(firstSample, builder.getInt32(sample)),
              ConstantFP::get(builder.getFloatTy(), options.center_pattern ? 0.0 :
                double(axis ? sampleY[sample] : sampleX[sample]) / 16.0), delta);
          delta = builder.CreateSelect(center, ConstantFP::get(builder.getFloatTy(), 0), delta);
          auto *coordinate = builder.CreateExtractElement(originalPos, builder.getInt32(axis));
          auto *pixelBase = builder.CreateUIToFP(builder.CreateFPToUI(coordinate, builder.getInt32Ty()),
            builder.getFloatTy());
          fixedPos = builder.CreateInsertElement(fixedPos, builder.CreateFAdd(
            builder.CreateFAdd(pixelBase, ConstantFP::get(builder.getFloatTy(), .5)), delta),
            builder.getInt32(axis));
        }
        auto *depthBase = recordPointer(builder, scratch, inputRecord, inputWords, centerZOffset);
        auto *depth = builder.CreateLoad(builder.getFloatTy(), builder.CreateGEP(builder.getFloatTy(),
          builder.CreateBitCast(depthBase, PointerType::get(builder.getFloatTy(),
            unsigned(air::AddressSpace::device))), clipIndex));
        fixedPos = builder.CreateInsertElement(fixedPos, depth, builder.getInt32(2));
        // The original signature prologue reciprocates native air.position.w.
        // Feed screen-linear 1/clipW directly, so SV_Position.w is one
        // reciprocal of the exact native interpolation at the chosen sample.
        auto *qPointer = recordPointer(builder, scratch, inputRecord, inputWords,
          reciprocalWOffset);
        auto *q = builder.CreateLoad(builder.getFloatTy(),
          builder.CreateGEP(builder.getFloatTy(),
            builder.CreateBitCast(qPointer, PointerType::get(builder.getFloatTy(),
              unsigned(air::AddressSpace::device))), clipIndex));
        fixedPos = builder.CreateInsertElement(fixedPos, q, builder.getInt32(3));
        // Keep center X/Y; use gathered Z and original-line reciprocal W
        // when the complete sample mask selects center.
        auto *centerPos = builder.CreateInsertElement(originalPos, depth, builder.getInt32(2));
        if (line) centerPos = builder.CreateInsertElement(centerPos, q, builder.getInt32(3));
        fixedPos = builder.CreateSelect(center, centerPos, fixedPos);
        callArgs[i] = fixedPos;
      }
    }
    Function *body = original;
    if (!pulled.empty()) {
      SmallVector<Type *, 32> bodyTypes(original->getFunctionType()->param_begin(),
                                         original->getFunctionType()->param_end());
      bodyTypes.push_back(PointerType::get(builder.getInt32Ty(), unsigned(air::AddressSpace::device)));
      bodyTypes.push_back(builder.getInt32Ty());
      bodyTypes.push_back(control->getType());
      body = Function::Create(FunctionType::get(original->getReturnType(), bodyTypes, false),
                              Function::InternalLinkage, originalName + ".finite_pull", module);
      body->copyAttributesFrom(original);
      for (unsigned i = 0; i < original->arg_size(); ++i)
        original->getArg(i)->replaceAllUsesWith(body->getArg(i));
      body->getBasicBlockList().splice(body->begin(), original->getBasicBlockList());
      for (auto &instruction : make_early_inc_range(instructions(*body))) {
        auto *call = dyn_cast<CallInst>(&instruction);
        if (!call || !call->getCalledFunction()) continue;
        unsigned location = pullLocation(call->getCalledFunction()->getName());
        if (location == ~0u) continue;
        auto *pointer = call->getArgOperand(0)->stripPointerCasts();
        auto found = std::find_if(pulled.begin(), pulled.end(), [&](const PulledInput &item) {
          return pointer == body->getArg(item.argument);
        });
        if (found == pulled.end() || found->offset[location] == ~0u)
          return invalid("unrecognized finite interpolant source");
        IRBuilder<> load(call);
        auto *index = pullIndex(load, *call, location, options.sample_count,
          body->getArg(original->arg_size()), body->getArg(original->arg_size() + 1),
          body->getArg(original->arg_size() + 2), inputWords);
        auto *base = recordPointer(load, body->getArg(original->arg_size()),
                                    body->getArg(original->arg_size() + 1), inputWords, found->offset[location]);
        auto *array = load.CreateBitCast(base, PointerType::get(call->getType(), unsigned(air::AddressSpace::device)));
        auto *slot = load.CreateGEP(call->getType(), array, index);
        call->replaceAllUsesWith(load.CreateAlignedLoad(call->getType(), slot, Align(4)));
        call->eraseFromParent();
      }
      callArgs.push_back(scratch);
      callArgs.push_back(inputRecord);
      callArgs.push_back(control);
    }
    for (auto &instruction : make_early_inc_range(instructions(*body))) {
      auto *call = dyn_cast<CallInst>(&instruction);
      if (!call || !call->getCalledFunction() ||
          call->getCalledFunction()->getName() != "air.get_num_samples.i32") continue;
      call->replaceAllUsesWith(builder.getInt32(options.sample_count));
      call->eraseFromParent();
    }
    auto *result = builder.CreateCall(body, callArgs);
    auto *out = entry->getArg(outputArg);
    if (!original->getReturnType()->isVoidTy()) {
      auto *ptr = builder.CreateBitCast(recordPointer(builder, out, pixel, outputWords, 2),
                             PointerType::get(original->getReturnType(), unsigned(air::AddressSpace::device)));
      conditionalStore(builder, *entry, covered, ptr, result);
    }
    // The single-sample RTV path counts virtual raster samples separately.
    // Preserve the PS output mask before native 1x output conversion: only
    // output sample zero can enable writes to that RTV.
    Value *outputMask = builder.getInt32(~0u);
    if (oldOutputs && !original->getReturnType()->isVoidTy())
      for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
        if (outputKind(*cast<MDNode>(oldOutputs->getOperand(i))) == "air.sample_mask")
          outputMask = builder.CreateExtractValue(result, {i});
    conditionalStore(builder, *entry, covered, recordPointer(builder, out, pixel, outputWords, 1), outputMask);
    conditionalStore(builder, *entry, covered, recordPointer(builder, out, pixel, outputWords, 0), builder.getInt32(1));
    builder.CreateRetVoid();
  } else {
    auto *mask = postMask(builder, module, entry->getArg(textureArg), position, options.sample_count);
    auto *validPixel = variant == SM50_MSAA_REPLAY && options.single_sample_target
      ? builder.CreateICmpNE(mask, builder.getInt32(0))
      : builder.CreateICmpNE(builder.CreateAnd(mask,
          builder.CreateShl(builder.getInt32(1), entry->getArg(sampleArg))), builder.getInt32(0));
    auto *out = variant == SM50_MSAA_REPLAY ? entry->getArg(outputArg) : nullptr;
    auto *published = variant == SM50_MSAA_REPLAY
      ? builder.CreateICmpNE(builder.CreateLoad(builder.getInt32Ty(),
          recordPointer(builder, out, pixel, outputWords, 0)), builder.getInt32(0))
      : builder.getTrue();
    auto *present = BasicBlock::Create(context, "replay_sample", entry);
    auto *masked = BasicBlock::Create(context, "replay_masked", entry);
    auto *logicalActive = builder.CreateAnd(validPixel, published);
    builder.CreateCondBr(logicalActive, present, masked);
    builder.SetInsertPoint(masked);
    builder.CreateCall(module.getOrInsertFunction("air.discard_fragment", FunctionType::get(builder.getVoidTy(), {}, false)));
    if (variant == SM50_MSAA_SAMPLE_APP) builder.CreateBr(present);
    else if (returnType->isVoidTy()) builder.CreateRetVoid();
    else builder.CreateRet(Constant::getNullValue(returnType));
    builder.SetInsertPoint(present);
    if (variant == SM50_MSAA_SAMPLE_APP) {
      Value *sampleRecord = builder.CreateSelect(validPixel, pixel,
        builder.CreateAdd(pixel, builder.getInt32(64)));
      SmallVector<Value *, 32> callArgs(original->arg_size());
      for (unsigned i = 0; i < originalArgs.size(); ++i)
        callArgs[originalArgs[i]] = entry->getArg(i);
      if (line)
        for (const auto &input : lineInputs) {
          if (input.sample_offset == ~0u) continue;
          Type *type = original->getArg(input.argument)->getType();
          auto *pointer = builder.CreateBitCast(recordPointer(builder,
            entry->getArg(inputArg), sampleRecord, inputWords, input.sample_offset),
            PointerType::get(type, unsigned(air::AddressSpace::device)));
          pointer = builder.CreateGEP(type, pointer, entry->getArg(sampleArg));
          callArgs[input.argument] = builder.CreateAlignedLoad(type, pointer, Align(4));
        }
      for (const auto &input : distanceInputs) {
        Value *slot = entry->getArg(sampleArg);
        if (input.centroid) {
          auto *coverage = builder.CreateLoad(builder.getInt32Ty(),
            recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, 1));
          auto *mask = builder.CreateLoad(builder.getInt32Ty(),
            builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 3));
          slot = centroidSlot(builder, coverage, mask, options.sample_count);
        }
        auto *word = recordPointer(builder, entry->getArg(inputArg), sampleRecord,
          inputWords, input.sample_offset);
        auto *sampleIndex = input.centroid ? builder.CreateSelect(
          builder.CreateICmpEQ(slot, builder.getInt32(0)), builder.getInt32(0),
          builder.CreateSub(slot, builder.getInt32(1))) : slot;
        auto *pointer = builder.CreateBitCast(builder.CreateGEP(builder.getInt32Ty(), word,
          builder.CreateMul(sampleIndex, builder.getInt32(4))),
          PointerType::get(original->getArg(input.argument)->getType(),
            unsigned(air::AddressSpace::device)));
        Value *sampleValue = builder.CreateAlignedLoad(original->getArg(input.argument)->getType(), pointer, Align(4));
        if (input.centroid) {
          auto *center = builder.CreateBitCast(recordPointer(builder, entry->getArg(inputArg),
            sampleRecord, inputWords, offsets[input.argument]),
            PointerType::get(original->getArg(input.argument)->getType(),
              unsigned(air::AddressSpace::device)));
          sampleValue = builder.CreateSelect(builder.CreateICmpEQ(slot, builder.getInt32(0)),
            builder.CreateAlignedLoad(original->getArg(input.argument)->getType(), center, Align(4)), sampleValue);
        }
        callArgs[input.argument] = sampleValue;
      }
      for (unsigned i = 0; i < original->arg_size(); ++i) {
        if (callArgs[i]) continue;
        if (i == sampleOld) { callArgs[i] = entry->getArg(sampleArg); continue; }
        if (i == coverageOld) { callArgs[i] = mask; continue; }
        if (i == primitiveOld) {
          callArgs[i] = builder.CreateLoad(builder.getInt32Ty(),
            builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 6));
          continue;
        }
        if (i == positionOld) {
          Value *slot = builder.getInt32(0);
          Value *firstSample = builder.getInt32(0);
          Value *center = builder.getTrue();
          if (centroidPosition || samplePosition) {
            auto *preCoverage = builder.CreateLoad(builder.getInt32Ty(),
              recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, 1));
            auto *psoMask = builder.CreateLoad(builder.getInt32Ty(),
              builder.CreateConstGEP1_32(builder.getInt32Ty(), control, 3));
            slot = centroidSlot(builder, preCoverage, psoMask, options.sample_count);
            center = builder.CreateICmpEQ(slot, builder.getInt32(0));
            firstSample = builder.CreateSelect(center, builder.getInt32(0),
              builder.CreateSub(slot, builder.getInt32(1)));
            if (samplePosition) {
              center = builder.getFalse();
              firstSample = entry->getArg(sampleArg);
              slot = builder.CreateAdd(firstSample, builder.getInt32(1));
            }
          }
          auto *raw = recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, centerZOffset);
          auto *rawZ = builder.CreateLoad(builder.getFloatTy(), builder.CreateGEP(builder.getFloatTy(),
            builder.CreateBitCast(raw, PointerType::get(builder.getFloatTy(),
              unsigned(air::AddressSpace::device))), slot));
          Value *fixedPos = builder.CreateInsertElement(position, rawZ, builder.getInt32(2));
          if (line) {
            auto *q = builder.CreateLoad(builder.getFloatTy(), builder.CreateBitCast(
              recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, reciprocalWOffset),
              PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device))));
            fixedPos = builder.CreateInsertElement(fixedPos, q, builder.getInt32(3));
          }
          if (centroidPosition || samplePosition) {
            const int8_t sampleX1[] = {0}, sampleY1[] = {0};
            const int8_t sampleX2[] = {4, -4}, sampleY2[] = {4, -4};
            const int8_t sampleX4[] = {-2, 6, -6, 2}, sampleY4[] = {-6, -2, 2, 6};
            const int8_t sampleX8[] = {1, -1, 5, -3, -5, -7, 3, 7};
            const int8_t sampleY8[] = {-3, 3, 1, -5, 5, -1, 7, -7};
            const int8_t *sx = options.sample_count == 1 ? sampleX1 : options.sample_count == 2 ? sampleX2 : options.sample_count == 4 ? sampleX4 : sampleX8;
            const int8_t *sy = options.sample_count == 1 ? sampleY1 : options.sample_count == 2 ? sampleY2 : options.sample_count == 4 ? sampleY4 : sampleY8;
            for (unsigned axis = 0; axis < 2; ++axis) {
              Value *delta = ConstantFP::get(builder.getFloatTy(), 0);
              for (unsigned sample = 0; sample < options.sample_count; ++sample)
                delta = builder.CreateSelect(builder.CreateICmpEQ(firstSample, builder.getInt32(sample)),
                  ConstantFP::get(builder.getFloatTy(), options.center_pattern ? 0.0 :
                    double(axis ? sy[sample] : sx[sample]) / 16.0), delta);
              delta = builder.CreateSelect(center, ConstantFP::get(builder.getFloatTy(), 0), delta);
              auto *coordinate = builder.CreateExtractElement(position, builder.getInt32(axis));
              auto *base = builder.CreateUIToFP(builder.CreateFPToUI(coordinate, builder.getInt32Ty()), builder.getFloatTy());
              fixedPos = builder.CreateInsertElement(fixedPos, builder.CreateFAdd(builder.CreateFAdd(
                base, ConstantFP::get(builder.getFloatTy(), .5)), delta), builder.getInt32(axis));
            }
            auto *qBase = recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, reciprocalWOffset);
            auto *q = builder.CreateLoad(builder.getFloatTy(), builder.CreateGEP(builder.getFloatTy(),
              builder.CreateBitCast(qBase, PointerType::get(builder.getFloatTy(),
                unsigned(air::AddressSpace::device))), slot));
            fixedPos = builder.CreateInsertElement(fixedPos, q, builder.getInt32(3));
            Value *centerPos = builder.CreateInsertElement(position, rawZ, builder.getInt32(2));
            if (line) {
              auto *centerQ = builder.CreateLoad(builder.getFloatTy(), builder.CreateBitCast(
                recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, reciprocalWOffset),
                PointerType::get(builder.getFloatTy(), unsigned(air::AddressSpace::device))));
              centerPos = builder.CreateInsertElement(centerPos, centerQ, builder.getInt32(3));
            }
            fixedPos = builder.CreateSelect(center, centerPos, fixedPos);
          }
          callArgs[i] = fixedPos;
          continue;
        }
        if (original->getArg(i)->getType()->isPointerTy() &&
            std::any_of(pulled.begin(), pulled.end(), [&](const PulledInput &item) {
              return item.argument == i;
            })) {
          callArgs[i] = UndefValue::get(original->getArg(i)->getType());
          continue;
        }
        return invalid("uncaptured sample-frequency input");
      }
      Function *body = original;
      {
        SmallVector<Type *, 32> bodyTypes(original->getFunctionType()->param_begin(),
                                           original->getFunctionType()->param_end());
        if (!pulled.empty()) {
          bodyTypes.push_back(PointerType::get(builder.getInt32Ty(), unsigned(air::AddressSpace::device)));
          bodyTypes.push_back(builder.getInt32Ty());
          bodyTypes.push_back(control->getType());
        }
        bodyTypes.push_back(builder.getInt1Ty());
        body = Function::Create(FunctionType::get(original->getReturnType(), bodyTypes, false),
                                Function::InternalLinkage, originalName + ".sample_body", module);
        body->copyAttributesFrom(original);
        for (unsigned i = 0; i < original->arg_size(); ++i)
          original->getArg(i)->replaceAllUsesWith(body->getArg(i));
        body->getBasicBlockList().splice(body->begin(), original->getBasicBlockList());
        // A defined helper may contain both derivatives and UAV writes. Inline
        // it before gating effects so the derivative path remains live in quad
        // helpers while only the actual resource operations are suppressed.
        while (true) {
          CallInst *nested = nullptr;
          for (auto &instruction : instructions(*body))
            if (auto *call = dyn_cast<CallInst>(&instruction); call &&
                call->getCalledFunction() && !call->getCalledFunction()->isDeclaration()) {
              nested = call;
              break;
            }
          if (!nested) break;
          if (nested->getCalledFunction() == body)
            return invalid("recursive sample-frequency shader helper");
          InlineFunctionInfo info;
          if (!InlineFunction(*nested, info).isSuccess())
            return invalid("cannot inline sample-frequency shader helper");
        }
        if (!pulled.empty()) for (auto &instruction : make_early_inc_range(instructions(*body))) {
          auto *call = dyn_cast<CallInst>(&instruction);
          if (!call || !call->getCalledFunction()) continue;
          unsigned location = pullLocation(call->getCalledFunction()->getName());
          if (location == ~0u) continue;
          auto *pointer = call->getArgOperand(0)->stripPointerCasts();
          auto found = std::find_if(pulled.begin(), pulled.end(), [&](const PulledInput &item) {
            return pointer == body->getArg(item.argument);
          });
          if (found == pulled.end() || found->offset[location] == ~0u)
            return invalid("unrecognized finite sample-frequency interpolant");
          IRBuilder<> load(call);
          auto *index = pullIndex(load, *call, location, options.sample_count,
            body->getArg(original->arg_size()), body->getArg(original->arg_size() + 1),
            body->getArg(original->arg_size() + 2), inputWords);
          auto *base = recordPointer(load, body->getArg(original->arg_size()),
                                      body->getArg(original->arg_size() + 1), inputWords, found->offset[location]);
          auto *array = load.CreateBitCast(base, PointerType::get(call->getType(), unsigned(air::AddressSpace::device)));
          auto *slot = load.CreateGEP(call->getType(), array, index);
          call->replaceAllUsesWith(load.CreateAlignedLoad(call->getType(), slot, Align(4)));
          call->eraseFromParent();
        }
        if (!pulled.empty()) {
          callArgs.push_back(entry->getArg(inputArg));
          callArgs.push_back(sampleRecord);
          callArgs.push_back(control);
        }
        // Helpers still execute the original shader once for quad derivatives;
        // only lanes covered by the original primitive can commit device writes.
        callArgs.push_back(logicalActive);
        guardSampleEffects(*body, body->getArg(body->arg_size() - 1));
      }
      auto *result = builder.CreateCall(body, callArgs);
      // Non-marker shaders serialize this annotation: refer to a call operand,
      // never a function-local SSA value in module metadata.
      result->setMetadata("dxmt.rov.logical_active", MDNode::get(context,
        {ConstantAsMetadata::get(builder.getInt32(result->arg_size() - 1))}));
      if (returnType->isVoidTy()) builder.CreateRetVoid();
      else {
        Value *value = PoisonValue::get(returnType);
        for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
          value = builder.CreateInsertValue(value, builder.CreateExtractValue(result, {i}), {i});
        for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
          if (outputKind(*cast<MDNode>(oldOutputs->getOperand(i))) == "air.depth")
            value = builder.CreateInsertValue(value, clampDepth(builder, module, control,
              builder.CreateExtractValue(value, {i})), {i});
        for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
          if (outputKind(*cast<MDNode>(oldOutputs->getOperand(i))) == "air.sample_mask")
            value = builder.CreateInsertValue(value, builder.CreateAnd(
              builder.CreateExtractValue(value, {i}),
              builder.CreateAnd(mask, builder.CreateShl(builder.getInt32(1),
                entry->getArg(sampleArg)))), {i});
        if (defaultDepth) {
          auto *raw = recordPointer(builder, entry->getArg(inputArg), sampleRecord, inputWords, sampleZOffset);
          auto *rawZ = builder.CreateLoad(builder.getFloatTy(), builder.CreateGEP(builder.getFloatTy(),
            builder.CreateBitCast(raw, PointerType::get(builder.getFloatTy(),
              unsigned(air::AddressSpace::device))), entry->getArg(sampleArg)));
          value = builder.CreateInsertValue(value, clampDepth(builder, module, control, rawZ),
                                            {oldOutputs->getNumOperands()});
        }
        builder.CreateRet(lineAlpha(value));
      }
    } else if (returnType->isVoidTy()) builder.CreateRetVoid();
    else {
      Value *value = PoisonValue::get(returnType);
      if (!original->getReturnType()->isVoidTy()) {
        auto *ptr = builder.CreateBitCast(recordPointer(builder, out, pixel, outputWords, 2),
                           PointerType::get(original->getReturnType(), unsigned(air::AddressSpace::device)));
        auto *saved = builder.CreateAlignedLoad(original->getReturnType(), ptr, Align(4));
        for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
          value = builder.CreateInsertValue(value, builder.CreateExtractValue(saved, {i}), {i});
      }
      for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
        if (outputKind(*cast<MDNode>(oldOutputs->getOperand(i))) == "air.sample_mask") {
          auto *appMask = builder.CreateExtractValue(value, {i});
          Value *effective = builder.CreateAnd(appMask, mask);
          if (options.single_sample_target) {
            // The RTV has only output sample zero, while mask describes the
            // virtual raster samples. Any covered raster sample can write it.
            auto *outputEnabled = builder.CreateICmpNE(
                builder.CreateAnd(appMask, builder.getInt32(1)), builder.getInt32(0));
            auto *rasterCovered = builder.CreateICmpNE(mask, builder.getInt32(0));
            effective = builder.CreateZExt(builder.CreateAnd(outputEnabled, rasterCovered), builder.getInt32Ty());
          }
          value = builder.CreateInsertValue(value, effective, {i});
        }
      for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i)
        if (outputKind(*cast<MDNode>(oldOutputs->getOperand(i))) == "air.depth")
          value = builder.CreateInsertValue(value, clampDepth(builder, module, control,
            builder.CreateExtractValue(value, {i})), {i});
      if (defaultDepth) {
        auto *raw = recordPointer(builder, entry->getArg(inputArg), pixel, inputWords, sampleZOffset);
        auto *rawZ = builder.CreateLoad(builder.getFloatTy(), builder.CreateGEP(builder.getFloatTy(),
          builder.CreateBitCast(raw, PointerType::get(builder.getFloatTy(),
            unsigned(air::AddressSpace::device))), entry->getArg(sampleArg)));
        value = builder.CreateInsertValue(value, clampDepth(builder, module, control, rawZ),
                                          {oldOutputs->getNumOperands()});
      }
      builder.CreateRet(lineAlpha(value));
    }
  }
  original->setLinkage(Function::InternalLinkage);
  fragments->setOperand(0, entryMetadata(*entry, *oldEntry, metadata, variant, defaultDepth));
  // MSAA uint texture reads crash the macOS 27 AGX compiler when encoded as
  // AIR 2.6, even though the metallib itself loads. AIR 2.8 is the earliest
  // proven target for 1x reads and interoperates with replay vertex AIR 2.6.
  if (variant != SM50_MSAA_COVERAGE) {
    module.setTargetTriple("air64_v28-apple-macosx26.0");
    auto *version = module.getOrInsertNamedMetadata("air.version");
    version->clearOperands();
    version->addOperand(MDTuple::get(context, {
      ConstantAsMetadata::get(builder.getInt32(2)),
      ConstantAsMetadata::get(builder.getInt32(8)),
      ConstantAsMetadata::get(builder.getInt32(0))
    }));
  }
  return Error::success();
}

} // namespace dxmt::msaa
