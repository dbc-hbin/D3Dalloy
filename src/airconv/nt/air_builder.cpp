#include "air_builder.hpp"
#include "../dxil_converter.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <format>

namespace llvm::air {
Value *
AIRBuilder::CreateAtomicPhaseMetadataLoad(Value *base, uint64_t offset, Type *type) {
  auto *bytes = builder.CreateBitCast(base, builder.getInt8Ty()->getPointerTo(1));
  auto *ptr = builder.CreateGEP(builder.getInt8Ty(), bytes, builder.getInt64(offset));
  auto *result = builder.CreateLoad(type, builder.CreateBitCast(ptr, type->getPointerTo(1)));
  result->setMetadata(dxmt::dxil::kAtomic64Metadata, MDNode::get(getContext(), {}));
  return result;
}

Value *
AIRBuilder::CreateAtomicPhaseTypedView(Value *context, Value *handle) {
  auto load = [&](Value *base, uint64_t offset, Type *type) -> Value * {
    return CreateAtomicPhaseMetadataLoad(base, offset, type);
  };
  auto *viewType = builder.getInt8Ty()->getPointerTo(1);
  auto *lookup = getModule()->getFunction("dxmt.atomic64.typed_view");
  if (!lookup) {
    lookup = Function::Create(FunctionType::get(viewType, {viewType, builder.getInt64Ty()}, false),
                              GlobalValue::InternalLinkage, "dxmt.atomic64.typed_view", getModule());
    lookup->addFnAttr(Attribute::NoUnwind);
    auto saved = builder.saveIP();
    auto *entry = BasicBlock::Create(getContext(), "entry", lookup);
    auto *scan = BasicBlock::Create(getContext(), "scan", lookup);
    auto *test = BasicBlock::Create(getContext(), "test", lookup);
    auto *next = BasicBlock::Create(getContext(), "next", lookup);
    auto *found = BasicBlock::Create(getContext(), "found", lookup);
    auto *absent = BasicBlock::Create(getContext(), "absent", lookup);
    builder.SetInsertPoint(entry);
    auto *count = load(lookup->getArg(0), 44, getIntTy());
    auto *views = builder.CreateIntToPtr(load(lookup->getArg(0), 8, builder.getInt64Ty()), viewType);
    builder.CreateBr(scan);
    builder.SetInsertPoint(scan);
    auto *index = builder.CreatePHI(getIntTy(), 2);
    index->addIncoming(getInt(0), entry);
    builder.CreateCondBr(builder.CreateICmpULT(index, count), test, absent);
    builder.SetInsertPoint(test);
    auto *view = builder.CreateGEP(builder.getInt8Ty(), views,
        builder.CreateMul(builder.CreateZExt(index, builder.getInt64Ty()), builder.getInt64(64)));
    builder.CreateCondBr(builder.CreateICmpEQ(load(view, 0, builder.getInt64Ty()), lookup->getArg(1)), found, next);
    builder.SetInsertPoint(next);
    auto *following = builder.CreateAdd(index, getInt(1));
    index->addIncoming(following, next);
    builder.CreateBr(scan);
    builder.SetInsertPoint(found);
    builder.CreateRet(view);
    builder.SetInsertPoint(absent);
    builder.CreateRet(ConstantPointerNull::get(viewType));
    builder.restoreIP(saved);
  }
  return builder.CreateCall(lookup, {context, builder.CreatePtrToInt(handle, builder.getInt64Ty())});
}

// Typed views expose ResourceIDs, never linear native texture GPU addresses.
// The phase's immutable layout snapshot supplies canonical roots for every byte.
std::pair<Value *, Value *>
AIRBuilder::CreateAtomicPhaseTypedAccess(
    const Texture &texture, Value *handle, Value *position, Value *arrayIndex, Value *sample, Value *level,
    Value *value, Value *comparison, AtomicRMWInst::BinOp operation, unsigned access, uint32_t writeMask,
    function_ref<std::pair<Value *, Value *>()> native) {
  using dxmt::dxil::EmitAtomic64CellRead;
  using dxmt::dxil::EmitAtomic64CellUpdate;
  auto *context = dxmt::dxil::EmitAtomic64Context(builder);
  auto load = [&](Value *base, uint64_t offset, Type *type) -> Value * {
    return CreateAtomicPhaseMetadataLoad(base, offset, type);
  };
  auto *viewType = builder.getInt8Ty()->getPointerTo(1);
  auto *view = CreateAtomicPhaseTypedView(context, handle);
  auto *resultType = access >= 4 ? builder.getInt64Ty() :
                     access == 2 || access == 3 ? getIntTy(4) : getTexelType(texture);
  auto empty = [&] { return std::pair<Value *, Value *>{Constant::getNullValue(resultType), builder.getInt8(0)}; };
  return CreateVolumeDispatch(texture, [&] {
    auto *mips = load(view, 20, getIntTy());
    auto *arrays = load(view, 24, getIntTy());
    auto *samples = load(view, 28, getIntTy());
    auto *unit = load(view, 32, getIntTy());
    auto *format = load(view, 36, getIntTy());
    auto *mip = level && !isa<UndefValue>(level) ? level : getInt(0);
    auto *array = arrayIndex && !isa<UndefValue>(arrayIndex) ? arrayIndex : getInt(0);
    auto *sampleIndex = sample && !isa<UndefValue>(sample) ? sample : getInt(0);
    auto *valid = builder.CreateAnd(builder.CreateICmpULT(mip, mips), builder.CreateICmpULT(array, arrays));
    valid = builder.CreateAnd(valid, builder.CreateICmpULT(sampleIndex, samples));
    return CreateVolumeDispatch(texture, [&] {
      auto *subresources = builder.CreateIntToPtr(load(view, 8, builder.getInt64Ty()), viewType);
      auto *subindex = builder.CreateAdd(builder.CreateMul(array, mips), mip);
      auto *sub = builder.CreateGEP(builder.getInt8Ty(), subresources,
          builder.CreateMul(builder.CreateZExt(subindex, builder.getInt64Ty()), builder.getInt64(32)));
      auto *width = load(sub, 16, getIntTy());
      auto *height = load(sub, 20, getIntTy());
      auto *depth = load(sub, 24, getIntTy());
      auto *pitch = load(sub, 28, getIntTy());
      Value *coords[3] = {getInt(0), getInt(0), getInt(0)};
      if (position->getType()->isVectorTy()) {
        unsigned dimensions = cast<FixedVectorType>(position->getType())->getNumElements();
        for (unsigned axis = 0; axis < std::min(dimensions, 3u); ++axis)
          coords[axis] = builder.CreateExtractElement(position, axis);
      } else coords[0] = position;
      if (texture.kind == Texture::texture_buffer)
        coords[0] = builder.CreateSub(coords[0], load(view, 52, getIntTy()));
      Value *inside = builder.CreateAnd(builder.CreateICmpULT(coords[0], width), builder.CreateICmpULT(coords[1], height));
      inside = builder.CreateAnd(inside, builder.CreateICmpULT(coords[2], depth));
      // A native view ID can back several UAV W-windows; callers guard each
      // descriptor's local WSize before adding its FirstWSlice to coordinates.
      return CreateVolumeDispatch(texture, [&] {
        auto *i64 = builder.getInt64Ty();
        auto zext = [&](Value *x) { return builder.CreateZExt(x, i64); };
        auto *row = builder.CreateAdd(builder.CreateMul(zext(coords[2]), zext(height)), zext(coords[1]));
        auto *texel = builder.CreateAdd(builder.CreateMul(zext(coords[0]), zext(samples)), zext(sampleIndex));
        auto *byte = builder.CreateAdd(load(sub, 8, i64), builder.CreateAdd(
            builder.CreateMul(row, zext(pitch)), builder.CreateMul(texel, zext(unit))));
        auto *firstCell = builder.CreateLShr(byte, 3);
        auto *firstBit = builder.CreateShl(builder.CreateAnd(byte, builder.getInt64(7)), 3);
        auto *map = builder.CreateIntToPtr(load(sub, 0, i64), getIntTy()->getPointerTo(1));
        auto *roots = builder.CreateIntToPtr(load(context, 16, i64), getIntTy()->getPointerTo(1));
        auto root = [&](unsigned cell) -> Value * {
          auto *id = builder.CreateLoad(getIntTy(), builder.CreateGEP(getIntTy(), map,
              builder.CreateAdd(firstCell, builder.getInt64(cell))));
          id->setMetadata(dxmt::dxil::kAtomic64Metadata, MDNode::get(getContext(), {}));
          // NULL sparse cells are not indices into the arena.
          return builder.CreateSelect(builder.CreateICmpEQ(id, getInt(~0u)),
              ConstantPointerNull::get(getIntTy()->getPointerTo(1)), builder.CreateGEP(getIntTy(), roots, id));
        };
        auto update = [&](Value *old, Value *operand, Value *compare) -> Value * {
          if (compare) return builder.CreateSelect(builder.CreateICmpEQ(old, compare), operand, old);
          switch (operation) {
          case AtomicRMWInst::Add: return builder.CreateAdd(old, operand);
          case AtomicRMWInst::Sub: return builder.CreateSub(old, operand);
          case AtomicRMWInst::And: return builder.CreateAnd(old, operand);
          case AtomicRMWInst::Or: return builder.CreateOr(old, operand);
          case AtomicRMWInst::Xor: return builder.CreateXor(old, operand);
          case AtomicRMWInst::Min: return builder.CreateSelect(builder.CreateICmpSLT(old, operand), old, operand);
          case AtomicRMWInst::Max: return builder.CreateSelect(builder.CreateICmpSGT(old, operand), old, operand);
          case AtomicRMWInst::UMin: return builder.CreateSelect(builder.CreateICmpULT(old, operand), old, operand);
          case AtomicRMWInst::UMax: return builder.CreateSelect(builder.CreateICmpUGT(old, operand), old, operand);
          default: return operand;
          }
        };
        if (access >= 4) {
          auto *old = EmitAtomic64CellUpdate(builder, root(0), [&](Value *previous) {
            return update(previous, value, comparison);
          });
          return std::pair<Value *, Value *>{old, builder.getInt8(0)};
        }
        if (access == 2 || access == 3) {
          auto *operand = builder.CreateExtractElement(value, uint64_t(0));
          auto *compare = comparison ? builder.CreateExtractElement(comparison, uint64_t(0)) : nullptr;
          auto *old = EmitAtomic64CellUpdate(builder, root(0), [&](Value *previous) -> Value * {
            auto *lane = builder.CreateTrunc(builder.CreateLShr(previous, firstBit), getIntTy());
            auto *desired = update(lane, operand, compare);
            auto *mask = builder.CreateShl(builder.getInt64(0xffffffffu), firstBit);
            return builder.CreateOr(builder.CreateAnd(previous, builder.CreateNot(mask)),
                builder.CreateShl(builder.CreateZExt(desired, i64), firstBit));
          });
          auto *scalar = builder.CreateTrunc(builder.CreateLShr(old, firstBit), getIntTy());
          return std::pair<Value *, Value *>{builder.CreateVectorSplat(4, scalar), builder.getInt8(0)};
        }
        auto ordinary = [&]() -> std::pair<Value *, Value *> {
        // One descriptor per component: width6, bit-offset7, encoding3.
        // All uncompressed D3D typed UAV formats have at most four 32-bit components.
        auto *formatInfo = getModule()->getFunction("dxmt.atomic64.typed_format");
        if (!formatInfo) {
          formatInfo = Function::Create(FunctionType::get(i64, {getIntTy()}, false),
              GlobalValue::InternalLinkage, "dxmt.atomic64.typed_format", getModule());
          formatInfo->addFnAttr(Attribute::NoUnwind);
          formatInfo->addFnAttr(Attribute::ReadNone);
          auto saved = builder.saveIP();
          auto *entry = BasicBlock::Create(getContext(), "entry", formatInfo);
          auto *missing = BasicBlock::Create(getContext(), "missing", formatInfo);
          builder.SetInsertPoint(entry);
          auto *dispatch = builder.CreateSwitch(formatInfo->getArg(0), missing);
          // kind: uint0, sint1, float32=2, unorm3, snorm4, float16=5, unsignedfloat=6.
          struct Format { unsigned id, channels, bits, kind; };
          constexpr Format formats[] = {
            {1,4,8,3},{40,3,5,3},{41,4,5,3},{42,4,4,3},{43,4,5,3},
            {10,1,8,3},{12,1,8,4},{13,1,8,0},{14,1,8,1},
            {11,1,8,3},{31,2,8,3},{71,4,8,3},{81,4,8,3},
            {20,1,16,3},{22,1,16,4},{23,1,16,0},{24,1,16,1},{25,1,16,5},
            {30,2,8,3},{32,2,8,4},{33,2,8,0},{34,2,8,1},
            {53,1,32,0},{54,1,32,1},{55,1,32,2},
            {60,2,16,3},{62,2,16,4},{63,2,16,0},{64,2,16,1},{65,2,16,5},
            {70,4,8,3},{72,4,8,4},{73,4,8,0},{74,4,8,1},{80,4,8,3},
            {90,4,10,3},{91,4,10,0},{92,3,11,6},{94,4,10,3},
            {103,2,32,0},{104,2,32,1},{105,2,32,2},
            {110,4,16,3},{112,4,16,4},{113,4,16,0},{114,4,16,1},{115,4,16,5},
            {123,4,32,0},{124,4,32,1},{125,4,32,2}
          };
          for (auto f : formats) {
            uint64_t descriptor = 0;
            unsigned offset = 0;
            for (unsigned channel = 0; channel < f.channels; ++channel) {
              unsigned bits = f.id == 90 || f.id == 91 || f.id == 94 ? (channel == 3 ? 2 : 10) :
                              f.id == 92 ? (channel == 2 ? 10 : 11) :
                              f.id == 1 ? (channel == 3 ? 8 : 0) :
                              f.id == 40 ? (channel == 1 ? 6 : 5) :
                              f.id == 41 || f.id == 43 ? (channel == 3 ? 1 : 5) : f.bits;
              unsigned physical = (f.id == 80 || f.id == 81 || f.id == 94) && channel < 3 ? 2 - channel : channel;
              unsigned bitOffset = f.id == 80 || f.id == 81 ? physical * 8 :
                                   f.id == 94 ? (channel == 3 ? 30 : physical * 10) :
                                   f.id == 1 ? 0 :
                                   f.id == 40 ? (channel == 0 ? 11 : channel == 1 ? 5 : 0) :
                                   f.id == 41 ? (channel == 0 ? 11 : channel == 1 ? 6 : channel == 2 ? 1 : 0) :
                                   f.id == 42 ? (3 - channel) * 4 :
                                   f.id == 43 ? (channel == 0 ? 10 : channel == 1 ? 5 : channel == 2 ? 0 : 15) : offset;
              descriptor |= uint64_t(bits | (bitOffset << 6) | (f.kind << 13)) << (channel * 16);
              offset += bits;
            }
            auto *block = BasicBlock::Create(getContext(), "format", formatInfo);
            dispatch->addCase(getInt(f.id), block);
            builder.SetInsertPoint(block);
            builder.CreateRet(builder.getInt64(descriptor));
          }
          builder.SetInsertPoint(missing);
          builder.CreateRet(builder.getInt64(0));
          builder.restoreIP(saved);
        }
        auto *descriptors = builder.CreateCall(formatInfo, {builder.CreateAnd(format, getInt(0xfff))});
        auto *scalarType = cast<FixedVectorType>(resultType)->getElementType();
        Value *words[3] = {builder.getInt64(0), builder.getInt64(0), builder.getInt64(0)};
        Value *masks[3] = {builder.getInt64(0), builder.getInt64(0), builder.getInt64(0)};
        auto *last = builder.CreateLShr(builder.CreateSub(builder.CreateAdd(byte, zext(unit)), builder.getInt64(1)), 3);
        if (access == 0) {
          for (unsigned cell = 0; cell < 3; ++cell) {
            auto *present = builder.CreateICmpULE(builder.CreateAdd(firstCell, builder.getInt64(cell)), last);
            words[cell] = CreateVolumeDispatch(texture, [&] {
              return std::pair<Value *, Value *>{EmitAtomic64CellRead(builder, root(cell)), nullptr};
            }, [&] { return std::pair<Value *, Value *>{builder.getInt64(0), nullptr}; }, present).first;
          }
        }
        Value *decoded = Constant::getNullValue(resultType);
        for (unsigned channel = 0; channel < 4; ++channel) {
          Value *descriptor = builder.CreateTrunc(builder.CreateLShr(descriptors, channel * 16), getIntTy());
          if (access == 1) {
            auto *selector = builder.CreateURem(builder.CreateAdd(getInt(channel + 2),
                builder.CreateAnd(builder.CreateLShr(format, 21 - channel * 3), getInt(7))), getInt(6));
            descriptor = getInt(0);
            for (unsigned physical = 0; physical < 4; ++physical)
              descriptor = builder.CreateSelect(builder.CreateICmpEQ(selector, getInt(physical + 2)),
                  builder.CreateTrunc(builder.CreateLShr(descriptors, physical * 16), getIntTy()), descriptor);
          }
          auto *bits = builder.CreateAnd(descriptor, getInt(63));
          auto *offset = builder.CreateAnd(builder.CreateLShr(descriptor, 6), getInt(127));
          auto *kind = builder.CreateAnd(builder.CreateLShr(descriptor, 13), getInt(7));
          auto *bit = builder.CreateAdd(firstBit, zext(offset));
          auto *cellIndex = builder.CreateLShr(bit, 6);
          auto *shift = builder.CreateAnd(bit, builder.getInt64(63));
          auto *bitMask = builder.CreateSub(builder.CreateShl(builder.getInt64(1), zext(bits)), builder.getInt64(1));
          if (access == 1) {
            if (!(writeMask & (1u << channel))) continue;
            auto *component = builder.CreateExtractElement(value, channel);
            Value *encoded = component;
            if (component->getType()->isFloatingPointTy()) {
              auto *f = builder.CreateFPCast(component, getFloatTy());
              auto *finite = builder.CreateSelect(builder.CreateFCmpORD(f, f), f, getFloat(0));
              auto *unorm = CreateFPBinOp(fmax, getFloat(0), CreateFPBinOp(fmin, getFloat(1), finite, false), false);
              auto *snorm = CreateFPBinOp(fmax, getFloat(-1), CreateFPBinOp(fmin, getFloat(1), finite, false), false);
              auto *unsignedScale = builder.CreateUIToFP(builder.CreateTrunc(bitMask, getIntTy()), getFloatTy());
              auto *signedScale = builder.CreateUIToFP(builder.CreateTrunc(builder.CreateLShr(bitMask, 1), getIntTy()), getFloatTy());
              auto *u = builder.CreateFPToUI(CreateFPUnOp(rint, builder.CreateFMul(unorm, unsignedScale), false), getIntTy());
              auto *s = builder.CreateFPToSI(CreateFPUnOp(rint, builder.CreateFMul(snorm, signedScale), false), getIntTy());
              auto *half = builder.CreateZExt(builder.CreateBitCast(builder.CreateFPTrunc(f, builder.getHalfTy()),
                  builder.getInt16Ty()), getIntTy());
              auto *mantissa = builder.CreateSub(bits, getInt(5));
              auto *discard = builder.CreateSub(getInt(23), mantissa);
              auto *safeDiscard = builder.CreateAnd(discard, getInt(31));
              auto *positive = CreateFPBinOp(fmax, getFloat(0), finite, false);
              auto *floatBits = builder.CreateBitCast(positive, getIntTy());
              auto *bias = builder.CreateSub(builder.CreateShl(getInt(1), builder.CreateAnd(builder.CreateSub(safeDiscard, getInt(1)), getInt(31))), getInt(1));
              auto *rounded = builder.CreateAdd(floatBits, builder.CreateAdd(bias,
                  builder.CreateAnd(builder.CreateLShr(floatBits, safeDiscard), getInt(1))));
              auto *normal = builder.CreateSub(builder.CreateLShr(rounded, safeDiscard), builder.CreateShl(getInt(112), mantissa));
              auto *subscaleBits = builder.CreateShl(builder.CreateAdd(getInt(141), mantissa), 23);
              auto *subnormal = builder.CreateFPToUI(CreateFPUnOp(rint, builder.CreateFMul(CreateFPBinOp(fmin, positive, getFloat(0.00006103515625f), false),
                builder.CreateBitCast(subscaleBits, getFloatTy())), false), getIntTy());
              auto *inf = builder.CreateShl(getInt(31), mantissa);
              auto *ufloat = builder.CreateSelect(builder.CreateFCmpOLT(positive, getFloat(0.00006103515625f)), subnormal,
                  builder.CreateSelect(builder.CreateICmpULT(normal, inf), normal, inf));
              ufloat = builder.CreateSelect(builder.CreateFCmpUNO(f, f), builder.CreateOr(inf,
                  builder.CreateShl(getInt(1), builder.CreateSub(mantissa, getInt(1)))), ufloat);
              encoded = builder.CreateBitCast(f, getIntTy());
              encoded = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(3)), u, encoded);
              encoded = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(4)), s, encoded);
              encoded = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(5)), half, encoded);
              encoded = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(6)), ufloat, encoded);
            }
            auto *raw = builder.CreateAnd(builder.CreateZExtOrTrunc(encoded, i64), bitMask);
            for (unsigned cell = 0; cell < 3; ++cell) {
              auto *here = builder.CreateICmpEQ(cellIndex, builder.getInt64(cell));
              auto *mask = builder.CreateShl(bitMask, shift);
              words[cell] = builder.CreateOr(words[cell], builder.CreateSelect(here,
                  builder.CreateShl(raw, shift), builder.getInt64(0)));
              masks[cell] = builder.CreateOr(masks[cell], builder.CreateSelect(here, mask, builder.getInt64(0)));
            }
            continue;
          }
          Value *word = words[0];
          for (unsigned cell = 1; cell < 3; ++cell)
            word = builder.CreateSelect(builder.CreateICmpEQ(cellIndex, builder.getInt64(cell)), words[cell], word);
          auto *raw = builder.CreateTrunc(builder.CreateAnd(builder.CreateLShr(word, shift), bitMask), getIntTy());
          auto *signedShift = builder.CreateAnd(builder.CreateSub(getInt(32), bits), getInt(31));
          auto *signedRaw = builder.CreateAShr(builder.CreateShl(raw, signedShift), signedShift);
          Value *component = raw;
          if (scalarType->isFloatingPointTy()) {
            auto *unorm = builder.CreateFDiv(builder.CreateUIToFP(raw, getFloatTy()),
                builder.CreateUIToFP(builder.CreateTrunc(bitMask, getIntTy()), getFloatTy()));
            auto *snorm = CreateFPBinOp(fmax, getFloat(-1), builder.CreateFDiv(builder.CreateSIToFP(signedRaw, getFloatTy()),
                builder.CreateUIToFP(builder.CreateTrunc(builder.CreateLShr(bitMask, 1), getIntTy()), getFloatTy())), false);
            auto *half = builder.CreateFPExt(builder.CreateBitCast(builder.CreateTrunc(raw, builder.getInt16Ty()),
                builder.getHalfTy()), getFloatTy());
            auto *mantissa = builder.CreateAnd(builder.CreateSub(bits, getInt(5)), getInt(31));
            auto *exponent = builder.CreateLShr(raw, mantissa);
            auto *fraction = builder.CreateAnd(raw, builder.CreateSub(builder.CreateShl(getInt(1), mantissa), getInt(1)));
            auto *normal = builder.CreateOr(builder.CreateShl(builder.CreateAdd(exponent, getInt(112)), 23),
                builder.CreateShl(fraction, builder.CreateAnd(builder.CreateSub(getInt(23), mantissa), getInt(31))));
            auto *subscale = builder.CreateBitCast(builder.CreateShl(builder.CreateSub(getInt(113), mantissa), 23), getFloatTy());
            auto *subnormal = builder.CreateFMul(builder.CreateUIToFP(fraction, getFloatTy()), subscale);
            auto *special = builder.CreateOr(getInt(0x7f800000), builder.CreateShl(fraction,
                builder.CreateAnd(builder.CreateSub(getInt(23), mantissa), getInt(31))));
            auto *ufloat = builder.CreateSelect(builder.CreateICmpEQ(exponent, getInt(0)), subnormal,
                builder.CreateBitCast(builder.CreateSelect(builder.CreateICmpEQ(exponent, getInt(31)), special, normal), getFloatTy()));
            component = builder.CreateBitCast(raw, getFloatTy());
            component = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(3)), unorm, component);
            component = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(4)), snorm, component);
            component = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(5)), half, component);
            component = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(6)), ufloat, component);
            if (channel < 3) {
              auto *baseFormat = builder.CreateAnd(format, getInt(0xfff));
              auto *srgb = builder.CreateOr(builder.CreateICmpEQ(baseFormat, getInt(11)),
                  builder.CreateOr(builder.CreateICmpEQ(baseFormat, getInt(31)), builder.CreateOr(
                    builder.CreateICmpEQ(baseFormat, getInt(71)), builder.CreateICmpEQ(baseFormat, getInt(81)))));
              component = CreateVolumeDispatch(texture, [&] {
                auto *linear = builder.CreateSelect(builder.CreateFCmpOLE(unorm, getFloat(0.04045f)),
                    builder.CreateFDiv(unorm, getFloat(12.92f)), CreateFPUnOp(exp2,
                      builder.CreateFMul(CreateFPUnOp(log2, builder.CreateFDiv(builder.CreateFAdd(unorm,
                          getFloat(0.055f)), getFloat(1.055f)), false), getFloat(2.4f)), false));
                return std::pair<Value *, Value *>{linear, nullptr};
              }, [&] { return std::pair<Value *, Value *>{component, nullptr}; }, srgb).first;
            }
            component = builder.CreateFPCast(component, scalarType);
          } else component = builder.CreateSelect(builder.CreateICmpEQ(kind, getInt(1)), signedRaw, raw);
          auto *defaultValue = scalarType->isFloatingPointTy() ? static_cast<Value *>(ConstantFP::get(scalarType, channel == 3 ? 1.0 : 0.0)) :
              static_cast<Value *>(ConstantInt::get(scalarType, channel == 3 ? 1 : 0));
          component = builder.CreateSelect(builder.CreateICmpEQ(bits, getInt(0)), defaultValue, component);
          decoded = builder.CreateInsertElement(decoded, component, channel);
        }
        if (access == 1) {
          for (unsigned cell = 0; cell < 3; ++cell) {
            auto *present = builder.CreateICmpNE(masks[cell], builder.getInt64(0));
            CreateVolumeDispatch(texture, [&] {
              EmitAtomic64CellUpdate(builder, root(cell), [&](Value *previous) {
                return builder.CreateOr(builder.CreateAnd(previous, builder.CreateNot(masks[cell])), words[cell]);
              });
              return std::pair<Value *, Value *>{nullptr, nullptr};
            }, [&] { return std::pair<Value *, Value *>{nullptr, nullptr}; }, present);
          }
          return empty();
        }
        Value *mapped = Constant::getNullValue(resultType);
        for (unsigned channel = 0; channel < 4; ++channel) {
          auto *selector = builder.CreateURem(builder.CreateAdd(getInt(channel + 2),
              builder.CreateAnd(builder.CreateLShr(format, 21 - channel * 3), getInt(7))), getInt(6));
          Value *component = Constant::getNullValue(scalarType);
          for (unsigned input = 0; input < 4; ++input)
            component = builder.CreateSelect(builder.CreateICmpEQ(selector, getInt(input + 2)),
                builder.CreateExtractElement(decoded, input), component);
          auto *one = scalarType->isFloatingPointTy() ? static_cast<Value *>(ConstantFP::get(scalarType, 1.0)) :
              static_cast<Value *>(ConstantInt::get(scalarType, 1));
          component = builder.CreateSelect(builder.CreateICmpEQ(selector, getInt(1)), one, component);
          mapped = builder.CreateInsertElement(mapped, component, channel);
        }
        return std::pair<Value *, Value *>{mapped, builder.getInt8(0)};
        };
        if (texture.sample_type == Texture::sample_float || texture.sample_type == Texture::sample_half) {
          auto *sharedExponent = builder.CreateICmpEQ(builder.CreateAnd(format, getInt(0xfff)), getInt(93));
          return CreateVolumeDispatch(texture, [&] {
            if (access == 1) {
              if (!(writeMask & 7)) return empty();
              EmitAtomic64CellUpdate(builder, root(0), [&](Value *previous) -> Value * {
                auto *old = builder.CreateTrunc(builder.CreateLShr(previous, firstBit), getIntTy());
                auto *oldExponent = builder.CreateLShr(old, 27);
                auto *oldScale = builder.CreateBitCast(builder.CreateShl(builder.CreateAdd(oldExponent, getInt(103)), 23), getFloatTy());
                Value *components[3];
                Value *maximum = getFloat(0);
                for (unsigned channel = 0; channel < 3; ++channel) {
                  auto *oldMantissa = builder.CreateAnd(builder.CreateLShr(old, channel * 9), getInt(511));
                  auto *component = writeMask & (1u << channel) ?
                      builder.CreateFPCast(builder.CreateExtractElement(value, channel), getFloatTy()) :
                      builder.CreateFMul(builder.CreateUIToFP(oldMantissa, getFloatTy()), oldScale);
                  component = builder.CreateSelect(builder.CreateFCmpORD(component, component), component, getFloat(0));
                  components[channel] = CreateFPBinOp(fmax, getFloat(0),
                      CreateFPBinOp(fmin, getFloat(65408), component, false), false);
                  maximum = CreateFPBinOp(fmax, maximum, components[channel], false);
                }
                auto *binaryExponent = builder.CreateAnd(builder.CreateLShr(builder.CreateBitCast(maximum, getIntTy()), 23), getInt(255));
                auto *exponent = builder.CreateSelect(builder.CreateICmpUGT(binaryExponent, getInt(111)),
                    builder.CreateSub(binaryExponent, getInt(111)), getInt(0));
                auto *scale = builder.CreateBitCast(builder.CreateShl(builder.CreateSub(getInt(151), exponent), 23), getFloatTy());
                auto *maxMantissa = builder.CreateFPToUI(CreateFPUnOp(rint, builder.CreateFMul(maximum, scale), false), getIntTy());
                exponent = builder.CreateAdd(exponent, builder.CreateZExt(builder.CreateICmpEQ(maxMantissa, getInt(512)), getIntTy()));
                scale = builder.CreateBitCast(builder.CreateShl(builder.CreateSub(getInt(151), exponent), 23), getFloatTy());
                Value *encoded = builder.CreateShl(exponent, 27);
                for (unsigned channel = 0; channel < 3; ++channel) {
                  auto *mantissa = builder.CreateFPToUI(CreateFPUnOp(rint,
                      builder.CreateFMul(components[channel], scale), false), getIntTy());
                  encoded = builder.CreateOr(encoded, builder.CreateShl(mantissa, channel * 9));
                }
                auto *mask = builder.CreateShl(builder.getInt64(0xffffffffu), firstBit);
                return builder.CreateOr(builder.CreateAnd(previous, builder.CreateNot(mask)),
                    builder.CreateShl(builder.CreateZExt(encoded, i64), firstBit));
              });
              return empty();
            }
            auto *packed = builder.CreateTrunc(builder.CreateLShr(EmitAtomic64CellRead(builder, root(0)), firstBit), getIntTy());
            auto *exponent = builder.CreateLShr(packed, 27);
            auto *scale = builder.CreateBitCast(builder.CreateShl(builder.CreateAdd(exponent, getInt(103)), 23), getFloatTy());
            Value *decoded = Constant::getNullValue(resultType);
            for (unsigned channel = 0; channel < 3; ++channel) {
              auto *mantissa = builder.CreateAnd(builder.CreateLShr(packed, channel * 9), getInt(511));
              decoded = builder.CreateInsertElement(decoded, builder.CreateFPCast(
                  builder.CreateFMul(builder.CreateUIToFP(mantissa, getFloatTy()), scale),
                  cast<FixedVectorType>(resultType)->getElementType()), channel);
            }
            auto *one = ConstantFP::get(cast<FixedVectorType>(resultType)->getElementType(), 1);
            decoded = builder.CreateInsertElement(decoded, one, 3);
            Value *mapped = Constant::getNullValue(resultType);
            for (unsigned channel = 0; channel < 4; ++channel) {
              auto *selector = builder.CreateURem(builder.CreateAdd(getInt(channel + 2),
                  builder.CreateAnd(builder.CreateLShr(format, 21 - channel * 3), getInt(7))), getInt(6));
              Value *component = ConstantFP::get(cast<FixedVectorType>(resultType)->getElementType(), 0);
              component = builder.CreateSelect(builder.CreateICmpEQ(selector, getInt(1)), one, component);
              for (unsigned physical = 0; physical < 4; ++physical)
                component = builder.CreateSelect(builder.CreateICmpEQ(selector, getInt(physical + 2)),
                    builder.CreateExtractElement(decoded, physical), component);
              mapped = builder.CreateInsertElement(mapped, component, channel);
            }
            return std::pair<Value *, Value *>{mapped, builder.getInt8(0)};
          }, ordinary, sharedExponent);
        }
        return ordinary();
      }, empty, inside);
    }, empty, valid);
  }, native, builder.CreateIsNotNull(view));
}

