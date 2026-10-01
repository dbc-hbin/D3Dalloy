#include "msaa_coarse.hpp"

#include "air_signature.hpp"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace dxmt::msaa::coarse {

namespace {

constexpr unsigned kDevice = unsigned(air::AddressSpace::device);

Value *controlWord(IRBuilder<> &b, Value *control, unsigned index) {
  return b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), control, index));
}

Value *word(IRBuilder<> &b, Value *scratch, Value *record, unsigned stride, unsigned offset) {
  auto *index = b.CreateAdd(b.CreateMul(b.CreateZExt(record, b.getInt64Ty()), b.getInt64(stride)),
                            b.getInt64(offset));
  return b.CreateGEP(b.getInt32Ty(), scratch, index);
}

Value *planeWord(IRBuilder<> &b, Value *scratch, unsigned stride, unsigned offset, Type *type) {
  return b.CreateBitCast(word(b, scratch, b.getInt32(kPlaneRecord), stride, offset),
                         PointerType::get(type, kDevice));
}

Value *origin(IRBuilder<> &b, Value *control) {
  Value *xy = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  for (unsigned axis = 0; axis < 2; ++axis)
    xy = b.CreateInsertElement(xy, b.CreateUIToFP(controlWord(b, control, axis), b.getFloatTy()), b.getInt32(axis));
  return xy;
}

Value *floorXY(IRBuilder<> &b, Value *position, unsigned axis) {
  return b.CreateFPToUI(b.CreateExtractElement(position, b.getInt32(axis)), b.getInt32Ty());
}

/* value(t) = origin + dx * t.x + dy * t.y for a scalar or float4 plane. */
Value *evaluatePlane(IRBuilder<> &b, Value *scratch, unsigned stride, unsigned offset, Type *type, Value *t) {
  auto *base = planeWord(b, scratch, stride, offset, type);
  Value *value = b.CreateAlignedLoad(type, base, Align(4));
  for (unsigned axis = 0; axis < 2; ++axis) {
    auto *slope = b.CreateAlignedLoad(type, b.CreateGEP(type, base, b.getInt32(axis + 1)), Align(4));
    Value *distance = b.CreateExtractElement(t, b.getInt32(axis));
    if (type->isVectorTy()) distance = b.CreateVectorSplat(4, distance);
    value = b.CreateFAdd(value, b.CreateFMul(slope, distance));
  }
  return value;
}

/* Standard D3D sample offsets from the pixel center in 1/16 pixel, for the coarse sample counts. */
Value *sampleDelta(IRBuilder<> &b, Value *sample, unsigned sampleCount, bool centerPattern) {
  static constexpr int8_t x2[] = {4, -4}, y2[] = {4, -4};
  static constexpr int8_t x4[] = {-2, 6, -6, 2}, y4[] = {-6, -2, 2, 6};
  Value *delta = Constant::getNullValue(FixedVectorType::get(b.getFloatTy(), 2));
  if (sampleCount == 1 || centerPattern) return delta;
  const int8_t *sx = sampleCount == 2 ? x2 : x4, *sy = sampleCount == 2 ? y2 : y4;
  for (unsigned axis = 0; axis < 2; ++axis) {
    Value *component = ConstantFP::get(b.getFloatTy(), 0);
    for (unsigned i = 0; i < sampleCount; ++i)
      component = b.CreateSelect(b.CreateICmpEQ(sample, b.getInt32(i)),
        ConstantFP::get(b.getFloatTy(), double(axis ? sy[i] : sx[i]) / 16.0), component);
    delta = b.CreateInsertElement(delta, component, b.getInt32(axis));
  }
  return delta;
}

unsigned log2Count(unsigned sampleCount) { return sampleCount == 4 ? 2 : sampleCount == 2 ? 1 : 0; }

struct Axes {
  Value *x, *y;
};

/* D3D12 support table: 1x2/2x1/2x2 at 1, 2 and 4 samples; 2x4/4x2/4x4 at 1 sample and 2x4 at 2 samples with
 * AdditionalShadingRatesSupported. An unsupported rate is reduced on its larger axis (x on ties) until supported,
 * after each axis is clamped to the largest supported size (implementation-defined sanitization). */
