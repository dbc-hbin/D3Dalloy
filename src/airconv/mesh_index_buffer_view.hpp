#pragma once

#include "air_signature.hpp"
#include "airconv_public.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"

namespace d3dalloy::dxbc {

inline llvm::StructType *mesh_index_buffer_view_type(llvm::LLVMContext &context) {
  return llvm::StructType::get(
      context, {llvm::Type::getInt64Ty(context), llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context)});
}

inline uint32_t
define_mesh_index_buffer_view(air::FunctionSignatureBuilder &signature, llvm::LLVMContext &context) {
  return signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = 16,
      .location_index = SM50_BINDING_INDEX_INDEX_BUFFER,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::MSLWhateverStruct{"index_buffer_view", mesh_index_buffer_view_type(context)},
      .arg_name = "index_buffer_view",
      .raster_order_group = {}
  });
}

// IA byte addressing wraps at 32 bits. A NULL IB, or a fetch outside the
// app-specified view, supplies index zero without suppressing the invocation.
// Keep both typed loads behind the guard: selecting a loaded value afterwards
// would still dereference inactive/null/out-of-view addresses.
inline llvm::Value *
load_mesh_index_buffer_view(llvm::IRBuilder<> &builder, llvm::Value *view_pointer, llvm::Value *index) {
  auto *view_type = mesh_index_buffer_view_type(builder.getContext());
  const auto address_space = llvm::cast<llvm::PointerType>(view_pointer->getType())->getAddressSpace();
  view_pointer = builder.CreateBitCast(view_pointer, view_type->getPointerTo(address_space));
  // The VS index-pull form receives this internal descriptor through device
  // memory. Keep its reads scalar so Atomic64 ordinary-load lowering can
  // process them without exempting the application IB fetched below.
  auto *address = builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(view_type, view_pointer, 0));
  auto *length = builder.CreateLoad(builder.getInt32Ty(), builder.CreateStructGEP(view_type, view_pointer, 1));
  auto *format = builder.CreateLoad(builder.getInt32Ty(), builder.CreateStructGEP(view_type, view_pointer, 2));
  auto *index32 = builder.CreateICmpEQ(format, builder.getInt32(42)); // DXGI_FORMAT_R32_UINT
  auto *index16 = builder.CreateICmpEQ(format, builder.getInt32(57)); // DXGI_FORMAT_R16_UINT
  auto *width = builder.CreateSelect(index32, builder.getInt32(4), builder.getInt32(2));
  auto *offset = builder.CreateMul(index, width);
  auto *in_bounds = builder.CreateAnd(builder.CreateICmpUGE(length, width),
                                      builder.CreateICmpULE(offset, builder.CreateSub(length, width)));
  auto *valid = builder.CreateAnd(builder.CreateOr(index16, index32),
      builder.CreateAnd(builder.CreateICmpNE(address, builder.getInt64(0)), in_bounds));
  auto *entry = builder.GetInsertBlock();
  auto *function = entry->getParent();
  auto &context = function->getContext();
  auto *fetch = llvm::BasicBlock::Create(context, "index_view.fetch", function);
  auto *fetch16 = llvm::BasicBlock::Create(context, "index_view.u16", function);
  auto *fetch32 = llvm::BasicBlock::Create(context, "index_view.u32", function);
  auto *done = llvm::BasicBlock::Create(context, "index_view.done", function);
  builder.CreateCondBr(valid, fetch, done);
  builder.SetInsertPoint(fetch);
  auto *at = builder.CreateAdd(address, builder.CreateZExt(offset, builder.getInt64Ty()));
  builder.CreateCondBr(index32, fetch32, fetch16);
  builder.SetInsertPoint(fetch16);
  auto *pointer16 = builder.CreateIntToPtr(at, builder.getInt16Ty()->getPointerTo(unsigned(air::AddressSpace::device)));
  auto *value16 = builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), pointer16), builder.getInt32Ty());
  builder.CreateBr(done);
  builder.SetInsertPoint(fetch32);
  auto *pointer32 = builder.CreateIntToPtr(at, builder.getInt32Ty()->getPointerTo(unsigned(air::AddressSpace::device)));
  auto *value32 = builder.CreateLoad(builder.getInt32Ty(), pointer32);
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  auto *value = builder.CreatePHI(builder.getInt32Ty(), 3, "index_view.value");
  value->addIncoming(builder.getInt32(0), entry);
  value->addIncoming(value16, fetch16);
  value->addIncoming(value32, fetch32);
  return value;
}

} // namespace d3dalloy::dxbc