Value *
AIRBuilder::CreateTypedAtomic64(const Texture &texture, Value *handle, Value *position, Value *arrayIndex,
                               AtomicRMWInst::BinOp operation, Value *value, Value *comparison) {
  return CreateAtomicPhaseTypedAccess(texture, handle, position, arrayIndex, nullptr, getInt(0), value,
      comparison, operation, comparison ? 5 : 4, 15, [&] {
        // Phase admission supplies every bound UAV; absent/null/OOB has no physical atomic64 fallback.
        return std::pair<Value *, Value *>{builder.getInt64(0), builder.getInt8(0)};
      }).first;
}

void
AIRBuilder::CreateTypedPhaseStore(const Texture &texture, Value *handle, Value *position, Value *arrayIndex,
                                 Value *sample, Value *value, uint32_t mask) {
  CreateAtomicPhaseTypedAccess(texture, handle, position, arrayIndex, sample, getInt(0), value, nullptr,
      AtomicRMWInst::Xchg, 1, mask, [&] {
        return std::pair<Value *, Value *>{Constant::getNullValue(getTexelType(texture)), builder.getInt8(0)};
      });
}

std::pair<Value *, Value *>
AIRBuilder::CreateVolumeDispatch(const Texture &texture,
    function_ref<std::pair<Value *, Value *>()> atlas,
    function_ref<std::pair<Value *, Value *>()> native, Value *condition) {
  auto *head = builder.GetInsertBlock();
  auto *function = head->getParent();
  auto point = builder.GetInsertPoint();
  auto *merge = point == head->end() ? BasicBlock::Create(getContext(), "volume.merge", function)
                                   : head->splitBasicBlock(point, "volume.merge");
  if (head->getTerminator()) head->getTerminator()->eraseFromParent();
  auto *yes = BasicBlock::Create(getContext(), "volume.atlas", function, merge);
  auto *no = BasicBlock::Create(getContext(), "volume.native", function, merge);
  builder.SetInsertPoint(head);
  if (!condition)
    condition = builder.CreateICmpNE(builder.CreateAnd(texture.volume_layout,
        builder.getInt64(uint64_t(1) << 62)), builder.getInt64(0));
  builder.CreateCondBr(condition, yes, no);
  builder.SetInsertPoint(yes);
  auto a = atlas();
  auto *a_tail = builder.GetInsertBlock();
  builder.CreateBr(merge);
  builder.SetInsertPoint(no);
  auto b = native();
  auto *b_tail = builder.GetInsertBlock();
  builder.CreateBr(merge);
  builder.SetInsertPoint(merge, merge->getFirstInsertionPt());
  auto phi = [&](Value *x, Value *y) -> Value * {
    if (!x) return nullptr;
    auto *value = builder.CreatePHI(x->getType(), 2);
    value->addIncoming(x, a_tail);
    value->addIncoming(y, b_tail);
    return value;
  };
  return {phi(a.first, b.first), phi(a.second, b.second)};
}

