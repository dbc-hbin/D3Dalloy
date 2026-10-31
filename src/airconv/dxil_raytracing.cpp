/*
 * Owned DXIL raytracing pipeline -> AIR/Metal compilation (dxil_raytracing.h).
 *
 * Implements the whole-pipeline entry points and the stage builder every export is
 * lowered through:
 *
 *   SM50RTEnumerateLibraryExports   reflect a DXIL library's exports and stages
 *   SM50CompileRaytracingPipeline   lower every export and serialize one metallib
 *   LowerRaytracingStage            build the AIR function of one export
 *   MergeRaytracingExport           link a lowered export into the pipeline module
 *
 * Everything is built on the generic DXIL front end (ModuleInfo::Parse,
 * LowerResources, LowerOperations, VerifyLoweredModule) plus the raytracing
 * additions in dxil_rt_operations.cpp. No vendor ABI is involved.
 *
 * Shape of a lowered pipeline, derived from the public Metal toolchain (probes and
 * their AIR: build/owned-direct-20260919/probes/rt-compiler, /tmp/rtprobe2; every
 * `air.` name below was read back out of Metal's own output):
 *
 *   - The single ray generation export is the pipeline's compute function: an
 *     `air.kernel` whose arguments are the dispatch-arguments constant buffer, the
 *     acceleration structure, the intersection function table, the visible function
 *     table and the uint3 dispatch index. Its body allocates the per-invocation
 *     block (SM50_RT_FRAME + user payload) and the hit state block.
 *   - Miss / closest-hit / callable exports are `air.visible` functions: public
 *     Metal has no attribute for any of them (the front end warns "unknown
 *     attribute" and lowers the function as an ordinary visible one), so the ray
 *     generation shader reaches them through the visible function table.
 *   - An intersection export is an `air.intersection` function. Its AIR contract is
 *     fixed by Metal's own lowering:
 *         i1 (<2 x float> barycentrics, T addrspace(5)* payload)
 *     with the `air.triangle` / `air.instancing` / `air.triangle_data` tags.
 *
 * Per-candidate hit control. D3D12 runs a hit group's any-hit shader for *every*
 * candidate and lets it reject the candidate or end the search, which the traversal
 * alone cannot express: `air.intersect` commits the closest hit itself. Public Metal's
 * equivalent hook is an `air.intersection` function installed in the intersection
 * function table, which the traversal invokes once per candidate and whose packed
 * `{ i1 accept, i1 continue_search }` result decides that candidate (proven with the
 * public toolchain and on a GPU: graphics-translator/tests/owned-metallib-rt.metal,
 * /tmp/rtfix/c1.ll). This file therefore emits
 *
 *   - a per-candidate dispatch callback (SM50_RT_FUNCTION_DISPATCH), one for the whole
 *     pipeline, installed by the runtime at every intersection function table slot the
 *     traversal can select. It recomputes the D3D12 hit group from the candidate's
 *     geometry id and the trace's contribution / multiplier, resolves the group's
 *     any-hit shader and runs it through the visible function table. That is what makes
 *     a triangle hit group's any-hit shader run per candidate, and what makes
 *     `dx.op.ignoreHit` / `dx.op.acceptHitAndEndSearch` reach the traversal.
 *   - a procedural hit group's intersection export as Metal's bounding-box
 *     `air.intersection` function, whose `dx.op.reportHit` reports a hit, runs the
 *     group's any-hit shader for it, and commits it only when it is the closest
 *     accepted hit so far.
 *
 * `air.intersect`'s `accept_any_intersection` operand is pinned false for exactly this
 * reason: setting it would let the traversal commit a candidate without running the
 * hook at all.
 */

#include "dxil_raytracing.h"
#include "dxil_workgraph.h"
#include "DXBCParser/DXBCUtils.h"
#include "DXBCParser/ShaderBytecode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "airconv_context.hpp"
#include "airconv_error.hpp"
#include "air_signature.hpp"
#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"
#include "dxil_rt_callbacks.hpp"
#include "dxil_rt_payload.hpp"
#include "dxil_signature.hpp"
#include "metallib_writer.hpp"

using namespace llvm;

namespace dxmt::dxil {

llvm::Error ValidateRayPayloadAnnotations(Module &module) {
  auto *annotations = module.getNamedMetadata("dx.dxrPayloadAnnotations");
  if (!annotations)
    return Error::success();
  for (MDNode *node : annotations->operands()) {
    auto *tag = node->getNumOperands() ? mdconst::dyn_extract<ConstantInt>(node->getOperand(0)) : nullptr;
    if (!tag || !tag->isZero() || node->getNumOperands() % 2 != 1)
      return createError("dxil: unsupported DXR payload annotation record");
    for (unsigned i = 1; i < node->getNumOperands(); i += 2) {
      auto *value = dyn_cast_or_null<ValueAsMetadata>(node->getOperand(i));
      auto *type = value ? dyn_cast<StructType>(value->getValue()->getType()) : nullptr;
      auto *fields = dyn_cast_or_null<MDNode>(node->getOperand(i + 1));
      if (!type || !type->isSized() || !fields || fields->getNumOperands() != type->getNumElements())
        return createError("dxil: malformed DXR payload field annotations");
      for (const MDOperand &operand : fields->operands()) {
        auto *field = dyn_cast_or_null<MDNode>(operand);
        if (!field || field->getNumOperands() != 2)
          return createError("dxil: unsupported DXR payload field annotation");
        auto *field_tag = mdconst::dyn_extract<ConstantInt>(field->getOperand(0));
        auto *mask = mdconst::dyn_extract<ConstantInt>(field->getOperand(1));
        if (!field_tag || !field_tag->isZero() || !mask || (mask->getZExtValue() & ~0x3333ull))
          return createError("dxil: unsupported DXR payload access qualifier");
      }
    }
  }
  return Error::success();
}

MDNode *RayPayloadAnnotation(Module &module, Type *type) {
  auto *annotations = module.getNamedMetadata("dx.dxrPayloadAnnotations");
  if (annotations)
    for (MDNode *node : annotations->operands())
      for (unsigned i = 1; i < node->getNumOperands(); i += 2)
        if (cast<ValueAsMetadata>(node->getOperand(i))->getValue()->getType() == type)
          return cast<MDNode>(node->getOperand(i + 1));
  return nullptr;
}

void CopyQualifiedRayPayload(
  IRBuilder<> &builder, Value *destination, Value *source, StructType *type, MDNode *fields,
  unsigned access_bit
) {
  const DataLayout &layout = builder.GetInsertBlock()->getModule()->getDataLayout();
  const StructLayout *structure = layout.getStructLayout(type);
  auto bytes = [&](Value *pointer) {
    unsigned space = cast<PointerType>(pointer->getType())->getAddressSpace();
    return builder.CreateBitCast(pointer, builder.getInt8PtrTy(space));
  };
  Value *dest = bytes(destination), *src = bytes(source);
  for (unsigned i = 0; i < fields->getNumOperands(); ++i) {
    auto *field = cast<MDNode>(fields->getOperand(i));
    const uint64_t mask = mdconst::extract<ConstantInt>(field->getOperand(1))->getZExtValue();
    if (!(mask & access_bit))
      continue;
    Value *offset = builder.getInt64(structure->getElementOffset(i));
    builder.CreateMemCpy(
      builder.CreateGEP(builder.getInt8Ty(), dest, offset), Align(1),
      builder.CreateGEP(builder.getInt8Ty(), src, offset), Align(1),
      layout.getTypeStoreSize(type->getElementType(i))
    );
  }
}

namespace {

/** One raytracing export discovered inside a library container. */
struct LibraryExport {
  uint32_t stage = 0; /* SM50_RT_STAGE */
  /** Public name: the container's RDAT unmangled name for the entry point. */
  std::string name;
  /** The `!dx.entryPoints` operand 1 the container declares the function as. */
  std::string entry_point;
};

/*
 * Runtime data (RDAT) of a DXIL library.
 *
 * `!dx.entryPoints` operand 1 is the container's internal name for an entry point,
 * and DXC emits it mangled - a leading 0x01 marker followed by the C++ signature,
 * e.g. "\x01?ClosestHit@@YAXURayPayload@@...@Z". No D3D12 name can be expected to
 * match that spelling, so the name D3D12 exports is the unmangled one, which the
 * container records in its RDAT chunk and in its DXC reflection.
 *
 * Rust reads the part table, strided records and bounded string bytes. C++ owns
 * the resulting names; no borrowed view escapes this call.
 */
/**
 * Collect the public name of every entry point the container records, keyed by the
 * container's own (mangled) name. False when the container has no usable RDAT.
 */
bool readRuntimeDataPublicNames(
  const void *bytecode, uint64_t size, std::map<std::string, std::string> &out
) {
  DxmtRdatInfo rdat;
  if (dxmt_shader_rdat_parse(bytecode, size, &rdat) != DXMT_SHADER_BYTECODE_SUCCESS)
    return false;

  const char *bytes = static_cast<const char *>(bytecode);
  for (uint32_t i = 0; i < rdat.record_count; ++i) {
    DxmtRdatFunction function;
    if (dxmt_shader_rdat_function(bytecode, size, &rdat, i, &function) != DXMT_SHADER_BYTECODE_SUCCESS)
      continue;
    out.emplace(std::string(bytes + function.name_offset, function.name_size),
                std::string(bytes + function.public_name_offset, function.public_name_size));
  }
  return !out.empty();
}

/** One value of a `!dx.entryPoints` `(tag, value)` properties pair. */
bool readEntryProperty(MDNode *properties, uint32_t wanted, uint32_t &value) {
  if (!properties)
    return false;
  for (unsigned i = 0; i + 1 < properties->getNumOperands(); i += 2) {
    auto *tag_constant = dyn_cast<ConstantAsMetadata>(properties->getOperand(i));
    if (!tag_constant)
      continue;
    auto *tag = dyn_cast<ConstantInt>(tag_constant->getValue());
    if (!tag || tag->getZExtValue() != wanted)
      continue;
    auto *value_constant = dyn_cast<ConstantAsMetadata>(properties->getOperand(i + 1));
    if (!value_constant)
      return false;
    auto *integer = dyn_cast<ConstantInt>(value_constant->getValue());
    if (!integer)
      return false;
    value = static_cast<uint32_t>(integer->getZExtValue());
    return true;
  }
  return false;
}

/** Shader kind of one `!dx.entryPoints` node; ordinary functions have no such node. */
bool stageOfEntry(MDNode *entry, uint32_t &stage) {
  if (!entry || entry->getNumOperands() < 5)
    return false;
  uint32_t kind = 0;
  if (!readEntryProperty(dyn_cast<MDNode>(entry->getOperand(4)), kDxilShaderKindTag, kind))
    return false;
  stage = kind;
  return true;
}

/** Enumerate shader and ordinary function exports of one container, in container order. */
llvm::Error enumerateExports(
  const void *bytecode, uint64_t size, std::vector<LibraryExport> &exports
) {
  auto program = FindDXILProgram(bytecode, static_cast<size_t>(size));
  if (!program)
    return program.takeError();

  /* A throwaway context: the container is parsed here only to read its metadata,
   * and every export is parsed again in its own context by ModuleInfo::Parse. */
  LLVMContext context;
  context.setOpaquePointers(false);
  auto buffer = MemoryBuffer::getMemBuffer(
    StringRef(reinterpret_cast<const char *>(program->bitcode), program->bitcode_size),
    "dxil-library", false
  );
  auto module = parseBitcodeFile(buffer->getMemBufferRef(), context);
  if (!module) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    llvm::handleAllErrors(module.takeError(), [&](const llvm::ErrorInfoBase &base) {
      base.log(stream);
    });
    return createError("failed to parse DXIL library bitcode: " + stream.str());
  }

  NamedMDNode *entries = (*module)->getNamedMetadata("dx.entryPoints");
  if (!entries || !entries->getNumOperands())
    return createError("DXIL container has no dx.entryPoints metadata");

  /*
   * The container's own name for an entry point is the mangled one, so the name the
   * library publishes for it comes from its runtime data. A container compiled with
   * its reflection stripped has no such record, and then the entry point's exact
   * identity - the name the container declares - is the only name that is true of
   * it; nothing is derived from the mangled spelling.
   */
  std::map<std::string, std::string> public_names;
  const bool have_public_names = readRuntimeDataPublicNames(bytecode, size, public_names);

  exports.clear();
  for (unsigned i = 0; i < entries->getNumOperands(); ++i) {
    MDNode *entry = entries->getOperand(i);
    if (!entry || entry->getNumOperands() < 2)
      continue;
    auto *name = dyn_cast<MDString>(entry->getOperand(1));
    if (!name || name->getString().empty())
      continue;
    uint32_t stage = 0;
    if (!stageOfEntry(entry, stage))
      continue;
    LibraryExport record;
    record.stage = stage;
    record.entry_point = name->getString().str();
    auto published = have_public_names ? public_names.find(record.entry_point) : public_names.end();
    record.name = published != public_names.end() ? published->second : record.entry_point;
    exports.push_back(std::move(record));
  }
  // Ordinary externally visible definitions have no shader entry-point metadata.
  for (const auto &function : **module) {
    if (function.isDeclaration() || function.hasLocalLinkage())
      continue;
    const std::string symbol = function.getName().str();
    if (std::any_of(exports.begin(), exports.end(), [&](const auto &entry) { return entry.entry_point == symbol; }))
      continue;
    auto published = public_names.find(symbol);
    exports.push_back({SM50_RT_STAGE_LIBRARY, published != public_names.end() ? published->second : symbol, symbol});
  }
  return llvm::Error::success();
}

/** A root signature blob handed to the front end. */
struct RootSignatureBlob {
  const void *data = nullptr;
  size_t size = 0;
};

/**
 * A D3D12 root signature reaches the compiler as a DXBC container holding an RTS0
 * chunk (that is what MTLD3D12RootSignature::GetBlob returns) or as a raw
 * versioned descriptor; the front end wants the raw descriptor. Unwrapping is
 * idempotent on a raw blob. A DXBC container goes through the same Rust-validated
 * RTS0 lookup as CreateRootSignature; one that fails it is handed on unchanged, so
 * setup_binding_rootsig rejects it instead of a fallback signature being chosen.
 */
RootSignatureBlob unwrapRootSignature(const void *blob, uint64_t size) {
  RootSignatureBlob result{blob, static_cast<size_t>(size)};
  if (!blob || size < 4 || std::memcmp(blob, "DXBC", 4) != 0)
    return result;
  const void *raw = nullptr;
  UINT raw_size = 0;
  if (SUCCEEDED(microsoft::DXBCGetRootSignature(blob, size, &raw, &raw_size)))
    result = {raw, raw_size};
  return result;
}

/**
 * Reduce a library container to the one export being lowered.
 *
 * A DXIL library is a single module that holds *every* export - `!dx.entryPoints`
 * names them all - and the lowering passes scan every function definition of the
 * module they are handed: LowerRaytracingOperations walks each definition's `dx.op`
 * calls and rejects one that is not valid in the stage being lowered, so an AnyHit
 * export would fail on the ray generation shader's own `dx.op.dispatchRaysIndex`
 * while that shader's body is still in the module. Each export is therefore lowered
 * against a module that contains the selected entry and nothing else that can be
 * scanned.
 *
 * The selected entry and every definition transitively reachable from it through
 * direct calls survive; every other definition is left as a declaration
 * (`deleteBody` is `dropAllReferences` plus external linkage), so a call the selected
 * entry still makes to a sibling export stays well formed instead of dangling.
 * Module-scope globals are deliberately left alone: the resource lowering resolves a
 * `dx.op.createHandleForLib` through the global's declaration, so erasing one would
 * turn a resource the export names into an unbound one, and `PrepareModuleStorage`
 * rewrites the same globals for this module.
 *
 * `!dx.entryPoints` is pruned to the selected entry for the same reason the bodies
 * are: it is the container's export list, and it no longer describes this module.
 * Nothing after the parse reads it - the stage and the resource declarations were
 * already recovered into ModuleInfo - so the pruned node is the container's own view
 * of the export, kept only because it is the entry's metadata.
 */
void IsolateSelectedExport(llvm::Module &module, llvm::Function &entry) {
  llvm::SmallPtrSet<llvm::Function *, 16> reachable;
  llvm::SmallVector<llvm::Function *, 16> worklist;
  auto enqueue = [&](llvm::Function *function) {
    if (function && reachable.insert(function).second)
      worklist.push_back(function);
  };
  enqueue(&entry);
  while (!worklist.empty()) {
    llvm::Function *function = worklist.pop_back_val();
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call)
          continue;
        /*
         * Resolve pointer casts and aliases before following a direct callee.
         */
        enqueue(llvm::dyn_cast<llvm::Function>(call->getCalledOperand()->stripPointerCastsAndAliases()));
      }
    }
  }

  for (llvm::Function &function : module) {
    if (function.isDeclaration() || reachable.contains(&function))
      continue;
    function.deleteBody();
  }

  llvm::NamedMDNode *entries = module.getNamedMetadata("dx.entryPoints");
  if (!entries)
    return;
  llvm::MDNode *selected = nullptr;
  for (unsigned i = 0; i < entries->getNumOperands() && !selected; ++i) {
    llvm::MDNode *node = entries->getOperand(i);
    if (!node || !node->getNumOperands())
      continue;
    auto *value = llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(node->getOperand(0).get());
    if (value && value->getValue() == &entry)
      selected = node;
  }
  entries->clearOperands();
  if (selected)
    entries->addOperand(selected);
  else
    module.eraseNamedMetadata(entries);
}

