#include "msaa_vertex.hpp"
#include "msaa_pixel.hpp"

#include "airconv_context.hpp"
#include "air_signature.hpp"
#include "capture_frontier.hpp"
#include "dxil_converter.hpp"
#include "mesh_index_buffer_view.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <functional>

using namespace llvm;

namespace d3dalloy::msaa {
namespace {

Error invalid(const char *message) {
  return createStringError(inconvertibleErrorCode(), "MSAA vertex: %s", message);
}

StringRef inputKind(const MDNode &node) {
  auto *kind = dyn_cast<MDString>(node.getOperand(1));
  return kind ? kind->getString() : StringRef{};
}

MDNode *reindex(MDNode &node, unsigned index, LLVMContext &context) {
  SmallVector<Metadata *, 12> fields(node.op_begin(), node.op_end());
  fields[0] = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index));
  return MDTuple::get(context, fields);
}

MDNode *builtin(unsigned index, StringRef kind, StringRef type, StringRef name, LLVMContext &context) {
  return MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), index)),
    MDString::get(context, kind), MDString::get(context, "air.arg_type_name"),
    MDString::get(context, type), MDString::get(context, "air.arg_name"),
    MDString::get(context, name)
  });
}

MDNode *entryMetadata(Function &function, MDNode &original, ArrayRef<Metadata *> inputs,
                      bool keepOutputs, ArrayRef<Metadata *> extraOutputs = {}) {
  LLVMContext &context = function.getContext();
  SmallVector<Metadata *, 6> fields(original.op_begin(), original.op_end());
  fields[0] = ValueAsMetadata::get(&function);
  if (keepOutputs && !extraOutputs.empty()) {
    auto *outputs = cast<MDTuple>(original.getOperand(1));
    SmallVector<Metadata *, 12> values(outputs->op_begin(), outputs->op_end());
    values.append(extraOutputs.begin(), extraOutputs.end());
    fields[1] = MDTuple::get(context, values);
  } else {
    fields[1] = keepOutputs ? original.getOperand(1).get() : MDTuple::get(context, {});
  }
  fields[2] = MDTuple::get(context, inputs);
  // A vertex-only function cannot carry a fragment/kernel-only entry attribute.
  for (unsigned i = 3; i < fields.size();) {
    if (auto *tag = dyn_cast<MDString>(fields[i]); tag && tag->getString() == "early_fragment_tests")
      fields.erase(fields.begin() + i);
    else
      ++i;
  }
  return MDTuple::get(context, fields);
}

air::ArgumentBindingBuffer recordsBinding(bool writable) {
  return {
    .buffer_size = {},
    .location_index = SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS,
    .array_size = 0,
    .memory_access = writable ? air::MemoryAccess::read_write : air::MemoryAccess::read,
    .address_space = air::AddressSpace::device,
    .type = air::MSLUint{},
    .arg_name = "msaa_vertex_records",
    .raster_order_group = {}
  };
}

air::ArgumentBindingBuffer lineBinding(unsigned slot, bool constant, const char *name) {
  return {
    .buffer_size = {}, .location_index = slot, .array_size = 0,
    .memory_access = air::MemoryAccess::read,
    .address_space = constant ? air::AddressSpace::constant : air::AddressSpace::device,
    .type = air::MSLUint{}, .arg_name = name, .raster_order_group = {}
  };
}

Value *component(IRBuilder<> &b, Value *vector, unsigned index) {
  return b.CreateExtractElement(vector, b.getInt32(index));
}

Value *plane(IRBuilder<> &b, Value *p, unsigned index) {
  Value *w = component(b, p, 3);
  switch (index) {
  case 0: return b.CreateFAdd(w, component(b, p, 0));
  case 1: return b.CreateFSub(w, component(b, p, 0));
  case 2: return b.CreateFAdd(w, component(b, p, 1));
  case 3: return b.CreateFSub(w, component(b, p, 1));
  case 4: return component(b, p, 2);
  default: return b.CreateFSub(w, component(b, p, 2));
  }
}

// Liang-Barsky clipping in homogeneous coordinates. A positive signed
// distance means inside. Keep the interval instead of moving the captured
// endpoints before all planes have been considered.
void clipPlane(IRBuilder<> &b, Value *a, Value *z, Value *&lo, Value *&hi,
               Value *&valid) {
  Value *aOut = b.CreateFCmpOLT(a, ConstantFP::get(b.getFloatTy(), 0));
  Value *zOut = b.CreateFCmpOLT(z, ConstantFP::get(b.getFloatTy(), 0));
  valid = b.CreateAnd(valid, b.CreateNot(b.CreateAnd(aOut, zOut)));
  Value *crossing = b.CreateFDiv(a, b.CreateFSub(a, z));
  lo = b.CreateSelect(b.CreateAnd(aOut, b.CreateNot(zOut)),
                      b.CreateSelect(b.CreateFCmpOGT(crossing, lo), crossing, lo), lo);
  hi = b.CreateSelect(b.CreateAnd(zOut, b.CreateNot(aOut)),
                      b.CreateSelect(b.CreateFCmpOLT(crossing, hi), crossing, hi), hi);
}

MDNode *lineOutput(LLVMContext &context, StringRef name) {
  return MDTuple::get(context, {
    MDString::get(context, "air.vertex_output"), MDString::get(context, ("user(" + name + ")").str()),
    MDString::get(context, "air.arg_type_name"), MDString::get(context, "float2"),
    MDString::get(context, "air.arg_name"), MDString::get(context, name)
  });
}

bool nonPerspective(const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options, const MDNode &output) {
  if (!options.interpolation) return false;
  auto *name = output.getNumOperands() > 1 ? dyn_cast<MDString>(output.getOperand(1)) : nullptr;
  if (!name) return false;
  uint64_t hash = userHash(name->getString());
  for (unsigned i = 0; i < options.interpolation->no_perspective_count; ++i)
    if (options.interpolation->no_perspective_user_hashes[i] == hash) return true;
  return false;
}

bool flat(const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options, const MDNode &output) {
  if (!options.interpolation) return false;
  auto *name = output.getNumOperands() > 1 ? dyn_cast<MDString>(output.getOperand(1)) : nullptr;
  if (!name) return false;
  uint64_t hash = userHash(name->getString());
  for (unsigned i = 0; i < options.interpolation->flat_count; ++i)
    if (options.interpolation->flat_user_hashes[i] == hash) return true;
  return false;
}