Value *
AIRBuilder::LoadVolumeWord(const Texture &texture, Value *index) {
  auto *address = builder.CreateAnd(texture.volume_layout, builder.getInt64((uint64_t(1) << 62) - 1));
  auto *layout = builder.CreateIntToPtr(address, getIntTy()->getPointerTo(1));
  auto *word = builder.CreateLoad(getIntTy(), builder.CreateGEP(getIntTy(), layout, index));
  // Runtime volume headers are transport metadata, not application UAV bytes.
  word->setMetadata(dxmt::dxil::kAtomic64Metadata, MDNode::get(getContext(), {}));
  return word;
}

Value *
AIRBuilder::VolumeMip(const Texture &texture, Value *level) {
  auto *first = builder.CreateAnd(builder.CreateTrunc(builder.CreateLShr(texture.volume_view, 32), getIntTy()),
                                  getInt(255));
  return builder.CreateAdd(first, level);
}

Value *
AIRBuilder::VolumeDimension(const Texture &texture, unsigned axis, Value *level) {
  auto *size = LoadVolumeWord(texture, getInt(axis));
  auto *mip = CreateIntBinOp(min, VolumeMip(texture, level), getInt(31));
  return CreateIntBinOp(max, builder.CreateLShr(size, mip), getInt(1));
}

Value *
AIRBuilder::VolumeInBounds(const Texture &texture, Value *position, Value *level) {
  auto *count = builder.CreateAnd(builder.CreateTrunc(builder.CreateLShr(texture.volume_view, 40), getIntTy()),
                                  getInt(255));
  Value *valid = builder.CreateICmpULT(level, count);
  for (unsigned axis = 0; axis < 3; ++axis)
    valid = builder.CreateAnd(valid, builder.CreateICmpULT(builder.CreateExtractElement(position, axis),
                                                         VolumeDimension(texture, axis, level)));
  return valid;
}

AIRBuilder::VolumeAtlasPosition
AIRBuilder::VolumePosition(const Texture &texture, Value *position, Value *level) {
  auto *mip_base = builder.CreateAdd(getInt(14), builder.CreateMul(VolumeMip(texture, level), getInt(4)));
  auto word = [&](unsigned offset) { return LoadVolumeWord(texture, builder.CreateAdd(mip_base, getInt(offset))); };
  auto *x = builder.CreateExtractElement(position, uint64_t(0));
  auto *y = builder.CreateExtractElement(position, 1);
  auto *z = builder.CreateExtractElement(position, 2);
  auto *sw = LoadVolumeWord(texture, getInt(4));
  auto *sh = LoadVolumeWord(texture, getInt(5));
  auto *sd = LoadVolumeWord(texture, getInt(6));
  auto *bw = LoadVolumeWord(texture, getInt(7));
  auto *bh = LoadVolumeWord(texture, getInt(8));
  auto *pw = LoadVolumeWord(texture, getInt(9));
  auto *ph = LoadVolumeWord(texture, getInt(10));
  auto *cols = LoadVolumeWord(texture, getInt(11));
  auto *rows = LoadVolumeWord(texture, getInt(12));
  auto *tile = builder.CreateAdd(word(0), builder.CreateAdd(builder.CreateUDiv(x, sw),
      builder.CreateMul(word(1), builder.CreateAdd(builder.CreateUDiv(y, sh),
          builder.CreateMul(word(2), builder.CreateUDiv(z, sd))))));
  auto *unit = builder.CreateAdd(builder.CreateUDiv(builder.CreateURem(x, sw), bw),
      builder.CreateMul(builder.CreateUDiv(sw, bw), builder.CreateAdd(builder.CreateUDiv(builder.CreateURem(y, sh), bh),
          builder.CreateMul(builder.CreateUDiv(sh, bh), builder.CreateURem(z, sd)))));
  auto *units_per_row = builder.CreateUDiv(pw, bw);
  auto *px = builder.CreateAdd(builder.CreateMul(builder.CreateURem(tile, cols), pw),
      builder.CreateAdd(builder.CreateMul(builder.CreateURem(unit, units_per_row), bw), builder.CreateURem(x, bw)));
  auto *py = builder.CreateAdd(builder.CreateMul(builder.CreateURem(builder.CreateUDiv(tile, cols), rows), ph),
      builder.CreateAdd(builder.CreateMul(builder.CreateUDiv(unit, units_per_row), bh), builder.CreateURem(y, bh)));
  auto *xy = builder.CreateInsertElement(UndefValue::get(getIntTy(2)), px, uint64_t(0));
  xy = builder.CreateInsertElement(xy, py, 1);
  return {xy, builder.CreateUDiv(tile, builder.CreateMul(cols, rows)), tile};
}

Value *
AIRBuilder::VolumeMapped(const Texture &texture, Value *tile) {
  auto *pointer_word = builder.CreateAdd(getInt(14), builder.CreateMul(LoadVolumeWord(texture, getInt(3)), getInt(4)));
  auto *low = builder.CreateZExt(LoadVolumeWord(texture, pointer_word), builder.getInt64Ty());
  auto *high = builder.CreateZExt(LoadVolumeWord(texture, builder.CreateAdd(pointer_word, getInt(1))),
                                  builder.getInt64Ty());
  auto *address = builder.CreateOr(low, builder.CreateShl(high, 32));
  // The absent table is the immutable, fully-backed ordinary-volume contract.
  return CreateVolumeDispatch(texture, [&] {
    auto *status = builder.CreateIntToPtr(address, getIntTy()->getPointerTo(1));
    auto *word = builder.CreateLoad(getIntTy(), builder.CreateGEP(getIntTy(), status, tile));
    word->setMetadata(dxmt::dxil::kAtomic64Metadata, MDNode::get(getContext(), {}));
    return std::pair<Value *, Value *>{builder.CreateICmpNE(word, getInt(0)), nullptr};
  }, [&] { return std::pair<Value *, Value *>{getBool(true), nullptr}; }, builder.CreateIsNotNull(address)).first;
}

std::pair<Value *, Value *>
AIRBuilder::CreateVolumeRead(const Texture &texture, Value *handle, Value *position, Value *level, bool coherent) {
  auto *valid = builder.CreateAnd(VolumeInBounds(texture, position, level), builder.CreateIsNotNull(handle));
  if (texture.memory_access == Texture::access_sample) {
    auto *minimum = builder.CreateBitCast(builder.CreateTrunc(texture.volume_metadata, getIntTy()), getFloatTy());
    auto *last = builder.CreateFSub(builder.CreateUIToFP(builder.CreateAnd(builder.CreateTrunc(
        builder.CreateLShr(texture.volume_view, 40), getIntTy()), getInt(255)), getFloatTy()), getFloat(1));
    valid = builder.CreateAnd(valid, builder.CreateAnd(builder.CreateFCmpOLE(minimum, last),
        builder.CreateFCmpOGE(builder.CreateUIToFP(level, getFloatTy()), CreateFPUnOp(floor, minimum, false))));
  }
  auto *defaults = VolumeDefaults(texture, handle);
  return CreateVolumeDispatch(texture, [&] {
    auto physical = texture;
    physical.kind = Texture::texture2d_array;
    physical.volume_layout = nullptr;
    auto [xy, slice, tile] = VolumePosition(texture, position, level);
    auto *mapped = VolumeMapped(texture, tile);
    auto *atlas = builder.CreateBitCast(handle, getTextureHandleType(physical));
    auto read = CreateRead(physical, atlas, xy, slice, nullptr, getInt(0), coherent);
    // Tier 2 NULL tiles preserve defaults in missing format components, then SRV mapping.
    read.first = builder.CreateSelect(mapped, read.first, defaults);
    read.second = builder.CreateSelect(mapped, builder.getInt8(0), builder.getInt8(1));
    return read;
  }, [&] { return std::pair<Value *, Value *>{defaults, builder.getInt8(0)}; }, valid);
}

Value *
AIRBuilder::VolumeDefaults(const Texture &texture, Value *handle) {
  auto *type = getTexelType(texture);
  Value *defaults = Constant::getNullValue(type);
  auto *bits = builder.CreateTrunc(texture.volume_view, getIntTy());
  auto *scalar = type->getScalarType();
  Value *one = scalar->isFloatingPointTy() ? static_cast<Value *>(ConstantFP::get(scalar, 1))
                                         : static_cast<Value *>(ConstantInt::get(scalar, 1));
  for (unsigned lane = 0; lane < 4; ++lane) {
    auto *value = builder.CreateSelect(builder.CreateICmpNE(builder.CreateAnd(bits, getInt(1u << lane)), getInt(0)),
                                        one, Constant::getNullValue(scalar));
    defaults = builder.CreateInsertElement(defaults, value, lane);
  }
  return builder.CreateSelect(builder.CreateIsNotNull(handle), defaults, Constant::getNullValue(type));
}

std::pair<Value *, Value *>
AIRBuilder::CreateVolumeSample(const Texture &texture, Value *handle, Value *coord,
    Value *offset, Value *lod, Value *minimum, Value *dx, Value *dy) {
  if (texture.sample_type == Texture::sample_half) {
    auto full = texture;
    full.sample_type = Texture::sample_float;
    auto result = CreateVolumeSample(full, handle, coord, offset, lod, minimum, dx, dy);
    result.first = builder.CreateFPTrunc(result.first, getTexelType(texture));
    return result;
  }
  const bool gradients = dx && dy;
  const auto name = std::format("dxmt.volume.sample.{}.{}.{}.{}", unsigned(texture.sample_type),
      unsigned(texture.memory_access), gradients, texture.atomic_phase_native);
  SmallVector<Value *, 16> args{handle, coord, offset ? offset : Constant::getNullValue(getIntTy(3)),
      lod, minimum, texture.volume_layout, texture.volume_view, texture.volume_metadata,
      texture.sampler_metadata, texture.sampler_lod_range,
      texture.sample_type == Texture::sample_float ? texture.sampler_float_border : texture.sampler_uint_border};
  if (gradients) {
    args.push_back(dx);
    args.push_back(dy);
  }
  auto *function = getModule()->getFunction(name);
  if (!function) {
    SmallVector<Type *, 16> types;
    for (auto *arg : args) types.push_back(arg->getType());
    auto *result_type = StructType::get(getTexelType(texture), builder.getInt8Ty());
    function = Function::Create(FunctionType::get(result_type, types, false),
        GlobalValue::InternalLinkage, name, getModule());
    // Expanding the atlas filter at each 3D sample makes real pixel shaders
    // hundreds of thousands of instructions long before Metal compilation.
    function->addFnAttr(Attribute::NoInline);
    function->addFnAttr(Attribute::NoUnwind);
    IRBuilderBase::InsertPointGuard insertion(builder);
    builder.SetInsertPoint(BasicBlock::Create(getContext(), "entry", function));
    auto sampled = texture;
    sampled.volume_layout = function->getArg(5);
    sampled.volume_view = function->getArg(6);
    sampled.volume_metadata = function->getArg(7);
    sampled.sampler_metadata = function->getArg(8);
    sampled.sampler_lod_range = function->getArg(9);
    sampled.sampler_float_border = sampled.sampler_uint_border = function->getArg(10);
    auto result = CreateVolumeSampleBody(sampled, function->getArg(0), function->getArg(1), function->getArg(2),
        function->getArg(3), function->getArg(4), gradients ? function->getArg(11) : nullptr,
        gradients ? function->getArg(12) : nullptr);
    auto *packed = builder.CreateInsertValue(UndefValue::get(result_type), result.first, 0);
    builder.CreateRet(builder.CreateInsertValue(packed, result.second, 1));
  }
  auto *result = builder.CreateCall(function, args);
  return {builder.CreateExtractValue(result, 0), builder.CreateExtractValue(result, 1)};
}