/** Which AIR entry kind a lowered stage is registered under. */
const char *entryKindOfStage(uint32_t stage) {
  switch (stage) {
  case SM50_RT_STAGE_RAYGEN:
    return "air.kernel";
  case SM50_RT_STAGE_INTERSECTION:
    return "air.intersection";
  default:
    /* Miss / closest-hit / any-hit / callable: public Metal has no attribute for
     * any of them, so they are ordinary visible functions. */
    return "air.visible";
  }
}

SM50_RT_FUNCTION_KIND functionKindOfStage(uint32_t stage) {
  switch (stage) {
  case SM50_RT_STAGE_RAYGEN:
    return SM50_RT_FUNCTION_RAYGEN;
  case SM50_RT_STAGE_INTERSECTION:
    return SM50_RT_FUNCTION_INTERSECTION;
  default:
    return SM50_RT_FUNCTION_VISIBLE;
  }
}

/**
 * The per-invocation block of one raygen invocation: SM50_RT_FRAME followed by the
 * user payload bytes. 16 byte aligned so the payload keeps its alignment.
 */
constexpr uint32_t kRtBlockAlignment = 16;

// Invocation storage, SBT/VFT and traversal metadata are physical transport,
// not application UAV cells. Mark them before resource operations are emitted.
void MarkRaytracingTransport(Function &function) {
  for (auto &block : function)
    for (auto &instruction : block) {
      bool transport = false;
      if (auto *load = dyn_cast<LoadInst>(&instruction))
        transport = load->getPointerAddressSpace() == 1;
      else if (auto *store = dyn_cast<StoreInst>(&instruction))
        transport = store->getPointerAddressSpace() == 1;
      else if (auto *atomic = dyn_cast<AtomicRMWInst>(&instruction))
        transport = atomic->getPointerAddressSpace() == 1;
      else if (auto *atomic = dyn_cast<AtomicCmpXchgInst>(&instruction))
        transport = atomic->getPointerAddressSpace() == 1;
      else if (auto *call = dyn_cast<CallInst>(&instruction))
        if (auto *callee = call->getCalledFunction())
          transport = callee->getName().startswith("air.atomic.");
      if (transport)
        instruction.setMetadata(kAtomic64Metadata, MDNode::get(function.getContext(), {}));
    }
}

/** Alloca an aligned byte block and return it as an i8*. */
Value *createByteBlock(IRBuilder<> &builder, uint32_t size, StringRef name) {
  LLVMContext &context = builder.getContext();
  auto *array = ArrayType::get(Type::getInt8Ty(context), std::max<uint32_t>(size, 1));
  AllocaInst *alloca = builder.CreateAlloca(array, nullptr, name);
  alloca->setAlignment(Align(kRtBlockAlignment));
  return builder.CreateBitCast(alloca, builder.getInt8PtrTy(alloca->getAddressSpace()));
}

/** Load one dword of a block region, addressed by byte offset. */
Value *loadBlockWord(IRBuilder<> &builder, Value *base, uint32_t byte_offset) {
  LLVMContext &context = base->getContext();
  unsigned space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(context), base, builder.getInt32(byte_offset)
  );
  return builder.CreateAlignedLoad(
    builder.getInt32Ty(), builder.CreateBitCast(field, Type::getInt32PtrTy(context, space)), Align(1)
  );
}

/** Load one qword of a block region, addressed by byte offset. */
Value *loadBlockQword(IRBuilder<> &builder, Value *base, uint32_t byte_offset) {
  LLVMContext &context = base->getContext();
  unsigned space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(context), base, builder.getInt32(byte_offset)
  );
  return builder.CreateAlignedLoad(
    builder.getInt64Ty(), builder.CreateBitCast(field, Type::getInt64PtrTy(context, space)), Align(1)
  );
}

/** Store one dword of a block region, addressed by byte offset. */
void storeBlockWord(IRBuilder<> &builder, Value *base, uint32_t byte_offset, Value *value) {
  LLVMContext &context = base->getContext();
  unsigned space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(context), base, builder.getInt32(byte_offset)
  );
  builder.CreateAlignedStore(
    value, builder.CreateBitCast(field, value->getType()->getPointerTo(space)), Align(1)
  );
}

/** Store one qword of a block region, addressed by byte offset. */
void storeBlockQword(IRBuilder<> &builder, Value *base, uint32_t byte_offset, Value *value) {
  LLVMContext &context = base->getContext();
  unsigned space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(context), base, builder.getInt32(byte_offset)
  );
  builder.CreateAlignedStore(
    value, builder.CreateBitCast(field, Type::getInt64PtrTy(context, space)), Align(1)
  );
}

/** Store `value` into `base + byte_offset`, converting it to the field's own type. */
void storeBlockField(
  IRBuilder<> &builder, Value *base, uint32_t byte_offset, Type *type, Value *value
) {
  LLVMContext &context = base->getContext();
  unsigned space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(context), base, builder.getInt32(byte_offset)
  );
  if (value->getType() != type) {
    if (value->getType()->isIntegerTy())
      value = builder.CreateZExtOrTrunc(value, type);
    else
      value = builder.CreateBitCast(value, type);
  }
  builder.CreateAlignedStore(value, builder.CreateBitCast(field, type->getPointerTo(space)), Align(1));
}

llvm::Error LowerRaytracingImmutableConstants(Module &module, SM50CompiledBitcodeInternal &compiled) {
  struct FixedEntry {
    Function *wrapper;
    std::string body_name;
    unsigned dispatch_index;
    unsigned payload_index;
    bool visible;
  };
  SmallVector<FixedEntry, 8> entries;
  // Keep Metal's callable/intersection ABI and its metadata on a declaration
  // while the ordinary pass threads the pool through genuine application bodies.
  for (const char *kind : {"air.visible", "air.intersection"}) {
    auto *metadata = module.getNamedMetadata(kind);
    if (!metadata)
      continue;
    for (unsigned i = 0; i < metadata->getNumOperands(); ++i) {
      auto *entry = metadata->getOperand(i);
      auto *body = cast<Function>(cast<ValueAsMetadata>(entry->getOperand(0))->getValue());
      unsigned dispatch_index = ~0u, payload_index = ~0u;
      for (auto &argument : cast<MDNode>(entry->getOperand(2))->operands()) {
        auto *fields = cast<MDNode>(argument.get());
        for (unsigned field = 1; field + 1 < fields->getNumOperands(); ++field) {
          auto *key = dyn_cast<MDString>(fields->getOperand(field));
          if (!key || key->getString() != "air.arg_name")
            continue;
          auto name = cast<MDString>(fields->getOperand(field + 1))->getString();
          unsigned index = mdconst::extract<ConstantInt>(fields->getOperand(0))->getZExtValue();
          if (name == "dispatch") dispatch_index = index;
          if (name == "payload") payload_index = index;
        }
      }
      if (dispatch_index == ~0u && payload_index == ~0u)
        return createError("atomic64 raytracing entry has no dispatch or payload binding");
      auto *wrapper = Function::Create(body->getFunctionType(), body->getLinkage(),
        body->getAddressSpace(), "", &module);
      wrapper->copyAttributesFrom(body);
      wrapper->removeFnAttr(Attribute::ReadNone);
      wrapper->removeFnAttr(Attribute::WriteOnly);
      wrapper->removeFnAttr(Attribute::InaccessibleMemOnly);
      wrapper->removeFnAttr(Attribute::Speculatable);
      wrapper->takeName(body);
      body->setName(wrapper->getName() + ".atomic64_body");
      body->setLinkage(GlobalValue::InternalLinkage);
      body->addFnAttr(Attribute::AlwaysInline);
      SmallVector<Metadata *, 4> fields(entry->op_begin(), entry->op_end());
      fields[0] = ValueAsMetadata::get(wrapper);
      metadata->setOperand(i, MDNode::get(module.getContext(), fields));
      entries.push_back({wrapper, body->getName().str(), dispatch_index, payload_index,
        StringRef(kind) == "air.visible"});
    }
  }
  if (auto failure = LowerImmutableConstants(module, compiled))
    return failure;
  for (auto &entry : entries) {
    auto *body = module.getFunction(entry.body_name);
    auto *wrapper = entry.wrapper;
    if (!body || (body->arg_size() != wrapper->arg_size() && body->arg_size() != wrapper->arg_size() + 1))
      return createError("atomic64 raytracing immutable body has an inconsistent signature");
    IRBuilder<> builder(BasicBlock::Create(module.getContext(), "entry", wrapper));
    SmallVector<Value *, 16> arguments;
    for (auto &argument : wrapper->args())
      arguments.push_back(&argument);
    if (body->arg_size() != wrapper->arg_size()) {
      Value *dispatch;
      if (entry.dispatch_index != ~0u) {
        auto *argument = wrapper->getArg(entry.dispatch_index);
        unsigned space = cast<PointerType>(argument->getType())->getAddressSpace();
        dispatch = builder.CreateBitCast(argument, builder.getInt8PtrTy(space));
      } else {
        Value *payload = wrapper->getArg(entry.payload_index);
        unsigned space = cast<PointerType>(payload->getType())->getAddressSpace();
        Value *frame = builder.CreateBitCast(payload, builder.getInt8PtrTy(space));
        // Visible shaders receive user payload; hardware intersections receive
        // the whole invocation block. The shared frame remains exactly80bytes.
        if (entry.visible)
          frame = builder.CreateGEP(builder.getInt8Ty(), frame, builder.getInt32(-int32_t(sizeof(SM50_RT_FRAME))));
        dispatch = builder.CreateIntToPtr(
          loadBlockQword(builder, frame, offsetof(SM50_RT_FRAME, DispatchArguments)), builder.getInt8PtrTy(1));
      }
      arguments.push_back(builder.CreateIntToPtr(
        loadBlockQword(builder, dispatch, offsetof(SM50_RT_DISPATCH_ARGUMENTS, ImmutableConstants)),
        builder.getInt32Ty()->getPointerTo(2)));
    }
    auto *call = builder.CreateCall(body, arguments);
    call->setCallingConv(body->getCallingConv());
    if (wrapper->getReturnType()->isVoidTy()) builder.CreateRetVoid();
    else builder.CreateRet(call);
    MarkRaytracingTransport(*wrapper);
  }
  return Error::success();
}

/**
 * Run body only when condition holds, returning its value through a phi.
 * A null device pointer must not be speculatively loaded on the fallback path.
 */
Value *emitGuardedValue(
  IRBuilder<> &builder, Value *condition, Value *fallback, StringRef name,
  function_ref<Value *(IRBuilder<> &)> body
) {
  if (auto *constant = dyn_cast<ConstantInt>(condition)) {
    if (constant->isOne())
      return body(builder);
    return fallback;
  }
  LLVMContext &context = builder.getContext();
  Function *function = builder.GetInsertBlock()->getParent();
  BasicBlock *head = builder.GetInsertBlock();
  BasicBlock *continuation = nullptr;
  if (builder.GetInsertPoint() != head->end())
    continuation = head->splitBasicBlock(builder.GetInsertPoint(), name + ".cont");
  if (head->getTerminator())
    head->getTerminator()->eraseFromParent();
  if (!continuation)
    continuation = BasicBlock::Create(context, name + ".cont", function);
  BasicBlock *guarded = BasicBlock::Create(context, name, function, continuation);
  builder.SetInsertPoint(head);
  builder.CreateCondBr(condition, guarded, continuation);
  builder.SetInsertPoint(guarded);
  Value *produced = body(builder);
  BasicBlock *guarded_end = builder.GetInsertBlock();
  builder.CreateBr(continuation);
  builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
  PHINode *merged = builder.CreatePHI(fallback->getType(), 2, name + ".value");
  merged->addIncoming(fallback, head);
  merged->addIncoming(produced, guarded_end);
  return merged;
}

/**
 * The application's `InstanceContributionToHitGroupIndex` for one instance, out of an
 * invocation frame.
 *
 * The frame's SM50_RT_FRAME::ContributionArray is the address of the `uint32_t` array
 * the runtime's acceleration-structure prepass filled for the structure the active
 * trace is tracing, indexed by the instance's position in that structure. D3D12 adds
 * the word to the hit group index, and Metal has no per-instance field for it. Zero
 * when the structure has no such array, so a non-instanced structure adds nothing;
 * the address is a device address and address space 1 has no address zero, so the
 * load is issued only when it is non-zero.
 */
/**
 * Copy the two byval float4x3 instance transforms an `[[intersection(..., world_space_data)]]`
 * function receives into SM50_RT_HIT_STATE::ObjectToWorld / WorldToObject (column-major, four
 * columns of three floats - the same layout the traversal result reports).
 */
void StoreInstanceTransforms(
  IRBuilder<> &builder, Value *hit_state, Value *object_to_world, Value *world_to_object
) {
  LLVMContext &llvm_context = builder.getContext();
  Type *column = FixedVectorType::get(Type::getFloatTy(llvm_context), 3);
  Type *matrix = ArrayType::get(column, 4);
  const unsigned space = cast<PointerType>(hit_state->getType())->getAddressSpace();
  const std::pair<Value *, uint32_t> sources[] = {
    {object_to_world, (uint32_t)offsetof(SM50_RT_HIT_STATE, ObjectToWorld)},
    {world_to_object, (uint32_t)offsetof(SM50_RT_HIT_STATE, WorldToObject)},
  };
  for (const auto &[source, offset] : sources) {
    for (uint32_t c = 0; c < 4; ++c) {
      Value *value = builder.CreateLoad(
        column, builder.CreateConstInBoundsGEP2_32(matrix, source, 0, c)
      );
      Value *field = builder.CreateGEP(
        Type::getInt8Ty(llvm_context), hit_state, builder.getInt32(offset + c * 12)
      );
      builder.CreateAlignedStore(value, builder.CreateBitCast(field, column->getPointerTo(space)), Align(1));
    }
  }
}