Value *expandedLine(IRBuilder<> &b, Value *encoded, Value *records, Value *indices,
                    Value *bounds, StructType *native, StructType *resultType,
                    unsigned positionField, MDTuple *outputs, unsigned stride,
                    const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options) {
  Value *slot = b.CreateUDiv(encoded, b.getInt32(8));
  Value *corner = b.CreateURem(encoded, b.getInt32(8));
  // Eighteen indices form start cap, exact center and end cap quads. Original
  // capture ordinals occupy words 18 and 19 of each 20-word slot.
  Value *indexBase = b.CreateMul(b.CreateZExt(slot, b.getInt64Ty()), b.getInt64(20));
  auto ordinal = [&](unsigned offset) -> Value * {
    return b.CreateLoad(b.getInt32Ty(), b.CreateGEP(b.getInt32Ty(), indices,
                                                    b.CreateAdd(indexBase, b.getInt64(offset))));
  };
  auto captured = [&](Value *which) -> Value * {
    Value *word = b.CreateMul(b.CreateZExt(which, b.getInt64Ty()), b.getInt64(stride));
    Value *pointer = b.CreateBitCast(b.CreateGEP(b.getInt32Ty(), records, word),
      PointerType::get(native, unsigned(air::AddressSpace::device)));
    auto *value = b.CreateAlignedLoad(native, pointer, Align(4));
    auto &module = *value->getModule();
    if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
      value->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
    return value;
  };
  Value *a = captured(ordinal(18));
  Value *z = captured(ordinal(19));
  Value *p0 = b.CreateExtractValue(a, positionField);
  Value *p1 = b.CreateExtractValue(z, positionField);
  Value *lo = ConstantFP::get(b.getFloatTy(), 0), *hi = ConstantFP::get(b.getFloatTy(), 1);
  Value *valid = b.getTrue();
  for (unsigned i = 0; i < 6; ++i) {
    if (!options.depth_clip_enable && i >= 4) continue;
    clipPlane(b, plane(b, p0, i), plane(b, p1, i), lo, hi, valid);
  }
  for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
    auto *field = cast<MDTuple>(outputs->getOperand(i));
    auto *kind = cast<MDString>(field->getOperand(0));
    if (kind->getString() != "air.clip_distance" && kind->getString() != "air.cull_distance")
      continue;
    auto *type = dyn_cast<ArrayType>(native->getElementType(i));
    if (!type) continue;
    Value *d0 = b.CreateExtractValue(a, i), *d1 = b.CreateExtractValue(z, i);
    for (unsigned j = 0; j < type->getNumElements(); ++j) {
      Value *start = b.CreateExtractValue(d0, j), *end = b.CreateExtractValue(d1, j);
      if (kind->getString() == "air.clip_distance")
        clipPlane(b, start, end, lo, hi, valid);
      else
        valid = b.CreateAnd(valid, b.CreateNot(b.CreateAnd(
          b.CreateFCmpOLT(start, ConstantFP::get(b.getFloatTy(), 0)),
          b.CreateFCmpOLT(end, ConstantFP::get(b.getFloatTy(), 0)))));
    }
  }
  valid = b.CreateAnd(valid, b.CreateFCmpOLE(lo, hi));
  Value *start = lineBlend(b, p0, p1, lo), *end = lineBlend(b, p0, p1, hi);
  Value *startW = component(b, start, 3), *endW = component(b, end, 3);
  valid = b.CreateAnd(valid, b.CreateAnd(b.CreateFCmpOGT(startW, ConstantFP::get(b.getFloatTy(), 0)),
                                             b.CreateFCmpOGT(endW, ConstantFP::get(b.getFloatTy(), 0))));
  auto loadBound = [&](unsigned index) -> Value * {
    return b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), bounds, index));
  };
  Value *viewportCount = loadBound(5);
  valid = b.CreateAnd(valid, b.CreateICmpNE(viewportCount, b.getInt32(0)));
  Value *viewport = b.getInt32(0);
  for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
    auto *field = cast<MDTuple>(outputs->getOperand(i));
    if (cast<MDString>(field->getOperand(0))->getString() == "air.viewport_array_index")
      viewport = b.CreateExtractValue(a, i);
  }
  // View instance location (MSAAPrimitiveBoundsParams::view_*): the replay pipeline's amplification mapping adds
  // the offset to the emitted index (out of range -> 0), so emit the shader's index unchanged and expand the line
  // in the effective viewport.
  Value *viewOffsets = b.CreateICmpNE(loadBound(142), b.getInt32(0));
  Value *rawViewport = viewport;
  Value *offsetViewport = b.CreateAdd(viewport, loadBound(140));
  offsetViewport = b.CreateSelect(b.CreateICmpULT(offsetViewport, viewportCount), offsetViewport, b.getInt32(0));
  viewport = b.CreateSelect(viewOffsets, offsetViewport, viewport);
  viewport = b.CreateSelect(b.CreateICmpULT(viewport, viewportCount), viewport,
                            b.CreateSub(viewportCount, b.getInt32(1)));
  viewport = b.CreateSelect(b.CreateICmpULT(viewport, b.getInt32(16)), viewport, b.getInt32(0));
  Value *emittedViewport = b.CreateSelect(viewOffsets, rawViewport, viewport);
  Value *viewportOffset = b.CreateAdd(b.CreateMul(viewport, b.getInt32(4)), b.getInt32(12));
  Value *floats = b.CreateBitCast(bounds, PointerType::get(b.getFloatTy(), unsigned(air::AddressSpace::constant)));
  auto vp = [&](unsigned index) -> Value * {
    return b.CreateLoad(b.getFloatTy(), b.CreateGEP(b.getFloatTy(), floats,
                           b.CreateAdd(viewportOffset, b.getInt32(index))));
  };
  Value *vx = vp(0), *vy = vp(1), *vw = vp(2), *vh = vp(3);
  auto screen = [&](Value *position) -> Value * {
    Value *w = component(b, position, 3);
    Value *x = b.CreateFAdd(vx, b.CreateFMul(b.CreateFAdd(
      b.CreateFDiv(component(b, position, 0), w), ConstantFP::get(b.getFloatTy(), 1)),
      b.CreateFMul(vw, ConstantFP::get(b.getFloatTy(), .5))));
    Value *y = b.CreateFAdd(vy, b.CreateFMul(b.CreateFSub(
      ConstantFP::get(b.getFloatTy(), 1), b.CreateFDiv(component(b, position, 1), w)),
      b.CreateFMul(vh, ConstantFP::get(b.getFloatTy(), .5))));
    Value *xy = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
    return b.CreateInsertElement(b.CreateInsertElement(xy, x, b.getInt32(0)), y, b.getInt32(1));
  };
  Value *xy0 = screen(start), *xy1 = screen(end);
  Value *dx = b.CreateFSub(component(b, xy1, 0), component(b, xy0, 0));
  Value *dy = b.CreateFSub(component(b, xy1, 1), component(b, xy0, 1));
  Value *length = b.CreateCall(b.GetInsertBlock()->getModule()->getOrInsertFunction(
    "llvm.sqrt.f32", FunctionType::get(b.getFloatTy(), {b.getFloatTy()}, false)),
    {b.CreateFAdd(b.CreateFMul(dx, dx), b.CreateFMul(dy, dy))});
  Value *invLength = b.CreateFDiv(ConstantFP::get(b.getFloatTy(), 1),
    b.CreateSelect(b.CreateFCmpOGT(length, ConstantFP::get(b.getFloatTy(), 0)),
                   length, ConstantFP::get(b.getFloatTy(), 1)));
  Value *ux = b.CreateSelect(b.CreateFCmpOGT(length, ConstantFP::get(b.getFloatTy(), 0)),
                             b.CreateFMul(dx, invLength), ConstantFP::get(b.getFloatTy(), 1));
  Value *uy = b.CreateFMul(dy, invLength);
  Value *atEnd = b.CreateICmpUGE(corner, b.getInt32(4));
  Value *side = b.CreateICmpNE(b.CreateAnd(corner, b.getInt32(1)), b.getInt32(0));
  Value *width = ConstantFP::get(b.getFloatTy(), options.line_mode == SM50_MSAA_LINE_QUADRILATERAL ? .7 : 1.5);
  Value *sign = b.CreateSelect(side, width, b.CreateFNeg(width));
  Value *baseXY = b.CreateSelect(atEnd, xy1, xy0);
  // Caps use constant endpoint attributes. They are separate triangles from
  // the center: their conservative raster extent cannot stretch interpolation
  // within the original clipped segment, even across a perspective horizon.
  Value *outer = b.CreateOr(b.CreateICmpULT(corner, b.getInt32(2)),
                             b.CreateICmpUGE(corner, b.getInt32(6)));
  Value *cap = b.CreateSelect(outer, b.CreateSelect(atEnd, width, b.CreateFNeg(width)),
                              ConstantFP::get(b.getFloatTy(), 0));
  Value *basePosition = b.CreateSelect(atEnd, end, start);
  Value *baseW = b.CreateSelect(atEnd, endW, startW);
  Value *offsetX = b.CreateFAdd(b.CreateFMul(b.CreateFNeg(uy), sign), b.CreateFMul(ux, cap));
  Value *offsetY = b.CreateFAdd(b.CreateFMul(ux, sign), b.CreateFMul(uy, cap));
  Value *px = b.CreateFAdd(component(b, baseXY, 0), offsetX);
  Value *py = b.CreateFAdd(component(b, baseXY, 1), offsetY);
  // Screen pixels -> clip coordinates at the unchanged endpoint w and depth.
  Value *clipX = b.CreateFMul(b.CreateFSub(b.CreateFMul(
    b.CreateFDiv(b.CreateFSub(px, vx), vw), ConstantFP::get(b.getFloatTy(), 2)),
    ConstantFP::get(b.getFloatTy(), 1)), baseW);
  Value *clipY = b.CreateFMul(b.CreateFSub(ConstantFP::get(b.getFloatTy(), 1),
    b.CreateFMul(b.CreateFDiv(b.CreateFSub(py, vy), vh), ConstantFP::get(b.getFloatTy(), 2))), baseW);
  Value *position = b.CreateInsertElement(b.CreateInsertElement(basePosition, clipX, b.getInt32(0)),
                                           clipY, b.getInt32(1));
  Value *outside = ConstantVector::get({ConstantFP::get(b.getFloatTy(), -2),
    ConstantFP::get(b.getFloatTy(), -2), ConstantFP::get(b.getFloatTy(), 0),
    ConstantFP::get(b.getFloatTy(), 1)});
  valid = b.CreateAnd(valid, b.CreateAnd(b.CreateFCmpOGT(vw, ConstantFP::get(b.getFloatTy(), 0)),
                                           b.CreateFCmpOGT(vh, ConstantFP::get(b.getFloatTy(), 0))));
  position = b.CreateSelect(valid, position, outside);
  Value *result = UndefValue::get(resultType);
  // A clipped perspective varying is linear in homogeneous edge t; a
  // no-perspective varying is linear in screen-edge t = t*w1 / w(t).
  auto screenT = [&](Value *t) -> Value * {
    return b.CreateFDiv(b.CreateFMul(t, component(b, p1, 3)),
      component(b, lineBlend(b, p0, p1, t), 3));
  };
  for (unsigned i = 0; i < native->getNumElements(); ++i) {
    Value *field = nullptr;
    auto *kind = cast<MDString>(cast<MDTuple>(outputs->getOperand(i))->getOperand(0));
    if (i == positionField) field = position;
    else if (kind->getString() == "air.viewport_array_index") field = emittedViewport;
    else if (flat(options, *cast<MDNode>(outputs->getOperand(i))))
      field = b.CreateExtractValue(a, i);
    else {
      Value *t = nonPerspective(options, *cast<MDNode>(outputs->getOperand(i)))
        ? b.CreateSelect(atEnd, screenT(hi), screenT(lo)) : b.CreateSelect(atEnd, hi, lo);
      field = lineBlend(b, b.CreateExtractValue(a, i), b.CreateExtractValue(z, i), t);
    }
    result = b.CreateInsertValue(result, field, i);
  }
  unsigned n = native->getNumElements();
  result = b.CreateInsertValue(result, basePosition, n);
  result = b.CreateInsertValue(result, xy0, n + 1);
  result = b.CreateInsertValue(result, xy1, n + 2);
  Value *interval = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  interval = b.CreateInsertElement(interval, lo, b.getInt32(0));
  interval = b.CreateInsertElement(interval, hi, b.getInt32(1));
  result = b.CreateInsertValue(result, interval, n + 3);
  Value *depth = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  depth = b.CreateInsertElement(depth, b.CreateFDiv(component(b, start, 2), startW), b.getInt32(0));
  depth = b.CreateInsertElement(depth, b.CreateFDiv(component(b, end, 2), endW), b.getInt32(1));
  result = b.CreateInsertValue(result, depth, n + 4);
  Value *carrier = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  carrier = b.CreateInsertElement(carrier, px, b.getInt32(0));
  carrier = b.CreateInsertElement(carrier, py, b.getInt32(1));
  result = b.CreateInsertValue(result, carrier, n + 5);
  return result;
}