std::pair<Value *, Value *>
AIRBuilder::CreateVolumeSampleBody(const Texture &texture, Value *handle, Value *coord,
    Value *offset, Value *lod, Value *minimum, Value *dx, Value *dy) {
  IRBuilderBase::FastMathFlagGuard math_flags(builder);
  builder.clearFastMathFlags();
  auto *flags = builder.CreateTrunc(builder.CreateLShr(texture.sampler_metadata, 32), getIntTy());
  auto *reduction = builder.CreateAnd(builder.CreateLShr(flags, 20), getInt(3));
  auto *resource_min = builder.CreateBitCast(builder.CreateTrunc(texture.volume_metadata, getIntTy()), getFloatTy());
  auto *sampler_min = builder.CreateBitCast(builder.CreateTrunc(texture.sampler_lod_range, getIntTy()), getFloatTy());
  auto *sampler_max = builder.CreateBitCast(builder.CreateTrunc(builder.CreateLShr(texture.sampler_lod_range, 32), getIntTy()), getFloatTy());
  auto *last = builder.CreateFSub(builder.CreateUIToFP(builder.CreateAnd(
      builder.CreateTrunc(builder.CreateLShr(texture.volume_view, 40), getIntTy()), getInt(255)), getFloatTy()), getFloat(1));
  auto *min_lod = CreateFPBinOp(fmax, sampler_min, CreateFPBinOp(fmax, resource_min, minimum));
  auto *valid = builder.CreateAnd(builder.CreateIsNotNull(handle),
      builder.CreateFCmpOLE(CreateFPBinOp(fmax, resource_min, minimum), last));
  auto *zero = Constant::getNullValue(getTexelType(texture));
  return CreateVolumeDispatch(texture, [&] {
    Value *direction = Constant::getNullValue(getFloatTy(3));
    Value *samples = getInt(1);
    if (dx && dy && texture.sample_type == Texture::sample_float) {
      auto *anisotropic = builder.CreateICmpNE(builder.CreateAnd(flags, getInt(1u << 9)), getInt(0));
      auto footprint = CreateVolumeDispatch(texture, [&] {
        Value *a = getFloat(0), *b = getFloat(0), *c = getFloat(0);
        for (unsigned axis = 0; axis < 3; ++axis) {
          auto *size = builder.CreateUIToFP(VolumeDimension(texture, axis, getInt(0)), getFloatTy());
          auto *x = builder.CreateFMul(builder.CreateExtractElement(dx, axis), size);
          auto *y = builder.CreateFMul(builder.CreateExtractElement(dy, axis), size);
          a = builder.CreateFAdd(a, builder.CreateFMul(x, x));
          b = builder.CreateFAdd(b, builder.CreateFMul(y, y));
          c = builder.CreateFAdd(c, builder.CreateFMul(x, y));
        }
        auto *delta = builder.CreateFSub(a, b);
        auto *disc = CreateFPUnOp(sqrt, builder.CreateFAdd(builder.CreateFMul(delta, delta),
            builder.CreateFMul(getFloat(4), builder.CreateFMul(c, c))), false);
        auto *major2 = builder.CreateFMul(getFloat(0.5f), builder.CreateFAdd(builder.CreateFAdd(a, b), disc));
        auto *minor2 = CreateFPBinOp(fmax, getFloat(0), builder.CreateFMul(getFloat(0.5f),
            builder.CreateFSub(builder.CreateFAdd(a, b), disc)));
        auto *cap = CreateIntBinOp(min, getInt(16), CreateIntBinOp(max, getInt(1),
            builder.CreateAnd(builder.CreateLShr(flags, 10), getInt(31))));
        auto *capf = builder.CreateUIToFP(cap, getFloatTy());
        minor2 = CreateFPBinOp(fmax, minor2, builder.CreateFDiv(major2, builder.CreateFMul(capf, capf)));
        auto *major = CreateFPUnOp(sqrt, major2, false);
        auto *minor = CreateFPUnOp(sqrt, minor2, false);
        auto *ratio = CreateFPBinOp(fmin, capf, CreateFPBinOp(fmax, getFloat(1),
            builder.CreateFDiv(major, CreateFPBinOp(fmax, minor, getFloat(0x1p-64f)))));
        auto *count = builder.CreateFPToUI(CreateFPUnOp(ceil, ratio, false), getIntTy());
        // Principal axis of the 3D footprint: an eigenvector of the 2x2 Gram matrix.
        auto *ex = c;
        auto *ey = builder.CreateFSub(major2, a);
        auto *norm = CreateFPUnOp(sqrt, builder.CreateFAdd(builder.CreateFMul(ex, ex), builder.CreateFMul(ey, ey)), false);
        auto *nonzero = builder.CreateFCmpOGT(norm, getFloat(0));
        auto *denominator = builder.CreateSelect(nonzero, norm, getFloat(1));
        ex = builder.CreateSelect(nonzero, builder.CreateFDiv(ex, denominator),
            builder.CreateSelect(builder.CreateFCmpOGE(a, b), getFloat(1), getFloat(0)));
        ey = builder.CreateSelect(nonzero, builder.CreateFDiv(ey, denominator),
            builder.CreateSelect(builder.CreateFCmpOGE(a, b), getFloat(0), getFloat(1)));
        auto *axis = builder.CreateFAdd(builder.CreateFMul(dx, builder.CreateVectorSplat(3, ex)),
                                      builder.CreateFMul(dy, builder.CreateVectorSplat(3, ey)));
        auto *isotropic_lod = builder.CreateFMul(CreateFPUnOp(log2, CreateFPBinOp(fmax, a, b), false), getFloat(0.5f));
        auto *minor_lod = builder.CreateFMul(CreateFPUnOp(log2, minor2, false), getFloat(0.5f));
        auto *adjusted = builder.CreateFAdd(lod, builder.CreateFSub(minor_lod, isotropic_lod));
        adjusted = builder.CreateSelect(builder.CreateFCmpOGT(major2, getFloat(0)), adjusted, lod);
        Value *packed = UndefValue::get(getFloatTy(4));
        for (unsigned axis_index = 0; axis_index < 3; ++axis_index)
          packed = builder.CreateInsertElement(packed, builder.CreateExtractElement(axis, axis_index), axis_index);
        packed = builder.CreateInsertElement(packed, adjusted, 3);
        return std::pair<Value *, Value *>{packed, count};
      }, [&] {
        return std::pair<Value *, Value *>{builder.CreateInsertElement(Constant::getNullValue(getFloatTy(4)), lod, 3), getInt(1)};
      }, anisotropic);
      direction = builder.CreateShuffleVector(footprint.first, footprint.first, ArrayRef<int>{0, 1, 2});
      lod = builder.CreateExtractElement(footprint.first, 3);
      samples = footprint.second;
    }
    // Sampler/resource/shader clamps participate in MIN/MAG selection; available mip count does not.
    lod = CreateFPBinOp(fmax, min_lod, CreateFPBinOp(fmax, sampler_min, CreateFPBinOp(fmin, sampler_max, lod)));
    auto *linear = builder.CreateICmpNE(builder.CreateAnd(flags, builder.CreateSelect(
        builder.CreateFCmpOLE(lod, getFloat(0)), getInt(1u << 7), getInt(1u << 6))), getInt(0));
    lod = CreateFPBinOp(fmax, getFloat(0), CreateFPBinOp(fmin, last, lod));
    auto *mip_linear = builder.CreateICmpNE(builder.CreateAnd(flags, getInt(1u << 8)), getInt(0));
    if (texture.sample_type != Texture::sample_float) {
      linear = getBool(false);
      mip_linear = getBool(false);
    }
    auto *lower = builder.CreateFPToUI(CreateFPUnOp(floor, builder.CreateSelect(mip_linear, lod,
        builder.CreateFAdd(lod, getFloat(0.5f))), false), getIntTy());
    auto *fraction = builder.CreateSelect(mip_linear,
        builder.CreateFSub(lod, CreateFPUnOp(floor, lod, false)), getFloat(0));
    Value *border = texture.sample_type == Texture::sample_float ? texture.sampler_float_border : texture.sampler_uint_border;
    if (texture.sample_type != Texture::sample_float) {
      auto *widths = builder.CreateTrunc(texture.volume_view, getIntTy());
      for (unsigned lane = 0; lane < 4; ++lane) {
        auto *width = builder.CreateAnd(builder.CreateLShr(widths, 8 + lane * 6), getInt(63));
        auto *shift = builder.CreateAnd(builder.CreateSub(getInt(32), width), getInt(31));
        auto *value = builder.CreateShl(builder.CreateExtractElement(border, lane), shift);
        value = texture.sample_type == Texture::sample_int ? builder.CreateAShr(value, shift) : builder.CreateLShr(value, shift);
        border = builder.CreateInsertElement(border, value, lane);
      }
    }
    Value *mapped_border = UndefValue::get(border->getType());
    for (unsigned lane = 0; lane < 4; ++lane) {
      auto *selector = builder.CreateAnd(builder.CreateTrunc(builder.CreateLShr(texture.volume_view, 48 + lane * 3), getIntTy()), getInt(7));
      auto *value = builder.CreateExtractElement(border, builder.CreateAnd(selector, getInt(3)));
      auto *one = texture.sample_type == Texture::sample_float ? static_cast<Value *>(getFloat(1)) : static_cast<Value *>(getInt(1));
      value = builder.CreateSelect(builder.CreateICmpULT(selector, getInt(4)), value,
          builder.CreateSelect(builder.CreateICmpEQ(selector, getInt(5)), one, Constant::getNullValue(one->getType())));
      mapped_border = builder.CreateInsertElement(mapped_border, value, lane);
    }
    border = mapped_border;
    auto sample_level = [&](Value *uv, Value *mip, bool interpolate) {
      Value *base = UndefValue::get(getIntTy(3));
      Value *weights[3];
      Value *sizes[3];
      for (unsigned axis = 0; axis < 3; ++axis) {
        sizes[axis] = VolumeDimension(texture, axis, mip);
        auto *coordinate = builder.CreateExtractElement(uv, axis);
        coordinate = getNaNToZero(coordinate);
        const unsigned shift = axis == 2 ? 17 : axis * 3;
        auto *mode = builder.CreateAnd(builder.CreateLShr(flags, shift), getInt(7));
        auto *wrap = builder.CreateFSub(coordinate, CreateFPUnOp(floor, coordinate, false));
        auto *half = builder.CreateFMul(coordinate, getFloat(0.5f));
        auto *mirror = builder.CreateFMul(builder.CreateFSub(half, CreateFPUnOp(floor, half, false)), getFloat(2));
        auto *bounded = CreateFPBinOp(fmax, getFloat(-2), CreateFPBinOp(fmin, getFloat(2), coordinate));
        coordinate = builder.CreateSelect(builder.CreateICmpEQ(mode, getInt(1)), wrap,
            builder.CreateSelect(builder.CreateICmpEQ(mode, getInt(2)), mirror,
                builder.CreateSelect(builder.CreateICmpEQ(mode, getInt(5)), CreateFPUnOp(fabs, bounded, false), bounded)));
        auto *position = builder.CreateFMul(coordinate, builder.CreateUIToFP(sizes[axis], getFloatTy()));
        if (interpolate) position = builder.CreateFSub(position, getFloat(0.5f));
        auto *floor_position = CreateFPUnOp(floor, position, false);
        weights[axis] = builder.CreateFSub(position, floor_position);
        auto *integer = builder.CreateFPToSI(floor_position, getIntTy());
        if (offset) integer = builder.CreateAdd(integer, builder.CreateExtractElement(offset, axis));
        base = builder.CreateInsertElement(base, integer, axis);
      }
      Value *color = zero;
      Value *status = builder.getInt8(0);
      Value *seen = getBool(false);
      for (unsigned tap = 0; tap < (interpolate ? 8u : 1u); ++tap) {
        Value *position = base, *weight = getFloat(1), *outside = getBool(false);
        for (unsigned axis = 0; axis < 3; ++axis) {
          auto *p = builder.CreateExtractElement(base, axis);
          if (interpolate) {
            const unsigned upper = (tap >> axis) & 1;
            p = builder.CreateAdd(p, getInt(upper));
            weight = builder.CreateFMul(weight, upper ? weights[axis] : builder.CreateFSub(getFloat(1), weights[axis]));
          }
          const unsigned shift = axis == 2 ? 17 : axis * 3;
          auto *mode = builder.CreateAnd(builder.CreateLShr(flags, shift), getInt(7));
          auto *size = sizes[axis];
          auto *off = builder.CreateICmpUGE(p, size);
          outside = builder.CreateOr(outside, builder.CreateAnd(builder.CreateICmpEQ(mode, getInt(4)), off));
          auto *period = builder.CreateMul(size, getInt(2));
          auto *wrapped = builder.CreateSRem(p, size);
          wrapped = builder.CreateSelect(builder.CreateICmpSLT(wrapped, getInt(0)), builder.CreateAdd(wrapped, size), wrapped);
          auto *mirrored = builder.CreateSRem(p, period);
          mirrored = builder.CreateSelect(builder.CreateICmpSLT(mirrored, getInt(0)), builder.CreateAdd(mirrored, period), mirrored);
          mirrored = builder.CreateSelect(builder.CreateICmpUGE(mirrored, size), builder.CreateSub(builder.CreateSub(period, getInt(1)), mirrored), mirrored);
          auto *clamped = CreateIntBinOp(min, CreateIntBinOp(max, p, getInt(0), true), builder.CreateSub(size, getInt(1)), true);
          p = builder.CreateSelect(builder.CreateICmpEQ(mode, getInt(1)), wrapped,
              builder.CreateSelect(builder.CreateICmpEQ(mode, getInt(2)), mirrored, clamped));
          position = builder.CreateInsertElement(position, p, axis);
        }
        auto read = CreateVolumeDispatch(texture, [&] {
          return CreateVolumeDispatch(texture, [&] { return std::pair<Value *, Value *>{border, builder.getInt8(0)}; },
              [&] { return CreateVolumeRead(texture, handle, position, mip, false); }, outside);
        }, [&] { return std::pair<Value *, Value *>{zero, builder.getInt8(0)}; }, builder.CreateFCmpOGT(weight, getFloat(0)));
        status = builder.CreateOr(status, read.second);
        if (texture.sample_type != Texture::sample_float) color = read.first;
        else {
          auto *sum = builder.CreateFAdd(color, builder.CreateFMul(read.first, builder.CreateVectorSplat(4, weight)));
          auto *reduced = builder.CreateSelect(builder.CreateICmpEQ(reduction, getInt(2)),
              CreateFPBinOp(fmin, color, read.first, false), CreateFPBinOp(fmax, color, read.first, false));
          reduced = builder.CreateSelect(seen, reduced, read.first);
          auto *active = builder.CreateFCmpOGT(weight, getFloat(0));
          reduced = builder.CreateSelect(active, reduced, color);
          color = builder.CreateSelect(builder.CreateICmpUGE(reduction, getInt(2)), reduced, sum);
          seen = builder.CreateOr(seen, active);
        }
      }
      return std::pair<Value *, Value *>{color, status};
    };
    auto sample = [&](Value *uv) {
      auto a = CreateVolumeDispatch(texture, [&] { return sample_level(uv, lower, true); },
          [&] { return sample_level(uv, lower, false); }, linear);
      if (texture.sample_type != Texture::sample_float) return a;
      auto b = CreateVolumeDispatch(texture, [&] {
        return CreateVolumeDispatch(texture, [&] { return sample_level(uv, builder.CreateAdd(lower, getInt(1)), true); },
            [&] { return sample_level(uv, builder.CreateAdd(lower, getInt(1)), false); }, linear);
      }, [&] { return a; }, builder.CreateFCmpOGT(fraction, getFloat(0)));
      auto *mixed = builder.CreateFAdd(builder.CreateFMul(a.first, builder.CreateVectorSplat(4, builder.CreateFSub(getFloat(1), fraction))),
                                      builder.CreateFMul(b.first, builder.CreateVectorSplat(4, fraction)));
      auto *reduced = builder.CreateSelect(builder.CreateICmpEQ(reduction, getInt(2)),
          CreateFPBinOp(fmin, a.first, b.first, false), CreateFPBinOp(fmax, a.first, b.first, false));
      return std::pair<Value *, Value *>{builder.CreateSelect(builder.CreateICmpUGE(reduction, getInt(2)), reduced, mixed),
                                       builder.CreateOr(a.second, b.second)};
    };
    if (!dx || !dy || texture.sample_type != Texture::sample_float) return sample(coord);
    return CreateVolumeDispatch(texture, [&] {
      auto *entry = builder.GetInsertBlock();
      auto *function = entry->getParent();
      auto *loop = BasicBlock::Create(getContext(), "volume.aniso", function);
      auto *done = BasicBlock::Create(getContext(), "volume.aniso.done", function);
      builder.CreateBr(loop);
      builder.SetInsertPoint(loop);
      auto *index = builder.CreatePHI(getIntTy(), 2);
      auto *sum = builder.CreatePHI(getTexelType(texture), 2);
      auto *status = builder.CreatePHI(builder.getInt8Ty(), 2);
      index->addIncoming(getInt(0), entry);
      sum->addIncoming(zero, entry);
      status->addIncoming(builder.getInt8(0), entry);
      auto *distance = builder.CreateFSub(builder.CreateFDiv(builder.CreateFAdd(builder.CreateUIToFP(index, getFloatTy()), getFloat(0.5f)),
          builder.CreateUIToFP(samples, getFloatTy())), getFloat(0.5f));
      auto *uv = builder.CreateFAdd(coord, builder.CreateFMul(direction, builder.CreateVectorSplat(3, distance)));
      auto read = sample(uv);
      auto *added = builder.CreateFAdd(sum, read.first);
      auto *reduced = builder.CreateSelect(builder.CreateICmpEQ(reduction, getInt(2)),
          CreateFPBinOp(fmin, sum, read.first, false), CreateFPBinOp(fmax, sum, read.first, false));
      reduced = builder.CreateSelect(builder.CreateICmpEQ(index, getInt(0)), read.first, reduced);
      auto *next_sum = builder.CreateSelect(builder.CreateICmpUGE(reduction, getInt(2)), reduced, added);
      auto *next_status = builder.CreateOr(status, read.second);
      auto *next = builder.CreateAdd(index, getInt(1));
      auto *tail = builder.GetInsertBlock();
      builder.CreateCondBr(builder.CreateICmpULT(next, samples), loop, done);
      index->addIncoming(next, tail);
      sum->addIncoming(next_sum, tail);
      status->addIncoming(next_status, tail);
      builder.SetInsertPoint(done);
      auto *averaged = builder.CreateFDiv(next_sum, builder.CreateVectorSplat(4, builder.CreateUIToFP(samples, getFloatTy())));
      return std::pair<Value *, Value *>{builder.CreateSelect(builder.CreateICmpUGE(reduction, getInt(2)), next_sum, averaged), next_status};
    }, [&] { return sample(coord); }, builder.CreateICmpUGT(samples, getInt(1)));
  }, [&] {
    return std::pair<Value *, Value *>{VolumeDefaults(texture, handle), builder.getInt8(0)};
  }, valid);
}

struct TextureOperationInfo {
  const char *air_symbol_suffix;
  bool is_array;
  uint8_t coord_dimension;
  bool is_cube;
  bool is_depth;
  bool is_ms;
  bool is_mipmaped;
};

TextureOperationInfo TextureInfo[] = {
    // texture_buffer,
    {"texture_buffer_1d", false, 1, false, false, false, false},
    // texture1d,
    {"texture_1d", false, 1, false, false, false, true /* but only 0 is valid */},
    // texture1d_array,
    {"texture_1d_array", true, 1, false, false, false, true /* but only 0 is valid */},
    // texture2d,
    {"texture_2d", false, 2, false, false, false, true},
    // texture2d_array,
    {"texture_2d_array", true, 2, false, false, false, true},
    // texture3d,
    {"texture_3d", false, 3, false, false, false, true},
    // texturecube,
    {"texture_cube", false, 3, true, false, false, true},
    // texturecube_array,
    {"texture_cube_array", true, 3, true, false, false, true},
    // texture2d_ms,
    {"texture_2d_ms", false, 2, false, false, true, false},
    // texture2d_ms_array,
    {"texture_2d_ms_array", true, 2, false, false, true, false},
    // depth2d,
    {"depth_2d", false, 2, false, true, false, true},
    // depth2d_array,
    {"depth_2d_array", true, 2, false, true, false, true},
    // depthcube,
    {"depth_cube", false, 3, true, true, false, true},
    // depthcube_array,
    {"depth_cube_array", true, 3, true, true, false, true},
    // depth_2d_ms,
    {"depth_2d_ms", false, 2, false, true, true, false},
    // depth_2d_ms_array,
    {"depth_2d_ms_array", true, 2, false, true, true, false},
};

std::string
AIRBuilder::getTypeOverloadSuffix(Type *Ty, Signedness Sign) {
  std::string Ret = ".";
  uint32_t PointerAddrSpace = ~0u;
  if (auto TyPtr = dyn_cast<PointerType>(Ty)) {
    PointerAddrSpace = TyPtr->getAddressSpace();
    Ty = TyPtr->getNonOpaquePointerElementType();
  }
  Type *TyScaler = Ty->getScalarType();
  uint32_t VectorSize = 0;
  if (isa<FixedVectorType>(Ty)) {
    VectorSize = cast<FixedVectorType>(Ty)->getNumElements();
  };
  if (TyScaler->isFloatTy()) {
    if (Sign != Signedness::DontCare)
      Ret += "f.";
    if (PointerAddrSpace != ~0u)
      Ret += std::format("p{}", PointerAddrSpace);
    if (VectorSize)
      Ret += std::format("v{}", VectorSize);
    Ret += "f32";
  } else if (TyScaler->isHalfTy()) {
    if (Sign != Signedness::DontCare)
      Ret += "f.";
    if (PointerAddrSpace != ~0u)
      Ret += std::format("p{}", PointerAddrSpace);
    if (VectorSize)
      Ret += std::format("v{}", VectorSize);
    Ret += "f16";
  } else if (TyScaler->isIntegerTy()) {
    if (Sign == Signedness::Signed)
      Ret += "s.";
    else if (Sign == Signedness::Unsigned)
      Ret += "u.";
    if (PointerAddrSpace != ~0u)
      Ret += std::format("p{}", PointerAddrSpace);
    if (VectorSize)
      Ret += std::format("v{}", VectorSize);
    Ret += std::format("i{}", cast<IntegerType>(TyScaler)->getBitWidth());
  } else {
    Ret += "unknown_type_overload";
  }
  return Ret;
}