Value *frameInstanceContribution(
  IRBuilder<> &builder, Value *block, Value *instance_index
) {
  LLVMContext &context = builder.getContext();
  Type *u32 = Type::getInt32Ty(context);
  Value *array = loadBlockQword(builder, block, offsetof(SM50_RT_FRAME, ContributionArray));
  return emitGuardedValue(
    builder, builder.CreateICmpNE(array, builder.getInt64(0)), builder.getInt32(0),
    "rt.contribution", [&](IRBuilder<> &inner) -> Value * {
      Value *words = inner.CreateIntToPtr(array, Type::getInt32PtrTy(context, 1));
      return inner.CreateLoad(u32, inner.CreateGEP(u32, words, instance_index));
    }
  );
}

/**
 * The local root arguments of one hit group record, i.e. the bytes after the record's
 * identifier in the application's table, resolved through the dispatch constants' D3D12
 * GPU virtual address (EmitShaderRecordLookup).
 *
 * This is the path an intersection shader takes: it has no bound arguments, so the
 * constants' address travels in its invocation frame (see SM50_RT_FRAME). A zero address
 * means nothing published the constants, so no load is issued at all (address space 1 has
 * no address zero) and the entry is zero - "no local root arguments", never the global
 * root arguments and never the ray generation shader's own record.
 */
Value *hitGroupLocalRootEntryAt(
  IRBuilder<> &builder, Value *dispatch_address, Value *hit_group
) {
  return emitGuardedValue(
    builder, builder.CreateICmpNE(dispatch_address, builder.getInt64(0)),
    builder.getInt64(0), "rt.hit_group_entry",
    [&](IRBuilder<> &inner) -> Value * {
      Value *dispatch = inner.CreateIntToPtr(dispatch_address, Type::getInt8PtrTy(inner.getContext(), 1));
      return EmitShaderRecordLookup(inner, dispatch, RtShaderTable::HitGroup, hit_group).local_entry;
    }
  );
}

/**
 * Hand a stage that declares no bound arguments the entries its root signature
 * resolves through.
 *
 * A visible function is reached through the visible function table with a fixed
 * argument list (payload, hit state, record, depth) and an intersection function
 * through the intersection function table, so neither can declare the root arguments,
 * the static sampler table or the local root table. Their addresses travel in the
 * invocation frame instead, which the party that allocated the block filled, and are
 * handed to the binding map here - before the resource lowering runs, which is what
 * makes a global range resolve to the dispatch's root arguments and a local range to
 * the record's own entry.
 *
 * `local_entry` is the record this invocation runs as: the entry the tracing shader
 * or the per-candidate callback published for a visible function, or the one this
 * stage computed for itself (an intersection shader). A zero entry is handed in as
 * zero, which the map reads as "no local root arguments".
 */
