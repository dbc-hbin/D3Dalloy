#pragma once

#include "air_signature.hpp"
#include "airconv_public.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include <cstddef>

namespace d3dalloy::dxbc {

static_assert(sizeof(SM50_CAPTURE_FRONTIER_CONTROL) == 64);
static_assert(offsetof(SM50_CAPTURE_FRONTIER_CONTROL, primitive_prefix) == 16);
static_assert(offsetof(SM50_CAPTURE_FRONTIER_CONTROL, producer_count) == 32);
static_assert(offsetof(SM50_CAPTURE_FRONTIER_CONTROL, first_patch) == 40);

inline bool capture_frontier_enabled(const SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  for (; args; args = static_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(args->next))
    if (args->type == SM50_SHADER_D3D12_CAPTURE_FRONTIER)
      return true;
  return false;
}

inline uint32_t define_capture_frontier(air::FunctionSignatureBuilder &signature) {
  return signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = sizeof(SM50_CAPTURE_FRONTIER_CONTROL),
      .location_index = SM50_BINDING_INDEX_CAPTURE_FRONTIER,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::MSLUint{},
      .arg_name = "capture_frontier",
      .raster_order_group = {}
  });
}

inline llvm::Value *load_capture_frontier(
    llvm::IRBuilder<> &builder, llvm::Value *control, size_t byte_offset) {
  return builder.CreateLoad(builder.getInt32Ty(),
      builder.CreateConstInBoundsGEP1_32(builder.getInt32Ty(), control, byte_offset / 4));
}

inline llvm::Value *capture_frontier_primitive(
    llvm::IRBuilder<> &builder, llvm::Value *control, llvm::Value *local_primitive) {
  // Application primitive IDs are 32-bit; the scheduler retains the full 64-bit
  // rank for progress. This addition carries no no-wrap assumptions.
  return builder.CreateAdd(local_primitive,
      load_capture_frontier(builder, control, offsetof(SM50_CAPTURE_FRONTIER_CONTROL, primitive_prefix)));
}

/* Native VS replay starts a new draw at each chunk. Rebase only the native PS
 * primitive builtin, after all lowering has introduced its uses. Mesh PSOs do
 * not request this header-only PS variant: their application stage already
 * provides the original primitive/patch ID. */
inline void offset_capture_frontier_pixel_primitive(
    llvm::Function *function, llvm::MDNode *metadata, uint32_t frontier_index) {
  if (frontier_index == ~0u)
    return;
  auto *inputs = llvm::cast<llvm::MDNode>(metadata->getOperand(2));
  for (unsigned index = 0; index < inputs->getNumOperands(); ++index) {
    auto *input = llvm::cast<llvm::MDNode>(inputs->getOperand(index));
    bool primitive = false;
    for (const auto &operand : input->operands())
      if (auto *name = llvm::dyn_cast<llvm::MDString>(operand.get()))
        primitive |= name->getString() == "air.primitive_id";
    if (!primitive)
      continue;
    auto *native = function->getArg(index);
    llvm::IRBuilder<> builder(&*function->getEntryBlock().getFirstInsertionPt());
    auto *rebased = llvm::cast<llvm::Instruction>(capture_frontier_primitive(
        builder, function->getArg(frontier_index), native));
    native->replaceAllUsesWith(rebased);
    rebased->setOperand(0, native);
  }
}

} // namespace d3dalloy::dxbc