Value *
AIRBuilder::CreateAtomicRMW(
    const Texture &Texture, Value *Handle, AtomicRMWInst::BinOp Op, Value *Pos, Value *ValVec4, Value *ArrayIndex
) {
  if (!Texture.atomic_phase_native && getModule()->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    auto native = Texture;
    native.atomic_phase_native = true;
    // Float exchange/CAS uses the scalar's bit pattern, not numerical conversion.
    native.sample_type = Texture::sample_uint;
    return CreateAtomicPhaseTypedAccess(Texture, Handle, Pos, ArrayIndex, nullptr, getInt(0), ValVec4, nullptr,
        Op, 2, 15, [&] { return std::pair<Value *, Value *>{CreateAtomicRMW(native, Handle, Op, Pos, ValVec4, ArrayIndex), builder.getInt8(0)}; }).first;
  }
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    return CreateVolumeDispatch(Texture, [&] {
      auto *valid = builder.CreateAnd(VolumeInBounds(Texture, Pos, getInt(0)), builder.CreateIsNotNull(Handle));
      return CreateVolumeDispatch(Texture, [&] {
        auto physical = native;
        physical.kind = Texture::texture2d_array;
        auto [xy, slice, tile] = VolumePosition(Texture, Pos, getInt(0));
        return CreateVolumeDispatch(Texture, [&] {
          auto *result = CreateAtomicRMW(physical, builder.CreateBitCast(Handle, getTextureHandleType(physical)),
                                        Op, xy, ValVec4, slice);
          return std::pair<Value *, Value *>{result, nullptr};
        }, [&] { return std::pair<Value *, Value *>{Constant::getNullValue(getTexelType(Texture)), nullptr}; },
            VolumeMapped(Texture, tile));
      }, [&] { return std::pair<Value *, Value *>{Constant::getNullValue(getTexelType(Texture)), nullptr}; }, valid);
    }, [&] { return std::pair<Value *, Value *>{CreateAtomicRMW(native, Handle, Op, Pos, ValVec4, ArrayIndex), nullptr}; }).first;
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  if (Texture.sample_type != Texture::sample_int && Texture.sample_type != Texture::sample_uint) {
    debug << "invalid operation: atomic operation on non integer texture.\n";
    return nullptr; // TODO:
  }

  if (TexInfo.is_depth) {
    debug << "invalid operation: atomic operation on depth texture.\n";
    return nullptr; // TODO:
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getTextureRWPositionType(Texture));
  Ops.push_back(Pos);

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  Tys.push_back(getTexelType(Texture));
  Ops.push_back(ValVec4);

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0)); // memory order relaxed

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(Texture.memory_access));

  std::string FnName = "air.atomic_";

  if (Op == llvm::AtomicRMWInst::Xchg) {
    FnName += "exchange_explicit_";
    FnName += TexInfo.air_symbol_suffix;
    FnName += getTypeOverloadSuffix(getTexelType(Texture), Signedness::Unsigned);
  } else {
    Signedness Sign = Signedness::Unsigned;
    FnName += "fetch_";
    switch (Op) {
    case llvm::AtomicRMWInst::Add:
      FnName += "add";
      break;
    case llvm::AtomicRMWInst::Sub:
      FnName += "sub";
      break;
    case llvm::AtomicRMWInst::And:
      FnName += "and";
      break;
    case llvm::AtomicRMWInst::Or:
      FnName += "or";
      break;
    case llvm::AtomicRMWInst::Xor:
      FnName += "xor";
      break;
    case llvm::AtomicRMWInst::Max:
      Sign = Signedness::Signed;
      FnName += "max";
      break;
    case llvm::AtomicRMWInst::Min:
      Sign = Signedness::Signed;
      FnName += "min";
      break;
    case llvm::AtomicRMWInst::UMax:
      FnName += "max";
      break;
    case llvm::AtomicRMWInst::UMin:
      FnName += "min";
      break;
    default:
      FnName += "<invalid atomic op>";
      break;
    }
    FnName += "_explicit_";
    FnName += TexInfo.air_symbol_suffix;
    FnName += getTypeOverloadSuffix(getTexelType(Texture), Sign);
  }

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getTexelType(Texture), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

std::pair<Value *, Value *>
AIRBuilder::CreateAtomicCmpXchg(
    const Texture &Texture, Value *Handle, Value *Pos, Value *CmpVec4, Value *NewVec4, Value *ArrayIndex
) {
  if (!Texture.atomic_phase_native && getModule()->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    auto native = Texture;
    native.atomic_phase_native = true;
    native.sample_type = Texture::sample_uint;
    auto result = CreateAtomicPhaseTypedAccess(Texture, Handle, Pos, ArrayIndex, nullptr, getInt(0), NewVec4,
        CmpVec4, AtomicRMWInst::Xchg, 3, 15, [&] {
          return std::pair<Value *, Value *>{CreateAtomicCmpXchg(native, Handle, Pos, CmpVec4, NewVec4, ArrayIndex).first, builder.getInt8(0)};
        });
    return {result.first, builder.CreateICmpEQ(builder.CreateExtractElement(result.first, uint64_t(0)),
                                              builder.CreateExtractElement(CmpVec4, uint64_t(0)))};
  }
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    return CreateVolumeDispatch(Texture, [&] {
      auto *valid = builder.CreateAnd(VolumeInBounds(Texture, Pos, getInt(0)), builder.CreateIsNotNull(Handle));
      return CreateVolumeDispatch(Texture, [&] {
        auto physical = native;
        physical.kind = Texture::texture2d_array;
        auto [xy, slice, tile] = VolumePosition(Texture, Pos, getInt(0));
        return CreateVolumeDispatch(Texture, [&] {
          return CreateAtomicCmpXchg(physical, builder.CreateBitCast(Handle, getTextureHandleType(physical)),
                                    xy, CmpVec4, NewVec4, slice);
        }, [&] { return std::pair<Value *, Value *>{Constant::getNullValue(getTexelType(Texture)), getBool(false)}; },
            VolumeMapped(Texture, tile));
      }, [&] { return std::pair<Value *, Value *>{Constant::getNullValue(getTexelType(Texture)), getBool(false)}; }, valid);
    }, [&] { return CreateAtomicCmpXchg(native, Handle, Pos, CmpVec4, NewVec4, ArrayIndex); });
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  if (Texture.sample_type != Texture::sample_int && Texture.sample_type != Texture::sample_uint) {
    debug << "invalid operation: atomic operation on non integer texture.\n";
    return {nullptr, nullptr}; // TODO:
  }

  if (TexInfo.is_depth) {
    debug << "invalid operation: atomic operation on depth texture.\n";
    return {nullptr, nullptr}; // TODO:
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Type *TyTexel = getTexelType(Texture);

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getTextureRWPositionType(Texture));
  Ops.push_back(Pos);

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  AllocaInst *Alloca = builder.CreateAlloca(TyTexel, 0u);
  Value *Ptr = builder.CreateConstGEP1_64(TyTexel, Alloca, 0u);
  builder.CreateStore(CmpVec4, Ptr);

  Tys.push_back(TyTexel->getPointerTo());
  Ops.push_back(Alloca);

  Tys.push_back(TyTexel);
  Ops.push_back(NewVec4);

  Tys.push_back(getIntTy()); // success
  Ops.push_back(getInt(0));  // memory order relaxed
  Tys.push_back(getIntTy()); // failure
  Ops.push_back(getInt(0));  // memory order relaxed

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(Texture.memory_access));

  std::string FnName = "air.atomic_compare_exchange_weak_explicit_";
  FnName += TexInfo.air_symbol_suffix;
  FnName += getTypeOverloadSuffix(getTexelType(Texture), Signedness::Unsigned);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getBoolTy(), Tys, false), Attrs);

  auto Ret = builder.CreateCall(Fn, Ops);

  return {builder.CreateLoad(TyTexel, Ptr), Ret};
}

Type *
AIRBuilder::getTextureHandleType(Texture::ResourceKind Kind) {
  assert(Kind <= Texture::last_resource_kind);
  return getOrCreateStructType(std::format("struct._{}_t", TextureInfo[Kind].air_symbol_suffix))
      ->getPointerTo(1);
};

Type *
AIRBuilder::getTexelType(const Texture &Texture) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &info = TextureInfo[Texture.kind];
  if (info.is_depth)
    return getFloatTy(); // depth texture must return a single float
  if (Texture.sample_type == Texture::sample_float)
    return getFloatTy(4);
  if (Texture.sample_type == Texture::sample_int)
    return getIntTy(4);
  if (Texture.sample_type == Texture::sample_uint)
    return getIntTy(4);
  llvm_unreachable("unhandled sample type");
}

Type *
AIRBuilder::getTexelGatherType(const Texture &Texture) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &info = TextureInfo[Texture.kind];
  if (info.is_depth)
    return getFloatTy(4); // depth texture gather must return float4
  return getTexelType(Texture);
}

Type *
AIRBuilder::getTextureSampleCoordType(const Texture &Texture) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &info = TextureInfo[Texture.kind];
  return getFloatTy(info.coord_dimension);
}

Type *
AIRBuilder::getTextureRWPositionType(const Texture &Texture) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &info = TextureInfo[Texture.kind];
  if (info.is_cube) {
    return getIntTy(2);
  }
  return getIntTy(info.coord_dimension);
}

Value *
AIRBuilder::CreateSampleFootprintLOD(const Texture &texture, Texture::ResourceKind logical, Value *handle,
                                   Value *dx, Value *dy) {
  IRBuilderBase::FastMathFlagGuard math_flags(builder);
  builder.clearFastMathFlags();
  const unsigned dimensions = logical == Texture::texture1d || logical == Texture::texture1d_array ? 1 :
                              logical == Texture::texture3d ? 3 : 2;
  static constexpr Texture::Query queries[] = {Texture::width, Texture::height, Texture::depth};
  Value *rx = getFloat(0), *ry = getFloat(0);
  for (unsigned axis = 0; axis < dimensions; ++axis) {
    auto *size = builder.CreateUIToFP(CreateTextureQuery(texture, handle, queries[axis], getInt(0)), getFloatTy());
    auto *x = builder.CreateFMul(dx->getType()->isVectorTy() ? builder.CreateExtractElement(dx, axis) : dx, size);
    auto *y = builder.CreateFMul(dy->getType()->isVectorTy() ? builder.CreateExtractElement(dy, axis) : dy, size);
    rx = builder.CreateFAdd(rx, builder.CreateFMul(x, x));
    ry = builder.CreateFAdd(ry, builder.CreateFMul(y, y));
  }
  return builder.CreateFMul(CreateFPUnOp(log2, CreateFPBinOp(fmax, rx, ry), false), getFloat(0.5f));
}


Value *
AIRBuilder::CreateIntegerBorder(const Texture &texture, Texture::ResourceKind logical, Value *handle,
    Value *metadata, Value *border, Value *lod_range, Value *resource_min_lod, Value *texel,
    Value *coord, Value *offset, Value *lod, Value *format_metadata, Value *gather_component) {
  if (!texel || !metadata || !border || texture.sample_type == Texture::sample_float)
    return texel;
  if (texture.kind == Texture::texture3d && texture.volume_layout) {
    auto native = texture;
    native.volume_layout = nullptr;
    return CreateVolumeDispatch(texture, [&] { return std::pair<Value *, Value *>{texel, nullptr}; }, [&] {
      return std::pair<Value *, Value *>{CreateIntegerBorder(native, logical, handle, metadata, border, lod_range,
          resource_min_lod, texel, coord, offset, lod, format_metadata, gather_component), nullptr};
    }).first;
  }
  if (logical == Texture::texturecube || logical == Texture::texturecube_array)
    return texel;
  IRBuilderBase::FastMathFlagGuard math_flags(builder);
  builder.clearFastMathFlags();
  const unsigned dimensions = logical == Texture::texture1d || logical == Texture::texture1d_array ? 1 :
                              logical == Texture::texture3d ? 3 : 2;
  auto *flags = builder.CreateTrunc(builder.CreateLShr(metadata, 32), getIntTy());
  auto *pixel = builder.CreateICmpNE(builder.CreateAnd(flags, getInt(1u << 15)), getInt(0));
  auto *enabled = builder.CreateICmpNE(builder.CreateAnd(flags, getInt(1u << 16)), getInt(0));
  auto *integer_sampler = enabled;
  enabled = builder.CreateAnd(enabled, builder.CreateICmpNE(handle, Constant::getNullValue(handle->getType())));
  static constexpr Texture::Query queries[] = {Texture::width, Texture::height, Texture::depth};
  if (!gather_component && lod_range) {
    auto *minimum = builder.CreateBitCast(builder.CreateTrunc(lod_range, getIntTy()), getFloatTy());
    auto *maximum = builder.CreateBitCast(builder.CreateTrunc(builder.CreateLShr(lod_range, 32), getIntTy()), getFloatTy());
    lod = CreateFPBinOp(fmax, minimum, CreateFPBinOp(fmin, maximum, lod));
    lod = CreateFPBinOp(fmax, lod, resource_min_lod);
  }
  if (format_metadata) {
    auto *widths = builder.CreateTrunc(format_metadata, getIntTy());
    for (unsigned channel = 0; channel < 4; ++channel) {
      auto *width = builder.CreateAnd(builder.CreateLShr(widths, 8 + channel * 6), getInt(63));
      auto *shift = builder.CreateAnd(builder.CreateSub(getInt(32), width), getInt(31));
      auto *value = builder.CreateShl(builder.CreateExtractElement(border, channel), shift);
      // Choose truncation, sign-extending the narrowed signed value. D3D also permits clamping.
      value = texture.sample_type == Texture::sample_int ? builder.CreateAShr(value, shift)
                                                       : builder.CreateLShr(value, shift);
      border = builder.CreateInsertElement(border, value, channel);
    }
  }
  auto *last = builder.CreateFSub(builder.CreateUIToFP(CreateTextureQuery(
      texture, handle, air::Texture::num_mip_levels, getInt(0)), getFloatTy()), getFloat(1));
  auto *view_empty = builder.CreateFCmpOGT(resource_min_lod, last);
  if (gather_component)
    view_empty = builder.CreateOr(view_empty, builder.CreateFCmpOGE(resource_min_lod, getFloat(1)));
  enabled = builder.CreateAnd(enabled, builder.CreateNot(view_empty));
  lod = CreateFPBinOp(fmax, getFloat(0), CreateFPBinOp(fmin, lod, last));
  lod = builder.CreateSelect(pixel, getFloat(0), lod);
  auto *mip = builder.CreateFPToUI(CreateFPUnOp(floor,
      builder.CreateFAdd(lod, getFloat(0.5f)), false), getIntTy());
  Value *result = texel;
  // Integer sampling is point-only. Gather has four independent point taps,
  // in D3D's lower-left, lower-right, upper-right, upper-left order.
  static constexpr unsigned tap_x[] = {0, 1, 1, 0}, tap_y[] = {1, 1, 0, 0};
  for (unsigned lane = 0; lane < (gather_component ? 4u : 1u); ++lane) {
    Value *outside = getBool(false);
    for (unsigned axis = 0; axis < dimensions; ++axis) {
      auto *size = builder.CreateUIToFP(CreateTextureQuery(texture, handle,
                                                              queries[axis], mip), getFloatTy());
      auto *uv = coord->getType()->isVectorTy() ? builder.CreateExtractElement(coord, axis) : coord;
      auto *position = builder.CreateSelect(pixel, uv, builder.CreateFMul(uv, size));
      if (gather_component)
        position = builder.CreateFSub(position, getFloat(0.5f));
      position = CreateFPUnOp(floor, position, false);
      if (offset) {
        auto *delta = offset->getType()->isVectorTy() ? builder.CreateExtractElement(offset, axis) : offset;
        position = builder.CreateFAdd(position, builder.CreateSIToFP(delta, getFloatTy()));
      }
      if (gather_component)
        position = builder.CreateFAdd(position, getFloat(axis == 0 ? tap_x[lane] : tap_y[lane]));
      const unsigned shift = axis == 2 ? 17 : 3 * axis;
      auto *border_axis = builder.CreateICmpEQ(builder.CreateAnd(builder.CreateLShr(flags, shift), getInt(7)), getInt(4));
      auto *off_axis = builder.CreateOr(builder.CreateFCmpOLT(position, getFloat(0)), builder.CreateFCmpOGE(position, size));
      outside = builder.CreateOr(outside, builder.CreateAnd(border_axis, off_axis));
    }
    outside = builder.CreateAnd(enabled, outside);
    if (!gather_component) {
      result = builder.CreateSelect(outside, border, result);
    } else {
      auto *border_component = builder.CreateExtractElement(border, gather_component);
      result = builder.CreateInsertElement(result, builder.CreateSelect(outside, border_component,
          builder.CreateExtractElement(result, lane)), lane);
    }
  }
  // An excluded mip has no border region: return the SRV's empty-view channel defaults.
  Value *empty = Constant::getNullValue(texel->getType());
  if (format_metadata) {
    auto *defaults = builder.CreateTrunc(format_metadata, getIntTy());
    for (unsigned channel = 0; channel < 4; ++channel) {
      auto *component = gather_component ? gather_component : getInt(channel);
      auto *one = builder.CreateAnd(builder.CreateLShr(defaults, component), getInt(1));
      empty = builder.CreateInsertElement(empty, one, channel);
    }
  }
  result = builder.CreateSelect(builder.CreateAnd(integer_sampler, view_empty), empty, result);
  return result;
}