Axes sanitize(IRBuilder<> &b, Axes rate, Value *largest, Value *additional, unsigned sampleCount) {
  auto clamp = [&](Value *axis) {
    return b.CreateSelect(b.CreateICmpUGT(axis, largest), largest, axis);
  };
  rate = {clamp(rate.x), clamp(rate.y)};
  for (unsigned round = 0; round < 2; ++round) {
    auto *small = b.CreateAnd(b.CreateICmpULE(rate.x, b.getInt32(1)), b.CreateICmpULE(rate.y, b.getInt32(1)));
    Value *extended = b.getFalse();
    if (sampleCount == 1)
      extended = b.CreateAnd(b.CreateICmpUGE(rate.x, b.getInt32(1)), b.CreateICmpUGE(rate.y, b.getInt32(1)));
    else if (sampleCount == 2)
      extended = b.CreateAnd(b.CreateICmpEQ(rate.x, b.getInt32(1)), b.CreateICmpEQ(rate.y, b.getInt32(2)));
    auto *ok = b.CreateOr(small, b.CreateAnd(additional, extended));
    auto *reduceY = b.CreateICmpUGT(rate.y, rate.x);
    rate = {b.CreateSelect(b.CreateOr(ok, reduceY), rate.x, b.CreateSub(rate.x, b.getInt32(1))),
            b.CreateSelect(b.CreateAnd(b.CreateNot(ok), reduceY), b.CreateSub(rate.y, b.getInt32(1)), rate.y)};
  }
  return rate;
}

Axes split(IRBuilder<> &b, Value *rate) {
  return {b.CreateAnd(b.CreateLShr(rate, 2), b.getInt32(3)), b.CreateAnd(rate, b.getInt32(3))};
}

Axes combine(IRBuilder<> &b, Value *combiner, Axes a, Axes c, Value *largest) {
  auto axis = [&](Value *left, Value *right) {
    auto *minimum = b.CreateSelect(b.CreateICmpULT(left, right), left, right);
    auto *maximum = b.CreateSelect(b.CreateICmpUGT(left, right), left, right);
    auto *sum = b.CreateAdd(left, right);
    sum = b.CreateSelect(b.CreateICmpUGT(sum, largest), largest, sum);
    Value *result = left; // PASSTHROUGH, and any undefined combiner value
    result = b.CreateSelect(b.CreateICmpEQ(combiner, b.getInt32(1)), right, result);
    result = b.CreateSelect(b.CreateICmpEQ(combiner, b.getInt32(2)), minimum, result);
    result = b.CreateSelect(b.CreateICmpEQ(combiner, b.getInt32(3)), maximum, result);
    return b.CreateSelect(b.CreateICmpEQ(combiner, b.getInt32(4)), sum, result);
  };
  return {axis(a.x, c.x), axis(a.y, c.y)};
}

void storeTyped(IRBuilder<> &b, Value *scratch, unsigned stride, unsigned offset, Value *value) {
  b.CreateAlignedStore(value, planeWord(b, scratch, stride, offset, value->getType()), Align(4));
}

void storePlane(IRBuilder<> &b, Value *scratch, unsigned stride, unsigned offset, Value *value,
                Value *dx, Value *dy, Value *t) {
  // Move the plane from the sampled point to the tile origin: value - dx * t.x - dy * t.y.
  for (unsigned axis = 0; axis < 2; ++axis) {
    Value *distance = b.CreateExtractElement(t, b.getInt32(axis));
    if (value->getType()->isVectorTy()) distance = b.CreateVectorSplat(4, distance);
    value = b.CreateFSub(value, b.CreateFMul(axis ? dy : dx, distance));
  }
  auto *type = value->getType();
  auto *base = planeWord(b, scratch, stride, offset, type);
  b.CreateAlignedStore(value, base, Align(4));
  b.CreateAlignedStore(dx, b.CreateGEP(type, base, b.getInt32(1)), Align(4));
  b.CreateAlignedStore(dy, b.CreateGEP(type, base, b.getInt32(2)), Align(4));
}

} // namespace

unsigned allocate(Planes &planes, unsigned words) {
  // Fine records keep their claim and coverage in dwords 0 and 1; the plane record reuses them for the rate
  // (read by fixed position) and its claim. Per-primitive inputs keep their fine-record offsets.
  planes.rate = 0;
  planes.claim = 1;
  planes.q = words;
  words += 3;
  planes.z = words;
  words += 3;
  for (auto &input : planes.inputs) {
    input.offset = words;
    words += 12;
  }
  return words;
}