constexpr unsigned kConservativeClipVectors = 6;

/*
 * Conservative triangle replay (SM50_SHADER_D3D12_CONSERVATIVE_RASTER). `encoded` = 16 * slot + corner of a
 * 24-word slot (18 fan corners, then the 3 original ordinals in D3D order). d3dalloy.conservative.corner returns the
 * affine plane weights of the corner and d3dalloy.conservative.corner_position its clip position
 * (air_conservative.metal); every other output is the affine combination of the three captured outputs, so
 * perspective-correct native interpolation is the exact plane extrapolation. Flat and integral outputs keep the
 * leading vertex. The primitive's clip positions, clip distances and flags are flat
 * outputs for msaa_gather_conservative_depth.
 */
Value *expandedConservative(IRBuilder<> &b, Value *encoded, Value *records, Value *indices,
                            Value *bounds, StructType *native, StructType *resultType,
                            unsigned positionField, MDTuple *outputs, unsigned stride,
                            const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options) {
  Module &module = *b.GetInsertBlock()->getModule();
  Value *slot = b.CreateUDiv(encoded, b.getInt32(16));
  Value *corner = b.CreateURem(encoded, b.getInt32(16));
  Value *indexBase = b.CreateMul(b.CreateZExt(slot, b.getInt64Ty()), b.getInt64(24));
  Value *captured[3];
  for (unsigned v = 0; v < 3; ++v) {
    Value *ordinal = b.CreateLoad(b.getInt32Ty(), b.CreateGEP(b.getInt32Ty(), indices,
      b.CreateAdd(indexBase, b.getInt64(18 + v))));
    Value *word = b.CreateMul(b.CreateZExt(ordinal, b.getInt64Ty()), b.getInt64(stride));
    auto *value = b.CreateAlignedLoad(native, b.CreateBitCast(b.CreateGEP(b.getInt32Ty(), records, word),
      PointerType::get(native, unsigned(air::AddressSpace::device))), Align(4));
    if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
      value->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
    captured[v] = value;
  }
  Value *p[3];
  for (unsigned v = 0; v < 3; ++v) p[v] = b.CreateExtractValue(captured[v], positionField);
  // Viewport of the leading vertex, as msaa_primitive_bounds selects it (view-instance offset included).
  auto loadBound = [&](unsigned index) -> Value * {
    return b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), bounds, index));
  };
  Value *viewportCount = loadBound(5);
  Value *viewport = b.getInt32(0);
  for (unsigned i = 0; i < outputs->getNumOperands(); ++i)
    if (cast<MDString>(cast<MDTuple>(outputs->getOperand(i))->getOperand(0))->getString() ==
        "air.viewport_array_index")
      viewport = b.CreateExtractValue(captured[0], i);
  Value *viewOffsets = b.CreateICmpNE(loadBound(142), b.getInt32(0));
  Value *rawViewport = viewport;
  Value *offsetViewport = b.CreateAdd(viewport, loadBound(140));
  offsetViewport = b.CreateSelect(b.CreateICmpULT(offsetViewport, viewportCount), offsetViewport, b.getInt32(0));
  viewport = b.CreateSelect(viewOffsets, offsetViewport, viewport);
  viewport = b.CreateSelect(b.CreateICmpULT(viewport, viewportCount), viewport,
                            b.CreateSub(viewportCount, b.getInt32(1)));
  viewport = b.CreateSelect(b.CreateICmpULT(viewport, b.getInt32(16)), viewport, b.getInt32(0));
  Value *emittedViewport = b.CreateSelect(viewOffsets, rawViewport, viewport);
  Value *floats = b.CreateBitCast(bounds, PointerType::get(b.getFloatTy(), unsigned(air::AddressSpace::constant)));
  Value *viewportRect = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 4));
  for (unsigned i = 0; i < 4; ++i)
    viewportRect = b.CreateInsertElement(viewportRect, b.CreateLoad(b.getFloatTy(), b.CreateGEP(b.getFloatTy(),
      floats, b.CreateAdd(b.CreateAdd(b.CreateMul(viewport, b.getInt32(4)), b.getInt32(12)), b.getInt32(i)))),
      b.getInt32(i));
  // Clip distances (native air.clip_distance array) and cull distances (user float4 rows) of every vertex.
  auto *float4Type = FixedVectorType::get(b.getFloatTy(), 4);
  SmallVector<Value *, 24> clip(24, ConstantFP::get(b.getFloatTy(), 0));
  SmallVector<Value *, 24> cull;
  unsigned clipCount = 0;
  for (unsigned i = 0; i < outputs->getNumOperands(); ++i) {
    auto *field = cast<MDTuple>(outputs->getOperand(i));
    auto kind = cast<MDString>(field->getOperand(0))->getString();
    auto *name = field->getNumOperands() > 1 ? dyn_cast<MDString>(field->getOperand(1)) : nullptr;
    if (kind == "air.clip_distance") {
      auto *type = dyn_cast<ArrayType>(native->getElementType(i));
      if (!type) continue;
      clipCount = std::min<unsigned>(type->getNumElements(), 8);
      for (unsigned v = 0; v < 3; ++v)
        for (unsigned k = 0; k < clipCount; ++k)
          clip[v * 8 + k] = b.CreateExtractValue(b.CreateExtractValue(captured[v], i), k);
    } else if (kind == "air.vertex_output" && name &&
               name->getString().startswith("user(semantic_sv_culldistance_") &&
               native->getElementType(i) == float4Type) {
      for (unsigned c = 0; c < 4; ++c) {
        Value *outside = b.getTrue();
        for (unsigned v = 0; v < 3; ++v)
          outside = b.CreateAnd(outside, b.CreateFCmpOLT(component(b,
            b.CreateExtractValue(captured[v], i), c), ConstantFP::get(b.getFloatTy(), 0)));
        cull.push_back(outside);
      }
    }
  }
  // A culled primitive emits an all-zero corner, which the helper also returns for any rejected primitive.
  Value *culled = b.getFalse();
  for (auto *outside : cull) culled = b.CreateOr(culled, outside);
  // Gather flags: bit 3 depth clipping, bits 4-7 the clip distance count. Corner flags: bit 4 depth clipping.
  const unsigned flags = (options.depth_clip_enable ? 8u : 0u) | (clipCount << 4);
  auto *cornerType = FunctionType::get(float4Type, {float4Type, float4Type, float4Type, float4Type, b.getInt32Ty()},
                                       false);
  Value *cornerFlags = b.CreateOr(corner, b.getInt32(options.depth_clip_enable ? 16u : 0u));
  cornerFlags = b.CreateOr(cornerFlags, b.getInt32(options.front_counter_clockwise ? 32u : 0u));
  Value *weights = b.CreateCall(module.getOrInsertFunction("d3dalloy.conservative.corner", cornerType),
                                {p[0], p[1], p[2], viewportRect, cornerFlags});
  Value *cornerPosition = b.CreateCall(module.getOrInsertFunction("d3dalloy.conservative.corner_position", cornerType),
                                       {p[0], p[1], p[2], viewportRect, cornerFlags});
  Value *valid = b.CreateAnd(b.CreateNot(culled),
    b.CreateFCmpOEQ(component(b, weights, 3), ConstantFP::get(b.getFloatTy(), 1)));
  Value *alpha[3] = {component(b, weights, 0), component(b, weights, 1), component(b, weights, 2)};
  // Affine combination of the three captured values. Integral (flat) components use the leading vertex.
  std::function<Value *(Value *, Value *, Value *)> combine = [&](Value *a, Value *c, Value *d) -> Value * {
    Type *type = a->getType();
    if (type->isFloatingPointTy() || (type->isVectorTy() && type->getScalarType()->isFloatingPointTy())) {
      auto weight = [&](Value *w) -> Value * {
        Value *scalar = b.CreateFPCast(w, type->getScalarType());
        if (auto *vector = dyn_cast<FixedVectorType>(type))
          return b.CreateVectorSplat(vector->getNumElements(), scalar);
        return scalar;
      };
      return b.CreateFAdd(b.CreateFAdd(b.CreateFMul(a, weight(alpha[0])), b.CreateFMul(c, weight(alpha[1]))),
                          b.CreateFMul(d, weight(alpha[2])));
    }
    if (auto *array = dyn_cast<ArrayType>(type)) {
      Value *out = UndefValue::get(type);
      for (unsigned i = 0; i < array->getNumElements(); ++i)
        out = b.CreateInsertValue(out, combine(b.CreateExtractValue(a, i), b.CreateExtractValue(c, i),
                                               b.CreateExtractValue(d, i)), i);
      return out;
    }
    return a;
  };
  Value *result = UndefValue::get(resultType);
  Value *outside = ConstantVector::get({ConstantFP::get(b.getFloatTy(), -2),
    ConstantFP::get(b.getFloatTy(), -2), ConstantFP::get(b.getFloatTy(), 0),
    ConstantFP::get(b.getFloatTy(), 1)});
  Value *position = nullptr;
  for (unsigned i = 0; i < native->getNumElements(); ++i) {
    auto *kind = cast<MDString>(cast<MDTuple>(outputs->getOperand(i))->getOperand(0));
    Value *field;
    if (kind->getString() == "air.viewport_array_index")
      field = emittedViewport;
    else if (kind->getString() == "air.clip_distance") {
      // The exact clip-distance test is part of the coverage polygon; native clipping would cut covered pixels.
      Type *type = native->getElementType(i);
      if (auto *array = dyn_cast<ArrayType>(type))
        field = ConstantArray::get(array, SmallVector<Constant *, 8>(array->getNumElements(),
          ConstantFP::get(array->getElementType(), 1.0)));
      else
        field = ConstantFP::get(type, 1.0);
    } else if (flat(options, *cast<MDNode>(outputs->getOperand(i))))
      field = b.CreateExtractValue(captured[0], i);
    else
      field = combine(b.CreateExtractValue(captured[0], i), b.CreateExtractValue(captured[1], i),
                      b.CreateExtractValue(captured[2], i));
    if (i == positionField) {
      // Native clipping must not cut the extrapolated plane: depth is clamped by the replay raster state and the
      // exact depth-clip test is part of the coverage polygon.
      field = b.CreateSelect(valid, cornerPosition, outside);
      position = field;
    }
    result = b.CreateInsertValue(result, field, i);
  }
  unsigned n = native->getNumElements();
  result = b.CreateInsertValue(result, position, n);
  for (unsigned v = 0; v < 3; ++v) result = b.CreateInsertValue(result, p[v], n + 1 + v);
  for (unsigned r = 0; r < kConservativeClipVectors; ++r) {
    Value *row = UndefValue::get(float4Type);
    for (unsigned c = 0; c < 4; ++c) row = b.CreateInsertElement(row, clip[r * 4 + c], b.getInt32(c));
    result = b.CreateInsertValue(result, row, n + 4 + r);
  }
  result = b.CreateInsertValue(result, b.getInt32(flags), n + 4 + kConservativeClipVectors);
  Value *screen = UndefValue::get(FixedVectorType::get(b.getFloatTy(), 2));
  for (unsigned axis = 0; axis < 2; ++axis) {
    Value *ndc = b.CreateFDiv(component(b, position, axis), component(b, position, 3));
    Value *unit = axis ? b.CreateFSub(ConstantFP::get(b.getFloatTy(), 1), ndc)
                      : b.CreateFAdd(ndc, ConstantFP::get(b.getFloatTy(), 1));
    screen = b.CreateInsertElement(screen, b.CreateFAdd(component(b, viewportRect, axis),
      b.CreateFMul(b.CreateFMul(unit, component(b, viewportRect, axis + 2)),
        ConstantFP::get(b.getFloatTy(), .5))), b.getInt32(axis));
  }
  return b.CreateInsertValue(result, screen, n + 5 + kConservativeClipVectors);
}