std::pair<Value *, Value *>
AIRBuilder::CreateCoordinateSample(
    Value *Metadata, Value *TextureHandle, Value *ResourceMinLOD, Value *FormatDefaults, Value *GatherComponent,
    function_ref<std::pair<Value *, Value *>()> Pixel,
    function_ref<std::pair<Value *, Value *>()> Normalized
) {
  if (!Metadata)
    return Normalized();
  auto *pixel = builder.CreateICmpNE(builder.CreateAnd(Metadata, builder.getInt64(uint64_t(1) << 47)),
                                     builder.getInt64(0));
  auto *head = builder.GetInsertBlock();
  auto *function = head->getParent();
  auto point = builder.GetInsertPoint();
  auto *merge = point == head->end() ? BasicBlock::Create(getContext(), "sample.coordinate.merge", function)
                                    : head->splitBasicBlock(point, "sample.coordinate.merge");
  if (head->getTerminator())
    head->getTerminator()->eraseFromParent();
  auto *pixel_block = BasicBlock::Create(getContext(), "sample.pixel", function, merge);
  auto *pixel_view = BasicBlock::Create(getContext(), "sample.pixel.view", function, merge);
  auto *pixel_access = BasicBlock::Create(getContext(), "sample.pixel.access", function, merge);
  auto *pixel_empty = BasicBlock::Create(getContext(), "sample.pixel.empty", function, merge);
  auto *normalized_block = BasicBlock::Create(getContext(), "sample.normalized", function, merge);
  builder.SetInsertPoint(head);
  builder.CreateCondBr(pixel, pixel_block, normalized_block);
  builder.SetInsertPoint(pixel_block);
  builder.CreateCondBr(builder.CreateICmpNE(TextureHandle, Constant::getNullValue(TextureHandle->getType())),
                       pixel_view, merge);
  builder.SetInsertPoint(pixel_view);
  builder.CreateCondBr(builder.CreateFCmpOLE(ResourceMinLOD, getFloat(0)), pixel_access, pixel_empty);
  builder.SetInsertPoint(pixel_access);
  auto a = Pixel();
  auto *pixel_tail = builder.GetInsertBlock();
  builder.CreateBr(merge);
  builder.SetInsertPoint(normalized_block);
  auto b = Normalized();
  auto *normalized_tail = builder.GetInsertBlock();
  builder.CreateBr(merge);
  builder.SetInsertPoint(pixel_empty);
  Value *empty = Constant::getNullValue(a.first->getType());
  if (FormatDefaults) {
    auto *vector_type = dyn_cast<FixedVectorType>(a.first->getType());
    auto *scalar_type = vector_type ? vector_type->getElementType() : a.first->getType();
    auto *zero = Constant::getNullValue(scalar_type);
    Constant *one = scalar_type->isFloatingPointTy() ? ConstantFP::get(scalar_type, 1)
                                                   : ConstantInt::get(scalar_type, 1);
    auto *mask = builder.CreateTrunc(FormatDefaults, getIntTy());
    for (unsigned channel = 0; channel < (vector_type ? vector_type->getNumElements() : 1); ++channel) {
      auto *component = GatherComponent ? GatherComponent : getInt(channel);
      auto *bit = builder.CreateAnd(builder.CreateLShr(mask, component), getInt(1));
      auto *value = builder.CreateSelect(builder.CreateICmpNE(bit, getInt(0)), one, zero);
      empty = vector_type ? builder.CreateInsertElement(empty, value, uint64_t(channel)) : value;
    }
  }
  builder.CreateBr(merge);
  builder.SetInsertPoint(merge, merge->getFirstInsertionPt());
  auto *texel = builder.CreatePHI(a.first->getType(), 4);
  texel->addIncoming(a.first, pixel_tail);
  texel->addIncoming(b.first, normalized_tail);
  texel->addIncoming(empty, pixel_empty);
  texel->addIncoming(Constant::getNullValue(a.first->getType()), pixel_block);
  auto *residency = builder.CreatePHI(a.second->getType(), 4);
  residency->addIncoming(a.second, pixel_tail);
  residency->addIncoming(b.second, normalized_tail);
  residency->addIncoming(Constant::getNullValue(a.second->getType()), pixel_empty);
  residency->addIncoming(Constant::getNullValue(a.second->getType()), pixel_block);
  return {texel, residency};
}

std::pair<Value *, Value *>
AIRBuilder::CreateSampleCommon(
    const Texture &Texture, Value *Handle, Value *Sampler, Value *Coord, Value *ArrayIndex, Value *Offset,
    bool ArgsControlBit, Value *Args1, Value *Args2
) {
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    Value *dx = nullptr, *dy = nullptr;
    Value *lod = Args1;
    if (!ArgsControlBit) {
      dx = CreateDerivative(Coord, false);
      dy = CreateDerivative(Coord, true);
      lod = builder.CreateFAdd(CreateSampleFootprintLOD(Texture, Texture::texture3d, Handle, dx, dy), Args1);
    }
    return CreateVolumeDispatch(Texture, [&] { return CreateVolumeSample(Texture, Handle, Coord, Offset, lod, Args2, dx, dy); },
        [&] { return CreateSampleCommon(native, Handle, Sampler, Coord, ArrayIndex, Offset, ArgsControlBit, Args1, Args2); });
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::Convergent)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getSamplerHandleType());
  Ops.push_back(Sampler);

  if (TexInfo.is_depth) {
    Tys.push_back(getIntTy());
    Ops.push_back(getInt(1));
  }

  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(Coord));
  } else {
    Ops.push_back(Coord);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  switch (Texture.kind) {
  case Texture::texture1d:
  case Texture::texture1d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(false));
    Tys.push_back(getIntTy());
    Ops.push_back(Offset);
    break;
  case Texture::texture2d:
  case Texture::texture2d_array:
  case Texture::depth2d:
  case Texture::depth2d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(2));
    Ops.push_back(Offset);
    break;
  case Texture::texture3d:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(3));
    Ops.push_back(Offset);
    break;
  default:
    break;
  }

  Tys.push_back(getBoolTy());
  Ops.push_back(getBool(ArgsControlBit));
  Tys.push_back(getFloatTy());
  Ops.push_back(Args1);
  Tys.push_back(getFloatTy());
  Ops.push_back(Args2);

  /* access: always 0 = sample */
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  std::string FnName = "air.sample_";
  FnName += TexInfo.air_symbol_suffix;
  FnName += getTypeOverloadSuffix(getTexelType(Texture), getTexelSign(Texture));

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getTextureSampleResultType(Texture), Tys, false), Attrs
  );

  auto Result = builder.CreateCall(Fn, Ops);

  return {builder.CreateExtractValue(Result, {0u}), builder.CreateExtractValue(Result, {1u})};
};

std::pair<Value *, Value *>
AIRBuilder::CreateSampleCmpCommon(
    const Texture &Texture, Value *Handle, Value *Sampler, Value *Coord, Value *ArrayIndex, Value *Reference,
    Value *Offset, bool ArgsControlBit, Value *Args1, Value *Args2
) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  if (!TexInfo.is_depth) {
    debug << "invalid operation: compare on non depth texture.\n";
    return {nullptr, nullptr}; // TODO
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::Convergent)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getSamplerHandleType());
  Ops.push_back(Sampler);

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(1));

  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(Coord));
  } else {
    Ops.push_back(Coord);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  Tys.push_back(getFloatTy());
  Ops.push_back(Reference);

  switch (Texture.kind) {
  case Texture::depth2d:
  case Texture::depth2d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(2));
    Ops.push_back(Offset);
    break;
  default:
    break;
  }

  Tys.push_back(getBoolTy());
  Ops.push_back(getBool(ArgsControlBit));
  Tys.push_back(getFloatTy());
  Ops.push_back(Args1);
  Tys.push_back(getFloatTy());
  Ops.push_back(Args2);

  /* access: always 0 = sample */
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  std::string FnName = "air.sample_compare_";
  FnName += TexInfo.air_symbol_suffix;
  FnName += getTypeOverloadSuffix(getTexelType(Texture), getTexelSign(Texture));

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getTextureSampleResultType(Texture), Tys, false), Attrs
  );

  auto Result = builder.CreateCall(Fn, Ops);

  return {builder.CreateExtractValue(Result, {0u}), builder.CreateExtractValue(Result, {1u})};
};

std::pair<Value *, Value *>
AIRBuilder::CreateSampleGrad(
    const Texture &Texture, Value *Handle, Value *Sampler, Value *Coord, Value *ArrayIndex, Value *DerivX,
    Value *DerivY, Value *MinLOD, Value *Offset, Value *Reference
) {
  if (Texture.kind == Texture::texture3d && Texture.volume_layout && !Reference) {
    auto native = Texture;
    native.volume_layout = nullptr;
    auto *bias = builder.CreateBitCast(builder.CreateTrunc(Texture.sampler_metadata, getIntTy()), getFloatTy());
    auto *lod = builder.CreateFAdd(CreateSampleFootprintLOD(Texture, Texture::texture3d, Handle, DerivX, DerivY), bias);
    return CreateVolumeDispatch(Texture, [&] { return CreateVolumeSample(Texture, Handle, Coord, Offset, lod, MinLOD, DerivX, DerivY); },
        [&] { return CreateSampleGrad(native, Handle, Sampler, Coord, ArrayIndex, DerivX, DerivY, MinLOD, Offset, Reference); });
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];
  if (Reference && !TexInfo.is_depth) {
    debug << "invalid operation: compare on non depth texture.\n";
    return {nullptr, nullptr};
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getSamplerHandleType());
  Ops.push_back(Sampler);

  if (TexInfo.is_depth) {
    Tys.push_back(getIntTy());
    Ops.push_back(getInt(1));
  }

  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(Coord));
  } else {
    Ops.push_back(Coord);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  /* `air.sample_compare_<depth>_grad` carries the reference right after the coordinate
   * (and array index), exactly as Metal's own `sample_compare(..., gradient2d)` does. */
  if (Reference) {
    Tys.push_back(getFloatTy());
    Ops.push_back(Reference);
  }

  Tys.push_back(getTextureSampleCoordType(Texture));
  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(DerivX));
    Ops.push_back(getNaNToZero(DerivY));
  } else {
    Ops.push_back(DerivX);
    Ops.push_back(DerivY);
  }

  Tys.push_back(getFloatTy());
  Ops.push_back(MinLOD);

  switch (Texture.kind) {
  case Texture::texture1d:
  case Texture::texture1d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(false));
    Tys.push_back(getIntTy());
    Ops.push_back(Offset);
    break;
  case Texture::texture2d:
  case Texture::texture2d_array:
  case Texture::depth2d:
  case Texture::depth2d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(2));
    Ops.push_back(Offset);
    break;
  case Texture::texture3d:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(3));
    Ops.push_back(Offset);
    break;
  default:
    break;
  }

  /* access: always 0 = sample */
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  std::string FnName = Reference ? "air.sample_compare_" : "air.sample_";
  FnName += TexInfo.air_symbol_suffix;
  FnName += "_grad";
  FnName += getTypeOverloadSuffix(getTexelType(Texture), getTexelSign(Texture));

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getTextureSampleResultType(Texture), Tys, false), Attrs
  );

  auto Result = builder.CreateCall(Fn, Ops);

  return {builder.CreateExtractValue(Result, {0u}), builder.CreateExtractValue(Result, {1u})};
}

std::pair<Value *, Value *>
AIRBuilder::CreateRead(
    const Texture &Texture, Value *Handle, Value *Pos, Value *ArrayIndex, Value *SampleIndexOrCubeFace, Value *Level,
    bool DeviceCoherent, Value *MSAAMetadata, Value *MSAASampleIds
) {
  if (!Texture.atomic_phase_native && getModule()->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    auto native = Texture;
    native.atomic_phase_native = true;
    return CreateAtomicPhaseTypedAccess(Texture, Handle, Pos, ArrayIndex, SampleIndexOrCubeFace, Level, nullptr,
        nullptr, AtomicRMWInst::Xchg, 0, 15, [&] {
          return CreateRead(native, Handle, Pos, ArrayIndex, SampleIndexOrCubeFace, Level,
                            DeviceCoherent, MSAAMetadata, MSAASampleIds);
        });
  }
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    return CreateVolumeDispatch(Texture, [&] { return CreateVolumeRead(Texture, Handle, Pos, Level, DeviceCoherent); },
        [&] { return CreateRead(native, Handle, Pos, ArrayIndex, SampleIndexOrCubeFace, Level, DeviceCoherent); });
  }
  if (SampleIndexOrCubeFace && MSAAMetadata && MSAASampleIds &&
      (Texture.kind == Texture::texture2d_ms || Texture.kind == Texture::texture2d_ms_array ||
       Texture.kind == Texture::texture2d_array)) {
    auto *head = builder.GetInsertBlock();
    auto *function = head->getParent();
    auto point = builder.GetInsertPoint();
    auto *merge = point == head->end() ? BasicBlock::Create(getContext(), "msaa.read.merge", function)
                                     : head->splitBasicBlock(point, "msaa.read.merge");
    if (head->getTerminator()) head->getTerminator()->eraseFromParent();
    auto *shadow = BasicBlock::Create(getContext(), "msaa.read.shadow", function, merge);
    auto *native = BasicBlock::Create(getContext(), "msaa.read.native", function, merge);
    builder.SetInsertPoint(head);
    auto *enabled = builder.CreateICmpNE(builder.CreateAnd(MSAAMetadata, builder.getInt64(2)), builder.getInt64(0));
    builder.CreateCondBr(enabled, shadow, native);
    builder.SetInsertPoint(shadow);
    auto *count = builder.CreateTrunc(builder.CreateLShr(MSAAMetadata, 8), getIntTy());
    count = builder.CreateAnd(count, getInt(255));
    auto *valid = builder.CreateICmpULT(SampleIndexOrCubeFace, count);
    auto *sample = builder.CreateSelect(valid, SampleIndexOrCubeFace, getInt(0));
    auto *table = builder.CreateIntToPtr(MSAASampleIds, builder.getInt64Ty()->getPointerTo(1));
    auto *id = builder.CreateLoad(builder.getInt64Ty(), builder.CreateGEP(builder.getInt64Ty(), table, sample));
    auto *handle = builder.CreateIntToPtr(id, getTextureHandleType(Texture::texture2d_array));
    auto physical = Texture;
    physical.kind = Texture::texture2d_array;
    CreateTextureFence(physical, handle);
    auto a = CreateRead(physical, handle, Pos, ArrayIndex ? ArrayIndex : getInt(0), nullptr, getInt(0), DeviceCoherent);
    a.first = builder.CreateSelect(valid, a.first, Constant::getNullValue(a.first->getType()));
    builder.CreateBr(merge);
    auto *shadow_tail = builder.GetInsertBlock();
    builder.SetInsertPoint(native);
    std::pair<Value *, Value *> b;
    if (Texture.kind == Texture::texture2d_array) {
      b = {Constant::getNullValue(a.first->getType()), builder.getInt8(0)};
    } else {
      b = CreateRead(Texture, Handle, Pos, ArrayIndex, SampleIndexOrCubeFace, Level, DeviceCoherent);
    }
    builder.CreateBr(merge);
    auto *native_tail = builder.GetInsertBlock();
    builder.SetInsertPoint(merge, merge->getFirstInsertionPt());
    auto *texel = builder.CreatePHI(a.first->getType(), 2);
    texel->addIncoming(a.first, shadow_tail);
    texel->addIncoming(b.first, native_tail);
    auto *residency = builder.CreatePHI(a.second->getType(), 2);
    residency->addIncoming(a.second, shadow_tail);
    residency->addIncoming(b.second, native_tail);
    return {texel, residency};
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  // AIR 2.9 reads take the built-in read sampler and explicit offsets.
  // Advertising 2.9 with the legacy signature crashes TextureCheckPass before
  // PSO creation; older AIR modules retain their original intrinsic ABI.
  auto *airVersion = getModule()->getNamedMetadata("air.version");
  bool modernRead = airVersion && airVersion->getNumOperands() &&
      mdconst::extract<ConstantInt>(airVersion->getOperand(0)->getOperand(1))->getZExtValue() >= 9;
  if (modernRead) {
    auto *samplerType = getSamplerHandleType();
    auto sampler = getModule()->getOrInsertFunction(
        "air.get_read_sampler", FunctionType::get(samplerType, false),
        AttributeList::get(Context, {{~0U, Attribute::get(Context, Attribute::ReadOnly)},
            {~0U, Attribute::get(Context, Attribute::InaccessibleMemOnly)},
            {~0U, Attribute::get(Context, Attribute::NoUnwind)},
            {~0U, Attribute::get(Context, Attribute::WillReturn)}}));
    Tys.push_back(samplerType);
    Ops.push_back(builder.CreateCall(sampler));
  }

  if (TexInfo.is_depth) {
    Tys.push_back(getIntTy());
    Ops.push_back(getInt(1));
  }

  Tys.push_back(getTextureRWPositionType(Texture));
  Ops.push_back(Pos);

  if (TexInfo.is_cube) {
    Tys.push_back(getIntTy());
    Ops.push_back(SampleIndexOrCubeFace);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  if (TexInfo.is_ms) {
    Tys.push_back(getIntTy());
    Ops.push_back(SampleIndexOrCubeFace);
  }

  if (modernRead && !TexInfo.is_ms && Texture.kind != Texture::texture_buffer) {
    auto *offsetType = getTextureRWPositionType(Texture);
    Tys.push_back(offsetType);
    Ops.push_back(Constant::getNullValue(offsetType));
  }

  if (TexInfo.is_mipmaped) {
    Tys.push_back(getIntTy());
    Ops.push_back(modernRead && (!Level || isa<UndefValue>(Level)) ? getInt(0) : Level);
  }

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(Texture.memory_access));

  std::string FnName = "air.read_";
  FnName += TexInfo.air_symbol_suffix;
  if (DeviceCoherent)
    FnName += ".device_coherent";
  FnName += getTypeOverloadSuffix(getTexelType(Texture), getTexelSign(Texture));

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getTextureSampleResultType(Texture), Tys, false), Attrs
  );

  auto Result = builder.CreateCall(Fn, Ops);

  return {builder.CreateExtractValue(Result, {0u}), builder.CreateExtractValue(Result, {1u})};
}