void handOverFrameEntries(
  dxbc::BindingMap &binding_map, IRBuilder<> &builder, Value *block, Value *local_entry
) {
  LLVMContext &context = builder.getContext();
  Type *i8ptr = Type::getInt8PtrTy(context, 1);
  auto address = [&](uint32_t byte_offset) -> Value * {
    Value *value = loadBlockQword(builder, block, byte_offset);
    return builder.CreateIntToPtr(value, i8ptr);
  };
  if (local_entry)
    binding_map.SetLocalRootArgumentPointer(
      builder.CreateIntToPtr(local_entry, i8ptr)
    );
  binding_map.SetGlobalRootArgumentPointer(
    address(offsetof(SM50_RT_FRAME, GlobalRootArguments))
  );
  binding_map.SetStaticSamplerArgumentPointer(
    address(offsetof(SM50_RT_FRAME, StaticSamplers))
  );
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Stage lowering                                                             */

bool ReadLibraryFunctionNames(const void *bytecode, uint64_t size, std::map<std::string, std::string> &names) {
  return readRuntimeDataPublicNames(bytecode, size, names);
}
/* -------------------------------------------------------------------------- */

llvm::Expected<std::unique_ptr<dxbc::BindingMap>> LowerRaytracingStage(
  ModuleInfo &module, const RaytracingExportContext &context,
  dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
) {
  if (!module.module || !module.entry)
    return createError("DXIL module was not parsed");
  if (!IsRaytracingStage(module.stage))
    return unsupported("DXIL entry point is not a raytracing stage");

  auto &module_ir = *module.module;
  auto &llvm_context = *module.context;
  if (auto error = ValidateRayPayloadAnnotations(module_ir))
    return std::move(error);

  /* DXIL containers carry the dxil-ms-dx 32 bit pointer layout; AIR needs air64
   * and the air.* module flags. */
  if (module_ir.getDataLayout().getPointerSizeInBits() != 64)
    dxmt::initializeModule(module_ir);
  InitializeAtomic64Phase(module_ir, args);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (dxbc::args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(args, &sm50_common))
    metal_version = sm50_common->metal_version;
  dxbc::setup_metal_version(module_ir, metal_version);

  /*
   * The DXIL module's own storage: threadgroup declarations become address-space-3
   * threadgroup globals and immutable module-scope globals become address-space-2
   * constant storage, exactly as on the ordinary DXIL path (LowerStage). A raytracing
   * export that reads such a global would otherwise keep it in address space 0 and
   * read it as device memory. It rewrites module-scope globals only, so it belongs
   * with the rest of the module-scope setup.
   */
  PrepareModuleStorage(module_ir);

  /* A raytracing export resolves its resources through the root signature it was
   * associated with (its local one, or the pipeline's global one); the runtime
   * passes exactly that blob in. */
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  dxbc::args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(args, &rootsig);
  if (!rootsig || !rootsig->bytecode || !rootsig->bytecode_length)
    return createError("a raytracing export requires a root signature");

  dxbc::ShaderInfo &shader_info = shader.shader_info;
  /*
   * The export's resource declarations have to reach the root-signature binding layer
   * before it is set up, exactly as on the ordinary DXIL path: without this the maps
   * setup_binding_rootsig walks are empty and every resource the shader names - a
   * RaytracingAccelerationStructure above all, but a UAV or constant buffer just the
   * same - is reported as not bound by the root signature. `PopulateShaderResourceBindings`
   * is the same loop PrepareShader runs, so the two paths cannot disagree about a
   * declaration.
   */
  PopulateShaderResourceBindings(shader_info, module.resources);
  shader.shader_type = (microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE)0xffffffffu;
  shader.max_input_register = 0;
  shader.max_output_register = 0;
  shader.pso_valid_output_reg_mask = 0;

  auto &func_signature = shader.func_signature;
  dxbc::io_binding_map resource_map;

  const uint32_t stage = context.stage;
  const bool hardware_intersection = stage == SM50_RT_STAGE_INTERSECTION && !context.query_callbacks;
  /*
   * The ray generation shader allocates the *whole* invocation block - frame, user
   * payload, hit state, committed attributes and the per-candidate region - because
   * the traversal is handed the payload pointer and every region a hit shader or a
   * per-candidate callback addresses lives between that pointer and the end of the
   * block (see RaytracingExportContext::invocationBlockSize). Allocating only
   * frame + payload would make the callback layer read and write past the end of the
   * allocation, so the block the raygen builds and the block the traversal lowering
   * builds per call site are the same size.
   */
  const uint32_t block_size = context.invocationBlockSize();

  uint32_t payload_index = ~0u;
  uint32_t hit_state_index = ~0u;
  uint32_t record_index = ~0u;
  uint32_t depth_index = ~0u;
  /* Argument positions of an intersection export's own inputs (see below). */
  uint32_t origin_index = ~0u;
  uint32_t direction_index = ~0u;
  uint32_t min_distance_index = ~0u;
  uint32_t max_distance_index = ~0u;
  uint32_t primitive_index = ~0u;
  uint32_t geometry_index = ~0u;
  uint32_t instance_index = ~0u;
  uint32_t user_instance_index = ~0u;
  uint32_t world_origin_index = ~0u;
  uint32_t world_direction_index = ~0u;
  uint32_t object_to_world_index = ~0u;
  uint32_t world_to_object_index = ~0u;
  uint32_t dispatch_arguments_index = ~0u;

  if (stage == SM50_RT_STAGE_RAYGEN) {
    dispatch_arguments_index = func_signature.DefineInput(air::InputRaytracingDispatchArguments{
      .location_index = context.dispatch_arguments_slot,
      .size = uint32_t(module_ir.getModuleFlag(kAtomic64PhaseFlag)
        ? sizeof(SM50_RT_DISPATCH_ARGUMENTS) : offsetof(SM50_RT_DISPATCH_ARGUMENTS, ImmutableConstants)),
      .arg_name = "dispatch",
    });
    func_signature.DefineInput(air::InputIntersectionFunctionTable{
      .location_index = context.intersection_function_table_slot,
      .memory_access = air::MemoryAccess::read_write,
      .arg_name = "ift",
    });
    func_signature.DefineInput(air::InputVisibleFunctionTable{
      .location_index = context.visible_function_table_slot,
      .memory_access = air::MemoryAccess::read,
      .arg_name = "vft",
    });
    func_signature.DefineInput(air::InputThreadPositionInGrid{});
    func_signature.UseMaxWorkgroupSize(1);
  } else if (hardware_intersection) {
    /*
     * Metal's own intersection contract, read back from the public front end
     * (/tmp/rtfix/c1.ll):
     *   i1 (<2 x float> barycentrics, T addrspace(5)* payload)
     *
     * The two-output `{ i1 accept, i1 continue_search }` form a per-candidate
     * callback needs is emitted by FunctionSignatureBuilder::DefineOutput with
     * OutputContinueSearch (see air_signature.cpp); the bounding-box form a
     * procedural intersection shader needs is
     * `{ float distance, i1 accept, i1 continue_search }` over
     * (origin, direction, min_distance, max_distance, primitive_id, geometry_id,
     * instance_id, user_instance_id, payload) with the `air.bounding_box` /
     * `air.instancing` / `air.triangle_data` tags
     * (/tmp/rtcb2/c.ll, /tmp/rtprobe3/t3_c1.ll).
     */
    /*
     * The ray parameters and the candidate ids. A procedural intersection shader
     * reports through `dx.op.reportHit`, so it needs the ray's own interval (which
     * Metal passes as `[[min_distance]]` / `[[max_distance]]`) and the ids of the
     * candidate it is reporting for: those ids are what `dx.op.reportHit` publishes
     * into the hit state for the any-hit shader it runs, and what PrimitiveIndex /
     * GeometryIndex / InstanceIndex / InstanceID answer inside that shader.
     */
    origin_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.origin", .type_name = "float3", .arg_name = "origin",
    });
    direction_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.direction", .type_name = "float3", .arg_name = "direction",
    });
    min_distance_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.min_distance", .type_name = "float", .arg_name = "min_distance",
    });
    max_distance_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.max_distance", .type_name = "float", .arg_name = "max_distance",
    });
    primitive_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.primitive_id", .type_name = "uint", .arg_name = "primitive_id",
    });
    geometry_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.geometry_id", .type_name = "uint", .arg_name = "geometry_id",
    });
    instance_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.instance_id", .type_name = "uint", .arg_name = "instance_id",
    });
    user_instance_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.user_instance_id", .type_name = "uint", .arg_name = "user_instance_id",
    });
    payload_index = func_signature.DefineInput(air::InputRaytracingPayload{
      .size = block_size,
      /* Metal's own intersection functions take the payload as a pointer into
       * address space 5 (the ray_data / object-data space); the AIR it emits for
       * `[[intersection(...)]]` uses exactly that. */
      .address_space = 5,
      .arg_name = "payload",
    });
    /*
     * `dx.op.reportHit` runs the hit group's any-hit shader for every candidate it
     * reports (that is where a procedural hit group's any-hit shader actually runs), and
     * the any-hit shader is reached through the visible function table. A probe proves an
     * `[[intersection(...)]]` function may declare one (/tmp/cbprobe/i1.ll).
     */
    func_signature.DefineInput(air::InputVisibleFunctionTable{
      .location_index = context.visible_function_table_slot,
      .memory_access = air::MemoryAccess::read,
      .arg_name = "vft",
    });
    /*
     * `air.origin` / `air.direction` above are the ray in the candidate instance's OBJECT
     * space (GPU-verified: build/local/p3/dxr/probes/wsprobe.m). World*RayOrigin and the
     * instance transforms come from the world_space_data inputs, appended after the table so
     * the fixed argument positions the bounding-box dispatcher forwards (3, 5, 6, 8) hold.
     */
    world_origin_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.world_space_origin", .type_name = "float3", .arg_name = "world_origin",
    });
    world_direction_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.world_space_direction", .type_name = "float3", .arg_name = "world_direction",
    });
    object_to_world_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.object_to_world_transform", .type_name = "float4x3", .arg_name = "object_to_world",
    });
    world_to_object_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.world_to_object_transform", .type_name = "float4x3", .arg_name = "world_to_object",
    });
    /* `{ float distance, i1 accept, i1 continue_search }`: the reported distance, the
     * accept flag of a committed `dx.op.reportHit`, and whether
     * `dx.op.acceptHitAndEndSearch` ended the search. */
    func_signature.DefineOutput(air::OutputDistance{});
    func_signature.DefineOutput(air::OutputAcceptIntersection{});
    func_signature.DefineOutput(air::OutputContinueSearch{});
    func_signature.UseFunctionTags(
      {"air.bounding_box", "air.instancing", "air.triangle_data", "air.world_space_data"}
    );
  } else {
    /* Miss / closest-hit / any-hit / callable: an ordinary visible function called
     * through the visible function table. */
    payload_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.visible_input",
      .type_name = "payload",
      .arg_name = "payload",
      /* Only any-hit is invoked on hardware ray-data storage. Other visible
       * stages receive the tracing/calling shader's thread-local child block. */
      .address_space = stage == SM50_RT_STAGE_ANY_HIT && !context.query_callbacks ? 5u : 0u,
    });
    hit_state_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.visible_input",
      .type_name = "hit_state",
      .arg_name = "hit_state",
      .address_space = stage == SM50_RT_STAGE_ANY_HIT && !context.query_callbacks ? 5u : 0u,
    });
    record_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.visible_input",
      .type_name = "uint",
      .arg_name = "record",
    });
    depth_index = func_signature.DefineInput(air::InputRaytracingValue{
      .kind = "air.visible_input",
      .type_name = "uint",
      .arg_name = "depth",
    });
  }

  /*
   * A raytracing export's resources resolve through the pipeline's *global* root
   * signature, and an export that also has a local one resolves the ranges of that
   * one through the local root arguments of the record it runs as. Both blobs are
   * therefore handed in: the global as the primary signature (so its ranges, its
   * root arguments entry and the dispatch's one static sampler table are resolved
   * exactly as for every other shader of the dispatch) and the export's local one as
   * the record's own signature. Passing only the local one - which is what a
   * "local signature wins" rule would do - would lose the global root signature for
   * exactly the exports that need both, and would stop the two from being
   * overlap-checked against each other.
   */
  dxil::RootSignatureBlob global =
    dxil::unwrapRootSignature(context.global_root_signature, context.global_root_signature_size);
  dxil::RootSignatureBlob local =
    dxil::unwrapRootSignature(context.local_root_signature, context.local_root_signature_size);
  /*
   * A visible function is reached through the visible function table with a fixed
   * argument list (payload, hit state, record, depth - see EmitVisibleFunctionCall),
   * so it declares neither the root arguments nor the static sampler table: it reads
   * the addresses of both out of its invocation frame, and its stage builder hands
   * them to the binding map below. The ray generation shader, which is the
   * pipeline's compute entry and is launched directly, takes them as arguments, and
   * is the one stage whose own local root entry - the raygen record's, i.e. the
   * table base - is bound at the local root slot.
   */
  /*
   * Only the ray generation shader declares the pipeline's bound arguments.
   *
   * It is the pipeline's compute entry, launched directly, so the runtime binds its
   * arguments on the encoder that dispatches it - the dispatch constants, the two
   * function tables and the root arguments entry. Every other stage is reached
   * through the intersection / visible function table, whose argument bindings are
   * the table's own, not the encoder's, so none of them declares a bound argument:
   * they read the same entries' addresses out of the invocation frame their caller
   * filled (see SM50_RT_FRAME), and their stage builder hands those to the binding
   * map below.
   */
  const bool takes_signature_arguments = stage == SM50_RT_STAGE_RAYGEN;
  /* No stage declares a local root table: every record's local root arguments are read in
   * place from the application's shader table (EmitShaderRecordLookup). */
  auto binding_map = dxbc::setup_binding_rootsig(
    &shader_info, func_signature, module_ir, shader.shader_type, global.data, global.size,
    SM50_BINDING_INDEX_ROOT_ARGUMENTS, SM50_BINDING_INDEX_STATIC_SAMPLERS,
    local.data, local.size, takes_signature_arguments
  );
  if (!binding_map)
    return createError("a raytracing export has an invalid root signature");

  /* The runtime looks the AIR function up by name, so the DXIL entry point has to
   * give the name up before the AIR function is created. */
  module.entry->setName(llvm::Twine(function_name) + ".dxil");
  auto [function, function_metadata] =
    func_signature.CreateFunction(function_name, llvm_context, module_ir, 0, false);

  auto *entry_bb = BasicBlock::Create(llvm_context, "entry", function);
  IRBuilder<> builder(entry_bb);

  /* The invocation block and the hit state block are the raygen's own storage; the
   * visible stages receive them as arguments. */
  Value *payload = nullptr;
  Value *hit_state = nullptr;
  /*
   * The block head (SM50_RT_FRAME) of this invocation: the payload pointer the
   * traversal / visible-function call handed this stage, `frame_size` bytes into the
   * block. The ray generation shader allocates the whole block itself and declares its
   * entries as bound arguments, so it needs no head; it stays null there.
   */
  Value *block = nullptr;
  if (stage == SM50_RT_STAGE_RAYGEN) {
    payload = createByteBlock(builder, block_size, "rt.payload");
    hit_state = builder.CreateGEP(
      Type::getInt8Ty(llvm_context), payload,
      builder.getInt32((int32_t)context.hitStateOffset())
    );
  } else if (hardware_intersection) {
    block = function->getArg(payload_index);
    payload = builder.CreateGEP(
      Type::getInt8Ty(llvm_context), block,
      builder.getInt32(context.frame_size)
    );
    /*
     * Publish the candidate's own system values into the hit state, which is where the
     * operation pass reads a hit shader's system values from. Metal hands an
     * `[[intersection(...)]]` function its primitive / geometry / instance ids and its
     * ray parameters as arguments rather than through the traversal result (the
     * traversal has not finished while it runs), so those arguments are the only source
     * of PrimitiveIndex / GeometryIndex / InstanceIndex / InstanceID / RayTMin /
     * RayTCurrent inside a procedural intersection shader - and of the same values
     * inside the any-hit shader that `dx.op.reportHit` runs for the candidate it
     * reports.
     */
    hit_state = builder.CreateGEP(
      Type::getInt8Ty(llvm_context), block, builder.getInt32(context.hitStateOffset())
    );
    Value *state = hit_state;
    auto stateField = [&](uint32_t byte_offset, Type *type) -> Value * {
      Value *field = builder.CreateGEP(
        Type::getInt8Ty(llvm_context), state, builder.getInt32(byte_offset)
      );
      unsigned space = cast<PointerType>(field->getType())->getAddressSpace();
      return builder.CreateBitCast(field, type->getPointerTo(space));
    };
    auto storeState = [&](uint32_t byte_offset, Value *value) {
      builder.CreateAlignedStore(value, stateField(byte_offset, value->getType()), Align(1));
    };
    storeState(
      offsetof(SM50_RT_HIT_STATE, WorldRayOrigin),
      builder.CreateShuffleVector(
        function->getArg(world_origin_index), PoisonValue::get(
          FixedVectorType::get(builder.getFloatTy(), 3)
        ),
        ArrayRef<int>{0, 1, 2, 3}
      )
    );
    storeState(
      offsetof(SM50_RT_HIT_STATE, WorldRayDirection),
      builder.CreateShuffleVector(
        function->getArg(world_direction_index), PoisonValue::get(
          FixedVectorType::get(builder.getFloatTy(), 3)
        ),
        ArrayRef<int>{0, 1, 2, 3}
      )
    );
    storeState(offsetof(SM50_RT_HIT_STATE, ObjectRayOrigin), function->getArg(origin_index));
    storeState(offsetof(SM50_RT_HIT_STATE, ObjectRayDirection), function->getArg(direction_index));
    StoreInstanceTransforms(
      builder, state, function->getArg(object_to_world_index), function->getArg(world_to_object_index)
    );
    storeState(offsetof(SM50_RT_HIT_STATE, RayTMin), function->getArg(min_distance_index));
    /* RayTCurrent starts at the ray's upper bound: nothing has been reported yet, so the
     * furthest the traversal may still find a hit. */
    storeState(offsetof(SM50_RT_HIT_STATE, RayTCurrent), function->getArg(max_distance_index));
    storeState(
      offsetof(SM50_RT_HIT_STATE, PrimitiveIndex), function->getArg(primitive_index)
    );
    storeState(offsetof(SM50_RT_HIT_STATE, GeometryIndex), function->getArg(geometry_index));
    /* D3D12's InstanceID is the application's user defined id and InstanceIndex the
     * autogenerated index of the instance in the structure. */
    storeState(
      offsetof(SM50_RT_HIT_STATE, InstanceID), function->getArg(user_instance_index)
    );
    storeState(
      offsetof(SM50_RT_HIT_STATE, InstanceIndex), function->getArg(instance_index)
    );
  } else {
    payload = function->getArg(payload_index);
    hit_state = function->getArg(hit_state_index);
    block = builder.CreateGEP(
      Type::getInt8Ty(llvm_context), payload,
      builder.getInt32(-static_cast<int32_t>(context.frame_size))
    );
    (void)record_index;
    (void)depth_index;
  }

  /*
   * The entries this stage's root signature resolves through.
   *
   * Only the ray generation shader declares them as bound arguments - it is the
   * pipeline's compute entry, launched directly - so it is the only stage whose
   * binding map resolves the root arguments, the static samplers and the local root
   * table from its own arguments. Every other stage is reached through the
   * intersection / visible function table, whose argument bindings belong to the
   * table rather than to the dispatch encoder, so it reads the addresses out of the
   * invocation frame the party that issued the call filled and hands them to the
   * binding map here. This is the only point where the map still asks: the resource
   * lowering runs on this same map immediately afterwards.
   *
   * A visible function's own record is the one its caller selected, so it takes the
   * entry out of the frame. An intersection shader runs as its hit group's record and
   * is the only party that knows which candidate it is judging, so it computes the
   * same entry the per-candidate callback computes - the hit group index, instance
   * contribution included - and publishes it into the block, which is what the
   * any-hit shader `dx.op.reportHit` runs for that candidate then inherits.
   */
  if (stage != SM50_RT_STAGE_RAYGEN) {
    Value *local_entry = nullptr;
    if (hardware_intersection) {
      Value *control = CandidateControlPointer(module_ir, context, block, builder);
      Value *hit_group = builder.CreateAdd(
        builder.CreateAdd(
          loadBlockWord(builder, control, kRtCandidateHitGroupBase),
          builder.CreateMul(
            function->getArg(geometry_index),
            loadBlockWord(builder, control, kRtCandidateGeometryMultiplier)
          )
        ),
        frameInstanceContribution(builder, block, function->getArg(instance_index))
      );
      local_entry = hitGroupLocalRootEntryAt(
        builder,
        loadBlockQword(builder, block, offsetof(SM50_RT_FRAME, DispatchArguments)),
        hit_group
      );
      storeBlockQword(
        builder, block, offsetof(SM50_RT_FRAME, LocalRootEntry), local_entry
      );
    } else {
      local_entry =
        loadBlockQword(builder, block, offsetof(SM50_RT_FRAME, LocalRootEntry));
    }
    handOverFrameEntries(*binding_map, builder, block, local_entry);
  } else {
    /* The ray generation shader's local root arguments follow its own record's identifier. */
    Value *dispatch = function->getArg(dispatch_arguments_index);
    Value *bytes = builder.CreateBitCast(
      dispatch, Type::getInt8PtrTy(llvm_context, cast<PointerType>(dispatch->getType())->getAddressSpace())
    );
    Value *local_entry =
      EmitShaderRecordLookup(builder, bytes, RtShaderTable::RayGeneration, builder.getInt32(0)).local_entry;
    binding_map->SetLocalRootArgumentPointer(builder.CreateIntToPtr(local_entry, Type::getInt8PtrTy(llvm_context, 1)));
  }

  /*
   * The transplanted body runs between the prologue above and the epilogue below, so
   * the epilogue is a block of its own: `TransplantEntryBody` redirects every `ret` of
   * the DXIL entry to the exit block it is given, and that block must not be the one the
   * prologue has already branched out of.
   */
  auto *epilogue_bb = BasicBlock::Create(llvm_context, "epilogue", function);
  auto body_entry = TransplantEntryBody(*module.entry, *function, *epilogue_bb);
  if (!body_entry)
    return body_entry.takeError();

  /*
   * The DXIL entry's own parameters must be rebound onto this invocation's storage.
   *
   * A DXR shader declares its payload (and, for a hit shader, its intersection
   * attributes) as function parameters - `void ClosestHit(inout RayPayload payload,
   * in BuiltInTriangleIntersectionAttributes attributes)` - and the transplanted body
   * reads them through those pointers. The AIR function's parameters are a different
   * list entirely (the payload region, the hit state, the record index and the depth),
   * so an unrebound body would read whatever the arguments happened to be. The
   * parameters are therefore pointed at the regions of the invocation block that hold
   * the values D3D12 defines them to hold:
   *
   *   payload     the user payload area of the block, i.e. the bytes the traversal
   *               carries and the caller copied in
   *   attributes  for an any-hit, the *candidate* attribute region: the attributes of
   *               the candidate it is judging, staged by `dx.op.reportHit` for a
   *               procedural hit group or by the shared dispatch callback for a
   *               triangle one
   *               for a closest-hit, the attributes of the hit the traversal committed:
   *               the barycentrics the traversal reported for a triangle hit, or the
   *               attribute bytes `dx.op.reportHit` committed for a procedural one
   *               (which hit group this is follows from SM50_RT_EXPORT: a group whose
   *               IntersectionIndex is set is described by its intersection shader, so
   *               its closest-hit reads the reported attributes rather than
   *               barycentrics)
   *
   * The block is thread-local storage and a DXIL pointer is address space 0, so the
   * pointers are bitcasts rather than address-space casts.
   */
  /* The DXIL entry's parameters cannot be removed from its signature, but every use of
   * them can be pointed at this invocation's storage; the entry function itself keeps no
   * body after the transplant, so the now-unused parameters are dropped with it. */
  SmallVector<Argument *, 2> dxil_parameters;
  Value *working_payload = nullptr;
  StructType *qualified_type = nullptr;
  MDNode *qualified_fields = nullptr;
  unsigned stage_access = stage == SM50_RT_STAGE_CLOSEST_HIT ? 4 :
                          stage == SM50_RT_STAGE_MISS ? 8 :
                          stage == SM50_RT_STAGE_ANY_HIT ? 12 : 0;
  for (Argument &argument : module.entry->args())
    dxil_parameters.push_back(&argument);
  for (Argument *argument : dxil_parameters) {
    if (argument->getArgNo() > 1)
      return createError(
        "dxil: the raytracing export '" + Twine(function_name) +
        "' declares more than the payload and attribute parameters a DXR shader may take"
      );
    if (!argument->getType()->isPointerTy())
      return createError(
        "dxil: a raytracing export's payload / attribute parameter is not a pointer"
      );
    const bool is_attribute = argument->getArgNo() == 1;
    if (is_attribute && stage != SM50_RT_STAGE_ANY_HIT && stage != SM50_RT_STAGE_CLOSEST_HIT)
      return createError(
        "dxil: only an any-hit or closest-hit shader takes intersection attributes"
      );
    uint32_t offset = context.frame_size;
    if (is_attribute)
      offset = stage == SM50_RT_STAGE_ANY_HIT
                 ? dxil::rtInvocationLayout(context).candidate_attribute
                 : (context.intersection_index != SM50_RT_INDEX_NONE
                      /* A procedural hit group: the attributes `dx.op.reportHit`
                       * committed for the winning candidate. */
                      ? context.attributeOffset()
                      /* A triangle hit group: the barycentrics the traversal reported
                       * for the committed hit. */
                      : context.hitStateOffset() + offsetof(SM50_RT_HIT_STATE, Barycentrics));
    /* `payload` is the block head for the raygen and the payload area for every other
     * stage, so the region is addressed from the head in both cases. */
    Value *head = stage == SM50_RT_STAGE_RAYGEN
                    ? payload
                    : builder.CreateGEP(
                        Type::getInt8Ty(llvm_context), payload,
                        builder.getInt32(-static_cast<int32_t>(context.frame_size))
                      );
    Value *field = builder.CreateGEP(
      Type::getInt8Ty(llvm_context), head, builder.getInt32(offset)
    );
    if (!is_attribute && stage_access) {
      Type *type = cast<PointerType>(argument->getType())->getNonOpaquePointerElementType();
      qualified_fields = RayPayloadAnnotation(module_ir, type);
      if (qualified_fields) {
        qualified_type = cast<StructType>(type);
        if (module_ir.getDataLayout().getTypeAllocSize(type) > context.payload_size)
          return createError("dxil: qualified payload exceeds configured payload size");
        // PAQ restricts inter-shader transfer, not accesses to this invocation's
        // working copy. Never expose transport storage to the shader body.
        working_payload = builder.CreateAlloca(type, nullptr, "rt.payload.working");
        CopyQualifiedRayPayload(builder, working_payload, field, qualified_type, qualified_fields, 1u << stage_access);
        field = working_payload;
      }
    }
    /* A DXIL pointer is address space 0 while the block a visible function was handed
     * is a ray-data pointer, so the cast between the two spaces is an addrspacecast. */
    const unsigned field_space = cast<PointerType>(field->getType())->getAddressSpace();
    const unsigned argument_space =
      cast<PointerType>(argument->getType())->getAddressSpace();
    Value *bound = field_space == argument_space
                     ? builder.CreateBitCast(field, argument->getType())
                     : builder.CreateAddrSpaceCast(field, argument->getType());
    argument->replaceAllUsesWith(bound);
  }
  // Parameter addresses must precede the branch into every use in the shader body.
  builder.CreateBr(*body_entry);

  /*
   * Thread the per-invocation storage to the operation pass. The pass cannot find
   * the allocations by itself (it must not guess which alloca is the payload), and
   * the values are function-local, so they are published in a module-level named
   * metadata node that names the function. StripDXILArtifacts drops the node once
   * the pass has run.
   */
  SmallVector<Metadata *, 6> locals;
  locals.push_back(ValueAsMetadata::get(function));
  locals.push_back(ValueAsMetadata::get(payload));
  locals.push_back(ValueAsMetadata::get(hit_state));
  locals.push_back(ConstantAsMetadata::get(builder.getInt32(context.export_index)));
  locals.push_back(ConstantAsMetadata::get(builder.getInt32(0)));
  module_ir.getOrInsertNamedMetadata("dxmt.rt.locals")->addOperand(
    MDTuple::get(llvm_context, locals)
  );

  /*
   * An intersection export returns Metal's packed intersection result, which is what
   * the traversal reads to accept or reject the candidate: the reported distance, the
   * accept flag of a committed `dx.op.reportHit`, and whether an any-hit shader ended
   * the search. The three live in the per-candidate control region of this
   * invocation's block, which the callback layer's `EmitIntersectionResult` reads; the
   * block head is `frame_size` bytes in front of the payload pointer the traversal
   * handed this function.
   */
  IRBuilder<> epilogue(epilogue_bb);
  if (working_payload) {
    CopyQualifiedRayPayload(epilogue, payload, working_payload, qualified_type, qualified_fields, 2u << stage_access);
    // Any-hit control intrinsics terminate before the normal epilogue. Commit
    // permitted writes at those exits as well, including IgnoreHit.
    if (stage == SM50_RT_STAGE_ANY_HIT)
      for (BasicBlock &bb : *function)
        for (Instruction &instruction : bb)
          if (auto *call = dyn_cast<CallInst>(&instruction))
            if (Function *callee = call->getCalledFunction(); callee && callee->getName().startswith("dx.op."))
              if (call->arg_size())
                if (auto *opcode = dyn_cast<ConstantInt>(call->getArgOperand(0));
                    opcode && (opcode->getZExtValue() == 155 || opcode->getZExtValue() == 156)) {
                  IRBuilder<> exit(call);
                  CopyQualifiedRayPayload(exit, payload, working_payload, qualified_type, qualified_fields, 2u << stage_access);
                }
  }
  if (hardware_intersection) {
    /* `block` is the head the prologue already computed in the entry block, which
     * dominates this one. */
    Value *control = CandidateControlPointer(module_ir, context, block, epilogue);
    const unsigned space = cast<PointerType>(control->getType())->getAddressSpace();
    auto controlField = [&](uint32_t byte_offset, Type *type) -> Value * {
      Value *field = epilogue.CreateGEP(
        Type::getInt8Ty(llvm_context), control, epilogue.getInt32(byte_offset)
      );
      return epilogue.CreateBitCast(field, type->getPointerTo(space));
    };
    Value *committed = epilogue.CreateICmpNE(
      epilogue.CreateAlignedLoad(
        epilogue.getInt32Ty(), controlField(kRtCandidateCommitted, epilogue.getInt32Ty()), Align(1)
      ),
      epilogue.getInt32(0)
    );
    Value *distance = epilogue.CreateAlignedLoad(
      epilogue.getFloatTy(), controlField(kRtCandidateDistance, epilogue.getFloatTy()), Align(1)
    );
    Value *accept_and_continue =
      EmitIntersectionResult(module_ir, context, block, committed, epilogue);
    Value *packed = PoisonValue::get(function->getReturnType());
    packed = epilogue.CreateInsertValue(packed, distance, 0);
    packed = epilogue.CreateInsertValue(
      packed, epilogue.CreateExtractValue(accept_and_continue, 0), 1
    );
    packed = epilogue.CreateInsertValue(
      packed, epilogue.CreateExtractValue(accept_and_continue, 1), 2
    );
    epilogue.CreateRet(packed);
  } else {
    epilogue.CreateRetVoid();
  }
  module_ir.getOrInsertNamedMetadata(
    context.query_callbacks && stage == SM50_RT_STAGE_INTERSECTION ? "air.visible" : entryKindOfStage(stage)
  )->addOperand(function_metadata);
  return std::move(binding_map);
}

