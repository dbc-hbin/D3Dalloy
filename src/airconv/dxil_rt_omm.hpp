#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace d3dalloy::dxil {

// The runtime shadow layout is shared by TraceRay, HitObject tracing and RayQuery.
// Returns 0 = discard, 1 = nonopaque candidate, 2 = opaque candidate. Opacity
// forcing/culling happens AFTER OMM classification; transparent is never forced into a hit.
inline llvm::Value *emitOmmClassification(llvm::IRBuilder<> &caller, llvm::Module &module,
    llvm::Value *address, llvm::Value *instance, llvm::Value *geometry,
    llvm::Value *primitive, llvm::Value *barycentrics, llvm::Value *rayFlags) {
  using namespace llvm;
  Function *fn = module.getFunction("d3dalloy.omm.classify");
  if (!fn) {
    LLVMContext &c = module.getContext();
    Type *i32 = caller.getInt32Ty(), *i64 = caller.getInt64Ty(), *f32 = caller.getFloatTy();
    Type *v2 = FixedVectorType::get(f32, 2);
    fn = Function::Create(FunctionType::get(i32, {i64, i32, i32, i32, v2, i32}, false),
      GlobalValue::InternalLinkage, "d3dalloy.omm.classify", module);
    fn->addFnAttr(Attribute::AlwaysInline);
    BasicBlock *entry = BasicBlock::Create(c, "entry", fn);
    BasicBlock *mapped = BasicBlock::Create(c, "mapped", fn);
    BasicBlock *indexed = BasicBlock::Create(c, "indexed", fn);
    BasicBlock *lookup = BasicBlock::Create(c, "lookup", fn);
    BasicBlock *special = BasicBlock::Create(c, "special", fn);
    BasicBlock *regular = BasicBlock::Create(c, "regular", fn);
    BasicBlock *state = BasicBlock::Create(c, "state", fn);
    BasicBlock *opacity = BasicBlock::Create(c, "opacity", fn);
    BasicBlock *reject = BasicBlock::Create(c, "reject", fn);
    IRBuilder<> b(entry);
    auto arg = fn->arg_begin();
    Value *as = &*arg++, *inst = &*arg++, *geom = &*arg++, *prim = &*arg++, *bc = &*arg++, *flags = &*arg;
    auto load = [&](Value *base, uint32_t offset, Type *type) {
      return b.CreateAlignedLoad(type, b.CreateIntToPtr(b.CreateAdd(base, b.getInt64(offset)), type->getPointerTo(1)), Align(1));
    };
    Value *instAddress = b.CreateAdd(as, b.CreateAdd(b.getInt64(64), b.CreateMul(b.CreateZExt(inst, i64), b.getInt64(64))));
    Value *instanceFlags = b.CreateLShr(load(instAddress, 52, i32), b.getInt32(24));
    Value *blas = load(instAddress, 56, i64);
    Value *record = b.CreateAdd(blas, b.CreateAdd(b.getInt64(64), b.CreateMul(b.CreateZExt(geom, i64), b.getInt64(96))));
    Value *originalOpaque = b.CreateICmpNE(b.CreateAnd(load(record, 4, i32), b.getInt32(1)), b.getInt32(0));
    Value *hasMap = b.CreateAnd(b.CreateICmpEQ(load(record, 0, i32), b.getInt32(2)),
      b.CreateICmpEQ(b.CreateAnd(instanceFlags, b.getInt32(32)), b.getInt32(0)));
    b.CreateCondBr(hasMap, mapped, opacity);
    b.SetInsertPoint(mapped);
    Value *array = load(record, 64, i64);
    Value *indexBytes = load(record, 84, i32);
    Value *base = load(record, 88, i32);
    Value *implicitIndex = b.CreateAdd(prim, base);
    b.CreateCondBr(b.CreateICmpNE(indexBytes, b.getInt32(0)), indexed, lookup);
    b.SetInsertPoint(indexed);
    Value *indexAddress = b.CreateAdd(blas, b.CreateAdd(load(record, 72, i64),
      b.CreateMul(b.CreateZExt(prim, i64), b.CreateZExt(load(record, 80, i32), i64))));
    // Shadow indices are zero-padded to four bytes, with their original bit width retained.
    Value *raw = load(indexAddress, 0, i32);
    Value *width = b.CreateMul(indexBytes, b.getInt32(8));
    Value *indexMask = b.CreateLShr(b.getInt32(-1), b.CreateSub(b.getInt32(32), width));
    Value *index = b.CreateAnd(raw, indexMask);
    Value *explicitIndex = b.CreateAdd(index, base);
    b.CreateCondBr(b.CreateICmpUGE(index, b.CreateSub(indexMask, b.getInt32(3))), special, lookup);
    b.SetInsertPoint(special);
    Value *specialState = b.CreateSub(indexMask, index);
    b.CreateBr(state);
    b.SetInsertPoint(lookup);
    PHINode *mapIndex = b.CreatePHI(i32, 2);
    mapIndex->addIncoming(implicitIndex, mapped);
    mapIndex->addIncoming(explicitIndex, indexed);
    b.CreateCondBr(b.CreateICmpULT(mapIndex, load(array, 28, i32)), regular, reject);
    b.SetInsertPoint(regular);
    Value *map = b.CreateAdd(array, b.CreateAdd(b.getInt64(64), b.CreateMul(b.CreateZExt(mapIndex, i64), b.getInt64(96))));
    Value *level = load(map, 12, i32), *format = load(map, 8, i32);
    Value *steps = b.CreateShl(b.getInt32(1), level);
    Value *scale = b.CreateUIToFP(steps, f32);
    Value *u = b.CreateExtractElement(bc, uint64_t(0)), *v = b.CreateExtractElement(bc, uint64_t(1));
    Value *w = b.CreateFSub(b.CreateFSub(ConstantFP::get(f32, 1), u), v);
    auto quantize = [&](Value *coordinate) -> Value * {
      Value *positive = b.CreateSelect(b.CreateFCmpOLT(coordinate, ConstantFP::get(f32, 0)), ConstantFP::get(f32, 0), coordinate);
      Value *value = b.CreateFPToUI(b.CreateFMul(positive, scale), i32);
      return b.CreateSelect(b.CreateICmpUGE(value, steps), b.CreateSub(steps, b.getInt32(1)), value);
    };
    Value *iu = quantize(u), *iv = quantize(v), *iw = quantize(w);
    Value *mask = b.CreateSub(steps, b.getInt32(1));
    Value *even = b.CreateAnd(b.CreateNot(b.CreateXor(iu, iw)), mask);
    Value *turns = b.CreateAnd(b.CreateXor(iu, iv), even);
    // OC1 bird-curve parity is the suffix parity of the discrete barycentric turns.
    // Compute all twelve suffix parities in four XORs, then interleave the even/odd bits.
    Value *parity = turns;
    for (unsigned shift : {1u, 2u, 4u, 8u})
      parity = b.CreateXor(parity, b.CreateLShr(parity, shift));
    Value *odd = b.CreateOr(b.CreateAnd(b.CreateXor(parity, iu), b.CreateNot(even)), turns);
    auto spread = [&](Value *x) {
      auto step = [&](unsigned shift, uint32_t mask) {
        x = b.CreateAnd(b.CreateOr(x, b.CreateShl(x, shift)), b.getInt32(mask));
      };
      step(8, 0x00ff00ff);
      step(4, 0x0f0f0f0f);
      step(2, 0x33333333);
      step(1, 0x55555555);
      return x;
    };
    Value *microIndex = b.CreateOr(spread(even), b.CreateShl(spread(odd), 1));
    Value *bitOffset = b.CreateMul(microIndex, format);
    Value *data = b.CreateAdd(array, b.CreateAdd(load(map, 32, i64), b.CreateZExt(b.CreateLShr(bitOffset, 3), i64)));
    Value *byte = b.CreateZExt(load(data, 0, b.getInt8Ty()), i32);
    Value *packedState = b.CreateAnd(b.CreateLShr(byte, b.CreateAnd(bitOffset, b.getInt32(7))), b.CreateSub(b.CreateShl(b.getInt32(1), format), b.getInt32(1)));
    b.CreateBr(state);
    b.SetInsertPoint(state);
    PHINode *classification = b.CreatePHI(i32, 2);
    classification->addIncoming(specialState, special);
    classification->addIncoming(packedState, regular);
    Value *twoState = b.CreateICmpNE(b.CreateOr(b.CreateAnd(flags, b.getInt32(0x400)), b.CreateAnd(instanceFlags, b.getInt32(0x10))), b.getInt32(0));
    Value *resolved = b.CreateSelect(twoState, b.CreateAnd(classification, b.getInt32(1)), classification);
    Value *mapOpaque = b.CreateICmpEQ(resolved, b.getInt32(1));
    b.CreateCondBr(b.CreateICmpEQ(resolved, b.getInt32(0)), reject, opacity);
    b.SetInsertPoint(opacity);
    PHINode *opaque = b.CreatePHI(b.getInt1Ty(), 2);
    opaque->addIncoming(originalOpaque, entry);
    opaque->addIncoming(mapOpaque, state);
    Value *instanceForce = b.CreateAnd(b.CreateLShr(instanceFlags, 2), b.getInt32(3));
    Value *rayForce = b.CreateAnd(flags, b.getInt32(3));
    Value *force = b.CreateSelect(b.CreateICmpNE(rayForce, b.getInt32(0)), rayForce, instanceForce);
    Value *finalOpaque = b.CreateSelect(b.CreateICmpNE(force, b.getInt32(0)), b.CreateICmpEQ(force, b.getInt32(1)), opaque);
    Value *cullBit = b.CreateSelect(finalOpaque, b.getInt32(0x40), b.getInt32(0x80));
    Value *culled = b.CreateICmpNE(b.CreateAnd(flags, cullBit), b.getInt32(0));
    b.CreateRet(b.CreateSelect(culled, b.getInt32(0), b.CreateSelect(finalOpaque, b.getInt32(2), b.getInt32(1))));
    b.SetInsertPoint(reject);
    b.CreateRet(b.getInt32(0));
  }
  return caller.CreateCall(fn, {address, instance, geometry, primitive, barycentrics, rayFlags});
}

} // namespace d3dalloy::dxil