CallInst *
AIRBuilder::CreateWrite(
    const Texture &Texture, Value *Handle, Value *Pos, Value *ArrayIndex, Value *CubeFace, Value *Level, Value *ValVec4,
    bool DeviceCoherent, uint32_t WriteMask
) {
  if (!Texture.atomic_phase_native && getModule()->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    auto native = Texture;
    native.atomic_phase_native = true;
    CallInst *call = nullptr;
    CreateAtomicPhaseTypedAccess(Texture, Handle, Pos, ArrayIndex, nullptr, Level, ValVec4, nullptr,
        AtomicRMWInst::Xchg, 1, WriteMask, [&] {
          call = CreateWrite(native, Handle, Pos, ArrayIndex, CubeFace, Level, ValVec4, DeviceCoherent, WriteMask);
          return std::pair<Value *, Value *>{Constant::getNullValue(getTexelType(Texture)), builder.getInt8(0)};
        });
    return call;
  }
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    CallInst *operation = nullptr;
    CreateVolumeDispatch(Texture, [&] {
      auto *valid = builder.CreateAnd(VolumeInBounds(Texture, Pos, Level), builder.CreateIsNotNull(Handle));
      return CreateVolumeDispatch(Texture, [&] {
        auto physical = native;
        physical.kind = Texture::texture2d_array;
        auto [xy, slice, tile] = VolumePosition(Texture, Pos, Level);
        return CreateVolumeDispatch(Texture, [&] {
          operation = CreateWrite(physical, builder.CreateBitCast(Handle, getTextureHandleType(physical)),
                                  xy, slice, nullptr, getInt(0), ValVec4, DeviceCoherent, WriteMask);
          return std::pair<Value *, Value *>{nullptr, nullptr};
        }, [&] { return std::pair<Value *, Value *>{nullptr, nullptr}; }, VolumeMapped(Texture, tile));
      }, [&] { return std::pair<Value *, Value *>{nullptr, nullptr}; }, valid);
    }, [&] {
      operation = CreateWrite(native, Handle, Pos, ArrayIndex, CubeFace, Level, ValVec4, DeviceCoherent, WriteMask);
      return std::pair<Value *, Value *>{nullptr, nullptr};
    });
    return operation;
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getTextureRWPositionType(Texture));
  Ops.push_back(Pos);

  if (TexInfo.is_cube) {
    Tys.push_back(getIntTy());
    Ops.push_back(CubeFace);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  Tys.push_back(getTexelType(Texture));
  Ops.push_back(ValVec4);

  if (TexInfo.is_mipmaped) {
    Tys.push_back(getIntTy());
    Ops.push_back(Level);
  }

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(Texture.memory_access));

  std::string FnName = "air.write_";
  FnName += TexInfo.air_symbol_suffix;
  if (DeviceCoherent)
    FnName += ".device_coherent";
  FnName += getTypeOverloadSuffix(getTexelType(Texture), getTexelSign(Texture));

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getVoidTy(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

std::pair<Value *, Value *>
AIRBuilder::CreateGather(
    const Texture &Texture, Value *Handle, Value *Sampler, Value *Coord, Value *ArrayIndex, Value *Offset,
    Value *Component
) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getSamplerHandleType());
  Ops.push_back(Sampler);

  if (TexInfo.is_depth) {
    Tys.push_back(getIntTy());
    Ops.push_back(getInt(1));
  }

  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(Coord));
  } else {
    Ops.push_back(Coord);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  switch (Texture.kind) {
  case Texture::texture2d:
  case Texture::texture2d_array:
  case Texture::depth2d:
  case Texture::depth2d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(2));
    Ops.push_back(Offset);
    break;
  default: // cube doesn't support offset, and others don't support gather
    break;
  }

  if (!TexInfo.is_depth) {
    Tys.push_back(getIntTy());
    Ops.push_back(Component);
  }

  /* access: always 0 = sample */
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  std::string FnName = "air.gather_";
  FnName += TexInfo.air_symbol_suffix;
  FnName += getTypeOverloadSuffix(getTexelGatherType(Texture), getTexelSign(Texture));

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getTextureGatherResultType(Texture), Tys, false), Attrs
  );

  auto Result = builder.CreateCall(Fn, Ops);

  return {builder.CreateExtractValue(Result, {0u}), builder.CreateExtractValue(Result, {1u})};
};

std::pair<Value *, Value *>
AIRBuilder::CreateGatherCompare(
    const Texture &Texture, Value *Handle, Value *Sampler, Value *Coord, Value *ArrayIndex, Value *Reference,
    Value *Offset
) {
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  if (!TexInfo.is_depth) {
    debug << "invalid operation: compare on non depth texture.\n";
    return {nullptr, nullptr}; // TODO
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getSamplerHandleType());
  Ops.push_back(Sampler);

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(1));

  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(Coord));
  } else {
    Ops.push_back(Coord);
  }

  if (TexInfo.is_array) {
    Tys.push_back(getIntTy());
    Ops.push_back(ArrayIndex);
  }

  Tys.push_back(getFloatTy());
  Ops.push_back(Reference);

  switch (Texture.kind) {
  case Texture::depth2d:
  case Texture::depth2d_array:
    Tys.push_back(getBoolTy());
    Ops.push_back(getBool(true));
    Tys.push_back(getIntTy(2));
    Ops.push_back(Offset);
    break;
  default: // cube doesn't support offset, and others don't support gather_c
    break;
  }

  /* access: always 0 = sample */
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  std::string FnName = "air.gather_compare_";
  FnName += TexInfo.air_symbol_suffix;
  FnName += ".f32";

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getTextureGatherResultType(Texture), Tys, false), Attrs
  );

  auto Result = builder.CreateCall(Fn, Ops);

  return {builder.CreateExtractValue(Result, {0u}), builder.CreateExtractValue(Result, {1u})};
};

Value *
AIRBuilder::CreateTextureQuery(const Texture &Texture, Value *Handle, Texture::Query Query, Value *Level) {
  if ((Texture.kind == Texture::texture2d_ms || Texture.kind == Texture::texture2d_ms_array ||
       Texture.kind == Texture::depth_2d_ms || Texture.kind == Texture::depth_2d_ms_array) &&
      Texture.volume_layout && Texture.volume_metadata) {
    // D3D12 q2 holds the per-sample view table when metadata bit 1 is set.
    // Depth casts expose sample zero as a 2D-array handle, not a native MSAA
    // handle; query the same physical views as CreateRead instead of passing
    // that handle to a multisample AIR intrinsic. D3D11 has no q2 table.
    auto native = Texture;
    native.volume_layout = nullptr;
    auto *shadow = builder.CreateICmpNE(builder.CreateAnd(Texture.volume_metadata, builder.getInt64(2)),
                                       builder.getInt64(0));
    return CreateVolumeDispatch(Texture, [&] {
      Value *value;
      if (Query == Texture::num_samples) {
        value = builder.CreateAnd(builder.CreateTrunc(builder.CreateLShr(Texture.volume_metadata, 8), getIntTy()),
                                  getInt(255));
      } else {
        auto *table = builder.CreateIntToPtr(Texture.volume_layout, builder.getInt64Ty()->getPointerTo(1));
        auto *id = builder.CreateLoad(builder.getInt64Ty(), table);
        auto physical = native;
        physical.kind = Texture::texture2d_array;
        auto *handle = builder.CreateIntToPtr(id, getTextureHandleType(physical));
        value = CreateTextureQuery(physical, handle, Query, getInt(0));
      }
      value = builder.CreateSelect(builder.CreateIsNull(Handle), getInt(0), value);
      return std::pair<Value *, Value *>{value, nullptr};
    }, [&] { return std::pair<Value *, Value *>{CreateTextureQuery(native, Handle, Query, Level), nullptr}; }, shadow).first;
  }
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    return CreateVolumeDispatch(Texture, [&] {
      auto *count = builder.CreateAnd(builder.CreateTrunc(builder.CreateLShr(Texture.volume_view, 40), getIntTy()), getInt(255));
      Value *value = getInt(1);
      if (Query == Texture::num_mip_levels) value = count;
      else if (Query <= Texture::depth) {
        value = VolumeDimension(Texture, unsigned(Query), Level);
        value = builder.CreateSelect(builder.CreateICmpULT(Level, count), value, getInt(0));
      }
      value = builder.CreateSelect(builder.CreateIsNull(Handle), getInt(0), value);
      return std::pair<Value *, Value *>{value, nullptr};
    }, [&] { return std::pair<Value *, Value *>{CreateTextureQuery(native, Handle, Query, Level), nullptr}; }).first;
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  static const char *QUERIES[] = {
      "get_width", "get_height", "get_depth", "get_array_size", "get_num_mip_levels", "get_num_samples",
  };

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  switch (Query) {
  case Texture::width:
  case Texture::height:
  case Texture::depth:
    if (TexInfo.is_mipmaped) {
      Tys.push_back(getIntTy());
      Ops.push_back(Level);
    }
    break;
  default:
    break;
  }

  std::string FnName = "air.";
  FnName += QUERIES[Query];
  FnName += "_";
  FnName += TexInfo.air_symbol_suffix;

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getIntTy(), Tys, false), Attrs);

  return builder.CreateSelect(builder.CreateIsNull(Handle), builder.getInt32(0), builder.CreateCall(Fn, Ops));
}

std::pair<Value *, Value *>
AIRBuilder::CreateCalculateLODGrad(const Texture &texture, Value *handle, Value *dx, Value *dy) {
  IRBuilderBase::FastMathFlagGuard math_flags(builder);
  builder.clearFastMathFlags();
  auto *unclamped = CreateSampleFootprintLOD(texture, Texture::texture3d, handle, dx, dy);
  auto *bias = builder.CreateBitCast(builder.CreateTrunc(texture.sampler_metadata, getIntTy()), getFloatTy());
  unclamped = builder.CreateFAdd(unclamped, bias);
  auto *flags = builder.CreateTrunc(builder.CreateLShr(texture.sampler_metadata, 32), getIntTy());
  auto *anisotropic = builder.CreateICmpNE(builder.CreateAnd(flags, getInt(1u << 9)), getInt(0));
  auto footprint = CreateVolumeDispatch(texture, [&] {
    Value *a = getFloat(0), *b = getFloat(0), *c = getFloat(0);
    for (unsigned axis = 0; axis < 3; ++axis) {
      auto *size = builder.CreateUIToFP(CreateTextureQuery(texture, handle, Texture::Query(axis), getInt(0)), getFloatTy());
      auto *x = builder.CreateFMul(builder.CreateExtractElement(dx, axis), size);
      auto *y = builder.CreateFMul(builder.CreateExtractElement(dy, axis), size);
      a = builder.CreateFAdd(a, builder.CreateFMul(x, x));
      b = builder.CreateFAdd(b, builder.CreateFMul(y, y));
      c = builder.CreateFAdd(c, builder.CreateFMul(x, y));
    }
    auto *delta = builder.CreateFSub(a, b);
    auto *disc = CreateFPUnOp(sqrt, builder.CreateFAdd(builder.CreateFMul(delta, delta),
        builder.CreateFMul(getFloat(4), builder.CreateFMul(c, c))), false);
    auto *major2 = builder.CreateFMul(getFloat(0.5f), builder.CreateFAdd(builder.CreateFAdd(a, b), disc));
    auto *minor2 = CreateFPBinOp(fmax, getFloat(0), builder.CreateFMul(getFloat(0.5f),
        builder.CreateFSub(builder.CreateFAdd(a, b), disc)));
    auto *cap = builder.CreateUIToFP(CreateIntBinOp(min, getInt(16), CreateIntBinOp(max, getInt(1),
        builder.CreateAnd(builder.CreateLShr(flags, 10), getInt(31)))), getFloatTy());
    minor2 = CreateFPBinOp(fmax, minor2, builder.CreateFDiv(major2, builder.CreateFMul(cap, cap)));
    auto *result = builder.CreateSelect(builder.CreateFCmpOGT(major2, getFloat(0)),
        builder.CreateFAdd(builder.CreateFMul(getFloat(0.5f), CreateFPUnOp(log2, minor2, false)), bias), unclamped);
    return std::pair<Value *, Value *>{result, nullptr};
  }, [&] { return std::pair<Value *, Value *>{unclamped, nullptr}; }, anisotropic);
  auto *minimum = builder.CreateBitCast(builder.CreateTrunc(texture.sampler_lod_range, getIntTy()), getFloatTy());
  auto *maximum = builder.CreateBitCast(builder.CreateTrunc(builder.CreateLShr(texture.sampler_lod_range, 32), getIntTy()), getFloatTy());
  auto *resource_min = builder.CreateBitCast(builder.CreateTrunc(texture.volume_metadata, getIntTy()), getFloatTy());
  auto *last = builder.CreateFSub(builder.CreateUIToFP(CreateTextureQuery(texture, handle, Texture::num_mip_levels, getInt(0)), getFloatTy()), getFloat(1));
  auto *clamped = CreateFPBinOp(fmin, last, CreateFPBinOp(fmax, resource_min, CreateFPBinOp(fmax, getFloat(0),
      CreateFPBinOp(fmax, minimum, CreateFPBinOp(fmin, maximum, footprint.first)))));
  clamped = builder.CreateSelect(builder.CreateOr(builder.CreateIsNull(handle), builder.CreateFCmpOGT(resource_min, last)), getFloat(0), clamped);
  return {clamped, footprint.first};
}

std::pair<Value *, Value *>
AIRBuilder::CreateCalculateLOD(const Texture &Texture, Value *Handle, Value *Sampler, Value *Coord) {
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    auto *dx = CreateDerivative(Coord, false);
    auto *dy = CreateDerivative(Coord, true);
    return CreateVolumeDispatch(Texture, [&] { return CreateCalculateLODGrad(Texture, Handle, dx, dy); },
        [&] { return CreateCalculateLOD(native, Handle, Sampler, Coord); });
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {2U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::Convergent)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  Tys.push_back(getSamplerHandleType());
  Ops.push_back(Sampler);

  Tys.push_back(getTextureSampleCoordType(Texture));
  if (options.sampleNaNToZero) {
    Ops.push_back(getNaNToZero(Coord));
  } else {
    Ops.push_back(Coord);
  }

  /* access: always 0 = sample */
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  std::string FnName = "air.calculate_clamped_lod_";
  FnName += TexInfo.air_symbol_suffix;

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getFloatTy(), Tys, false), Attrs);

  std::string FnUnclampedName = "air.calculate_unclamped_lod_";
  FnUnclampedName += TexInfo.air_symbol_suffix;

  auto FnUnclamped =
      getModule()->getOrInsertFunction(FnUnclampedName, FunctionType::get(getFloatTy(), Tys, false), Attrs);

  return {builder.CreateCall(Fn, Ops), builder.CreateCall(FnUnclamped, Ops)};
}

CallInst *
AIRBuilder::CreateTextureFence(const Texture &Texture, Value *Handle) {
  if (Texture.kind == Texture::texture3d && Texture.volume_layout) {
    auto native = Texture;
    native.volume_layout = nullptr;
    CallInst *operation = nullptr;
    CreateVolumeDispatch(Texture, [&] {
      auto physical = native;
      physical.kind = Texture::texture2d_array;
      operation = CreateTextureFence(physical, builder.CreateBitCast(Handle, getTextureHandleType(physical)));
      return std::pair<Value *, Value *>{nullptr, nullptr};
    }, [&] {
      operation = CreateTextureFence(native, Handle);
      return std::pair<Value *, Value *>{nullptr, nullptr};
    });
    return operation;
  }
  assert(Texture.kind <= Texture::last_resource_kind);
  auto &TexInfo = TextureInfo[Texture.kind];

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::MustProgress)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getTextureHandleType(Texture));
  Ops.push_back(Handle);

  std::string FnName = "air.fence_";
  FnName += TexInfo.air_symbol_suffix;

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getVoidTy(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateGetNumSamples() {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context,
      {
          {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
          {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
      }
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(0));

  auto Fn =
      getModule()->getOrInsertFunction("air.get_num_samples.i32", FunctionType::get(getIntTy(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateDerivative(Value *Val, bool YAxis) {
  if (!Val->getType()->getScalarType()->isFloatingPointTy()) {
    debug << "invalid operation: derivative on non-fp type.\n";
    return nullptr; // TODO
  }
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(Val->getType());
  Ops.push_back(Val);

  std::string FnName = "air.";
  FnName += YAxis ? "dfdy" : "dfdx";
  FnName += getTypeOverloadSuffix(Val->getType(), Signedness::DontCare);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(Val->getType(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateGetSamplePosition(Value *SampleCount, Value *Index) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
  );
  auto Fn = getModule()->getOrInsertFunction(
      "dxmt.sample_pos.i32", FunctionType::get(getFloatTy(2), {getIntTy(), getIntTy()}, false), Attrs
  );
  return builder.CreateCall(Fn, {SampleCount, Index});
}

CallInst *
AIRBuilder::CreateDiscard() {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  auto Fn = getModule()->getOrInsertFunction("air.discard_fragment", FunctionType::get(getVoidTy(), {}, false), Attrs);

  return builder.CreateCall(Fn, {});
}

CallInst *
AIRBuilder::CreateBarrier(MemFlags Flags, bool SimdGroup) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::Convergent)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(uint32_t(Flags)));
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(1)); // always 1 ?

  auto Fn = getModule()->getOrInsertFunction(
      SimdGroup ? "air.simdgroup.barrier" : "air.wg.barrier", FunctionType::get(getVoidTy(), Tys, false), Attrs
  );

  return builder.CreateCall(Fn, Ops);
}

CallInst *
AIRBuilder::CreateAtomicFence(MemFlags Flags, ThreadScope Scope, bool Relaxed) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::MustProgress)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(getIntTy());
  Ops.push_back(getInt(uint32_t(Flags)));
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(Relaxed ? 0 : 5));
  Tys.push_back(getIntTy());
  Ops.push_back(getInt(uint32_t(Scope)));

  auto Fn = getModule()->getOrInsertFunction("air.atomic.fence", FunctionType::get(getVoidTy(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateFMA(Value *X, Value *Y, Value *Z) {
  if (!X->getType()->getScalarType()->isFloatingPointTy()) {
    debug << "invalid operation: fma on non-fp type.\n";
    return nullptr; // TODO
  }
  if (X->getType() != Y->getType() || X->getType() != Z->getType()) {
    debug << "invalid operation: fma has unmatched operand types.\n";
    return nullptr; // TODO
  }
  if (options.defuseFma) {
    return builder.CreateFAdd(builder.CreateFMul(X, Y), Z);
  }
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(X->getType());
  Ops.push_back(X);
  Tys.push_back(Y->getType());
  Ops.push_back(Y);
  Tys.push_back(Z->getType());
  Ops.push_back(Z);

  std::string FnName = "air.fma";
  FnName += getTypeOverloadSuffix(X->getType());

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(X->getType(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateDotProduct(Value *LHS, Value *RHS) {
  if (!LHS->getType()->getScalarType()->isFloatingPointTy()) {
    debug << "invalid operation: dotproduct on non-fp type.\n";
    return nullptr; // TODO
  }
  if (LHS->getType() != RHS->getType()) {
    debug << "invalid operation: dotproduct has unmatched operand types.\n";
    return nullptr; // TODO
  }

  // just in case
  if (!LHS->getType()->isVectorTy()) {
    return builder.CreateFMul(LHS, RHS);
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(LHS->getType());
  Ops.push_back(LHS);
  Tys.push_back(RHS->getType());
  Ops.push_back(RHS);

  std::string FnName = "air.dot";
  FnName += getTypeOverloadSuffix(LHS->getType());

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getFloatTy(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateCountZero(Value *Val, bool TrailingZero) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  SmallVector<Value *> Ops;
  SmallVector<Type *> Tys;

  Tys.push_back(Val->getType());
  Ops.push_back(Val);
  Tys.push_back(getBoolTy());
  Ops.push_back(getBool(false));

  std::string FnName = "air.";
  FnName += TrailingZero ? "ctz" : "clz";
  FnName += getTypeOverloadSuffix(Val->getType());

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(Val->getType(), Tys, false), Attrs);

  return builder.CreateCall(Fn, Ops);
}

Value *
AIRBuilder::CreateConvertToFloat(Value *Val, Signedness SrcSign) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  Type *TySrc = Val->getType();
  Type *TyDst = isa<FixedVectorType>(TySrc) ? getFloatTy(cast<FixedVectorType>(TySrc)->getNumElements()) : getFloatTy();

  std::string FnName = "air.convert";
  FnName += getTypeOverloadSuffix(TyDst, Signedness::Signed);
  FnName += getTypeOverloadSuffix(TySrc, SrcSign);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(TyDst, {TySrc}, false), Attrs);

  return builder.CreateCall(Fn, {Val});
}

Value *
AIRBuilder::CreateConvertToHalf(Value *Val, Signedness SrcSign) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  Type *TySrc = Val->getType();
  Type *TyDst = isa<FixedVectorType>(TySrc) ? getHalfTy(cast<FixedVectorType>(TySrc)->getNumElements()) : getHalfTy();

  std::string FnName = "air.convert";
  FnName += getTypeOverloadSuffix(TyDst, Signedness::Signed);
  FnName += getTypeOverloadSuffix(TySrc, SrcSign);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(TyDst, {TySrc}, false), Attrs);

  return builder.CreateCall(Fn, {Val});
}

Value *
AIRBuilder::CreateConvertToSigned(Value *Val) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  Type *TySrc = Val->getType();
  Type *TyDst = isa<FixedVectorType>(TySrc) ? getIntTy(cast<FixedVectorType>(TySrc)->getNumElements()) : getIntTy();

  std::string FnName = "air.convert";
  FnName += getTypeOverloadSuffix(TyDst, Signedness::Signed);
  FnName += getTypeOverloadSuffix(TySrc, Signedness::Signed);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(TyDst, {TySrc}, false), Attrs);

  return builder.CreateCall(Fn, {Val});
}

Value *
AIRBuilder::CreateConvertToUnsigned(Value *Val) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  Type *TySrc = Val->getType();
  Type *TyDst = isa<FixedVectorType>(TySrc) ? getIntTy(cast<FixedVectorType>(TySrc)->getNumElements()) : getIntTy();

  std::string FnName = "air.convert";
  FnName += getTypeOverloadSuffix(TyDst, Signedness::Unsigned);
  FnName += getTypeOverloadSuffix(TySrc, Signedness::Unsigned);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(TyDst, {TySrc}, false), Attrs);

  return builder.CreateCall(Fn, {Val});
}