/* -------------------------------------------------------------------------- */
/* Shared per-candidate dispatch callback                                     */
/* -------------------------------------------------------------------------- */

namespace {

/** D3D12_HIT_KIND_*; what HitKind() reports for a fixed-function triangle hit. */
constexpr uint32_t kHitKindTriangleFrontFace = 0xfe;
constexpr uint32_t kHitKindTriangleBackFace = 0xff;

/**
 * Build the one shared per-candidate dispatch callback (SM50_RT_FUNCTION_DISPATCH).
 *
 * D3D12 runs an any-hit shader for *every* candidate the traversal offers, and lets
 * it reject the candidate (`dx.op.ignoreHit`) or end the search
 * (`dx.op.acceptHitAndEndSearch`). Public Metal's per-candidate hook is an
 * `air.intersection` function installed in the intersection function table, invoked
 * once per candidate, whose packed result is `{ i1 accept, i1 continue_search }`
 * (proven with the public toolchain and on a GPU; see dxil_rt_callbacks.hpp). One such
 * function serves the whole pipeline because the hardware selects it by
 * `intersectionFunctionTableOffset + geometry`, which is *not* the D3D12 hit group
 * `rayContribution + geometry * multiplier`: the callback recomputes the D3D12 index
 * from the candidate's own geometry id and the trace's contribution / multiplier
 * (published into the invocation block before the traversal), resolves the hit group's
 * export through the record index table, and runs that group's any-hit shader through
 * the visible function table. The callback is what makes `dx.op.ignoreHit` and
 * `dx.op.acceptHitAndEndSearch` reach the traversal.
 */
static llvm::Error EmitDispatchCallback(
  llvm::Module &module, const SM50_RT_PIPELINE_DESC &desc,
  const RaytracingExportContext &context, llvm::StringRef name
) {
  LLVMContext &llvm_context = module.getContext();
  const uint32_t export_count = desc.ExportCount;

  /*
   * The hit group a triangle candidate belongs to is
   *   rayContributionToHitGroupIndex + geometryIndex * multiplier
   * (D3D12's selection rule, the same one the traversal lowering uses for the
   * closest-hit record), and the record index table turns that into the export
   * index of the group. The group's any-hit member is a *different* export, so the
   * callback reads it from a compile-time table built from the export description:
   * element i is `SM50_RT_EXPORT[i].AnyHitIndex`, i.e. the any-hit shader of the hit
   * group whose primary shader is export i. A group without one holds the
   * SM50_RT_INDEX_NONE sentinel and the candidate is accepted without a call.
   */
  std::vector<Constant *> any_hit_words;
  any_hit_words.reserve(export_count);
  for (uint32_t i = 0; i < export_count; ++i)
    any_hit_words.push_back(
      ConstantInt::get(llvm_context, APInt(32, desc.Exports[i].AnyHitIndex))
    );
  auto *any_hit_type = ArrayType::get(Type::getInt32Ty(llvm_context), std::max(export_count, 1u));
  auto *any_hit_table = new GlobalVariable(
    module, any_hit_type, /*isConstant=*/true, GlobalValue::PrivateLinkage,
    ConstantArray::get(any_hit_type, any_hit_words), "dxmt.rt.any_hit_table",
    nullptr, GlobalValue::NotThreadLocal, /*AddressSpace=*/2
  );

  air::FunctionSignatureBuilder signature;
  /*
   * The triangle form of an intersection function, exactly as the public front end
   * emits it (/tmp/rtfix/c1.ll): the barycentrics and the ids of the candidate the
   * traversal is offering, the distance and facing of that candidate, and the payload.
   * The barycentrics are not read by this callback - the any-hit shader reads them
   * from the hit state it is handed - but Metal's triangle contract has them first.
   */
  const uint32_t bary_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.barycentric_coord", .type_name = "float2", .arg_name = "barycentrics",
  });
  const uint32_t primitive_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.primitive_id", .type_name = "uint", .arg_name = "primitive_id",
  });
  const uint32_t geometry_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.geometry_id", .type_name = "uint", .arg_name = "geometry_id",
  });
  const uint32_t instance_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.instance_id", .type_name = "uint", .arg_name = "instance_id",
  });
  const uint32_t user_instance_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.user_instance_id", .type_name = "uint", .arg_name = "user_instance_id",
  });
  const uint32_t distance_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.distance", .type_name = "float", .arg_name = "distance",
  });
  const uint32_t facing_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.front_facing", .type_name = "bool", .arg_name = "front_facing",
  });
  /*
   * The dispatch constants and the visible function table are what the callback
   * resolves the hit group with and calls its any-hit shader through. A probe proves
   * an `[[intersection(...)]]` function may declare both (/tmp/rtprobe3/p_bufr.metal
   * for the buffer, /tmp/cbprobe/i1.ll for the table).
   */
  const uint32_t dispatch_index = signature.DefineInput(air::InputRaytracingDispatchArguments{
    .location_index = context.dispatch_arguments_slot,
    .size = uint32_t(module.getModuleFlag(kAtomic64PhaseFlag)
      ? sizeof(SM50_RT_DISPATCH_ARGUMENTS) : offsetof(SM50_RT_DISPATCH_ARGUMENTS, ImmutableConstants)),
    .arg_name = "dispatch",
  });
  const uint32_t table_index = signature.DefineInput(air::InputVisibleFunctionTable{
    .location_index = context.visible_function_table_slot,
    .memory_access = air::MemoryAccess::read,
    .arg_name = "vft",
  });
  const uint32_t payload_index = signature.DefineInput(air::InputRaytracingPayload{
    .size = context.invocationBlockSize(),
    .address_space = 5,
    .arg_name = "payload",
  });
  /* The candidate instance's transforms and the ray in its object space (world_space_data). */
  const uint32_t object_origin_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.origin", .type_name = "float3", .arg_name = "origin",
  });
  const uint32_t object_direction_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.direction", .type_name = "float3", .arg_name = "direction",
  });
  const uint32_t object_to_world_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.object_to_world_transform", .type_name = "float4x3", .arg_name = "object_to_world",
  });
  const uint32_t world_to_object_index = signature.DefineInput(air::InputRaytracingValue{
    .kind = "air.world_to_object_transform", .type_name = "float4x3", .arg_name = "world_to_object",
  });
  signature.DefineOutput(air::OutputAcceptIntersection{});
  signature.DefineOutput(air::OutputContinueSearch{});
  signature.UseFunctionTags({"air.triangle", "air.instancing", "air.triangle_data", "air.world_space_data"});

  auto [function, metadata] =
    signature.CreateFunction(name.str(), llvm_context, module, 0, false);
  auto *entry_bb = BasicBlock::Create(llvm_context, "entry", function);
  IRBuilder<> builder(entry_bb);

  Value *block = function->getArg(payload_index);
  Value *vft = function->getArg(table_index);
  Value *dispatch = function->getArg(dispatch_index);
  /* Hardware carries the whole block; visible shaders receive its user payload. */
  Value *payload = builder.CreateGEP(
    Type::getInt8Ty(llvm_context), block,
    builder.getInt32(context.frame_size)
  );
  Value *control = CandidateControlPointer(module, context, block, builder);
  Value *hit_state = builder.CreateGEP(
    Type::getInt8Ty(llvm_context), block, builder.getInt32(context.hitStateOffset())
  );

  /*
   * Publish the candidate's own system values into the hit state before the any-hit
   * shader runs, so a shader that reads PrimitiveIndex / GeometryIndex / InstanceIndex
   * / InstanceID / RayTCurrent / HitKind / barycentrics sees the candidate it is
   * judging rather than whatever the traversal left behind.
   */
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, PrimitiveIndex), builder.getInt32Ty(),
    function->getArg(primitive_index)
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, GeometryIndex), builder.getInt32Ty(),
    function->getArg(geometry_index)
  );
  /* D3D12's InstanceID is the application's user defined id (the traversal's *user*
   * instance id) and InstanceIndex is the autogenerated index (the traversal's
   * instance id); the two are separate fields and must not be interchanged. */
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, InstanceID), builder.getInt32Ty(),
    function->getArg(user_instance_index)
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, InstanceIndex), builder.getInt32Ty(),
    function->getArg(instance_index)
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, RayTCurrent), builder.getFloatTy(),
    function->getArg(distance_index)
  );
  Value *front_facing = builder.CreateZExt(function->getArg(facing_index), builder.getInt32Ty());
  /* D3D12_HIT_KIND_TRIANGLE_FRONT_FACE / _BACK_FACE: what HitKind() reports for a
   * fixed-function triangle hit, which is what a triangle hit group's candidate is. */
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, HitKind), builder.getInt32Ty(),
    builder.CreateSelect(
      builder.CreateICmpNE(front_facing, builder.getInt32(0)),
      builder.getInt32(kHitKindTriangleFrontFace),
      builder.getInt32(kHitKindTriangleBackFace)
    )
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, Barycentrics),
    FixedVectorType::get(builder.getFloatTy(), 2), function->getArg(bary_index)
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, FrontFace), builder.getInt32Ty(),
    front_facing
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, ObjectRayOrigin),
    FixedVectorType::get(builder.getFloatTy(), 3), function->getArg(object_origin_index)
  );
  storeBlockField(
    builder, hit_state, offsetof(SM50_RT_HIT_STATE, ObjectRayDirection),
    FixedVectorType::get(builder.getFloatTy(), 3), function->getArg(object_direction_index)
  );
  StoreInstanceTransforms(
    builder, hit_state, function->getArg(object_to_world_index), function->getArg(world_to_object_index)
  );

  /* Hit group -> export index, through the record index table the runtime's
   * preparation pass built. A pipeline with no hit-group table publishes a zero base
   * and the candidate has no record to run.
   *
   * The hit group is the D3D12 index
   *   InstanceContributionToHitGroupIndex + RayContributionToHitGroupIndex
   *     + GeometryIndex * MultiplierForGeometryContributionToHitGroupIndex
   * where the instance contribution is the application's per-instance word for the
   * instance this candidate belongs to: the tracing shader published the traced
   * structure's contribution array into the frame before the traversal, because the
   * callback runs *while* the traversal is still running and cannot read the
   * traversal result. A structure with no such array contributes nothing. */
  Value *contribution = loadBlockWord(builder, control, kRtCandidateHitGroupBase);
  Value *multiplier = loadBlockWord(builder, control, kRtCandidateGeometryMultiplier);
  Value *instance_contribution =
    frameInstanceContribution(builder, block, function->getArg(instance_index));
  Value *group = builder.CreateAdd(
    builder.CreateAdd(
      contribution,
      builder.CreateMul(function->getArg(geometry_index), multiplier)
    ),
    instance_contribution
  );
  /*
   * SM50_RT_DISPATCH_ARGUMENTS is a plain constant buffer the runtime binds at the
   * callback's own argument index; the hit group record is resolved straight out of the
   * application's table (EmitShaderRecordLookup), which also yields the record's local
   * root arguments - the bytes after its identifier.
   */
  const unsigned dispatch_space =
    cast<PointerType>(dispatch->getType())->getAddressSpace();
  Value *dispatch_bytes = builder.CreateBitCast(
    dispatch, Type::getInt8PtrTy(llvm_context, dispatch_space)
  );
  RtShaderRecord hit_group_record =
    EmitShaderRecordLookup(builder, dispatch_bytes, RtShaderTable::HitGroup, group);
  Value *record = hit_group_record.export_index;
  Value *valid = builder.CreateAnd(
    builder.CreateICmpNE(record, builder.getInt32(SM50_RT_INDEX_NONE)),
    builder.CreateICmpULT(record, builder.getInt32(export_count))
  );
  /* An unresolvable record has no any-hit shader to run, exactly like a group without
   * one: the candidate is accepted and the search continues. */
  Value *any_hit = emitGuardedValue(
    builder, valid, builder.getInt32(SM50_RT_INDEX_NONE), "rt.dispatch.any_hit",
    [&](IRBuilder<> &inner) -> Value * {
      return inner.CreateLoad(
        inner.getInt32Ty(),
        inner.CreateGEP(any_hit_type, any_hit_table, {inner.getInt32(0), record})
      );
    }
  );
  Value *run_any_hit = builder.CreateAnd(
    valid, builder.CreateICmpNE(any_hit, builder.getInt32(SM50_RT_INDEX_NONE))
  );
  /*
   * The local root arguments of the record this candidate's hit group runs as, published
   * into the frame the any-hit shader reads: this callback is the only party that knows
   * the record's position (the hit group index, instance contribution included) while
   * the traversal runs. Never the global root arguments, never the ray generation
   * shader's own record; zero for an unresolved record.
   */
  Value *any_hit_entry = hit_group_record.local_entry;
  {
    LLVMContext &context = builder.getContext();
    Function *function = builder.GetInsertBlock()->getParent();
    BasicBlock *head = builder.GetInsertBlock();
    // The callback is built sequentially; head has no terminator or suffix to split.
    BasicBlock *continuation = BasicBlock::Create(context, "rt.dispatch.call.cont", function);
    BasicBlock *call = BasicBlock::Create(context, "rt.dispatch.call", function, continuation);
    builder.SetInsertPoint(head);
    builder.CreateCondBr(run_any_hit, call, continuation);
    IRBuilder<> inner(call);
    /* The candidate's decision belongs to this candidate alone: clearing it first is
     * what makes the verdict read back below the any-hit shader's answer. */
    storeBlockWord(inner, control, kRtCandidateDecision, inner.getInt32(kRtCandidateNone));
    /* The entry the any-hit shader resolves its own local root ranges through, and
     * what `dx.op.reportHit`'s any-hit call of a procedural group inherits. */
    storeBlockQword(
      inner, block, offsetof(SM50_RT_FRAME, LocalRootEntry), any_hit_entry
    );
    Value *depth = loadBlockWord(inner, block, offsetof(SM50_RT_FRAME, Depth));
    if (!EmitVisibleFunctionCall(module, vft, any_hit, depth, payload, hit_state, inner))
      return createError("dxil: the per-candidate dispatch callback could not reach the "
                         "visible function table");
    inner.CreateBr(continuation);
    builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
  }

  Value *decision = loadBlockWord(builder, control, kRtCandidateDecision);
  Value *accepted = builder.CreateICmpNE(decision, builder.getInt32(kRtCandidateReject));
  Value *result = EmitIntersectionResult(module, context, block, accepted, builder);
  builder.CreateRet(result);
  module.getOrInsertNamedMetadata("air.intersection")->addOperand(metadata);
  return llvm::Error::success();
}