Value *resolveRate(IRBuilder<> &b, Value *control, unsigned sampleCount, Value *primitive, Value *image) {
  auto *state = controlWord(b, control, kRateDword);
  auto *additional = b.CreateICmpNE(b.CreateAnd(state, b.getInt32(kRateAdditional)), b.getInt32(0));
  auto *largest = b.CreateSelect(additional, b.getInt32(2), b.getInt32(1));
  auto clean = [&](Axes rate) { return sanitize(b, rate, largest, additional, sampleCount); };
  auto *combiner0 = b.CreateAnd(b.CreateLShr(state, kRateCombiner0Shift), b.getInt32(0xf));
  auto *combiner1 = b.CreateAnd(b.CreateLShr(state, kRateCombiner1Shift), b.getInt32(0xf));
  Axes rate = clean(split(b, b.CreateAnd(state, b.getInt32(kRateBaseMask))));
  rate = clean(combine(b, combiner0, rate, clean(split(b, primitive)), largest));
  rate = clean(combine(b, combiner1, rate, clean(split(b, image)), largest));
  return b.CreateOr(b.CreateShl(rate.x, 2), rate.y);
}

void capture(IRBuilder<> &b, Module &module, Function &entry, Value *scratch, unsigned stride, Value *position,
             Value *control, Value *pixel, unsigned centerZ, const Planes &planes, ArrayRef<Value *> interpolants,
             Value *reciprocalW, Value *rate, ArrayRef<std::pair<unsigned, Value *>> constants,
             function_ref<Value *(unsigned, Value *)> capturedInput) {
  auto &context = module.getContext();
  auto *floatTy = b.getFloatTy();
  auto *float2 = FixedVectorType::get(floatTy, 2);
  auto *float4 = FixedVectorType::get(floatTy, 4);
  auto *tileOrigin = origin(b, control);
  // Metal interpolate_at_offset offsets are relative to the fragment's pixel corner and extrapolate linearly.
  Value *corner = UndefValue::get(float2);
  for (unsigned axis = 0; axis < 2; ++axis)
    corner = b.CreateInsertElement(corner, b.CreateUIToFP(floorXY(b, position, axis), floatTy), b.getInt32(axis));
  auto *base = b.CreateFSub(tileOrigin, corner);
  Value *points[3] = {base, b.CreateFAdd(base, ConstantVector::get({ConstantFP::get(floatTy, 8),
                                                                    ConstantFP::get(floatTy, 0)})),
                      b.CreateFAdd(base, ConstantVector::get({ConstantFP::get(floatTy, 0),
                                                              ConstantFP::get(floatTy, 8)}))};
  auto atOffset = [&](Value *interpolant, bool perspective, Type *type, Value *offset) -> Value * {
    std::string name = std::string("air.interpolate_offset_") + (perspective ? "perspective" : "no_perspective") +
                       (type == floatTy ? ".f32" : ".v4f32");
    return b.CreateCall(module.getOrInsertFunction(name, FunctionType::get(type, {interpolant->getType(), float2},
                                                                           false)), {interpolant, offset});
  };
  auto *eighth = ConstantFP::get(floatTy, 1.0 / 8.0);
  // {value at the tile origin, slope x, slope y} from the three evaluations.
  auto plane = [&](std::array<Value *, 3> at) -> std::array<Value *, 3> {
    Value *scale = at[0]->getType()->isVectorTy() ? b.CreateVectorSplat(4, eighth) : static_cast<Value *>(eighth);
    return {at[0], b.CreateFMul(b.CreateFSub(at[1], at[0]), scale), b.CreateFMul(b.CreateFSub(at[2], at[0]), scale)};
  };
  std::array<Value *, 3> q;
  for (unsigned i = 0; i < 3; ++i) q[i] = atOffset(reciprocalW, false, floatTy, points[i]);
  SmallVector<std::array<Value *, 3>, 8> values;
  for (unsigned i = 0; i < planes.inputs.size(); ++i) {
    std::array<Value *, 3> at;
    for (unsigned j = 0; j < 3; ++j) {
      at[j] = interpolants[i]
        ? atOffset(interpolants[i], planes.inputs[i].perspective, float4, points[j])
        : capturedInput(i, b.CreateFAdd(corner, points[j]));
      // Perspective inputs are v * q, affine in screen space; evaluation divides by the q plane again.
      if (planes.inputs[i].perspective) at[j] = b.CreateFMul(at[j], b.CreateVectorSplat(4, q[j]));
    }
    values.push_back(plane(at));
  }
  auto *depth = b.CreateExtractElement(position, b.getInt32(2));
  auto *zDx = b.CreateCall(module.getOrInsertFunction("air.dfdx.f32", FunctionType::get(floatTy, {floatTy}, false)),
                           {depth});
  auto *zDy = b.CreateCall(module.getOrInsertFunction("air.dfdy.f32", FunctionType::get(floatTy, {floatTy}, false)),
                           {depth});
  // Only a fragment of the primitive claims: helper lanes' device writes are discarded.
  auto *helper = b.CreateCall(module.getOrInsertFunction("air.simd_is_helper_thread",
                                                         FunctionType::get(b.getInt1Ty(), {}, false)));
  auto *claimBlock = BasicBlock::Create(context, "coarse_plane_claim", &entry);
  auto *store = BasicBlock::Create(context, "coarse_plane_capture", &entry);
  auto *join = BasicBlock::Create(context, "coarse_plane_captured", &entry);
  b.CreateCondBr(helper, join, claimBlock);
  b.SetInsertPoint(claimBlock);
  auto *claim = b.CreateAtomicCmpXchg(word(b, scratch, b.getInt32(kPlaneRecord), stride, planes.claim),
    b.getInt32(0), b.getInt32(1), MaybeAlign(), AtomicOrdering::Monotonic, AtomicOrdering::Monotonic);
  b.CreateCondBr(b.CreateICmpEQ(b.CreateExtractValue(claim, 0), b.getInt32(0)), store, join);
  b.SetInsertPoint(store);
  auto *zero = Constant::getNullValue(float2);
  storeTyped(b, scratch, stride, planes.rate, rate);
  auto qPlane = plane(q);
  storePlane(b, scratch, stride, planes.q, qPlane[0], qPlane[1], qPlane[2], zero);
  // The gather pass's center depth (window z at this pixel's center, with depth bias) moved to the tile origin.
  auto *gathered = b.CreateLoad(floatTy, b.CreateBitCast(word(b, scratch, pixel, stride, centerZ),
                                                         PointerType::get(floatTy, kDevice)));
  auto *centerT = b.CreateFSub(b.CreateFAdd(corner, ConstantVector::getSplat(ElementCount::getFixed(2),
                                                                            ConstantFP::get(floatTy, 0.5))),
                               tileOrigin);
  storePlane(b, scratch, stride, planes.z, gathered, zDx, zDy, centerT);
  for (unsigned i = 0; i < planes.inputs.size(); ++i)
    storePlane(b, scratch, stride, planes.inputs[i].offset, values[i][0], values[i][1], values[i][2], zero);
  for (auto &[offset, value] : constants) storeTyped(b, scratch, stride, offset, value);
  b.CreateBr(join);
  b.SetInsertPoint(join);
}