CallInst *
AIRBuilder::CreateDeviceCoherentLoad(Type *Ty, Value *Ptr) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoFree)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoSync)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  Type *TyPtr = Ptr->getType();

  std::string FnName = "air.load.device_coherent";
  FnName += getTypeOverloadSuffix(Ty, Signedness::DontCare);
  FnName += getTypeOverloadSuffix(TyPtr, Signedness::DontCare);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(Ty, {TyPtr}, false), Attrs);

  return builder.CreateCall(Fn, {Ptr});
}

CallInst *
AIRBuilder::CreateDeviceCoherentStore(Value *Val, Value *Ptr) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{2U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {2U, Attribute::get(Context, Attribute::AttrKind::WriteOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoFree)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoSync)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  Type *Ty = Val->getType();
  Type *TyPtr = Ptr->getType();

  std::string FnName = "air.store.device_coherent";
  FnName += getTypeOverloadSuffix(Ty, Signedness::DontCare);
  FnName += getTypeOverloadSuffix(TyPtr, Signedness::DontCare);

  auto Fn = getModule()->getOrInsertFunction(FnName, FunctionType::get(getVoidTy(), {Ty, TyPtr}, false), Attrs);

  return builder.CreateCall(Fn, {Val, Ptr});
}

Value *
AIRBuilder::CreateInterpolateAtCenter(Value *Interpoant, bool Perspective) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      Perspective ? "air.interpolate_center_perspective.v4f32" : "air.interpolate_center_no_perspective.v4f32",
      FunctionType::get(getFloatTy(4), {Interpoant->getType()}, false), Attrs
  );

  return builder.CreateCall(Fn, {Interpoant});
}

Value *
AIRBuilder::CreateInterpolateAtCentroid(Value *Interpoant, bool Perspective) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      Perspective ? "air.interpolate_centroid_perspective.v4f32" : "air.interpolate_centroid_no_perspective.v4f32",
      FunctionType::get(getFloatTy(4), {Interpoant->getType()}, false), Attrs
  );

  return builder.CreateCall(Fn, {Interpoant});
}

Value *
AIRBuilder::CreateInterpolateAtSample(Value *Interpoant, Value *SampleIndex, bool Perspective) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      Perspective ? "air.interpolate_sample_perspective.v4f32" : "air.interpolate_sample_no_perspective.v4f32",
      FunctionType::get(getFloatTy(4), {Interpoant->getType(), getIntTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {Interpoant, SampleIndex});
}

Value *
AIRBuilder::CreateInterpolateAtOffset(Value *Interpoant, Value *Offset, bool Perspective) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {1U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      Perspective ? "air.interpolate_offset_perspective.v4f32" : "air.interpolate_offset_no_perspective.v4f32",
      FunctionType::get(getFloatTy(4), {Interpoant->getType(), getFloatTy(2)}, false), Attrs
  );

  return builder.CreateCall(Fn, {Interpoant, Offset});
}

CallInst *
AIRBuilder::CreateSetMeshProperties(Value *GridSizeVec3) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::MustProgress)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_threadgroups_per_grid_mesh_properties",
      FunctionType::get(getVoidTy(), {getMeshGridPropsType(), getIntTy(3)}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshGridProps(), GridSizeVec3});
}

CallInst *
AIRBuilder::CreateSetMeshRenderTargetArrayIndex(Value *Vertex, Value *ArrayIndex) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_render_target_array_index_mesh.i32",
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getIntTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Vertex, ArrayIndex});
}

CallInst *
AIRBuilder::CreateSetMeshViewportArrayIndex(Value *Vertex, Value *ArrayIndex) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_viewport_array_index_mesh.i32",
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getIntTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Vertex, ArrayIndex});
}

CallInst *
AIRBuilder::CreateSetMeshPosition(Value *Vertex, Value *Position) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_position_mesh", FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getFloatTy(4)}, false),
      Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Vertex, SanitizePosition(Position)});
}

CallInst *
AIRBuilder::CreateSetMeshClipDistance(Value *Vertex, Value *Index, Value *Value) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_clip_distance_mesh",
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getIntTy(), getFloatTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Index, Vertex, Value});
}

CallInst *
AIRBuilder::CreateSetMeshVertexData(Value *Vertex, Value *DataIndex, Value *DataValue) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  std::string FnName = "air.set_vertex_data_mesh";
  FnName += getTypeOverloadSuffix(DataValue->getType());

  auto Fn = getModule()->getOrInsertFunction(
      FnName,
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getIntTy(), DataValue->getType()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), DataIndex, Vertex, DataValue});
}

CallInst *
AIRBuilder::CreateSetMeshPrimitiveData(Value *Primitive, Value *DataIndex, Value *DataValue) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  std::string FnName = "air.set_primitive_data_mesh";
  FnName += getTypeOverloadSuffix(DataValue->getType());

  auto Fn = getModule()->getOrInsertFunction(
      FnName,
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getIntTy(), DataValue->getType()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), DataIndex, Primitive, DataValue});
}

CallInst *
AIRBuilder::CreateSetMeshIndex(Value *Index, Value *Vertex) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_index_mesh", FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getByteTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Index, builder.CreateZExtOrTrunc(Vertex, getByteTy())});
}

CallInst *
AIRBuilder::CreateSetMeshPrimitiveCount(Value *Count) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      "air.set_primitive_count_mesh", FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Count});
}

CallInst *
AIRBuilder::CreateSetMeshPointSize(Value *Vertex, Value *Size) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );

  std::string FnName = "air.set_point_size_mesh";

  auto Fn = getModule()->getOrInsertFunction(
      FnName, FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getFloatTy()}, false), Attrs
  );

  return builder.CreateCall(Fn, {getMeshHandle(), Vertex, Size});
}

CallInst *
AIRBuilder::CreateSetMeshPrimitiveID(Value *Primitive, Value *ID) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );
  auto Fn = getModule()->getOrInsertFunction(
      "air.set_primitive_id_mesh",
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), getIntTy()}, false), Attrs
  );
  return builder.CreateCall(Fn, {getMeshHandle(), Primitive, ID});
}

CallInst *
AIRBuilder::CreateSetMeshPrimitiveCulled(Value *Primitive, Value *Culled) {
  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ArgMemOnly)}}
  );
  auto Fn = getModule()->getOrInsertFunction(
      "air.set_primitive_culled_mesh",
      FunctionType::get(getVoidTy(), {getMeshHandleType(), getIntTy(), builder.getInt1Ty()}, false), Attrs
  );
  return builder.CreateCall(Fn, {getMeshHandle(), Primitive, Culled});
}

Value *
AIRBuilder::CreateFPUnOp(FPUnOp Op, Value *Operand, bool FastVariant) {
  static char const *FnNames[] = {
      "saturate", "log2", "exp2", "sqrt", "rsqrt", "fract", "rint", "floor", "ceil", "trunc", "cos", "sin", "fabs",
  };

  if (uint32_t(Op) >= std::size(FnNames)) {
    debug << "invalid operation: unknown fp unary operation.\n";
    return nullptr; // TODO
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
  );
  auto OperandType = Operand->getType();

  std::string FnName = "air.";
  if (FastVariant)
    FnName += "fast_";
  FnName += FnNames[Op];
  FnName += getTypeOverloadSuffix(OperandType);
  auto Fn = getModule()->getOrInsertFunction(FnName, llvm::FunctionType::get(OperandType, {OperandType}, false), Attrs);
  return builder.CreateCall(Fn, {Operand});
}

Value *
AIRBuilder::CreateIsNaN(Value *Operand) {
  auto OperandType = Operand->getType();
  llvm::Value *BitsL31 = getInt(0x7fffffffu);
  llvm::Value *BitsInf = getInt(0x7f800000u);
  llvm::Type *TyCast = getIntTy();
  if (auto TyVec = dyn_cast<llvm::FixedVectorType>(OperandType)) {
    BitsL31 = builder.CreateVectorSplat(TyVec->getNumElements(), BitsL31);
    BitsInf = builder.CreateVectorSplat(TyVec->getNumElements(), BitsInf);
    TyCast = getIntTy(TyVec->getNumElements());
  }
  return builder.CreateICmpUGT(builder.CreateAnd(builder.CreateBitCast(Operand, TyCast), BitsL31), BitsInf);
}

Value *
AIRBuilder::CreateFPBinOp(FPBinOp Op, Value *LHS, Value *RHS, bool FastVariant) {
  static char const *FnNames[] = {
      "fmax",
      "fmin",
  };

  if (uint32_t(Op) >= std::size(FnNames)) {
    debug << "invalid operation: unknown fp binary operation.\n";
    return nullptr; // TODO
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
  );
  auto OperandType = LHS->getType();

  std::string FnName = "air.";
  if (FastVariant)
    FnName += "fast_";
  FnName += FnNames[Op];
  FnName += getTypeOverloadSuffix(OperandType);
  auto Fn = getModule()->getOrInsertFunction(
      FnName, llvm::FunctionType::get(OperandType, {OperandType, OperandType}, false), Attrs
  );
  return builder.CreateCall(Fn, {LHS, RHS});
}

Value *
AIRBuilder::CreateIntUnOp(IntUnOp Op, Value *Operand) {
  static char const *FnNames[] = {
      "reverse_bits",
      "popcount",
  };

  if (uint32_t(Op) >= std::size(FnNames)) {
    debug << "invalid operation: unknown integer unary operation.\n";
    return nullptr; // TODO
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
  );
  auto OperandType = Operand->getType();

  std::string FnName = "air.";
  FnName += FnNames[Op];
  FnName += getTypeOverloadSuffix(OperandType);
  auto Fn = getModule()->getOrInsertFunction(FnName, llvm::FunctionType::get(OperandType, {OperandType}, false), Attrs);
  return builder.CreateCall(Fn, {Operand});
}

Value *
AIRBuilder::CreateIntBinOp(IntBinOp Op, Value *LHS, Value *RHS, bool Signed) {
  static char const *FnNames[] = {
      "max",
      "min",
  };

  if (uint32_t(Op) >= std::size(FnNames)) {
    debug << "invalid operation: unknown integer binary operation.\n";
    return nullptr; // TODO
  }

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
  );
  auto OperandType = LHS->getType();

  std::string FnName = "air.";
  FnName += FnNames[Op];
  FnName += getTypeOverloadSuffix(OperandType, Signed ? Signedness::Signed : Signedness::Unsigned);
  auto Fn = getModule()->getOrInsertFunction(
      FnName, llvm::FunctionType::get(OperandType, {OperandType, OperandType}, false), Attrs
  );
  return builder.CreateCall(Fn, {LHS, RHS});
}

Value *
AIRBuilder::CreateAtomicRMW(AtomicRMWInst::BinOp Op, Value *Ptr, Value *Val) {
  std::string FnName = "air.atomic.";
  auto TyPtr = dyn_cast<PointerType>(Ptr->getType());
  if (!TyPtr) {
    debug << "invalid operation: atomicrmw: not a pointer.\n";
    return nullptr; // TODO
  }
  if (TyPtr->getNonOpaquePointerElementType() != Val->getType()) {
    debug << "invalid operation: atomicrmw: mismatched atomic operands.\n";
    return nullptr; // TODO
  }
  Value *MemFlags = nullptr;
  switch (TyPtr->getAddressSpace()) {
  case 1:
    FnName += "global.";
    MemFlags = getInt(3);
    break;
  case 3:
    FnName += "local.";
    MemFlags = getInt(1);
    break;
  default:
    debug << "invalid operation: atomicrmw: not a valid address space.\n";
    return nullptr; // TODO
  }
  Signedness Sign = Signedness::Unsigned;
  switch (Op) {
  case llvm::AtomicRMWInst::Add:
    FnName += "add";
    break;
  case llvm::AtomicRMWInst::Sub:
    FnName += "sub";
    break;
  case llvm::AtomicRMWInst::And:
    FnName += "and";
    break;
  case llvm::AtomicRMWInst::Or:
    FnName += "or";
    break;
  case llvm::AtomicRMWInst::Xor:
    FnName += "xor";
    break;
  case llvm::AtomicRMWInst::Max:
    Sign = Signedness::Signed;
    FnName += "max";
    break;
  case llvm::AtomicRMWInst::Min:
    Sign = Signedness::Signed;
    FnName += "min";
    break;
  case llvm::AtomicRMWInst::UMax:
    FnName += "max";
    break;
  case llvm::AtomicRMWInst::UMin:
    FnName += "min";
    break;
  case llvm::AtomicRMWInst::Xchg:
    FnName += "xchg";
    Sign = Signedness::DontCare;
    break;
  default:
    FnName += "<invalid atomic op>";
    break;
  }
  auto TyOp = TyPtr->getNonOpaquePointerElementType();
  FnName += getTypeOverloadSuffix(TyPtr->getNonOpaquePointerElementType(), Sign);

  auto &Context = getContext();
  auto Attrs = AttributeList::get(
      Context, {{1U, Attribute::get(Context, Attribute::AttrKind::NoCapture)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );

  auto Fn = getModule()->getOrInsertFunction(
      FnName, llvm::FunctionType::get(TyOp, {TyPtr, TyOp, getIntTy(), getIntTy(), getBoolTy()}, false), Attrs
  );
  return builder.CreateCall(Fn, {Ptr, Val, getInt(0), MemFlags, getBool(true)});
}

llvm::Value *
AIRBuilder::SanitizePosition(llvm::Value *Pos) {
  // isfinite(Pos)
  auto Mask = builder.CreateICmpNE(
      builder.CreateAnd(builder.CreateBitCast(Pos, getIntTy(4)), 0x7f800000ull),
      getInt4(0x7f800000, 0x7f800000, 0x7f800000, 0x7f800000)
  );
  auto ValidPos = builder.CreateAnd(
      builder.CreateAnd(builder.CreateExtractElement(Mask, 0ull), builder.CreateExtractElement(Mask, 1ull)),
      builder.CreateAnd(builder.CreateExtractElement(Mask, 2ull), builder.CreateExtractElement(Mask, 3ull))
  );
  auto PosClipped = llvm::ConstantVector::get({getFloat(0.0f), getFloat(0.0f), getFloat(1.0f), getFloat(0.0f)});
  return builder.CreateSelect(ValidPos, Pos, PosClipped);
}

} // namespace llvm::air