/** A fixed hardware bounding-box callback dispatches the execution-time SBT record.
 * Export bodies retain their own local-root and ReportHit group context. */
static llvm::Error EmitBoundingBoxDispatch(
  Module &module, const SM50_RT_PIPELINE_DESC &desc,
  const RaytracingExportContext &context, StringRef name
) {
  SmallVector<std::pair<uint32_t, Function *>, 4> targets;
  for (uint32_t i = 0; i < desc.ExportCount; ++i) {
    if (desc.Exports[i].Stage != SM50_RT_STAGE_INTERSECTION)
      continue;
    Function *target = module.getFunction(desc.Exports[i].EntryPoint);
    if (!target)
      return createError("dxil: missing procedural intersection body");
    targets.emplace_back(i, target);
  }
  if (targets.empty())
    return llvm::Error::success();

  LLVMContext &ctx = module.getContext();
  Function *first = targets.front().second;
  Function *callback = Function::Create(first->getFunctionType(),
    GlobalValue::ExternalLinkage, name, module);
  IRBuilder<> b(BasicBlock::Create(ctx, "entry", callback));
  // LowerRaytracingStage's fixed intersection ABI: geometry, instance, frame at 5, 6, 8.
  Value *block = callback->getArg(8);

  Value *control = CandidateControlPointer(module, context, block, b);
  Value *group = b.CreateAdd(
    b.CreateAdd(loadBlockWord(b, control, kRtCandidateHitGroupBase),
      b.CreateMul(callback->getArg(5), loadBlockWord(b, control, kRtCandidateGeometryMultiplier))),
    frameInstanceContribution(b, block, callback->getArg(6)));
  Value *dispatch = b.CreateIntToPtr(
    loadBlockQword(b, block, offsetof(SM50_RT_FRAME, DispatchArguments)),
    Type::getInt8PtrTy(ctx, 1));
  BasicBlock *reject = BasicBlock::Create(ctx, "no_record", callback);
  IRBuilder<> no_record(reject);
  Value *result = PoisonValue::get(callback->getReturnType());
  result = no_record.CreateInsertValue(result, callback->getArg(3), 0);
  result = no_record.CreateInsertValue(result, no_record.getFalse(), 1);
  result = no_record.CreateInsertValue(result, no_record.getTrue(), 2);
  no_record.CreateRet(result);
  /* An unresolved record (SM50_RT_INDEX_NONE) takes the switch's default: no candidate. */
  Value *record = EmitShaderRecordLookup(b, dispatch, RtShaderTable::HitGroup, group).export_index;
  SwitchInst *select = b.CreateSwitch(record, reject, targets.size());
  SmallVector<Value *, 12> arguments;
  for (Argument &argument : callback->args())
    arguments.push_back(&argument);
  for (const auto &[index, target] : targets) {
    if (target->getFunctionType() != callback->getFunctionType())
      return createError("dxil: inconsistent procedural intersection ABI");
    BasicBlock *invoke = BasicBlock::Create(ctx, "group_" + std::to_string(index), callback);
    IRBuilder<> call(invoke);
    call.CreateRet(call.CreateCall(target, arguments));
    select->addCase(b.getInt32(index), invoke);
    target->setLinkage(GlobalValue::InternalLinkage);
    target->addFnAttr(Attribute::AlwaysInline);
  }

  NamedMDNode *entries = module.getNamedMetadata("air.intersection");
  SmallVector<MDNode *, 4> kept;
  MDNode *callback_metadata = nullptr;
  for (MDNode *entry : entries->operands()) {
    auto *value = dyn_cast<ValueAsMetadata>(entry->getOperand(0));
    Function *function = value ? dyn_cast<Function>(value->getValue()) : nullptr;
    bool body = false;
    for (const auto &[index, target] : targets)
      body |= function == target;
    if (!body) {
      kept.push_back(entry);
    } else if (function == first) {
      SmallVector<Metadata *, 8> fields;
      for (const MDOperand &operand : entry->operands())
        fields.push_back(operand.get());
      fields[0] = ValueAsMetadata::get(callback);
      callback_metadata = MDNode::get(ctx, fields);
    }
  }
  if (!callback_metadata)
    return createError("dxil: missing procedural intersection metadata");
  entries->clearOperands();
  for (MDNode *entry : kept)
    entries->addOperand(entry);
  entries->addOperand(callback_metadata);
  return llvm::Error::success();
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Export merging                                                             */
/* -------------------------------------------------------------------------- */

llvm::Error MergeRaytracingExport(
  llvm::Module &target, llvm::Module &source, llvm::StringRef entry_name
) {
  /*
   * The stage builder hands the operation pass its per-invocation storage through the
   * `dxmt.rt.locals` named node, which names the payload block and the hit state block -
   * function-local values, i.e. `LocalAsMetadata` operands. The pass has consumed the
   * node by now and nothing reads it again, but `StripDXILArtifacts` only drops the
   * `dx.`-prefixed scaffolding, so the node is still in the module here. A named
   * metadata node is serialized by the bitcode writer's *module* enumerator, which
   * accepts only nodes, strings and constants (`enumerateMetadataImpl` asserts
   * "Invalid metadata kind"), so leaving it in faults inside `WriteBitcodeToFile`
   * below. It is dropped here, at the last point before the export is serialized, and
   * the AIR entry metadata - the only metadata the merged pipeline is built from - is
   * untouched.
   */
  if (NamedMDNode *locals = source.getNamedMetadata("dxmt.rt.locals"))
    source.eraseNamedMetadata(locals);
  if (llvm::verifyModule(source, &llvm::errs())) {
    llvm::errs() << "OWNED_RT_INVALID_EXPORT_BEGIN " << entry_name << "\n";
    source.print(llvm::errs(), nullptr);
    llvm::errs() << "OWNED_RT_INVALID_EXPORT_END\n";
    return createError("invalid raytracing export before merge: " + entry_name);
  }

  /*
   * Every export is lowered in its own ModuleInfo, i.e. its own LLVMContext, so the
   * module cannot be moved into the pipeline module directly: it is round-tripped
   * through bitcode and parsed into the target's context. The bitcode is what the
   * front end already produced (DXIL requires typed pointers, so opaque-pointer
   * remapping is not an option).
   */
  SmallVector<char, 0> bitcode;
  raw_svector_ostream stream(bitcode);
  WriteBitcodeToFile(source, stream);

  LLVMContext &context = target.getContext();
  auto buffer = MemoryBuffer::getMemBuffer(
    StringRef(bitcode.data(), bitcode.size()), "raytracing-export", false
  );
  auto parsed = parseBitcodeFile(buffer->getMemBufferRef(), context);
  if (!parsed) {
    std::string message;
    llvm::raw_string_ostream error(message);
    llvm::handleAllErrors(parsed.takeError(), [&](const llvm::ErrorInfoBase &base) {
      base.log(error);
    });
    return createError("failed to merge raytracing export '" + entry_name + "': " + error.str());
  }

  if (Linker::linkModules(target, std::move(*parsed)))
    return createError("failed to link raytracing export '" + entry_name + "'");
  return llvm::Error::success();
}

} // namespace dxmt::dxil

/* -------------------------------------------------------------------------- */
/* Export enumeration                                                         */
/* -------------------------------------------------------------------------- */

namespace {



/** Hand an llvm::Error to the caller, or release it when no slot was offered. */
void reportError(llvm::Error failure, sm50_error_t *ppError) {
  auto *error = new dxmt::dxil::SM50ErrorInternal();
  llvm::raw_svector_ostream stream(error->buf);
  llvm::handleAllErrors(
    std::move(failure), [&](const llvm::ErrorInfoBase &base) { base.log(stream); }
  );
  stream << '\0';
  if (ppError)
    *ppError = (sm50_error_t)error;
  else
    delete error;
}

void reportMessage(const llvm::Twine &message, sm50_error_t *ppError) {
  auto *error = new dxmt::dxil::SM50ErrorInternal();
  llvm::raw_svector_ostream stream(error->buf);
  stream << message << '\0';
  if (ppError)
    *ppError = (sm50_error_t)error;
  else
    delete error;
}

} // namespace