Grid grid(IRBuilder<> &b, Value *position, Value *control, Value *rate) {
  Grid result;
  auto axes = split(b, rate);
  result.log2x = axes.x;
  result.log2y = axes.y;
  auto *localX = b.CreateSub(floorXY(b, position, 0), controlWord(b, control, 0));
  auto *localY = b.CreateSub(floorXY(b, position, 1), controlWord(b, control, 1));
  result.valid = b.CreateAnd(b.CreateICmpULT(localX, b.CreateLShr(b.getInt32(8), axes.x)),
                             b.CreateICmpULT(localY, b.CreateLShr(b.getInt32(8), axes.y)));
  // Unconditional so quad neighbors outside the coarse grid still provide consistent derivatives; the coarse
  // grid size (8 >> log2) is even, so no native quad straddles its edge.
  result.fineX = b.CreateShl(localX, axes.x);
  result.fineY = b.CreateShl(localY, axes.y);
  auto *floatTy = b.getFloatTy();
  Value *center = UndefValue::get(FixedVectorType::get(floatTy, 2));
  Value *fine[] = {result.fineX, result.fineY};
  Value *log2[] = {axes.x, axes.y};
  for (unsigned axis = 0; axis < 2; ++axis) {
    auto *half = b.CreateFMul(b.CreateUIToFP(b.CreateShl(b.getInt32(1), log2[axis]), floatTy),
                              ConstantFP::get(floatTy, 0.5));
    auto *start = b.CreateUIToFP(b.CreateAdd(controlWord(b, control, axis), fine[axis]), floatTy);
    center = b.CreateInsertElement(center, b.CreateFAdd(start, half), b.getInt32(axis));
  }
  result.center = center;
  return result;
}