MDNode *conservativeOutput(LLVMContext &context, StringRef name, StringRef type) {
  return MDTuple::get(context, {
    MDString::get(context, "air.vertex_output"), MDString::get(context, ("user(" + name + ")").str()),
    MDString::get(context, "air.arg_type_name"), MDString::get(context, type),
    MDString::get(context, "air.arg_name"), MDString::get(context, name)
  });
}

// Capture stores the exact packed native AIR return, not a reconstruction from
// DXIL/DXBC registers. Ordinary replay returns it unchanged; line replay clips
// the original pair and expands it without invoking the application VS again.
Error transform(Module &module, const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options, bool capture,
                bool conservative) {
  const bool gpuTileTasks = options.variant & SM50_MSAA_GPU_TILE_TASKS;
  const bool nativeTransport = options.variant == SM50_CAPTURE_VERTEX_NATIVE ||
                               options.variant == SM50_REPLAY_VERTEX_NATIVE;
  if (gpuTileTasks && (capture || (options.variant & ~SM50_MSAA_GPU_TILE_TASKS) != SM50_MSAA_VERTEX_REPLAY))
    return invalid("GPU tile tasks require captured vertex replay");
  if (options.line_mode != SM50_MSAA_LINE_NONE && options.line_mode != SM50_MSAA_LINE_ALIASED &&
      options.line_mode != SM50_MSAA_LINE_ALPHA_AA && options.line_mode != SM50_MSAA_LINE_QUADRILATERAL)
    return invalid("unknown line raster mode");
  if (!capture && options.line_mode != SM50_MSAA_LINE_NONE &&
      (!options.interpolation || options.interpolation->no_perspective_count > 128 ||
       options.interpolation->flat_count > 128))
    return invalid("line replay requires bounded pixel interpolation metadata");
  NamedMDNode *vertices = module.getNamedMetadata("air.vertex");
  if (!vertices || vertices->getNumOperands() != 1)
    return invalid("expected one lowered native vertex entry");
  auto *oldMetadata = vertices->getOperand(0);
  auto *original = dyn_cast<Function>(cast<ValueAsMetadata>(oldMetadata->getOperand(0))->getValue());
  auto *oldInputs = dyn_cast<MDTuple>(oldMetadata->getOperand(2));
  if (!original || !oldInputs || original->arg_size() != oldInputs->getNumOperands())
    return invalid("missing rasterizing vertex return or AIR arguments");
  // SO_NO_RASTERIZED_STREAM permits a VS with no position, including a void
  // native return. Its SO epilogue is still executed exactly once. The private
  // record only needs a dummy position because no raster consumer uses it.
  if (capture && nativeTransport) {
    auto *outputs = dyn_cast<MDTuple>(oldMetadata->getOperand(1));
    bool hasPosition = false;
    if (outputs)
      for (const auto &operand : outputs->operands()) {
        auto *field = dyn_cast<MDNode>(operand.get());
        auto *kind = field && field->getNumOperands() ? dyn_cast<MDString>(field->getOperand(0)) : nullptr;
        hasPosition |= kind && kind->getString() == "air.position";
      }
    if (original->getReturnType()->isVoidTy() || !hasPosition) {
      LLVMContext &context = module.getContext();
      auto *float4 = FixedVectorType::get(Type::getFloatTy(context), 4);
      auto *wrapperType = StructType::get(context, {float4}, true);
      auto originalName = original->getName().str();
      original->setName(originalName + ".so_body");
      auto *wrapper = Function::Create(FunctionType::get(
          wrapperType, original->getFunctionType()->params(), false),
          Function::ExternalLinkage, originalName, module);
      wrapper->setCallingConv(original->getCallingConv());
      IRBuilder<> wrapperBuilder(BasicBlock::Create(context, "entry", wrapper));
      SmallVector<Value *, 20> arguments;
      for (auto &argument : wrapper->args()) arguments.push_back(&argument);
      wrapperBuilder.CreateCall(original, arguments);
      Value *position = Constant::getNullValue(float4);
      position = wrapperBuilder.CreateInsertElement(position, ConstantFP::get(Type::getFloatTy(context), 1),
                                                      wrapperBuilder.getInt32(3));
      wrapperBuilder.CreateRet(wrapperBuilder.CreateInsertValue(UndefValue::get(wrapperType), position, 0));
      original->setLinkage(Function::InternalLinkage);
      SmallVector<Metadata *, 8> fields(oldMetadata->op_begin(), oldMetadata->op_end());
      fields[0] = ValueAsMetadata::get(wrapper);
      fields[1] = MDTuple::get(context, {MDTuple::get(context, {
          MDString::get(context, "air.position"), MDString::get(context, "air.arg_type_name"),
          MDString::get(context, "float4"), MDString::get(context, "air.arg_name"),
          MDString::get(context, "so_private_position")})});
      oldMetadata = MDTuple::get(context, fields);
      vertices->setOperand(0, oldMetadata);
      original = wrapper;
    }
  }
  if (original->getReturnType()->isVoidTy())
    return invalid("missing rasterizing vertex return");
  uint64_t bytes = module.getDataLayout().getTypeAllocSize(original->getReturnType()).getFixedValue();
  if (!bytes || (bytes & 3) || bytes / 4 > UINT32_MAX)
    return invalid("native output cannot be represented as dword records");
  const uint32_t stride = uint32_t(bytes / 4);
  auto *nativeStruct = dyn_cast<StructType>(original->getReturnType());
  auto *oldOutputs = dyn_cast<MDTuple>(oldMetadata->getOperand(1));
  if (!nativeStruct || !oldOutputs || nativeStruct->getNumElements() != oldOutputs->getNumOperands())
    return invalid("native output metadata does not match the return struct");
  SM50_MSAA_VERTEX_LAYOUT layout{
    .record_dwords = stride,
    .position_offset_dwords = ~0u,
    .viewport_index_offset_dwords = ~0u,
    .render_target_array_index_offset_dwords = ~0u,
    .clip_distance_offset_dwords = ~0u,
    .cull_distance_offset_dwords = ~0u,
    .shading_rate_offset_dwords = ~0u,
    .primitive_id_offset_dwords = ~0u,
  };
  auto *structLayout = module.getDataLayout().getStructLayout(nativeStruct);
  unsigned positionField = ~0u, clipField = ~0u;
  for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i) {
    auto *field = dyn_cast<MDTuple>(oldOutputs->getOperand(i));
    auto *kind = field && field->getNumOperands() ? dyn_cast<MDString>(field->getOperand(0)) : nullptr;
    if (!kind) return invalid("malformed native vertex output metadata");
    uint64_t offset = structLayout->getElementOffset(i);
    if ((offset & 3) || offset / 4 > UINT32_MAX)
      return invalid("native vertex output is not dword aligned");
    auto *target = kind->getString() == "air.position" ? &layout.position_offset_dwords
                 : kind->getString() == "air.viewport_array_index" ? &layout.viewport_index_offset_dwords
                 : kind->getString() == "air.render_target_array_index" ? &layout.render_target_array_index_offset_dwords
                 : kind->getString() == "air.clip_distance" ? &layout.clip_distance_offset_dwords
                 : kind->getString() == "air.cull_distance" ? &layout.cull_distance_offset_dwords
                 : nullptr;
    if (target) *target = uint32_t(offset / 4);
    if (kind->getString() == "air.position") positionField = i;
    if (kind->getString() == "air.clip_distance") {
      auto *distances = dyn_cast<ArrayType>(nativeStruct->getElementType(i));
      if (clipField != ~0u || !distances || !distances->getElementType()->isFloatTy() ||
          distances->getNumElements() > 8)
        return invalid("native clip distance output must be one bounded float array");
      clipField = i;
      layout.clip_distance_count = unsigned(distances->getNumElements());
    }
    if (kind->getString() == "air.vertex_output") {
      auto *name = field->getNumOperands() > 1 ? dyn_cast<MDString>(field->getOperand(1)) : nullptr;
      if (name && name->getString() == "user(d3dalloy_primitive_id)")
        layout.primitive_id_offset_dwords = uint32_t(offset / 4);
      if (name && name->getString() == "user(d3dalloy_shading_rate)")
        layout.shading_rate_offset_dwords = uint32_t(offset / 4);

      if (!name || !name->getString().startswith("user(") || layout.user_count == 128)
        return invalid("captured vertex user output map is malformed or exceeds 128 fields");
      uint64_t size = module.getDataLayout().getTypeStoreSize(nativeStruct->getElementType(i)).getFixedValue();
      if (size > UINT32_MAX)
        return invalid("native vertex user output exceeds the fragment record format");
      auto &user = layout.users[layout.user_count++];
      user.hash = userHash(name->getString());
      user.offset_dwords = uint32_t(offset / 4);
      user.size_bytes = uint32_t(size);
      if (name->getString().startswith("user(semantic_sv_culldistance_"))
        user.flags |= SM50_MSAA_CAPTURED_USER_CULL_DISTANCE;
    }
  }
  // Native vertex/fragment linkage carries 124 user scalars. Reserve room for
  // replay helpers and reconstruct excess fields from their exact records.
  uint32_t user_scalars = 0;
  for (uint32_t i = 0; i < layout.user_count; ++i)
    user_scalars += (layout.users[i].size_bytes + 3) / 4;
  for (uint32_t i = layout.user_count; !nativeTransport && i-- && user_scalars > 116;) {
    layout.users[i].flags |= 1;
    ++layout.spill_count;
    user_scalars -= (layout.users[i].size_bytes + 3) / 4;
  }
  if (layout.position_offset_dwords == ~0u)
    return invalid("rasterizing vertex shader has no position output");
  if (nativeStruct->getElementType(positionField) != FixedVectorType::get(Type::getFloatTy(module.getContext()), 4))
    return invalid("native vertex position must be float4");
  // Task bounds are applied by native scissoring. Re-clipping the captured geometry here would create
  // newly snapped edge endpoints and change sample coverage, even with a guard around the tile.
  layout.flags |= SM50_MSAA_VERTEX_GPU_TILE_TASKS_SUPPORTED;
  if (options.layout_out) *options.layout_out = layout;

  unsigned vertexArg = ~0u, baseVertexArg = ~0u, instanceArg = ~0u, baseInstanceArg = ~0u;
  unsigned controlArg = ~0u, indicesArg = ~0u, frontierArg = ~0u;
  for (unsigned i = 0; i < oldInputs->getNumOperands(); ++i) {
    auto kind = inputKind(*cast<MDNode>(oldInputs->getOperand(i)));
    if (capture && kind == "air.vertex_input")
      return invalid("compute capture requires IA input pulling, not native vertex attributes");
    if (kind == "air.vertex_id") vertexArg = i;
    else if (kind == "air.base_vertex") baseVertexArg = i;
    else if (kind == "air.instance_id") instanceArg = i;
    else if (kind == "air.base_instance") baseInstanceArg = i;
    else if (kind == "air.buffer") {
      auto *fields = cast<MDNode>(oldInputs->getOperand(i));
      for (unsigned j = 2; j + 1 < fields->getNumOperands(); ++j) {
        auto *tag = dyn_cast<MDString>(fields->getOperand(j));
        if (!tag || tag->getString() != "air.location_index") continue;
        auto *location = dyn_cast<ConstantAsMetadata>(fields->getOperand(j + 1));
        if (!location) break;
        auto *value = dyn_cast<ConstantInt>(location->getValue());
        if (!value) break;
        if (value->getZExtValue() == SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL) controlArg = i;
        if (value->getZExtValue() == SM50_BINDING_INDEX_VERTEX_INDICES) indicesArg = i;
        if (value->getZExtValue() == SM50_BINDING_INDEX_CAPTURE_FRONTIER) frontierArg = i;
        break;
      }
    }
  }
  if (vertexArg == ~0u || baseVertexArg == ~0u || instanceArg == ~0u || baseInstanceArg == ~0u)
    return invalid("native vertex IDs are missing");
  if (capture && (controlArg == ~0u || indicesArg == ~0u))
    return invalid("capture requires IA pulling and SO capture control");

  SmallVector<Type *, 20> parameters;
  SmallVector<Metadata *, 20> metadata;
  SmallVector<unsigned, 20> originalArgs;
  LLVMContext &context = module.getContext();
  if (capture) {
    for (unsigned i = 0; i < original->arg_size(); ++i) {
      if (i == vertexArg || i == baseVertexArg || i == instanceArg || i == baseInstanceArg)
        continue;
      originalArgs.push_back(i);
      parameters.push_back(original->getArg(i)->getType());
      metadata.push_back(reindex(*cast<MDNode>(oldInputs->getOperand(i)), metadata.size(), context));
    }
  }
  unsigned recordArg = parameters.size();
  parameters.push_back(PointerType::get(Type::getInt32Ty(context), unsigned(air::AddressSpace::device)));
  metadata.push_back(air::BuildBufferArgumentMetadata(recordArg, recordsBinding(capture), context,
                                                      module.getDataLayout()));
  unsigned boundsArg = ~0u, replayIndicesArg = ~0u;
  const bool line = !capture && !nativeTransport && options.line_mode != SM50_MSAA_LINE_NONE;
  const bool conservativeTriangles = !capture && !nativeTransport && !line && conservative;
  if (line || conservativeTriangles) {
    boundsArg = parameters.size();
    parameters.push_back(PointerType::get(Type::getInt32Ty(context), unsigned(air::AddressSpace::constant)));
    metadata.push_back(air::BuildBufferArgumentMetadata(boundsArg,
      lineBinding(23, true, "msaa_line_bounds"), context, module.getDataLayout()));
    replayIndicesArg = parameters.size();
    parameters.push_back(PointerType::get(Type::getInt32Ty(context), unsigned(air::AddressSpace::device)));
    metadata.push_back(air::BuildBufferArgumentMetadata(replayIndicesArg,
      lineBinding(24, false, "msaa_line_indices"), context, module.getDataLayout()));
  }
  unsigned positionArg = parameters.size();
  if (capture) {
    parameters.push_back(FixedVectorType::get(Type::getInt32Ty(context), 3));
    metadata.push_back(builtin(positionArg, "air.thread_position_in_grid", "uint3",
                               "mtl_thread_position_in_grid", context));
  } else {
    parameters.push_back(Type::getInt32Ty(context));
    metadata.push_back(builtin(positionArg, "air.vertex_id", "uint", "mtl_vertex_id", context));
  }

  auto originalName = original->getName().str();
  original->setName(originalName + ".msaa_body");
  auto nativeFields = nativeStruct->elements();
  SmallVector<Type *, 12> replayFields(nativeFields.begin(), nativeFields.end());
  if (!capture && !nativeTransport) {
    replayFields.push_back(nativeStruct->getElementType(positionField));
    if (line) {
      replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
      replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
      replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
      replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
      replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
    }
    if (conservativeTriangles) {
      for (unsigned i = 0; i < 3 + kConservativeClipVectors; ++i)
        replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 4));
      replayFields.push_back(Type::getInt32Ty(context));
      replayFields.push_back(FixedVectorType::get(Type::getFloatTy(context), 2));
    }
  }
  auto *replayReturn = StructType::get(context, replayFields, true);
  SmallVector<unsigned, 40> keptFields;
  SmallVector<Type *, 40> compactFields;
  for (unsigned i = 0; i < replayFields.size(); ++i) {
    bool spilled = false;
    if (i < oldOutputs->getNumOperands()) {
      auto *field = cast<MDNode>(oldOutputs->getOperand(i));
      auto *name = field->getNumOperands() > 1 ? dyn_cast<MDString>(field->getOperand(1)) : nullptr;
      if (name) for (uint32_t u = 0; u < layout.user_count; ++u)
        spilled |= (layout.users[u].flags & 1) && layout.users[u].hash == userHash(name->getString());
    }
    if (!spilled) { keptFields.push_back(i); compactFields.push_back(replayFields[i]); }
  }
  auto *compactReturn = StructType::get(context, compactFields, true);
  auto *entry = Function::Create(FunctionType::get(capture ? Type::getVoidTy(context)
                                                          : nativeTransport ? nativeStruct : compactReturn,
                                                   parameters, false),
                                 Function::ExternalLinkage, originalName, module);
  entry->setCallingConv(original->getCallingConv());
  auto *first = BasicBlock::Create(context, "entry", entry);
  IRBuilder<> builder(first);
  auto *records = entry->getArg(recordArg);
  auto *ordinal = capture ? builder.CreateExtractElement(entry->getArg(positionArg), builder.getInt32(0))
                          : static_cast<Value *>(entry->getArg(positionArg));
  Value *frontier = nullptr;
  if (capture && frontierArg != ~0u)
    for (unsigned i = 0; i < originalArgs.size(); ++i)
      if (originalArgs[i] == frontierArg) frontier = entry->getArg(i);
  Value *recordOrdinal = ordinal;
  if (frontier)
    recordOrdinal = builder.CreateAdd(ordinal, dxbc::load_capture_frontier(builder, frontier,
        offsetof(SM50_CAPTURE_FRONTIER_CONTROL, producer_record_base)));
  auto *offset = builder.CreateMul(builder.CreateZExt(recordOrdinal, builder.getInt64Ty()), builder.getInt64(stride));
  auto *record = builder.CreateBitCast(builder.CreateGEP(builder.getInt32Ty(), records, offset),
                                      PointerType::get(original->getReturnType(), unsigned(air::AddressSpace::device)));

  if (capture) {
    // The existing capture argument is always SM50_SO_CAPTURE_CONTROL. The
    // runtime dispatches record_count threads, including rounded-up extras.
    unsigned controlIndex = ~0u, indicesIndex = ~0u;
    for (unsigned i = 0; i < originalArgs.size(); ++i) {
      if (originalArgs[i] == controlArg) controlIndex = i;
      if (originalArgs[i] == indicesArg) indicesIndex = i;
    }
    Value *control = entry->getArg(controlIndex);
    auto load = [&](unsigned word) -> Value * {
      return builder.CreateLoad(builder.getInt32Ty(),
                                builder.CreateConstInBoundsGEP1_32(builder.getInt32Ty(), control, word));
    };
    auto *positions = load(offsetof(SM50_SO_CAPTURE_CONTROL, positions_per_instance) / 4);
    Value *count = load(offsetof(SM50_SO_CAPTURE_CONTROL, record_count) / 4);
    Value *validCount = builder.CreateAnd(builder.CreateICmpULT(ordinal, count),
                                          builder.CreateICmpNE(positions, builder.getInt32(0)));
    if (frontier) {
      count = dxbc::load_capture_frontier(builder, frontier,
          offsetof(SM50_CAPTURE_FRONTIER_CONTROL, producer_count));
      validCount = builder.CreateAnd(builder.CreateICmpULT(ordinal, count),
          builder.CreateICmpULT(recordOrdinal, dxbc::load_capture_frontier(builder, frontier,
              offsetof(SM50_CAPTURE_FRONTIER_CONTROL, local_key_count))));
    }
    auto *validBlock = BasicBlock::Create(context, "valid_ordinal", entry);
    auto *done = BasicBlock::Create(context, "done", entry);
    builder.CreateCondBr(validCount, validBlock, done);
    builder.SetInsertPoint(validBlock);
    auto *positionInInstance = frontier ? ordinal : builder.CreateURem(ordinal, positions);
    auto *activePositions = frontier ? count :
        load(offsetof(SM50_MSAA_VERTEX_CAPTURE_CONTROL, active_positions_per_instance) / 4);
    auto *activeBlock = BasicBlock::Create(context, "valid_position", entry);
    builder.CreateCondBr(builder.CreateICmpULT(positionInInstance, activePositions), activeBlock, done);
    builder.SetInsertPoint(activeBlock);
    auto *key = builder.CreateAdd(positionInInstance, frontier ?
        dxbc::load_capture_frontier(builder, frontier, offsetof(SM50_CAPTURE_FRONTIER_CONTROL, first_position)) :
        load(offsetof(SM50_SO_CAPTURE_CONTROL, first_position) / 4));
    auto *instance = frontier ? dxbc::load_capture_frontier(builder, frontier,
        offsetof(SM50_CAPTURE_FRONTIER_CONTROL, instance_base)) : builder.CreateUDiv(ordinal, positions);
    auto *indexSize = load(offsetof(SM50_SO_CAPTURE_CONTROL, index_size) / 4);
    auto *indexed = builder.CreateICmpNE(indexSize, builder.getInt32(0));
    auto *word = builder.CreateAdd(load(offsetof(SM50_SO_CAPTURE_CONTROL, index_first) / 4),
                                   builder.CreateSelect(builder.CreateICmpEQ(indexSize, builder.getInt32(4)),
                                                        builder.CreateShl(key, 1), key));
    auto *indexWords = entry->getArg(indicesIndex);
    auto *indexBlock = BasicBlock::Create(context, "check_index", entry);
    auto *shadeBlock = BasicBlock::Create(context, "shade", entry);
    builder.CreateCondBr(indexed, indexBlock, shadeBlock);
    builder.SetInsertPoint(indexBlock);
    auto *viewBlock = BasicBlock::Create(context, "read_index_view", entry);
    auto *rawBlock = BasicBlock::Create(context, "read_raw_index", entry);
    builder.CreateCondBr(builder.CreateICmpEQ(indexSize, builder.getInt32(8)), viewBlock, rawBlock);
    builder.SetInsertPoint(viewBlock);
    auto *viewIndex = dxbc::load_mesh_index_buffer_view(builder, indexWords,
      builder.CreateAdd(key, load(offsetof(SM50_SO_CAPTURE_CONTROL, index_first) / 4)));
    auto *viewWords = builder.CreateBitCast(indexWords,
      builder.getInt32Ty()->getPointerTo(cast<PointerType>(indexWords->getType())->getAddressSpace()));
    auto *viewFormat = builder.CreateLoad(builder.getInt32Ty(),
      builder.CreateConstGEP1_32(builder.getInt32Ty(), viewWords, 3));
    auto *viewRestart = builder.CreateSelect(builder.CreateICmpEQ(viewFormat, builder.getInt32(42)),
                                            builder.getInt32(~0u), builder.getInt32(0xffff));
    auto *viewEnd = builder.GetInsertBlock();
    auto *joinIndex = BasicBlock::Create(context, "join_index", entry);
    builder.CreateBr(joinIndex);
    builder.SetInsertPoint(rawBlock);
    auto *lo = builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(),
      builder.CreateGEP(builder.getInt16Ty(), indexWords, builder.CreateZExt(word, builder.getInt64Ty()))),
      builder.getInt32Ty());
    auto *isWide = builder.CreateICmpEQ(indexSize, builder.getInt32(4));
    auto *rawRestart = builder.CreateSelect(isWide, builder.getInt32(~0u), builder.getInt32(0xffff));
    auto *highBlock = BasicBlock::Create(context, "read_high_index", entry);
    builder.CreateCondBr(isWide, highBlock, joinIndex);
    builder.SetInsertPoint(highBlock);
    auto *hi = builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(),
      builder.CreateGEP(builder.getInt16Ty(), indexWords,
        builder.CreateZExt(builder.CreateAdd(word, builder.getInt32(1)), builder.getInt64Ty()))), builder.getInt32Ty());
    auto *wideIndex = builder.CreateOr(lo, builder.CreateShl(hi, 16));
    builder.CreateBr(joinIndex);
    builder.SetInsertPoint(joinIndex);
    auto *index = builder.CreatePHI(builder.getInt32Ty(), 3);
    index->addIncoming(lo, rawBlock);
    index->addIncoming(wideIndex, highBlock);
    index->addIncoming(viewIndex, viewEnd);
    auto *restart = builder.CreatePHI(builder.getInt32Ty(), 3);
    restart->addIncoming(rawRestart, rawBlock);
    restart->addIncoming(rawRestart, highBlock);
    restart->addIncoming(viewRestart, viewEnd);
    // List topologies can legitimately reference the maximum index; strip cut
    // semantics are topology dependent, not a property of the index format.
    auto *stripCut = builder.CreateICmpNE(
      load(offsetof(SM50_MSAA_VERTEX_CAPTURE_CONTROL, strip_cut_enabled) / 4), builder.getInt32(0));
    builder.CreateCondBr(builder.CreateAnd(stripCut, builder.CreateICmpEQ(index, restart)),
                         done, shadeBlock);
    builder.SetInsertPoint(shadeBlock);
    auto *base = builder.CreateSelect(indexed, frontier ?
      load(offsetof(SM50_SO_CAPTURE_CONTROL, base_vertex) / 4) : builder.getInt32(0),
      load(offsetof(SM50_SO_CAPTURE_CONTROL, padding) / 4 + 1));
    auto *vertex = builder.CreateAdd(key, base);
    auto *startInstance = load(offsetof(SM50_SO_CAPTURE_CONTROL, padding) / 4);
    SmallVector<Value *, 20> callArgs(original->arg_size());
    for (unsigned i = 0; i < originalArgs.size(); ++i)
      callArgs[originalArgs[i]] = entry->getArg(i);
    callArgs[vertexArg] = vertex;
    callArgs[baseVertexArg] = base;
    callArgs[instanceArg] = builder.CreateAdd(instance, startInstance);
    callArgs[baseInstanceArg] = startInstance;
    auto *nativeOutput = builder.CreateCall(original, callArgs);
    auto *store = builder.CreateStore(nativeOutput, record);
    store->setAlignment(Align(4));
    // The packed native return is private capture transport, not an application UAV.
    if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
      store->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
    original->setLinkage(Function::InternalLinkage);
    auto *kernel = module.getOrInsertNamedMetadata("air.kernel");
    kernel->addOperand(entryMetadata(*entry, *oldMetadata, metadata, false));
  } else {
    if (nativeTransport) {
      auto *value = builder.CreateAlignedLoad(nativeStruct, record, Align(4));
      if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
        value->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
      builder.CreateRet(value);
      // Keep every original return member and its metadata, including native
      // distance/viewport/layer outputs. Native replay needs no MSAA varyings.
      vertices->setOperand(0, entryMetadata(*entry, *oldMetadata, metadata, true));
      original->dropAllReferences();
      original->eraseFromParent();
      return Error::success();
    }
    Value *result;
    if (line) {
      result = expandedLine(builder, ordinal, records, entry->getArg(replayIndicesArg),
                            entry->getArg(boundsArg), nativeStruct, replayReturn,
                            positionField, oldOutputs, stride, options);
    } else if (conservativeTriangles) {
      result = expandedConservative(builder, ordinal, records, entry->getArg(replayIndicesArg),
                                    entry->getArg(boundsArg), nativeStruct, replayReturn,
                                    positionField, oldOutputs, stride, options);
    } else {
      auto *value = builder.CreateAlignedLoad(original->getReturnType(), record, Align(4));
      if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
        value->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
      result = UndefValue::get(replayReturn);
      for (unsigned i = 0; i < nativeStruct->getNumElements(); ++i)
        result = builder.CreateInsertValue(result, builder.CreateExtractValue(value, i), i);
      auto *clipPosition = builder.CreateExtractValue(value, positionField);
      result = builder.CreateInsertValue(result, clipPosition, nativeStruct->getNumElements());
    }
    // The capture record retains raw IEEE distances, but no native rasterized
    // user varying may carry NaN or infinity into Metal interpolation.
    for (unsigned i = 0; i < oldOutputs->getNumOperands(); ++i) {
      auto *field = cast<MDTuple>(oldOutputs->getOperand(i));
      auto *kind = dyn_cast<MDString>(field->getOperand(0));
      auto *name = field->getNumOperands() > 1 ? dyn_cast<MDString>(field->getOperand(1)) : nullptr;
      if (!kind || kind->getString() != "air.vertex_output" || !name ||
          (!name->getString().startswith("user(semantic_sv_clipdistance_") &&
            !name->getString().startswith("user(semantic_sv_culldistance_"))) continue;
      auto *type = nativeStruct->getElementType(i);
      if (type != FixedVectorType::get(builder.getFloatTy(), 4))
        return invalid("distance replay output must be float4");
      auto *raw = builder.CreateExtractValue(result, i);
      auto *absolute = builder.CreateAnd(builder.CreateBitCast(raw,
        FixedVectorType::get(builder.getInt32Ty(), 4)),
        builder.CreateVectorSplat(4, builder.getInt32(0x7fffffffu)));
      auto *nonfinite = builder.CreateICmpUGE(absolute,
        builder.CreateVectorSplat(4, builder.getInt32(0x7f800000u)));
      result = builder.CreateInsertValue(result, builder.CreateSelect(nonfinite,
        Constant::getNullValue(type), raw), i);
    }
    Value *compact = UndefValue::get(compactReturn);
    for (unsigned i = 0; i < keptFields.size(); ++i)
      compact = builder.CreateInsertValue(compact, builder.CreateExtractValue(result, keptFields[i]), i);
    builder.CreateRet(compact);
    auto *clipOutput = MDTuple::get(context, {
      MDString::get(context, "air.vertex_output"), MDString::get(context, "user(msaa_clip_position)"),
      MDString::get(context, "air.arg_type_name"), MDString::get(context, "float4"),
      MDString::get(context, "air.arg_name"), MDString::get(context, "msaa_clip_position")
    });
    SmallVector<Metadata *, 8> extra{clipOutput};
    if (line) {
      extra.push_back(lineOutput(context, "msaa_line_start"));
      extra.push_back(lineOutput(context, "msaa_line_end"));
      extra.push_back(lineOutput(context, "msaa_line_clip_interval"));
      extra.push_back(lineOutput(context, "msaa_line_depth_pair"));
      extra.push_back(lineOutput(context, "msaa_carrier_screen"));
    }
    if (conservativeTriangles) {
      for (const char *name : {"msaa_conservative_p0", "msaa_conservative_p1", "msaa_conservative_p2",
                               "msaa_conservative_clip0", "msaa_conservative_clip1", "msaa_conservative_clip2",
                               "msaa_conservative_clip3", "msaa_conservative_clip4", "msaa_conservative_clip5"})
        extra.push_back(conservativeOutput(context, name, "float4"));
      extra.push_back(conservativeOutput(context, "msaa_conservative_flags", "uint"));
      extra.push_back(conservativeOutput(context, "msaa_carrier_screen", "float2"));
    }
    auto *finalMetadata = entryMetadata(*entry, *oldMetadata, metadata, true, extra);
    auto *fullOutputs = cast<MDNode>(finalMetadata->getOperand(1));
    SmallVector<Metadata *, 40> compactOutputs;
    for (unsigned i : keptFields) compactOutputs.push_back(fullOutputs->getOperand(i));
    SmallVector<Metadata *, 8> finalFields(finalMetadata->op_begin(), finalMetadata->op_end());
    finalFields[1] = MDTuple::get(context, compactOutputs);
    vertices->setOperand(0, MDTuple::get(context, finalFields));
    original->dropAllReferences();
    original->eraseFromParent();
  }
  if (capture)
    vertices->eraseFromParent();
  return Error::success();
}

} // namespace

Error transformVertex(Module &module, const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options, bool conservative) {
  const unsigned variant = options.variant & ~SM50_MSAA_GPU_TILE_TASKS;
  if (variant == SM50_MSAA_VERTEX_CAPTURE || variant == SM50_CAPTURE_VERTEX_NATIVE)
    return transform(module, options, true, false);
  if (variant == SM50_REPLAY_VERTEX_NATIVE)
    return transform(module, options, false, false);
  if (variant == SM50_MSAA_VERTEX_REPLAY) {
    if (auto error = transform(module, options, false, conservative))
      return error;
    if (conservative && options.line_mode == SM50_MSAA_LINE_NONE)
      linkConservative(module);
    return Error::success();
  }
  return invalid("unknown vertex variant");
}

} // namespace d3dalloy::msaa