AIRCONV_API int SM50RTEnumerateLibraryExports(
  const void *pBytecode, uint64_t BytecodeSize, struct sm50_rt_library_export_info *pExports,
  uint32_t ExportCapacity, char *pNames, uint64_t NamesSize, uint32_t *pExportCount,
  uint64_t *pNamesSize, sm50_error_t *ppError
) {
  using namespace dxmt;

  if (ppError)
    *ppError = nullptr;
  if (!pBytecode || !BytecodeSize || !pExportCount || !pNamesSize) {
    reportMessage(
      "SM50RTEnumerateLibraryExports requires bytecode and both count outputs", ppError
    );
    return 0;
  }

  std::vector<dxil::LibraryExport> exports;
  if (auto failure = dxil::enumerateExports(pBytecode, BytecodeSize, exports)) {
    reportError(std::move(failure), ppError);
    return 0;
  }

  uint64_t names_size = 0;
  for (const auto &entry : exports)
    /* Both names of an export, each NUL terminated. */
    names_size += entry.name.size() + 1 + entry.entry_point.size() + 1;

  const bool query_only = !pExports || !pNames;
  if (query_only || ExportCapacity < exports.size() || NamesSize < names_size) {
    /* Two-call protocol: always report the required sizes, and only fail when the
     * caller offered buffers that are too small. */
    *pExportCount = static_cast<uint32_t>(exports.size());
    *pNamesSize = names_size;
    if (query_only)
      return 1;
    reportMessage(
      "SM50RTEnumerateLibraryExports buffers are too small for the library exports", ppError
    );
    return 0;
  }

  uint64_t offset = 0;
  auto write_name = [&](const std::string &value) {
    const uint32_t name_offset = static_cast<uint32_t>(offset);
    std::memcpy(pNames + offset, value.c_str(), value.size() + 1);
    offset += value.size() + 1;
    return name_offset;
  };
  for (size_t i = 0; i < exports.size(); ++i) {
    auto &info = pExports[i];
    info.stage = exports[i].stage;
    info.name_offset = write_name(exports[i].name);
    info.name_length = static_cast<uint32_t>(exports[i].name.size());
    info.entry_point_offset = write_name(exports[i].entry_point);
    info.entry_point_length = static_cast<uint32_t>(exports[i].entry_point.size());
  }
  *pExportCount = static_cast<uint32_t>(exports.size());
  *pNamesSize = names_size;
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Ray generation dispatcher for multi-raygen pipelines                       */
/* -------------------------------------------------------------------------- */

static llvm::Error EmitRayGenerationDispatch(
  Module &module, const std::vector<uint32_t> &raygen_indices,
  const std::vector<std::string> &names, StringRef name
) {
  LLVMContext &ctx = module.getContext();
  NamedMDNode *kernels = module.getNamedMetadata("air.kernel");
  if (!kernels || names.empty() || names.size() != raygen_indices.size())
    return dxmt::dxil::createError("dxil: missing raygen dispatcher entries");

  SmallVector<Function *, 4> targets;
  SmallVector<MDNode *, 4> entries;
  std::vector<std::vector<unsigned>> mappings;
  SmallVector<Type *, 8> types;
  SmallVector<MDNode *, 8> inputs;
  std::map<std::string, unsigned> slots;
  unsigned dispatch_index = ~0u;
  for (const auto &target_name : names) {
    Function *target = module.getFunction(target_name);
    MDNode *entry = nullptr;
    for (MDNode *candidate : kernels->operands()) {
      auto *value = dyn_cast<ValueAsMetadata>(candidate->getOperand(0));
      if (value && value->getValue() == target)
        entry = candidate;
    }
    if (!target || !entry || entry->getNumOperands() < 3)
      return dxmt::dxil::createError("dxil: missing raygen function metadata: " + target_name);
    auto *arguments = dyn_cast<MDNode>(entry->getOperand(2));
    if (!arguments || arguments->getNumOperands() != target->arg_size())
      return dxmt::dxil::createError("dxil: inconsistent raygen argument metadata");
    targets.push_back(target);
    entries.push_back(entry);
    mappings.emplace_back(target->arg_size(), ~0u);
    for (const MDOperand &operand : arguments->operands()) {
      auto *input = cast<MDNode>(operand.get());
      unsigned argument = mdconst::extract<ConstantInt>(input->getOperand(0))->getZExtValue();
      if (argument >= target->arg_size())
        return dxmt::dxil::createError("dxil: raygen argument index is out of range");
      StringRef kind = cast<MDString>(input->getOperand(1))->getString();
      uint64_t location = ~0ull;
      uint64_t size = 0;
      for (unsigned i = 2; i + 1 < input->getNumOperands(); ++i) {
        auto *key = dyn_cast<MDString>(input->getOperand(i));
        if (!key)
          continue;
        if (key->getString() == "air.location_index")
          location = mdconst::extract<ConstantInt>(input->getOperand(i + 1))->getZExtValue();
        if (key->getString() == "air.buffer_size")
          size = mdconst::extract<ConstantInt>(input->getOperand(i + 1))->getZExtValue();
      }
      const bool buffer = kind == "air.buffer" || kind == "air.indirect_buffer";
      std::string key = (buffer ? "buffer" : kind.str()) + ":" + std::to_string(location);
      auto [slot, inserted] = slots.emplace(key, types.size());
      unsigned index = slot->second;
      Type *type = target->getArg(argument)->getType();
      if (inserted) {
        types.push_back(type);
        SmallVector<Metadata *, 16> fields;
        for (const MDOperand &field : input->operands())
          fields.push_back(field.get());
        fields[0] = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(ctx), index));
        inputs.push_back(MDNode::get(ctx, fields));
      } else if (types[index] != type) {
        auto *old_pointer = dyn_cast<PointerType>(types[index]);
        auto *new_pointer = dyn_cast<PointerType>(type);
        if (!buffer || !old_pointer || !new_pointer ||
            old_pointer->getAddressSpace() != new_pointer->getAddressSpace())
          return dxmt::dxil::createError("dxil: incompatible raygen binding types");
        // Different local-root layouts share one byte-addressed buffer binding.
        // Each selected body casts it back to its own validated root-signature layout.
        for (unsigned i = 2; i + 1 < inputs[index]->getNumOperands(); ++i) {
          auto *field = dyn_cast<MDString>(inputs[index]->getOperand(i));
          if (field && field->getString() == "air.buffer_size")
            size = std::max(size, mdconst::extract<ConstantInt>(inputs[index]->getOperand(i + 1))->getZExtValue());
        }
        types[index] = Type::getInt8PtrTy(ctx, old_pointer->getAddressSpace());
        auto word = [&](uint64_t value) -> Metadata * {
          return ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(ctx), value));
        };
        inputs[index] = MDNode::get(ctx, {word(index), MDString::get(ctx, "air.buffer"),
          MDString::get(ctx, "air.buffer_size"), word(size), MDString::get(ctx, "air.location_index"),
          word(location), word(1), MDString::get(ctx, "air.read"), MDString::get(ctx, "air.address_space"),
          word(old_pointer->getAddressSpace()), MDString::get(ctx, "air.arg_type_name"),
          MDString::get(ctx, "uchar"), MDString::get(ctx, "air.arg_name"), MDString::get(ctx, "root_union")});
      }
      mappings.back()[argument] = index;
      if (buffer && location == SM50_RT_BINDING_INDEX_DISPATCH)
        dispatch_index = index;
    }
  }
  if (dispatch_index == ~0u)
    return dxmt::dxil::createError("dxil: raygen dispatcher has no dispatch binding");
  Function *dispatcher = Function::Create(FunctionType::get(Type::getVoidTy(ctx), types, false),
    GlobalValue::ExternalLinkage, name, module);
  IRBuilder<> b(BasicBlock::Create(ctx, "entry", dispatcher));
  Value *dispatch = dispatcher->getArg(dispatch_index);
  unsigned space = cast<PointerType>(dispatch->getType())->getAddressSpace();
  Value *bytes = b.CreateBitCast(dispatch, Type::getInt8PtrTy(ctx, space));
  BasicBlock *done = BasicBlock::Create(ctx, "done", dispatcher);
  IRBuilder<>(done).CreateRetVoid();
  /* The identifier of the application's ray generation record names the raygen to run;
   * a record of another pipeline (SM50_RT_INDEX_NONE) takes the default: nothing runs. */
  Value *record = EmitShaderRecordLookup(b, bytes, dxmt::dxil::RtShaderTable::RayGeneration, b.getInt32(0)).export_index;
  SwitchInst *select = b.CreateSwitch(record, done, targets.size());
  for (unsigned i = 0; i < targets.size(); ++i) {
    Function *target = targets[i];
    BasicBlock *invoke = BasicBlock::Create(ctx, "raygen_" + std::to_string(raygen_indices[i]), dispatcher);
    IRBuilder<> call(invoke);
    SmallVector<Value *, 8> arguments;
    for (Argument &argument : target->args()) {
      Value *value = dispatcher->getArg(mappings[i][argument.getArgNo()]);
      if (value->getType() != argument.getType())
        value = call.CreateBitCast(value, argument.getType());
      arguments.push_back(value);
    }
    call.CreateCall(target, arguments);
    call.CreateBr(done);
    select->addCase(b.getInt32(raygen_indices[i]), invoke);
    target->setLinkage(GlobalValue::InternalLinkage);
    target->addFnAttr(Attribute::AlwaysInline);
  }
  SmallVector<Metadata *, 8> dispatcher_fields;
  for (const MDOperand &operand : entries.front()->operands())
    dispatcher_fields.push_back(operand.get());
  dispatcher_fields[0] = ValueAsMetadata::get(dispatcher);
  dispatcher_fields[2] = MDNode::get(ctx, SmallVector<Metadata *, 8>(inputs.begin(), inputs.end()));
  MDNode *metadata = MDNode::get(ctx, dispatcher_fields);
  SmallVector<MDNode *, 4> kept;
  for (MDNode *entry : kernels->operands())
    if (std::find(entries.begin(), entries.end(), entry) == entries.end())
      kept.push_back(entry);
  kernels->clearOperands();
  for (MDNode *entry : kept)
    kernels->addOperand(entry);
  kernels->addOperand(metadata);
  return Error::success();
}

/* -------------------------------------------------------------------------- */
/* Pipeline compilation                                                       */
/* -------------------------------------------------------------------------- */