Coverage coverage(IRBuilder<> &b, const Grid &grid, Value *control, Value *scratch, unsigned stride,
                  unsigned sampleCount, function_ref<Value *(Value *)> fineMask) {
  auto *floatTy = b.getFloatTy();
  Value *geometric = b.getInt32(0), *post = b.getInt32(0);
  auto *width = b.CreateShl(b.getInt32(1), grid.log2x);
  auto *height = b.CreateShl(b.getInt32(1), grid.log2y);
  const uint32_t sampleBits = (1u << sampleCount) - 1;
  for (unsigned j = 0; j < 4; ++j)
    for (unsigned i = 0; i < 4; ++i) {
      auto *exists = b.CreateAnd(b.CreateAnd(b.CreateICmpULT(b.getInt32(i), width),
                                             b.CreateICmpULT(b.getInt32(j), height)), grid.valid);
      auto *x = b.CreateAdd(grid.fineX, b.getInt32(i));
      auto *y = b.CreateAdd(grid.fineY, b.getInt32(j));
      auto *record = b.CreateSelect(exists, b.CreateAdd(b.CreateMul(y, b.getInt32(8)), x), b.getInt32(0));
      auto *primitive = b.CreateAnd(b.CreateLoad(b.getInt32Ty(), word(b, scratch, record, stride, 1)),
                                    b.getInt32(sampleBits));
      auto *covered = b.CreateAnd(exists, b.CreateICmpNE(primitive, b.getInt32(0)));
      // An uncovered fine pixel may lie outside the render target: read the tile origin instead and drop it.
      Value *at = Constant::getNullValue(FixedVectorType::get(floatTy, 4));
      at = b.CreateInsertElement(at, ConstantFP::get(floatTy, 1), b.getInt32(3));
      Value *coordinates[] = {x, y};
      for (unsigned axis = 0; axis < 2; ++axis) {
        auto *local = b.CreateSelect(covered, coordinates[axis], b.getInt32(0));
        at = b.CreateInsertElement(at, b.CreateFAdd(b.CreateUIToFP(b.CreateAdd(controlWord(b, control, axis), local),
          floatTy), ConstantFP::get(floatTy, 0.5)), b.getInt32(axis));
      }
      auto *mask = b.CreateSelect(covered, b.CreateAnd(fineMask(at), b.getInt32(sampleBits)), b.getInt32(0));
      auto *shift = b.CreateMul(b.CreateAdd(b.CreateShl(b.getInt32(j), grid.log2x), b.getInt32(i)),
                                b.getInt32(sampleCount));
      auto *place = b.CreateSelect(exists, shift, b.getInt32(0));
      geometric = b.CreateOr(geometric, b.CreateSelect(covered, b.CreateShl(primitive, place), b.getInt32(0)));
      post = b.CreateOr(post, b.CreateShl(mask, place));
    }
  return {geometric, post};
}

Value *load(IRBuilder<> &b, Value *scratch, unsigned stride, unsigned offset, Type *type) {
  return b.CreateAlignedLoad(type, planeWord(b, scratch, stride, offset, type), Align(4));
}

Value *position(IRBuilder<> &b, Value *scratch, unsigned stride, const Planes &planes, Value *control,
                Value *center) {
  auto *t = b.CreateFSub(center, origin(b, control));
  Value *result = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 4));
  for (unsigned axis = 0; axis < 2; ++axis)
    result = b.CreateInsertElement(result, b.CreateExtractElement(center, b.getInt32(axis)), b.getInt32(axis));
  result = b.CreateInsertElement(result, evaluatePlane(b, scratch, stride, planes.z, b.getFloatTy(), t),
                                 b.getInt32(2));
  return b.CreateInsertElement(result, evaluatePlane(b, scratch, stride, planes.q, b.getFloatTy(), t),
                               b.getInt32(3));
}

SmallVector<Type *, kBodyArguments> bodyTypes(Type *scratch, Type *control) {
  auto &context = scratch->getContext();
  return {scratch, control, FixedVectorType::get(Type::getFloatTy(context), 2),
          FixedVectorType::get(Type::getInt32Ty(context), 4), Type::getInt32Ty(context)};
}

SmallVector<Value *, kBodyArguments> bodyArguments(IRBuilder<> &b, Value *scratch, Value *control,
                                                   const Grid &grid, Value *geometric) {
  Value *info = UndefValue::get(FixedVectorType::get(b.getInt32Ty(), 4));
  Value *fields[] = {grid.fineX, grid.fineY, grid.log2x, grid.log2y};
  for (unsigned i = 0; i < 4; ++i) info = b.CreateInsertElement(info, fields[i], b.getInt32(i));
  return {scratch, control, grid.center, info, geometric};
}

Error rewritePulls(Function &body, unsigned extra, const Planes &planes, unsigned stride, unsigned sampleCount,
                   bool centerPattern, bool conservative) {
  auto *scratch = body.getArg(extra);
  auto *control = body.getArg(extra + 1);
  auto *center = body.getArg(extra + 2);
  auto *info = body.getArg(extra + 3);
  auto *geometric = body.getArg(extra + 4);
  auto *float4 = FixedVectorType::get(Type::getFloatTy(body.getContext()), 4);
  for (auto &instruction : make_early_inc_range(instructions(body))) {
    auto *call = dyn_cast<CallInst>(&instruction);
    if (!call || !call->getCalledFunction() || call->arg_size() == 0) continue;
    auto name = call->getCalledFunction()->getName();
    if (!name.startswith("air.interpolate_")) continue;
    auto *pointer = call->getArgOperand(0)->stripPointerCasts();
    auto found = std::find_if(planes.inputs.begin(), planes.inputs.end(), [&](const PlaneInput &input) {
      return pointer == body.getArg(input.argument);
    });
    if (found == planes.inputs.end())
      return createStringError(inconvertibleErrorCode(), "coarse interpolant has no captured plane");
    if (call->getType() != float4)
      return createStringError(inconvertibleErrorCode(), "coarse interpolant must be float4");
    IRBuilder<> b(call);
    auto *fineX = b.CreateExtractElement(info, b.getInt32(0));
    auto *fineY = b.CreateExtractElement(info, b.getInt32(1));
    auto *log2x = b.CreateExtractElement(info, b.getInt32(2));
    auto *log2y = b.CreateExtractElement(info, b.getInt32(3));
    auto *pixels = b.CreateShl(b.getInt32(1), b.CreateAdd(log2x, log2y));
    // Coarse sample `index` (coverage order: fine pixel index * sampleCount + sample) to a render-target point.
    auto samplePoint = [&](Value *index) {
      auto *fine = b.CreateLShr(index, log2Count(sampleCount));
      auto *sample = b.CreateAnd(index, b.getInt32(sampleCount - 1));
      auto *column = b.CreateAnd(fine, b.CreateSub(b.CreateShl(b.getInt32(1), log2x), b.getInt32(1)));
      auto *row = b.CreateLShr(fine, log2x);
      Value *point = UndefValue::get(center->getType());
      Value *offsets[] = {b.CreateAdd(fineX, column), b.CreateAdd(fineY, row)};
      for (unsigned axis = 0; axis < 2; ++axis)
        point = b.CreateInsertElement(point, b.CreateFAdd(b.CreateUIToFP(b.CreateAdd(controlWord(b, control, axis),
          offsets[axis]), b.getFloatTy()), ConstantFP::get(b.getFloatTy(), 0.5)), b.getInt32(axis));
      return b.CreateFAdd(point, sampleDelta(b, sample, sampleCount, centerPattern));
    };
    Value *point = center;
    if (name.startswith("air.interpolate_sample_")) {
      auto *count = b.CreateMul(pixels, b.getInt32(sampleCount));
      point = samplePoint(b.CreateAnd(call->getArgOperand(1), b.CreateSub(count, b.getInt32(1))));
    } else if (name.startswith("air.interpolate_centroid_")) {
      // The first covered sample of the coarse pixel (coverage order) after SampleMask.
      const uint32_t sampleBits = (1u << sampleCount) - 1;
      auto *perPixel = b.CreateAnd(controlWord(b, control, 3), b.getInt32(sampleBits));
      Value *psoMask = b.getInt32(0);
      for (unsigned fine = 0; fine < 16 / sampleCount; ++fine)
        psoMask = b.CreateOr(psoMask, b.CreateSelect(b.CreateICmpULT(b.getInt32(fine), pixels),
          b.CreateShl(perPixel, b.getInt32(fine * sampleCount)), b.getInt32(0)));
      auto *remaining = b.CreateAnd(geometric, psoMask);
      auto *candidates = b.CreateSelect(b.CreateICmpEQ(remaining, b.getInt32(0)), psoMask, remaining);
      Value *first = b.getInt32(0);
      for (unsigned bit = 16; bit-- > 0;)
        first = b.CreateSelect(b.CreateICmpNE(b.CreateAnd(candidates, b.getInt32(1u << bit)), b.getInt32(0)),
                               b.getInt32(bit), first);
      // VRS centroid uses the first covered fine sample; conservative rasterization uses center.
      point = conservative ? center : samplePoint(first);
    } else if (name.startswith("air.interpolate_offset_")) {
      // The lowering passes a D3D snapped offset n (1/16 pixel) as the Metal pixel-corner offset (n + 8) / 16.
      // A coarse pixel of 2^l fine pixels on an axis uses 4 + l signed offset bits, and D3D centers the offset
      // grid at the coarse pixel center (VRS spec, EvaluateAttributeSnapped offset range).
      Value *offset = UndefValue::get(center->getType());
      Value *log2[] = {log2x, log2y};
      for (unsigned axis = 0; axis < 2; ++axis) {
        auto *corner = b.CreateExtractElement(call->getArgOperand(1), b.getInt32(axis));
        auto *n = b.CreateSub(b.CreateFPToSI(b.CreateFMul(corner, ConstantFP::get(b.getFloatTy(), 16.0)),
                                             b.getInt32Ty()), b.getInt32(8));
        auto *unused = b.CreateSub(b.getInt32(28), log2[axis]);
        auto *wrapped = b.CreateAShr(b.CreateShl(n, unused), unused);
        offset = b.CreateInsertElement(offset, b.CreateFMul(b.CreateSIToFP(wrapped, b.getFloatTy()),
                                                            ConstantFP::get(b.getFloatTy(), 1.0 / 16.0)),
                                       b.getInt32(axis));
      }
      point = b.CreateFAdd(center, offset);
    } else if (!name.startswith("air.interpolate_center_")) {
      return createStringError(inconvertibleErrorCode(), "unknown coarse interpolation function");
    }
    auto *t = b.CreateFSub(point, origin(b, control));
    Value *value = evaluatePlane(b, scratch, stride, found->offset, float4, t);
    if (found->perspective)
      value = b.CreateFDiv(value, b.CreateVectorSplat(4, evaluatePlane(b, scratch, stride, planes.q,
                                                                         b.getFloatTy(), t)));
    call->replaceAllUsesWith(value);
    call->eraseFromParent();
  }
  return Error::success();
}

Replay replay(IRBuilder<> &b, Value *position, Value *control, Value *rate, unsigned sampleCount) {
  auto axes = split(b, rate);
  auto *localX = b.CreateSub(floorXY(b, position, 0), controlWord(b, control, 0));
  auto *localY = b.CreateSub(floorXY(b, position, 1), controlWord(b, control, 1));
  auto *record = b.CreateAdd(b.CreateMul(b.CreateLShr(localY, axes.y), b.getInt32(8)),
                             b.CreateLShr(localX, axes.x));
  auto low = [&](Value *value, Value *log2) {
    return b.CreateAnd(value, b.CreateSub(b.CreateShl(b.getInt32(1), log2), b.getInt32(1)));
  };
  auto *fine = b.CreateAdd(b.CreateShl(low(localY, axes.y), axes.x), low(localX, axes.x));
  return {record, b.CreateMul(fine, b.getInt32(sampleCount))};
}

} // namespace dxmt::msaa::coarse