AIRCONV_API int SM50CompileRaytracingPipeline(
  const struct SM50_RT_PIPELINE_DESC *pDesc, sm50_bitcode_t *ppBitcode,
  struct SM50_COMPILED_BITCODE *pMetallib, struct SM50_RT_FUNCTION_INFO *pFunctions,
  uint32_t FunctionCapacity, uint32_t *pFunctionCount, sm50_error_t *ppError
) {
  using namespace dxmt;



  if (ppError)
    *ppError = nullptr;
  if (!pDesc || !ppBitcode || !pMetallib || !pFunctionCount) {
    reportMessage("SM50CompileRaytracingPipeline requires a description and outputs", ppError);
    return 0;
  }
  if (!pDesc->Exports || !pDesc->ExportCount || !pDesc->Libraries || !pDesc->LibraryCount) {
    reportMessage("the raytracing pipeline description has no libraries or exports", ppError);
    return 0;
  }
  /*
   * Reflection includes exports, triangle and optional bounding-box callbacks,
   * and an optional synthesized multi-raygen dispatcher.
   */
  uint32_t raygen_count = 0;
  uint32_t procedural_continuation_count = 0;
  bool has_intersection = false;
  for (uint32_t i = 0; i < pDesc->ExportCount; ++i) {
    raygen_count += pDesc->Exports[i].Stage == SM50_RT_STAGE_RAYGEN;
    has_intersection |= pDesc->Exports[i].Stage == SM50_RT_STAGE_INTERSECTION;
    procedural_continuation_count += pDesc->Exports[i].Stage == SM50_RT_STAGE_INTERSECTION &&
      pDesc->Exports[i].ClosestHitIndex != SM50_RT_INDEX_NONE;
  }
  const uint64_t reflection_count = uint64_t(pDesc->ExportCount) + 1 +
    uint32_t(has_intersection) + uint32_t(raygen_count > 1) + procedural_continuation_count;
  if (reflection_count > FunctionCapacity) {
    reportMessage(
      "the reflection buffer is smaller than the export count plus compiler callbacks",
      ppError
    );
    return 0;
  }

  /*
   * Validate the description before any DXIL is touched. A hit group is one entry
   * whose *_index fields name its members, so the checks below are what stops a
   * member from being silently ignored.
   */

  std::vector<uint32_t> raygen_indices;
  bool have_raygen = false;
  for (uint32_t i = 0; i < pDesc->ExportCount; ++i) {
    const SM50_RT_EXPORT &export_desc = pDesc->Exports[i];
    if (!export_desc.EntryPoint || !export_desc.EntryPoint[0]) {
      reportMessage("a raytracing export has no entry point name", ppError);
      return 0;
    }
    /*
     * The container entry point name is what selects the function inside the DXIL
     * library; without it the export could only be compiled by guessing, so a
     * description that omits it is refused rather than lowered from the wrong entry.
     */
    if (!export_desc.ContainerEntryPoint || !export_desc.ContainerEntryPoint[0]) {
      reportMessage(
        "a raytracing export has no DXIL container entry point name", ppError
      );
      return 0;
    }
    if (export_desc.LibraryIndex >= pDesc->LibraryCount) {
      reportMessage("a raytracing export names a library that is not in the description", ppError);
      return 0;
    }
    switch (export_desc.Stage) {
    case SM50_RT_STAGE_LIBRARY:
      break;
    case SM50_RT_STAGE_RAYGEN:
      have_raygen = true;
      raygen_indices.push_back(i);
      break;
    case SM50_RT_STAGE_MISS:
    case SM50_RT_STAGE_CLOSEST_HIT:
    case SM50_RT_STAGE_CALLABLE:
    case SM50_RT_STAGE_ANY_HIT:
    case SM50_RT_STAGE_INTERSECTION:
      break;
    default:
      reportMessage(
        "a raytracing export has a stage that is not a raytracing stage; only ray "
        "generation, miss, any-hit, closest-hit, intersection and callable are supported",
        ppError
      );
      return 0;
    }
    /*
     * A hit group is described by exactly one entry whose *_index fields name its
     * members. The primary shader of a procedural group is its intersection shader
     * and of a triangle group its closest-hit shader, which is what the runtime
     * resolves; a group whose primary stage disagrees with its type cannot be
     * installed consistently, so it is refused.
     */
    if (export_desc.HitGroupType == SM50_RT_HIT_GROUP_PROCEDURAL &&
        export_desc.Stage != SM50_RT_STAGE_INTERSECTION) {
      reportMessage(
        "a procedural hit group must be described by its intersection shader", ppError
      );
      return 0;
    }
    if (export_desc.HitGroupType == SM50_RT_HIT_GROUP_TRIANGLES &&
        export_desc.Stage == SM50_RT_STAGE_INTERSECTION) {
      reportMessage(
        "a triangle hit group has no intersection shader: it is described by its "
        "closest-hit shader",
        ppError
      );
      return 0;
    }
    if (export_desc.AnyHitIndex != SM50_RT_INDEX_NONE &&
        export_desc.AnyHitIndex >= pDesc->ExportCount) {
      reportMessage("a hit group names an any-hit export that does not exist", ppError);
      return 0;
    }
    if (export_desc.ClosestHitIndex != SM50_RT_INDEX_NONE &&
        export_desc.ClosestHitIndex >= pDesc->ExportCount) {
      reportMessage("a hit group names a closest-hit export that does not exist", ppError);
      return 0;
    }
    if (export_desc.IntersectionIndex != SM50_RT_INDEX_NONE &&
        export_desc.IntersectionIndex >= pDesc->ExportCount) {
      reportMessage("a hit group names an intersection export that does not exist", ppError);
      return 0;
    }
  }
  if (!have_raygen) {
    reportMessage("a raytracing pipeline needs one ray generation shader", ppError);
    return 0;
  }

  /* One metallib for the whole pipeline: every export becomes its own function
   * record inside it (MetallibWriter isolates each export's AIR bitcode), so the
   * runtime loads the library once and creates every function from it. */
  LLVMContext context;
  context.setOpaquePointers(false);
  Module air_module("raytracing.air", context);
  dxmt::initializeModule(air_module);

  std::vector<SM50_RT_FUNCTION_INFO> functions;
  functions.reserve(reflection_count);
  std::vector<std::string> function_names;
  function_names.reserve(reflection_count);
  std::vector<uint32_t> continuations(pDesc->ExportCount, SM50_RT_INDEX_NONE);
  std::vector<uint32_t> any_hits;
  std::vector<uint32_t> intersections;
  // Prepare every live closure once: phase admission cannot be decided from the
  // first export, or from unrelated device64 code elsewhere in its container.
  std::vector<std::array<std::unique_ptr<dxil::ModuleInfo>, 2>> export_modules(pDesc->ExportCount);
  std::vector<SM50_WG_LIBRARY> link_libraries;
  link_libraries.reserve(pDesc->LibraryCount);
  for (uint32_t i = 0; i < pDesc->LibraryCount; ++i)
    link_libraries.push_back({pDesc->Libraries[i].Bytecode, pDesc->Libraries[i].Size});
  SM50_WG_NODE_COMPILE_DESC linkage{};
  linkage.Libraries = link_libraries.data();
  linkage.LibraryCount = pDesc->LibraryCount;
  linkage.Exports = pDesc->LinkExports;
  linkage.ExportCount = pDesc->LinkExportCount;
  bool query_callbacks = (pDesc->Flags & 0x400) != 0;
  bool uses_atomic64 = false;
  for (uint32_t i = 0; i < pDesc->ExportCount; ++i) {
    if (pDesc->Exports[i].ClosestHitIndex != SM50_RT_INDEX_NONE)
      continuations[i] = i;
    const auto &entry = pDesc->Exports[i];
    if (entry.Stage == SM50_RT_STAGE_LIBRARY)
      continue;
    const uint32_t variants = entry.Stage == SM50_RT_STAGE_INTERSECTION &&
      entry.ClosestHitIndex != SM50_RT_INDEX_NONE ? 2 : 1;
    for (uint32_t variant = 0; variant < variants; ++variant) {
      const auto &source = variant ? pDesc->Exports[entry.ClosestHitIndex] : entry;
      const auto &library = pDesc->Libraries[source.LibraryIndex];
      auto parsed = dxil::ModuleInfo::Parse(library.Bytecode, size_t(library.Size), source.ContainerEntryPoint);
      if (!parsed) {
        reportError(parsed.takeError(), ppError);
        return 0;
      }
      export_modules[i][variant] = std::move(*parsed);
      auto &info = *export_modules[i][variant];
      linkage.EntryLibraryIndex = source.LibraryIndex;
      linkage.EntryExportIndex = SM50_RT_INDEX_NONE;
      for (uint32_t function = 0; function < linkage.ExportCount; ++function) {
        const auto &candidate = linkage.Exports[function];
        if (candidate.LibraryIndex != source.LibraryIndex ||
            std::strcmp(candidate.EntryPoint, source.ContainerEntryPoint) != 0)
          continue;
        linkage.EntryExportIndex = function;
        if (std::strcmp(candidate.Name, entry.EntryPoint) == 0)
          break;
      }
      if (auto failure = dxil::LinkLibraryDependencies(info, linkage)) {
        reportError(std::move(failure), ppError);
        return 0;
      }
      dxmt::dxil::IsolateSelectedExport(*info.module, *info.entry);
      const auto stage = dxil::stageFromShaderKind(variant ? SM50_RT_STAGE_CLOSEST_HIT : entry.Stage);
      if (info.stage != dxil::Stage::Library && info.stage != stage) {
        reportMessage("a raytracing export's entry point stage does not match its state object stage", ppError);
        return 0;
      }
      info.stage = stage;
      if (!uses_atomic64)
        uses_atomic64 = dxil::HasDeviceAtomic64(*info.module);
      if (variant || query_callbacks ||
          (entry.Stage != SM50_RT_STAGE_ANY_HIT && entry.Stage != SM50_RT_STAGE_INTERSECTION))
        continue;
      for (const auto &function : *info.module)
        if (!function.isDeclaration())
          for (const auto &block : function)
            for (const auto &instruction : block)
              if (const auto *call = dyn_cast<CallInst>(&instruction))
                if (const auto *callee = call->getCalledFunction())
                  query_callbacks |= callee->getName().startswith("dx.op.allocateRayQuery");
    }
  }
  SM50_SHADER_COMPILATION_ARGUMENT_DATA phase_args{nullptr, SM50_SHADER_D3D12_ATOMIC64_PHASE};
  if (uses_atomic64)
    dxil::InitializeAtomic64Phase(air_module, &phase_args);
  if (query_callbacks) {
    any_hits.resize(pDesc->ExportCount, SM50_RT_INDEX_NONE);
    intersections.resize(pDesc->ExportCount, SM50_RT_INDEX_NONE);
    for (uint32_t i = 0; i < pDesc->ExportCount; ++i) {
      any_hits[i] = pDesc->Exports[i].AnyHitIndex;
      if (pDesc->Exports[i].Stage == SM50_RT_STAGE_INTERSECTION)
        intersections[i] = pDesc->ExportCount + i;
    }
  }

  for (uint32_t i = 0; i < pDesc->ExportCount; ++i) {
    const SM50_RT_EXPORT &group = pDesc->Exports[i];
    if (group.Stage == SM50_RT_STAGE_LIBRARY)
      continue;
    const uint32_t variants = group.Stage == SM50_RT_STAGE_INTERSECTION &&
      group.ClosestHitIndex != SM50_RT_INDEX_NONE ? 2 : 1;
    for (uint32_t variant = 0; variant < variants; ++variant) {
      SM50_RT_EXPORT export_desc = group;
      if (variant) {
        const auto &closest_hit = pDesc->Exports[group.ClosestHitIndex];
        export_desc.Stage = SM50_RT_STAGE_CLOSEST_HIT;
        export_desc.LibraryIndex = closest_hit.LibraryIndex;
        export_desc.ContainerEntryPoint = closest_hit.ContainerEntryPoint;
      }
      dxil::RaytracingExportContext export_context;
      export_context.stage = export_desc.Stage;
      export_context.export_index = i;
      export_context.query_callbacks = query_callbacks;
      export_context.hit_group_any_hits = any_hits.data();
      export_context.hit_group_intersections = intersections.data();
      export_context.dispatch_arguments_slot = SM50_RT_BINDING_INDEX_DISPATCH;
      export_context.intersection_function_table_slot =
        SM50_RT_BINDING_INDEX_INTERSECTION_FUNCTION_TABLE;
      export_context.visible_function_table_slot = SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE;
      export_context.frame_size = sizeof(SM50_RT_FRAME);
      export_context.payload_size = pDesc->MaxPayloadSize;
      export_context.attribute_size = pDesc->MaxAttributeSize;
      export_context.hit_state_size = (uint32_t)sizeof(SM50_RT_HIT_STATE);
      /*
       * The per-candidate region holds the control block *and* one attribute region
       * (kRtCandidateRegionSize), because the traversal lowering and the callback layer
       * address the candidate's attributes right after the control block. Publishing
       * only the control block would make every `dx.op.reportHit` fail its layout check.
       */
      export_context.candidate_size = dxil::kRtCandidateRegionSize(pDesc->MaxAttributeSize);
      /*
       * The hit-group members this export runs as, taken straight from the export
       * description. A hit group is one entry whose *_index fields name its members, and
       * the per-candidate callback layer resolves the group's other members through
       * these: without them a triangle hit group would accept every candidate
       * unconditionally (its any-hit shader would never run, because the shared dispatch
       * callback resolves it from the group's export index) and a procedural hit group
       * would report hits with no any-hit hook at all.
       */
      export_context.closest_hit_index = export_desc.ClosestHitIndex;
      export_context.any_hit_index = export_desc.AnyHitIndex;
      export_context.intersection_index = export_desc.IntersectionIndex;
      export_context.hit_group_continuations = continuations.data();
      export_context.hit_group_continuation_count = continuations.size();
      /* 0 is a legal depth: D3D12 then disallows TraceRay altogether. */
      export_context.max_recursion_depth = pDesc->MaxRecursionDepth;
      export_context.pipeline_flags = pDesc->Flags;
      export_context.global_root_signature = pDesc->GlobalRootSignature;
      export_context.global_root_signature_size = pDesc->GlobalRootSignatureSize;
      export_context.local_root_signature = export_desc.LocalRootSignature;
      export_context.local_root_signature_size = export_desc.LocalRootSignatureSize;
      export_context.is_dispatch_entry = (raygen_indices.size() == 1 && i == raygen_indices[0]);

      /* The AIR function name is the export name: that is what the runtime looks the
       * function up by inside the metallib. */
      const std::string function_name = variant
        ? std::string(export_desc.EntryPoint) + ".closest_hit" : export_desc.EntryPoint;

      dxil::RootSignatureBlob local = dxil::unwrapRootSignature(
        export_desc.LocalRootSignature, export_desc.LocalRootSignatureSize
      );
      dxil::RootSignatureBlob global =
        dxil::unwrapRootSignature(pDesc->GlobalRootSignature, pDesc->GlobalRootSignatureSize);
      const dxil::RootSignatureBlob &rootsig = local.data ? local : global;
      if (!rootsig.data || !rootsig.size) {
        reportMessage("a raytracing pipeline needs a global or local root signature", ppError);
        return 0;
      }

      auto info = std::move(export_modules[i][variant]);

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_data{};
      SM50_SHADER_COMMON_DATA common_data{};
      rootsig_data.type = SM50_SHADER_ROOT_SIGNATURE;
      rootsig_data.bytecode = rootsig.data;
      rootsig_data.bytecode_length = rootsig.size;
      rootsig_data.next = uses_atomic64 ? &phase_args : nullptr;
      common_data.type = SM50_SHADER_COMMON;
      common_data.metal_version = (SM50_SHADER_METAL_VERSION)pDesc->MetalVersion;
      common_data.flags = (SM50_SHADER_FLAG)0;
      common_data.next = &rootsig_data;

      dxbc::SM50ShaderInternal shader;

      auto binding = dxil::LowerRaytracingStage(
        *info, export_context, shader, function_name.c_str(),
        (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common_data
      );
      if (!binding) {
        reportError(binding.takeError(), ppError);
        return 0;
      }
      /*
       * A RaytracingAccelerationStructure is an ordinary root-signature SRV, so the
       * operation pass resolves its descriptor - and therefore the structure's D3D12 GPU
       * virtual address the traversal loads its Metal structure out of - through the same
       * binding map this export was staged with. Without it every shader that names a
       * structure fails as unbound. The map outlives the call: the unique_ptr is held
       * here and LowerResources takes the same object by reference immediately afterwards.
       */
      export_context.binding = binding->get();

      if (auto failure = dxil::LowerRaytracingOperations(*info, export_context, function_name)) {
        reportError(std::move(failure), ppError);
        return 0;
      }

      if (uses_atomic64)
        for (auto &function : *info->module)
          dxil::MarkRaytracingTransport(function);
      if (auto failure = dxil::LowerResources(*info, **binding)) {
        reportError(std::move(failure), ppError);
        return 0;
      }

      if (auto failure = dxil::LowerOperations(*info->module)) {
        reportError(std::move(failure), ppError);
        return 0;
      }

      if (auto failure = dxil::VerifyLoweredModule(*info->module)) {
        reportError(std::move(failure), ppError);
        return 0;
      }
      dxil::StripDXILArtifacts(*info->module);

      if (auto failure = dxil::MergeRaytracingExport(air_module, *info->module, function_name)) {
        reportError(std::move(failure), ppError);
        return 0;
      }

      SM50_RT_FUNCTION_INFO function{};
      function_names.push_back(function_name);
      function.Name = nullptr; /* filled once the vector stops reallocating */
      function.ExportName = export_desc.EntryPoint;
      function.Kind = dxil::functionKindOfStage(export_desc.Stage);
      if (query_callbacks && export_desc.Stage == SM50_RT_STAGE_INTERSECTION)
        function.Kind = SM50_RT_FUNCTION_VISIBLE;
      function.ExportIndex = i;
      /* Export bodies have no hardware IFT slot: shared callbacks select them on GPU. */
      function.TableIndex = query_callbacks && export_desc.Stage == SM50_RT_STAGE_INTERSECTION
        ? pDesc->ExportCount + i + 1 : SM50_RT_INDEX_NONE;
      function.Flags = (export_context.is_dispatch_entry ? SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY : 0u) |
        (uses_atomic64 ? SM50_RT_FUNCTION_FLAG_DEVICE_ATOMIC64_PHASE : 0u);
      functions.push_back(function);
    }
  }

  /*
   * One shared per-candidate dispatch callback for the whole pipeline: it is what runs
   * every hit group's any-hit shader per candidate (see EmitDispatchCallback). It is
   * emitted after the exports so the module it is added to already has the entry kinds
   * the metallib writer looks for, and it is registered in the reflection as a
   * DISPATCH record the runtime installs in the intersection function table.
   */
  {
    dxil::RaytracingExportContext callback_context;
    callback_context.dispatch_arguments_slot = SM50_RT_BINDING_INDEX_DISPATCH;
    callback_context.visible_function_table_slot = SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE;
    callback_context.frame_size = sizeof(SM50_RT_FRAME);
    callback_context.payload_size = pDesc->MaxPayloadSize;
    callback_context.attribute_size = pDesc->MaxAttributeSize;
    callback_context.hit_state_size = (uint32_t)sizeof(SM50_RT_HIT_STATE);
    callback_context.candidate_size = dxil::kRtCandidateRegionSize(pDesc->MaxAttributeSize);

    // Preserve the bound IFT ABI without linking hit shaders into native callbacks
    // when nested queries require candidate traversal in the kernel.
    std::vector<SM50_RT_EXPORT> native_exports;
    SM50_RT_PIPELINE_DESC native_desc = *pDesc;
    if (query_callbacks) {
      native_exports.assign(pDesc->Exports, pDesc->Exports + pDesc->ExportCount);
      for (auto &entry : native_exports) entry.AnyHitIndex = SM50_RT_INDEX_NONE;
      native_desc.Exports = native_exports.data();
    }
    if (auto failure = dxmt::dxil::EmitDispatchCallback(
          air_module, native_desc, callback_context, "dxmt.rt.candidate"
        )) {
      reportError(std::move(failure), ppError);
      return 0;
    }
    /* Geometry kind alone selects the hardware callback slot. */
    SM50_RT_FUNCTION_INFO function{};
    function_names.push_back("dxmt.rt.candidate");
    function.Name = nullptr;
    function.ExportName = nullptr;
    function.Kind = SM50_RT_FUNCTION_DISPATCH;
    function.ExportIndex = SM50_RT_INDEX_NONE;
    function.TableIndex = 0;
    function.Flags = uses_atomic64 ? SM50_RT_FUNCTION_FLAG_DEVICE_ATOMIC64_PHASE : 0u;
    functions.push_back(function);
    if (has_intersection && !query_callbacks) {
      if (auto failure = dxmt::dxil::EmitBoundingBoxDispatch(
            air_module, *pDesc, callback_context, "dxmt.rt.bounding_box")) {
        reportError(std::move(failure), ppError);
        return 0;
      }
      function_names.push_back("dxmt.rt.bounding_box");
      function.TableIndex = 1;
      functions.push_back(function);
    }
  }

  if (raygen_indices.size() > 1) {

    std::vector<std::string> raygen_function_names;
    for (uint32_t idx : raygen_indices)
      raygen_function_names.push_back(pDesc->Exports[idx].EntryPoint);
    if (auto failure = EmitRayGenerationDispatch(
          air_module, raygen_indices, raygen_function_names, "dxmt.rt.raygen_dispatcher"
        )) {
      reportError(std::move(failure), ppError);
      return 0;
    }
    SM50_RT_FUNCTION_INFO function{};
    function_names.push_back("dxmt.rt.raygen_dispatcher");
    function.Name = nullptr;
    function.ExportName = nullptr;
    function.Kind = SM50_RT_FUNCTION_RAYGEN;
    function.ExportIndex = SM50_RT_INDEX_NONE;
    function.TableIndex = SM50_RT_INDEX_NONE;
    function.Flags = SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY |
      (uses_atomic64 ? SM50_RT_FUNCTION_FLAG_DEVICE_ATOMIC64_PHASE : 0u);
    functions.push_back(function);
  }


  if (uses_atomic64) {
    // These functions are emitted after application resource lowering. Mark only
    // their physical transport, never the already-lowered user UAV operations.
    for (const char *name : {"dxmt.rt.candidate", "dxmt.rt.bounding_box", "dxmt.rt.raygen_dispatcher"})
      if (auto *function = air_module.getFunction(name))
        dxil::MarkRaytracingTransport(*function);
    if (auto failure = dxil::LowerAtomic64Phase(air_module)) {
      reportError(std::move(failure), ppError);
      return 0;
    }
  }
  auto compiled = std::make_unique<dxil::SM50CompiledBitcodeInternal>();
  if (uses_atomic64) {
    if (auto failure = dxil::LowerRaytracingImmutableConstants(air_module, *compiled)) {
      reportError(std::move(failure), ppError);
      return 0;
    }
  }
  if (llvm::verifyModule(air_module, &llvm::errs())) {
    llvm::errs() << "OWNED_RT_INVALID_IR_BEGIN\n";
    air_module.print(llvm::errs(), nullptr);
    llvm::errs() << "OWNED_RT_INVALID_IR_END\n";
    reportMessage("invalid merged raytracing AIR module", ppError);
    return 0;
  }

  uint64_t names_size = 0;
  for (const auto &name : function_names)
    names_size += name.size() + 1;
  if (!pDesc->FunctionNames || pDesc->FunctionNamesSize < names_size) {
    reportMessage("raytracing function name storage is too small", ppError);
    return 0;
  }
  char *next_name = pDesc->FunctionNames;
  for (size_t i = 0; i < functions.size(); ++i) {
    const auto &name = function_names[i];
    std::memcpy(next_name, name.c_str(), name.size() + 1);
    functions[i].Name = next_name;
    next_name += name.size() + 1;
  }
  /*
   * The merged module is optimized as a whole, exactly as a raster shader is after its
   * lowering: `runOptimizationPasses`'s first pass is InferAddressSpacesPass(0), which
   * folds the address-space casts `PrepareModuleStorage` left on module-scope globals
   * back onto their new storage space. It has to run before the metallib writer, which
   * serializes what it is given, and on the merged module so one call covers every
   * export.
   */

  dxmt::runOptimizationPasses(air_module);


  raw_svector_ostream stream(compiled->vec);
  metallib::MetallibWriter writer;
  if (auto failure = writer.Write(air_module, stream)) {
    reportError(std::move(failure), ppError);
    return 0;
  }

  *ppBitcode = (sm50_bitcode_t)compiled.release();
  SM50GetCompiledBitcode(*ppBitcode, pMetallib);
  for (size_t i = 0; i < functions.size(); ++i)
    pFunctions[i] = functions[i];
  *pFunctionCount = static_cast<uint32_t>(functions.size());

  return 1;
}
