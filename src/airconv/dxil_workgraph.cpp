/*
 * D3D12 work graph node shaders (DXIL lib_6_8, ShaderKind 15) -> AIR compute kernels.
 *
 * Contract: dxil_workgraph.h. A node kernel is an ordinary Metal compute kernel that
 * processes one batch of the records waiting in its input queue; the runtime scheduler
 * (src/d3d12/d3d12_work_graph.cpp) sizes every batch so the records the batch can
 * write fit the queues they target. Node-specific DXIL operations (238-253) become
 * calls into the d3dalloy.wg.* helpers of shaders/air_workgraph.metal, which address the
 * SM50_WG_NODE_CONTEXT the runtime binds at SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT.
 *
 * Node and record handles are group uniform (spec "Handle value uniformity"), so a
 * record handle is lowered to a single value that every thread of the group computes
 * identically:
 *
 *   input record handle   -> first input record index (queue = the input queue)
 *   group output records  -> first reserved record index of the output's queue; the
 *                            reservation is made by thread 0 of the group and
 *                            broadcast through threadgroup memory
 *   thread output records -> per-thread first record index: one group-wide atomic
 *                            reservation of the per-thread counts summed with a
 *                            threadgroup prefix sum
 *
 * so getNodeRecordPtr(handle, i) is an address in the right queue at index base + i.
 */

#include "dxil_workgraph.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "air_signature.hpp"
#include "airconv_context.hpp"
#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"
#include "dxil_signature.hpp"
#include "nt/air_builder.hpp"

#include "air_workgraph.h"

using namespace llvm;

namespace d3dalloy::dxil {

namespace {
/* Call after unreachable function bodies are removed; llvm.used is not shader resource use. */
void PruneUnusedLibraryResources(ModuleInfo &info) {
  std::erase_if(info.resources, [&](const ResourceBinding &resource) {
    SmallVector<const User *, 8> users;
    SmallPtrSet<const User *, 16> seen;
    if (resource.symbol) for (const User *user : resource.symbol->users()) users.push_back(user);
    while (!users.empty()) {
      const User *user = users.pop_back_val();
      if (!seen.insert(user).second) continue;
      if (isa<Instruction>(user)) return false;
      if (auto *global = dyn_cast<GlobalValue>(user); global && global->getName().startswith("llvm.")) continue;
      for (const User *next : user->users()) users.push_back(next);
    }
    for (Function &function : *info.module) for (BasicBlock &block : function) for (Instruction &instruction : block) {
      auto *call = dyn_cast<CallInst>(&instruction);
      auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee) continue;
      if (callee->getName().startswith("dx.op.createHandleForLib.") && call->arg_size() >= 2) {
        // DXC can strip the resource symbol to undef while retaining its name.
        // Match the actual library handle global exactly as resource lowering
        // does; llvm.used alone must still not keep an unreachable declaration.
        Value *symbol = call->getArgOperand(1);
        if (auto *load = dyn_cast<LoadInst>(symbol)) symbol = load->getPointerOperand();
        else if (auto *gep = dyn_cast<GEPOperator>(symbol)) symbol = gep->getPointerOperand();
        auto *global = dyn_cast<GlobalVariable>(symbol->stripPointerCasts());
        if (global && ((resource.symbol && resource.symbol->stripPointerCasts() == global) ||
            (!resource.name.empty() && global->getName().contains("?" + resource.name + "@@")))) return false;
      } else if (callee->getName().startswith("dx.op.createHandle.") && call->arg_size() >= 3) {
        auto *klass = dyn_cast<ConstantInt>(call->getArgOperand(1)); auto *id = dyn_cast<ConstantInt>(call->getArgOperand(2));
        if (klass && id && klass->getZExtValue() == uint32_t(resource.klass) && id->getZExtValue() == resource.declaration_id) return false;
      } else if (callee->getName().startswith("dx.op.createHandleFromBinding.") && call->arg_size() >= 3) {
        auto *binding = dyn_cast<Constant>(call->getArgOperand(1));
        if (!binding) continue;
        auto *lower = dyn_cast_or_null<ConstantInt>(binding->getAggregateElement(0u));
        auto *space = dyn_cast_or_null<ConstantInt>(binding->getAggregateElement(2u));
        auto *klass = dyn_cast_or_null<ConstantInt>(binding->getAggregateElement(3u));
        if (lower && space && klass && lower->getZExtValue() == resource.lower_bound &&
            space->getZExtValue() == resource.space && klass->getZExtValue() == uint32_t(resource.klass)) return false;
      }
    }
    return true;
  });
}
}

Error LinkLibraryDependencies(ModuleInfo &info, const SM50_WG_NODE_COMPILE_DESC &desc) {
  if (!desc.LibraryCount) return Error::success();
  if (!desc.Libraries || (desc.ExportCount && !desc.Exports) || desc.EntryLibraryIndex >= desc.LibraryCount || !info.entry)
    return createError("DXIL library linkage: incomplete namespace");
  LLVMContext &context = info.module->getContext();
  std::vector<std::unique_ptr<ModuleInfo>> libraries(desc.LibraryCount);
  std::vector<ModuleInfo *> modules(desc.LibraryCount);
  std::vector<std::map<std::string, Function *>> definitions(desc.LibraryCount);
  for (uint32_t i = 0; i < desc.LibraryCount; ++i) {
    if (i == desc.EntryLibraryIndex) modules[i] = &info;
    else {
      const auto &library = desc.Libraries[i];
      auto parsed = ModuleInfo::Parse(library.Bytecode, library.Size, nullptr, &context);
      if (!parsed) return parsed.takeError();
      libraries[i] = std::move(*parsed); modules[i] = libraries[i].get();
    }
    for (Function &function : *modules[i]->module)
      if (!function.isDeclaration()) definitions[i].emplace(function.getName().str(), &function);
  }
  auto exported_symbol = [](const SM50_WG_EXPORT &exp) {
    std::string entry = exp.EntryPoint;
    const auto separator = entry.find("@@");
    const size_t prefix = entry.starts_with("\x01?") ? 2 : (entry.starts_with("?") ? 1 : 0);
    if (prefix && separator != std::string::npos && exp.Name[0] != '\x01' && exp.Name[0] != '?')
      return entry.substr(0, prefix) + exp.Name + entry.substr(separator);
    return std::string(exp.Name);
  };
  uint32_t selected_scope = 0, selected_flags = 0;
  if (desc.EntryExportIndex >= desc.ExportCount) return createError("DXIL library linkage: invalid selected export");
  const auto &selected = desc.Exports[desc.EntryExportIndex];
  if (selected.LibraryIndex != desc.EntryLibraryIndex || !selected.EntryPoint || StringRef(selected.EntryPoint) != info.entry->getName())
    return createError("DXIL library linkage: selected export does not match its definition");
  selected_scope = selected.Scope; selected_flags = selected.Flags;
  for (uint32_t i = 0; i < desc.ExportCount; ++i) {
    const auto &exp = desc.Exports[i];
    if (!exp.Name || !exp.EntryPoint || exp.LibraryIndex >= desc.LibraryCount)
      return createError("DXIL library linkage: invalid export");
  }
  SmallPtrSet<Function *, 32> reachable;
  SmallVector<std::pair<Function *, uint32_t>, 32> worklist;
  auto enqueue = [&](Function *function, uint32_t library) {
    if (reachable.insert(function).second) worklist.emplace_back(function, library);
  };
  enqueue(info.entry, desc.EntryLibraryIndex);
  struct Reference { Function *declaration; Function *definition; };
  std::vector<Reference> references;
  std::vector<bool> used(desc.LibraryCount);
  auto compatible = [](const void *root, uint64_t size, const void *other, uint64_t other_size) {
    if (!other) return true;
    if (!root) return false;
    DXBCChunkView left, right;
    return FindDXBCChunk(root, size, "RTS0", left) && FindDXBCChunk(other, other_size, "RTS0", right) &&
        left.size == right.size && !std::memcmp(left.data, right.data, left.size);
  };
  while (!worklist.empty()) {
    auto [function, library] = worklist.pop_back_val(); used[library] = true;
    uint32_t scope = selected_scope, flags = selected_flags;
    for (uint32_t i = 0; i < desc.ExportCount; ++i) {
      const auto &exp = desc.Exports[i];
      if (exp.LibraryIndex == library && definitions[library].contains(exp.EntryPoint) && definitions[library].at(exp.EntryPoint) == function) {
        scope = exp.Scope; flags = exp.Flags; break;
      }
    }
    if (function != info.entry) {
      const SM50_WG_EXPORT *local_export = nullptr;
      for (uint32_t i = 0; i < desc.ExportCount; ++i) {
        const auto &exp = desc.Exports[i];
        auto definition = definitions[library].find(exp.EntryPoint);
        if (exp.LibraryIndex != library || definition == definitions[library].end() || definition->second != function) continue;
        if ((exp.Reserved & 1u) || exported_symbol(exp) == function->getName()) {
          local_export = &exp;
          if (exp.Reserved & 1u) break;
        }
      }
      if (local_export &&
          (!compatible(selected.GlobalRootSignature, selected.GlobalRootSignatureSize, local_export->GlobalRootSignature, local_export->GlobalRootSignatureSize) ||
           !compatible(selected.LocalRootSignature, selected.LocalRootSignatureSize, local_export->LocalRootSignature, local_export->LocalRootSignatureSize)))
        return createError("DXIL library linkage: incompatible root signatures on a private function dependency");
    }
    for (BasicBlock &block : *function) for (Instruction &instruction : block) {
      auto *call = dyn_cast<CallBase>(&instruction);
      if (!call) continue;
      auto *callee = dyn_cast<Function>(call->getCalledOperand()->stripPointerCastsAndAliases());
      if (!callee) return createError("DXIL library linkage: indirect function call");
      if (callee->isIntrinsic() || callee->getName().startswith("dx.op.")) continue;
      if (!callee->isDeclaration()) { enqueue(callee, library); continue; }
      Function *target = nullptr; uint32_t target_library = 0;
      for (uint32_t i = 0; i < desc.ExportCount; ++i) {
        const auto &exp = desc.Exports[i];
        if ((exp.Reserved & 1u) && scope != exp.Scope) continue;
        if (callee->getName() != exported_symbol(exp) && callee->getName() != exp.Name) continue;
        auto definition = definitions[exp.LibraryIndex].find(exp.EntryPoint);
        if (definition == definitions[exp.LibraryIndex].end()) continue;
        // The typed HLSL symbol is the overload contract. Independently parsed
        // libraries may have equivalent identified struct types with different
        // LLVM identities; the linker remaps those types when merging modules.
        const auto name = callee->getName();
        const bool typed = (name.startswith("\x01?") || name.startswith("?")) && name.contains("@@");
        if (!typed && definition->second->getFunctionType() != callee->getFunctionType()) continue;
        if (!compatible(selected.GlobalRootSignature, selected.GlobalRootSignatureSize, exp.GlobalRootSignature, exp.GlobalRootSignatureSize) ||
            !compatible(selected.LocalRootSignature, selected.LocalRootSignatureSize, exp.LocalRootSignature, exp.LocalRootSignatureSize))
          return createError("DXIL library linkage: incompatible root signatures on a function dependency");
        if (scope != exp.Scope && ((scope && !(flags & 1u)) || (exp.Scope && !(exp.Flags & 2u))))
          return createError("DXIL library linkage: collection dependency flags do not permit external reference");
        if (target && target != definition->second) return createError("DXIL library linkage: ambiguous function export");
        target = definition->second; target_library = exp.LibraryIndex;
      }
      if (!target) {
        if (desc.AllowUnresolved) continue;
        return createError(Twine("DXIL library linkage: unresolved function '") + callee->getName() + "'");
      }
      references.push_back({callee, target}); enqueue(target, target_library);
    }
  }
  if (desc.AllowUnresolved) return Error::success();
  /* Every definition/global is library-private until a call resolves it through the public namespace. */
  std::vector<ResourceBinding> resources;
  std::vector<std::string> symbols;
  uint32_t next_id[4]{};
  for (uint32_t i = 0; i < desc.LibraryCount; ++i) {
    if (!used[i]) continue;
    auto &source = *modules[i];
    const std::string prefix = "d3dalloy.lib." + std::to_string(i) + ".";
    for (auto &[name, function] : definitions[i]) function->setName(prefix + name);
    for (GlobalVariable &global : source.module->globals())
      if (!global.getName().startswith("llvm.")) global.setName(prefix + global.getName());
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ids;
    for (auto resource : source.resources) {
      const uint32_t klass = uint32_t(resource.klass);
      ids[{klass, resource.declaration_id}] = next_id[klass]; resource.declaration_id = next_id[klass]++;
      auto *symbol = dyn_cast_or_null<GlobalValue>(resource.symbol ? resource.symbol->stripPointerCasts() : nullptr);
      symbols.push_back(symbol ? symbol->getName().str() : ""); resources.push_back(std::move(resource));
    }
    for (Function &function : *source.module) {
      if (!reachable.contains(&function)) { if (!function.isDeclaration()) function.deleteBody(); continue; }
      for (BasicBlock &block : function) for (Instruction &instruction : block) {
        auto *call = dyn_cast<CallInst>(&instruction);
        auto *callee = call ? call->getCalledFunction() : nullptr;
        if (!callee || !callee->getName().startswith("dx.op.createHandle.") || call->arg_size() < 3) continue;
        auto *klass = dyn_cast<ConstantInt>(call->getArgOperand(1)); auto *id = dyn_cast<ConstantInt>(call->getArgOperand(2));
        if (!klass || !id || !ids.contains({klass->getZExtValue(), id->getZExtValue()}))
          return createError("DXIL library linkage: invalid resource range ID");
        call->setArgOperand(2, ConstantInt::get(id->getType(), ids.at({klass->getZExtValue(), id->getZExtValue()})));
      }
    }
    for (const char *name : {"dx.entryPoints", "dx.resources", "dx.shaderModel", "dx.typeAnnotations"})
      if (auto *md = source.module->getNamedMetadata(name)) source.module->eraseNamedMetadata(md);
  }
  for (const auto &reference : references) reference.declaration->setName(reference.definition->getName());
  for (uint32_t i = 0; i < desc.LibraryCount; ++i)
    if (i != desc.EntryLibraryIndex && used[i] && Linker::linkModules(*info.module, std::move(libraries[i]->module)))
      return createError("DXIL library linkage: cannot merge dependency");
  info.resources.clear();
  for (size_t i = 0; i < resources.size(); ++i) {
    resources[i].symbol = symbols[i].empty() ? nullptr : info.module->getNamedValue(symbols[i]);
    info.resources.push_back(std::move(resources[i]));
  }
  /* Materialize helpers in the entry so resource/record operations use its actual argument bindings. */
  while (true) {
    CallBase *candidate = nullptr;
    for (BasicBlock &block : *info.entry) for (Instruction &instruction : block)
      if (auto *call = dyn_cast<CallBase>(&instruction))
        if (auto *callee = call->getCalledFunction(); callee && !callee->isDeclaration() && !callee->isIntrinsic()) candidate = call;
    if (!candidate) break;
    InlineFunctionInfo inline_info;
    auto result = InlineFunction(*candidate, inline_info);
    if (!result.isSuccess()) return createError(Twine("DXIL library linkage: cannot inline helper: ") + result.getFailureReason());
  }
  for (Function &function : *info.module)
    if (&function != info.entry && !function.isDeclaration()) function.deleteBody();
  PruneUnusedLibraryResources(info);
  return Error::success();
}

namespace {

/* ------------------------------------------------------------------------- */
/* Metadata                                                                  */
/* ------------------------------------------------------------------------- */

constexpr uint32_t kShaderKindNode = 15;
/* !dx.entryPoints property tags of a node shader (DxilConstants.h). */
constexpr uint32_t kTagNumThreads = 4;
constexpr uint32_t kTagShaderKind = 8;
constexpr uint32_t kTagNodeLaunch = 13;
constexpr uint32_t kTagIsProgramEntry = 14;
constexpr uint32_t kTagNodeId = 15;
constexpr uint32_t kTagLocalRootIndex = 16;
constexpr uint32_t kTagShareInputOf = 17;
constexpr uint32_t kTagDispatchGrid = 18;
constexpr uint32_t kTagMaxRecursionDepth = 19;
constexpr uint32_t kTagInputs = 20;
constexpr uint32_t kTagOutputs = 21;
constexpr uint32_t kTagMaxDispatchGrid = 22;
/* Node IO tags. */
constexpr uint32_t kIoOutputId = 0;
constexpr uint32_t kIoFlags = 1;
constexpr uint32_t kIoRecordType = 2;
constexpr uint32_t kIoMaxRecords = 3;
constexpr uint32_t kIoMaxRecordsSharedWith = 4;
constexpr uint32_t kIoArraySize = 5;
constexpr uint32_t kIoAllowSparse = 6;

Metadata *operandOf(MDNode *node, unsigned i) { return node && i < node->getNumOperands() ? node->getOperand(i).get() : nullptr; }

bool readU32(Metadata *md, uint32_t &value) {
  auto *v = dyn_cast_or_null<ConstantAsMetadata>(md);
  auto *c = v ? dyn_cast<ConstantInt>(v->getValue()) : nullptr;
  if (!c)
    return false;
  value = static_cast<uint32_t>(c->getZExtValue());
  return true;
}

uint32_t u32Or(MDNode *node, unsigned i, uint32_t fallback) {
  uint32_t value;
  return readU32(operandOf(node, i), value) ? value : fallback;
}

struct NodeId {
  std::string name;
  uint32_t index = 0;
};

bool readNodeId(Metadata *md, NodeId &id) {
  auto *node = dyn_cast_or_null<MDNode>(md);
  auto *name = node ? dyn_cast_or_null<MDString>(operandOf(node, 0)) : nullptr;
  if (!name)
    return false;
  id.name = name->getString().str();
  id.index = u32Or(node, 1, 0);
  return true;
}

/** Tag/value property list lookup. */
Metadata *property(MDNode *properties, uint32_t tag) {
  if (!properties)
    return nullptr;
  for (unsigned i = 0; i + 1 < properties->getNumOperands(); i += 2) {
    uint32_t t;
    if (readU32(operandOf(properties, i), t) && t == tag)
      return operandOf(properties, i + 1);
  }
  return nullptr;
}

struct NodeIo {
  SM50_WG_NODE_IO io{};
  NodeId target;
};

NodeIo parseIo(MDNode *list, bool output) {
  NodeIo result;
  auto &io = result.io;
  io.RecordAlignment = 1;
  io.MaxRecords = output ? 0 : 1;
  io.MaxRecordsSharedWith = SM50_WG_NONE;
  io.ArraySize = 1;
  if (Metadata *flags = property(list, kIoFlags))
    readU32(flags, io.Flags);
  if (auto *type = dyn_cast_or_null<MDNode>(property(list, kIoRecordType))) {
    readU32(property(type, 0), io.RecordSize);
    if (auto *grid = dyn_cast_or_null<MDNode>(property(type, 1))) {
      io.DispatchGridOffset = u32Or(grid, 0, 0);
      io.DispatchGridComponentType = u32Or(grid, 1, 0);
      io.DispatchGridComponents = u32Or(grid, 2, 0);
    }
    uint32_t alignment = 0;
    if (readU32(property(type, 2), alignment) && alignment)
      io.RecordAlignment = alignment;
  }
  uint32_t value;
  if (readU32(property(list, kIoMaxRecords), value))
    io.MaxRecords = value;
  if (output && readU32(property(list, kIoMaxRecordsSharedWith), value))
    io.MaxRecordsSharedWith = value;
  if (readU32(property(list, kIoArraySize), value))
    io.ArraySize = value;
  if (readU32(property(list, kIoAllowSparse), value))
    io.AllowSparseNodes = value;
  if (output)
    readNodeId(property(list, kIoOutputId), result.target);
  return result;
}

struct NodeReflection {
  SM50_WG_NODE_INFO info{};
  std::string entry;
  std::string public_name;
  NodeId id;
  NodeId share_input;
  std::vector<NodeIo> outputs;
};

bool readGrid(Metadata *md, uint32_t (&grid)[3]) {
  auto *node = dyn_cast_or_null<MDNode>(md);
  if (!node)
    return false;
  for (unsigned i = 0; i < 3; ++i)
    grid[i] = u32Or(node, i, 1);
  return true;
}

/** Reflect one `!dx.entryPoints` entry; false when it is not a node shader. */
bool reflectEntry(MDNode *entry, NodeReflection &out) {
  auto *name = dyn_cast_or_null<MDString>(operandOf(entry, 1));
  auto *properties = dyn_cast_or_null<MDNode>(operandOf(entry, 4));
  uint32_t kind = ~0u;
  if (!name || !properties || !readU32(property(properties, kTagShaderKind), kind) || kind != kShaderKindNode)
    return false;
  auto &info = out.info;
  out.entry = name->getString().str();
  info.Launch = SM50_WG_LAUNCH_BROADCASTING;
  readU32(property(properties, kTagNodeLaunch), info.Launch);
  readU32(property(properties, kTagIsProgramEntry), info.IsProgramEntry);
  if (!readNodeId(property(properties, kTagNodeId), out.id))
    out.id = {out.entry, 0};
  info.LocalRootArgumentsTableIndex = SM50_WG_NONE;
  readU32(property(properties, kTagLocalRootIndex), info.LocalRootArgumentsTableIndex);
  info.HasShareInputOf = readNodeId(property(properties, kTagShareInputOf), out.share_input);
  info.NumThreads[0] = info.NumThreads[1] = info.NumThreads[2] = 1;
  readGrid(property(properties, kTagNumThreads), info.NumThreads);
  info.HasFixedDispatchGrid = readGrid(property(properties, kTagDispatchGrid), info.DispatchGrid);
  if (!info.HasFixedDispatchGrid)
    info.DispatchGrid[0] = info.DispatchGrid[1] = info.DispatchGrid[2] = 0;
  if (!readGrid(property(properties, kTagMaxDispatchGrid), info.MaxDispatchGrid))
    info.MaxDispatchGrid[0] = info.MaxDispatchGrid[1] = info.MaxDispatchGrid[2] = 0;
  readU32(property(properties, kTagMaxRecursionDepth), info.MaxRecursionDepth);
  if (auto *inputs = dyn_cast_or_null<MDNode>(property(properties, kTagInputs))) {
    if (inputs->getNumOperands()) {
      info.HasInput = 1;
      info.Input = parseIo(dyn_cast_or_null<MDNode>(operandOf(inputs, 0)), false).io;
    }
  }
  if (auto *outputs = dyn_cast_or_null<MDNode>(property(properties, kTagOutputs)))
    for (unsigned i = 0; i < outputs->getNumOperands(); ++i)
      out.outputs.push_back(parseIo(dyn_cast_or_null<MDNode>(operandOf(outputs, i)), true));
  info.OutputCount = static_cast<uint32_t>(out.outputs.size());
  return true;
}

Expected<std::vector<NodeReflection>> reflectLibrary(const void *bytecode, uint64_t size) {
  auto program = FindDXILProgram(bytecode, static_cast<size_t>(size));
  if (!program)
    return program.takeError();
  LLVMContext context;
  context.setOpaquePointers(false);
  auto buffer = MemoryBuffer::getMemBuffer(
    StringRef(reinterpret_cast<const char *>(program->bitcode), program->bitcode_size), "dxil-library", false
  );
  auto module = parseBitcodeFile(buffer->getMemBufferRef(), context);
  if (!module) {
    std::string message;
    raw_string_ostream stream(message);
    handleAllErrors(module.takeError(), [&](const ErrorInfoBase &base) { base.log(stream); });
    return createError("failed to parse DXIL library bitcode: " + stream.str());
  }
  std::vector<NodeReflection> nodes;
  SmallPtrSet<Function *, 16> shaders;
  std::map<std::string, std::string> public_names;
  ReadLibraryFunctionNames(bytecode, size, public_names);
  if (NamedMDNode *entries = (*module)->getNamedMetadata("dx.entryPoints"))
    for (unsigned i = 0; i < entries->getNumOperands(); ++i) {
      NodeReflection node;
      auto *entry = entries->getOperand(i);
      uint32_t kind = 6;
      readU32(property(dyn_cast_or_null<MDNode>(operandOf(entry, 4)), kTagShaderKind), kind);
      if (kind != 6) if (auto *value = dyn_cast_or_null<ValueAsMetadata>(operandOf(entry, 0)))
        if (auto *function = dyn_cast<Function>(value->getValue())) shaders.insert(function);
      if (reflectEntry(entry, node)) {
        auto name = public_names.find(node.entry);
        node.public_name = name == public_names.end() ? node.entry : name->second;
        nodes.push_back(std::move(node));
      }
    }
  for (const auto &[entry, name] : public_names) {
    auto *function = (*module)->getFunction(entry);
    if (!function || function->isDeclaration() || shaders.contains(function)) continue;
    NodeReflection node; node.entry = entry; node.public_name = name;
    nodes.push_back(std::move(node));
  }
  return nodes;
}

/* ------------------------------------------------------------------------- */
/* Lowering                                                                  */
/* ------------------------------------------------------------------------- */

enum : uint32_t {
  OpThreadId = 93,
  OpGroupId = 94,
  OpThreadIdInGroup = 95,
  OpFlattenedThreadIdInGroup = 96,
  OpAllocateNodeOutputRecords = 238,
  OpGetNodeRecordPtr = 239,
  OpIncrementOutputCount = 240,
  OpOutputComplete = 241,
  OpGetInputRecordCount = 242,
  OpFinishedCrossGroupSharing = 243,
  OpBarrierByMemoryType = 244,
  OpBarrierByNodeRecordHandle = 246,
  OpCreateNodeOutputHandle = 247,
  OpIndexNodeHandle = 248,
  OpAnnotateNodeHandle = 249,
  OpCreateNodeInputRecordHandle = 250,
  OpAnnotateNodeRecordHandle = 251,
  OpNodeOutputIsValid = 252,
  OpGetRemainingRecursionLevels = 253,
};

constexpr uint32_t kQueueInput = 0xfffffffeu;
/* SM50_WG_NODE_CONTEXT dword offsets used directly by the lowering. */
constexpr uint32_t kCtxBatchStart = offsetof(SM50_WG_NODE_CONTEXT, BatchStart) / 4;
constexpr uint32_t kCtxBatchRecords = offsetof(SM50_WG_NODE_CONTEXT, BatchRecords) / 4;
constexpr uint32_t kCtxRemainingRecursion = offsetof(SM50_WG_NODE_CONTEXT, RemainingRecursionLevels) / 4;
constexpr uint32_t kCtxLocalRoot = offsetof(SM50_WG_NODE_CONTEXT, LocalRootArguments) / 4;
static_assert(offsetof(SM50_WG_NODE_CONTEXT, QueueTable) == 0);
static_assert(offsetof(SM50_WG_NODE_CONTEXT, BatchTable) == 8);
static_assert(offsetof(SM50_WG_NODE_CONTEXT, InputQueue) == 24);
static_assert(offsetof(SM50_WG_NODE_CONTEXT, BatchRecords) == 32);
static_assert(offsetof(SM50_WG_NODE_CONTEXT, GroupStart) == 40);
static_assert(offsetof(SM50_WG_NODE_CONTEXT, OutputElementCount) == 48);
static_assert(offsetof(SM50_WG_NODE_CONTEXT, Outputs) == 56);
static_assert(sizeof(SM50_WG_QUEUE) == 32);
static_assert(sizeof(SM50_WG_BATCH_RECORD) == 16);

uint32_t opcodeOf(const CallInst &call) {
  if (!call.getCalledFunction() || !call.getCalledFunction()->getName().startswith("dx.op.") || call.arg_empty())
    return ~0u;
  auto *c = dyn_cast<ConstantInt>(call.getArgOperand(0));
  return c ? static_cast<uint32_t>(c->getZExtValue()) : ~0u;
}

Expected<uint32_t> constantArg(const CallInst &call, unsigned i, StringRef what) {
  auto *c = i < call.arg_size() ? dyn_cast<ConstantInt>(call.getArgOperand(i)) : nullptr;
  if (!c)
    return createError(Twine("dxil: ") + what + " must be a constant");
  return static_cast<uint32_t>(c->getZExtValue());
}

/* A lowered node handle: the output element index (i32) it designates. */
struct NodeHandle {
  uint32_t output = 0;
  Value *element = nullptr; /* i32: OutputElementBase[output] + array index */
  Value *array_index = nullptr;
};

/* A lowered record handle: queue (i32, kQueueInput for the input) and first index (i32). */
struct RecordHandle {
  Value *queue = nullptr;
  Value *base = nullptr;
  bool input = false;
};

class NodeLowering {
public:
  NodeLowering(
    Module &module, Function &function, const SM50_WG_NODE_COMPILE_DESC &desc, const NodeReflection &node,
    Value *context, Value *thread_index_flat
  )
      : module(module), function(function), desc(desc), node(node), context(context),
        thread_flat(thread_index_flat),
        i32(Type::getInt32Ty(module.getContext())), i64(Type::getInt64Ty(module.getContext())) {}

  Error run();
  bool usesHelpers() const { return uses_helpers; }

  /* The batch-relative input record index this group / thread handles (set by the caller's prologue). */
  Value *input_first = nullptr;
  Value *input_count = nullptr;

private:
  Module &module;
  Function &function;
  const SM50_WG_NODE_COMPILE_DESC &desc;
  const NodeReflection &node;
  Value *context;
  Value *thread_flat;
  Type *i32;
  Type *i64;
  std::map<Value *, NodeHandle> nodes;
  std::map<Value *, RecordHandle> records;
  GlobalVariable *scratch = nullptr;
  bool uses_helpers = false;

  FunctionCallee helper(StringRef name, Type *result, ArrayRef<Type *> params) {
    uses_helpers = true;
    return module.getOrInsertFunction(name, FunctionType::get(result, params, false));
  }
  Value *contextWord(IRBuilder<> &b, uint32_t dword) {
    auto *pointer = b.CreateConstInBoundsGEP1_32(i32, context, dword);
    return b.CreateLoad(i32, pointer);
  }
  Value *contextQword(IRBuilder<> &b, uint32_t dword) {
    auto *low = b.CreateZExt(contextWord(b, dword), i64);
    auto *high = b.CreateZExt(contextWord(b, dword + 1), i64);
    return b.CreateOr(low, b.CreateShl(high, 32));
  }
  Value *outputQueue(IRBuilder<> &b, Value *element) {
    return b.CreateCall(helper("d3dalloy.wg.output_queue", i32, {context->getType(), i32}), {context, element});
  }
  Value *reserve(IRBuilder<> &b, Value *queue, Value *count) {
    return b.CreateCall(helper("d3dalloy.wg.reserve", i32, {context->getType(), i32, i32}), {context, queue, count});
  }
  Value *recordAddress(IRBuilder<> &b, Value *queue, Value *index) {
    return b.CreateCall(
      helper("d3dalloy.wg.record_address", i64, {context->getType(), i32, i32}), {context, queue, index}
    );
  }
  GlobalVariable *groupScratch() {
    /* One word broadcasts a group reservation or sharing-lifetime result. */
    if (!scratch) {
      auto *type = ArrayType::get(i32, 1);
      scratch = new GlobalVariable(
        module, type, false, GlobalValue::InternalLinkage, UndefValue::get(type), "d3dalloy.wg.scratch", nullptr,
        GlobalValue::NotThreadLocal, 3
      );
      scratch->setAlignment(Align(4));
    }
    return scratch;
  }
  void groupBarrier(IRBuilder<> &b) {
    llvm::air::AIRBuilder air(b, nulls());
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  }
  bool threadLaunch() const { return desc.Launch == SM50_WG_LAUNCH_THREAD; }

  Expected<NodeHandle> nodeHandle(Value *value);
  Expected<RecordHandle> recordHandle(Value *value);
  Error lowerCall(CallInst &call, uint32_t opcode);
  Expected<Value *> groupReserve(IRBuilder<> &b, Value *queue, Value *count);
};

Expected<NodeHandle> NodeLowering::nodeHandle(Value *value) {
  auto found = nodes.find(value);
  if (found == nodes.end())
    return createError("dxil: node handle does not come from createNodeOutputHandle");
  return found->second;
}

Expected<RecordHandle> NodeLowering::recordHandle(Value *value) {
  auto found = records.find(value);
  if (found == records.end())
    return createError("dxil: node record handle does not come from a node record operation");
  return found->second;
}

/* Thread 0 reserves `count` (group uniform) records; every thread reads the first index. */
Expected<Value *> NodeLowering::groupReserve(IRBuilder<> &b, Value *queue, Value *count) {
  if (threadLaunch())
    return reserve(b, queue, count); /* one thread per group of the D3D program model */
  auto *slot = b.CreateConstInBoundsGEP2_32(groupScratch()->getValueType(), groupScratch(), 0, 0);
  auto *first_thread = b.CreateICmpEQ(thread_flat, b.getInt32(0));
  auto *block = b.GetInsertBlock();
  auto *rest = block->splitBasicBlock(b.GetInsertPoint(), "wg.reserved");
  auto *then = BasicBlock::Create(module.getContext(), "wg.reserve", &function, rest);
  block->getTerminator()->eraseFromParent();
  b.SetInsertPoint(block);
  b.CreateCondBr(first_thread, then, rest);
  b.SetInsertPoint(then);
  b.CreateStore(reserve(b, queue, count), slot);
  b.CreateBr(rest);
  b.SetInsertPoint(&*rest->getFirstInsertionPt());
  groupBarrier(b);
  auto *value = b.CreateLoad(i32, slot);
  /* Keep the slot stable until every thread has read it (a later reservation reuses it). */
  groupBarrier(b);
  return value;
}

Error NodeLowering::lowerCall(CallInst &call, uint32_t opcode) {
  IRBuilder<> b(&call);
  switch (opcode) {
  case OpCreateNodeOutputHandle: {
    auto index = constantArg(call, 1, "createNodeOutputHandle index");
    if (!index)
      return index.takeError();
    if (*index >= desc.OutputCount)
      return createError("dxil: createNodeOutputHandle index is out of range");
    NodeHandle handle;
    handle.output = *index;
    handle.array_index = b.getInt32(0);
    handle.element = b.getInt32(desc.OutputElementBase[*index]);
    nodes[&call] = handle;
    return Error::success();
  }
  case OpAnnotateNodeHandle: {
    auto handle = nodeHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    nodes[&call] = *handle;
    return Error::success();
  }
  case OpIndexNodeHandle: {
    auto handle = nodeHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    NodeHandle indexed = *handle;
    indexed.array_index = b.CreateAdd(handle->array_index, call.getArgOperand(2));
    indexed.element = b.CreateAdd(b.getInt32(desc.OutputElementBase[handle->output]), indexed.array_index);
    nodes[&call] = indexed;
    return Error::success();
  }
  case OpNodeOutputIsValid: {
    auto handle = nodeHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    /* Past the array (or a sparse hole / the recursion leaf): the runtime maps it to SM50_WG_NONE. */
    auto *in_range = b.CreateICmpULT(handle->array_index, b.getInt32(desc.OutputElementCount[handle->output]));
    auto *queue = outputQueue(b, b.CreateSelect(in_range, handle->element, b.getInt32(0xffffffffu)));
    call.replaceAllUsesWith(b.CreateICmpNE(queue, b.getInt32(0xffffffffu)));
    return Error::success();
  }
  case OpCreateNodeInputRecordHandle: {
    RecordHandle handle;
    handle.input = true;
    handle.queue = b.getInt32(kQueueInput);
    handle.base = input_first;
    records[&call] = handle;
    return Error::success();
  }
  case OpAnnotateNodeRecordHandle: {
    auto handle = recordHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    records[&call] = *handle;
    return Error::success();
  }
  case OpAllocateNodeOutputRecords: {
    auto handle = nodeHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    auto per_thread = constantArg(call, 3, "allocateNodeOutputRecords perThread");
    if (!per_thread)
      return per_thread.takeError();
    auto *in_range = b.CreateICmpULT(handle->array_index, b.getInt32(desc.OutputElementCount[handle->output]));
    auto *queue = outputQueue(b, b.CreateSelect(in_range, handle->element, b.getInt32(0xffffffffu)));
    Value *count = call.getArgOperand(2);
    /* A count against an absent node is ignored (the thread did not reference it). */
    count = b.CreateSelect(b.CreateICmpEQ(queue, b.getInt32(0xffffffffu)), b.getInt32(0), count);
    auto first = *per_thread ? Expected<Value *>(reserve(b, queue, count)) : groupReserve(b, queue, count);
    if (!first)
      return first.takeError();
    RecordHandle record;
    record.queue = queue;
    record.base = *first;
    records[&call] = record;
    return Error::success();
  }
  case OpGetNodeRecordPtr: {
    auto handle = recordHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    const bool coherent = handle->input && (node.info.Input.Flags & SM50_WG_IO_READ_WRITE) &&
                          !module.getModuleFlag(kAtomic64PhaseFlag);
    auto *index = b.CreateAdd(handle->base, call.getArgOperand(2));
    auto *address = recordAddress(b, handle->queue, index);
    auto *type = cast<PointerType>(call.getType());
    /* Records live in device memory (AIR address space 1). */
    auto *pointer = b.CreateIntToPtr(address, type->getNonOpaquePointerElementType()->getPointerTo(1));
    /* Replace the addrspace(6) uses by rewriting the pointer's users onto address space 1. */
    SmallVector<std::pair<Value *, Value *>, 8> work{{&call, pointer}};
    while (!work.empty()) {
      auto [old_value, new_value] = work.pop_back_val();
      for (User *user : make_early_inc_range(old_value->users())) {
        auto *inst = cast<Instruction>(user);
        if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
          IRBuilder<> g(gep);
          SmallVector<Value *, 4> indices(gep->indices());
          auto *replacement = g.CreateGEP(gep->getSourceElementType(), new_value, indices);
          if (auto *new_gep = dyn_cast<GetElementPtrInst>(replacement))
            new_gep->setIsInBounds(gep->isInBounds());
          work.push_back({gep, replacement});
        } else if (auto *cast_inst = dyn_cast<BitCastInst>(inst)) {
          IRBuilder<> g(cast_inst);
          auto *target = cast<PointerType>(cast_inst->getType());
          work.push_back({cast_inst, g.CreateBitCast(new_value, target->getNonOpaquePointerElementType()->getPointerTo(1))});
        } else if (auto *load = dyn_cast<LoadInst>(inst)) {
          if (coherent && !load->getType()->isAggregateType()) {
            IRBuilder<> g(load);
            llvm::air::AIRBuilder air(g, nulls());
            auto *coherent_load = air.CreateDeviceCoherentLoad(load->getType(), new_value);
            load->replaceAllUsesWith(coherent_load);
            load->eraseFromParent();
          } else {
            load->setOperand(load->getPointerOperandIndex(), new_value);
          }
          continue;
        } else if (auto *store = dyn_cast<StoreInst>(inst)) {
          if (store->getValueOperand() == old_value)
            return createError("dxil: a node record pointer is stored to memory");
          if (coherent && !store->getValueOperand()->getType()->isAggregateType()) {
            IRBuilder<> g(store);
            llvm::air::AIRBuilder air(g, nulls());
            air.CreateDeviceCoherentStore(store->getValueOperand(), new_value);
            store->eraseFromParent();
          } else {
            store->setOperand(store->getPointerOperandIndex(), new_value);
          }
          continue;
        } else if (auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
          if (rmw->getValOperand()->getType()->isIntegerTy(64) && module.getModuleFlag(kAtomic64PhaseFlag)) {
            // Record payloads participate in the same canonical device cells as
            // UAVs; the runtime registers the actual queue/CPU-seed payload span.
            rmw->setOperand(rmw->getPointerOperandIndex(), new_value);
            continue;
          }
          /* Device-scope AIR atomics (air.atomic.global.*), as for UAV atomics. */
          IRBuilder<> g(rmw);
          llvm::air::AIRBuilder air(g, nulls());
          auto *result = rmw->getValOperand()->getType()->isIntegerTy(32)
                           ? air.CreateAtomicRMW(rmw->getOperation(), new_value, rmw->getValOperand())
                           : nullptr;
          if (!result)
            return createError("dxil: unsupported atomic on a node record");
          rmw->replaceAllUsesWith(result);
          rmw->eraseFromParent();
          continue;
        } else if (auto *exchange = dyn_cast<AtomicCmpXchgInst>(inst)) {
          exchange->setOperand(exchange->getPointerOperandIndex(), new_value);
          if (!module.getModuleFlag(kAtomic64PhaseFlag)) {
            exchange->setSuccessOrdering(AtomicOrdering::Monotonic);
            exchange->setFailureOrdering(AtomicOrdering::Monotonic);
          }
          continue;
        } else {
          std::string text;
          raw_string_ostream stream(text);
          inst->print(stream);
          return createError("dxil: unsupported use of a node record pointer: " + stream.str());
        }
      }
      if (auto *inst = dyn_cast<Instruction>(old_value); inst && inst != &call && inst->use_empty())
        inst->eraseFromParent();
    }
    return Error::success();
  }
  case OpIncrementOutputCount: {
    auto handle = nodeHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    auto per_thread = constantArg(call, 3, "incrementOutputCount perThread");
    if (!per_thread)
      return per_thread.takeError();
    auto *in_range = b.CreateICmpULT(handle->array_index, b.getInt32(desc.OutputElementCount[handle->output]));
    auto *queue = outputQueue(b, b.CreateSelect(in_range, handle->element, b.getInt32(0xffffffffu)));
    Value *count = b.CreateSelect(b.CreateICmpEQ(queue, b.getInt32(0xffffffffu)), b.getInt32(0), call.getArgOperand(2));
    /* Empty records only exist as a count: reserving them is the whole operation. */
    // Per-thread emission permits divergent control flow: never synchronize inactive lanes.
    auto first = *per_thread ? Expected<Value *>(reserve(b, queue, count)) : groupReserve(b, queue, count);
    if (!first)
      return first.takeError();
    return Error::success();
  }
  case OpOutputComplete: {
    /* Records become visible to consumers at the end of the producing dispatch, which the
     * scheduler orders with a memory barrier; nothing has to happen per call. */
    if (auto handle = recordHandle(call.getArgOperand(1)); !handle)
      return handle.takeError();
    return Error::success();
  }
  case OpGetInputRecordCount: {
    if (auto handle = recordHandle(call.getArgOperand(1)); !handle)
      return handle.takeError();
    call.replaceAllUsesWith(input_count);
    return Error::success();
  }
  case OpFinishedCrossGroupSharing: {
    auto handle = recordHandle(call.getArgOperand(1));
    if (!handle)
      return handle.takeError();
    if (desc.Launch != SM50_WG_LAUNCH_BROADCASTING || !handle->input)
      return createError("dxil: FinishedCrossGroupSharing outside a broadcasting node input");
    /* Thread 0 of each group decrements the record's counter once; every thread reads the result. */
    auto *slot = b.CreateConstInBoundsGEP2_32(groupScratch()->getValueType(), groupScratch(), 0, 0);
    auto *block = b.GetInsertBlock();
    auto *rest = block->splitBasicBlock(b.GetInsertPoint(), "wg.shared");
    auto *then = BasicBlock::Create(module.getContext(), "wg.share", &function, rest);
    block->getTerminator()->eraseFromParent();
    b.SetInsertPoint(block);
    b.CreateCondBr(b.CreateICmpEQ(thread_flat, b.getInt32(0)), then, rest);
    b.SetInsertPoint(then);
    {
      /* Order this group's input writes before the decrement others may observe. */
      llvm::air::AIRBuilder air(b, nulls());
      air.CreateAtomicFence(llvm::air::MemFlags::Device, llvm::air::ThreadScope::Device);
    }
    auto *last = b.CreateCall(
      helper("d3dalloy.wg.finish_sharing", i32, {context->getType(), i32}), {context, handle->base}
    );
    b.CreateStore(last, slot);
    {
      llvm::air::AIRBuilder air(b, nulls());
      air.CreateAtomicFence(llvm::air::MemFlags::Device, llvm::air::ThreadScope::Device);
    }
    b.CreateBr(rest);
    b.SetInsertPoint(&*rest->getFirstInsertionPt());
    groupBarrier(b);
    auto *value = b.CreateICmpNE(b.CreateLoad(i32, slot), b.getInt32(0));
    groupBarrier(b);
    call.replaceAllUsesWith(value);
    return Error::success();
  }
  case OpGetRemainingRecursionLevels:
    call.replaceAllUsesWith(contextWord(b, kCtxRemainingRecursion));
    return Error::success();
  case OpBarrierByMemoryType: {
    auto types = constantArg(call, 1, "barrierByMemoryType memory types");
    auto semantics = constantArg(call, 2, "barrierByMemoryType semantics");
    if (!types)
      return types.takeError();
    if (!semantics)
      return semantics.takeError();
    if (*types & ~0xfu || *semantics & ~0xfu)
      return createError("dxil: barrierByMemoryType has unknown flag bits");
    /* Node input/output records are device memory: fold them into the UAV class. */
    uint32_t mapped = *types & 0x3u;
    if (*types & 0xcu)
      mapped |= 0x1u;
    call.setArgOperand(1, b.getInt32(mapped));
    return Error::success(); /* LowerOperations lowers the rewritten call. */
  }
  case OpBarrierByNodeRecordHandle: {
    if (auto handle = recordHandle(call.getArgOperand(1)); !handle)
      return handle.takeError();
    auto semantics = constantArg(call, 2, "barrierByNodeRecordHandle semantics");
    if (!semantics)
      return semantics.takeError();
    if (*semantics & ~0xfu)
      return createError("dxil: barrierByNodeRecordHandle has unknown semantic flag bits");
    auto callee = module.getOrInsertFunction(
      "dx.op.barrierByMemoryType", FunctionType::get(b.getVoidTy(), {i32, i32, i32}, false)
    );
    b.CreateCall(callee, {b.getInt32(OpBarrierByMemoryType), b.getInt32(1), b.getInt32(*semantics)});
    return Error::success();
  }
  default:
    return Error::success();
  }
}

Error NodeLowering::run() {
  /* Program order matters: a handle is defined before its uses, and DXIL keeps them in
   * dominating blocks, so a reverse-post-order walk sees definitions first. */
  SmallVector<std::pair<CallInst *, uint32_t>, 32> calls;
  ReversePostOrderTraversal<Function *> order(&function);
  for (BasicBlock *block : order)
    for (Instruction &instruction : *block)
      if (auto *call = dyn_cast<CallInst>(&instruction)) {
        uint32_t opcode = opcodeOf(*call);
        if ((opcode >= OpAllocateNodeOutputRecords && opcode <= OpGetRemainingRecursionLevels) &&
            opcode != 245 /* barrierByMemoryHandle: LowerResources */)
          calls.push_back({call, opcode});
      }
  for (auto &[call, opcode] : calls)
    if (auto error = lowerCall(*call, opcode))
      return error;
  /* Handles have no uses left once every consumer was rewritten; erase in reverse. */
  for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
    CallInst *call = it->first;
    if (it->second == OpBarrierByMemoryType)
      continue;
    if (!call->use_empty()) {
      if (call->getType()->isStructTy()) /* node / record handle values */
        call->replaceAllUsesWith(UndefValue::get(call->getType()));
      else if (it->second != OpGetNodeRecordPtr)
        return createError("dxil: lowered node operation still has uses");
    }
    if (it->second == OpGetNodeRecordPtr && !call->use_empty())
      return createError("dxil: node record pointer still has uses");
    call->eraseFromParent();
  }
  return Error::success();
}

/* Reduce a library module to the selected export (see IsolateSelectedExport in dxil_raytracing.cpp). */
void isolateExport(Module &module, Function &entry) {
  SmallPtrSet<Function *, 16> reachable;
  SmallVector<Function *, 16> worklist;
  auto enqueue = [&](Function *function) {
    if (function && reachable.insert(function).second)
      worklist.push_back(function);
  };
  enqueue(&entry);
  while (!worklist.empty()) {
    Function *function = worklist.pop_back_val();
    for (BasicBlock &block : *function)
      for (Instruction &instruction : block)
        if (auto *call = dyn_cast<CallBase>(&instruction))
          enqueue(dyn_cast<Function>(call->getCalledOperand()->stripPointerCastsAndAliases()));
  }
  for (Function &function : module)
    if (!function.isDeclaration() && !reachable.contains(&function))
      function.deleteBody();
  if (NamedMDNode *entries = module.getNamedMetadata("dx.entryPoints"))
    module.eraseNamedMetadata(entries);
}

Error linkHelpers(Module &module) {
  auto buffer = MemoryBuffer::getMemBufferCopy(
    StringRef(reinterpret_cast<const char *>(air_workgraph), sizeof(air_workgraph))
  );
  auto parsed = parseBitcodeFile(buffer->getMemBufferRef(), module.getContext());
  if (!parsed) return parsed.takeError();
  auto helpers = std::move(*parsed);
  for (const char *name : {"air.compile_options", "air.version", "air.language_version", "air.source_file_name",
                           "llvm.ident", "llvm.module.flags"})
    if (auto *md = helpers->getNamedMetadata(name))
      helpers->eraseNamedMetadata(md);
  if (Linker::linkModules(module, std::move(helpers), Linker::LinkOnlyNeeded))
    return createError("dxil: cannot link work graph AIR helpers");
  return Error::success();
}

Error compileNode(const SM50_WG_NODE_COMPILE_DESC &desc, SM50CompiledBitcodeInternal &compiled,
                  SM50_WG_NODE_COMPILED_INFO &out) {
  if (!desc.Library || !desc.EntryPoint || !desc.FunctionName || (!desc.Reserved && !desc.GlobalRootSignature))
    return createError("work graph node compilation: incomplete description");
  if (desc.Reserved) {
    auto parsed = ModuleInfo::Parse(desc.Library, desc.LibrarySize, nullptr);
    if (!parsed) return parsed.takeError();
    (*parsed)->entry = (*parsed)->module->getFunction(desc.EntryPoint);
    if (!(*parsed)->entry) return createError("collection export has no function definition");
    if (auto error = LinkLibraryDependencies(**parsed, desc)) return error;
    if (desc.AllowUnresolved) return Error::success();
    ModuleInfo &info = **parsed;
    Module &module = *info.module;
    isolateExport(module, *info.entry);
    PruneUnusedLibraryResources(info);
    out.HasAtomic64 = HasDeviceAtomic64(module) ? 1 : 0;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{nullptr, SM50_SHADER_D3D12_ATOMIC64_PHASE};
    if (out.HasAtomic64) InitializeAtomic64Phase(module, &atomic_phase);
    PrepareModuleStorage(module);
    dxbc::SM50ShaderInternal shader;
    PopulateShaderResourceBindings(shader.shader_info, info.resources);
    shader.shader_type = (microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE)kShaderKindNode;
    auto binding = dxbc::setup_binding_rootsig(
      &shader.shader_info, shader.func_signature, module, shader.shader_type, desc.GlobalRootSignature,
      desc.GlobalRootSignatureSize, SM50_BINDING_INDEX_ROOT_ARGUMENTS, SM50_BINDING_INDEX_STATIC_SAMPLERS,
      desc.LocalRootSignature, desc.LocalRootSignatureSize, true
    );
    if (!binding) return createError("collection export has invalid resource bindings");
    auto [validation, metadata] = shader.func_signature.CreateFunction(desc.FunctionName, *info.context, module, 0, false);
    IRBuilder<> builder(BasicBlock::Create(*info.context, "entry", validation));
    // Validation has no dispatch record, but local bindings must still resolve
    // against their signature. No generated instructions are executed or kept.
    if (desc.LocalRootSignature)
      binding->SetLocalRootArgumentPointer(UndefValue::get(builder.getInt8PtrTy(1)));
    SmallVector<Value *, 8> arguments;
    for (Argument &argument : info.entry->args()) arguments.push_back(UndefValue::get(argument.getType()));
    auto *call = builder.CreateCall(info.entry, arguments); builder.CreateRetVoid();
    InlineFunctionInfo inline_info;
    if (!InlineFunction(*call, inline_info).isSuccess()) return createError("collection export cannot be validated");
    info.entry = validation;
    isolateExport(module, *validation);
    return LowerResources(info, *binding);
  }
  if (desc.Launch < SM50_WG_LAUNCH_BROADCASTING || desc.Launch > SM50_WG_LAUNCH_THREAD)
    return createError("work graph node compilation: invalid launch");

  /* Reflection of the selected node (inputs, launch, thread group size). */
  auto reflected = reflectLibrary(desc.Library, desc.LibrarySize);
  if (!reflected)
    return reflected.takeError();
  const NodeReflection *node = nullptr;
  for (const auto &candidate : *reflected)
    if (candidate.entry == desc.EntryPoint)
      node = &candidate;
  if (!node)
    return createError(Twine("work graph node compilation: no node shader '") + desc.EntryPoint + "'");
  if (node->info.Launch != desc.Launch)
    return createError("work graph node compilation: launch mode does not match the shader");
  if (node->outputs.size() != desc.OutputCount)
    return createError("work graph node compilation: output count does not match the shader");

  auto parsed = ModuleInfo::Parse(desc.Library, desc.LibrarySize, desc.EntryPoint);
  if (!parsed)
    return parsed.takeError();
  ModuleInfo &info = **parsed;
  if (!info.entry)
    return createError("work graph node compilation: entry point not found");
  if (auto error = LinkLibraryDependencies(info, desc)) return error;
  Module &module = *info.module;
  LLVMContext &context = *info.context;
  isolateExport(module, *info.entry);
  if (module.getDataLayout().getPointerSizeInBits() != 64)
    d3dalloy::initializeModule(module);
  out.HasAtomic64 = HasDeviceAtomic64(module) ? 1 : 0;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{nullptr, SM50_SHADER_D3D12_ATOMIC64_PHASE};
  if (out.HasAtomic64) InitializeAtomic64Phase(module, &atomic_phase);
  dxbc::setup_metal_version(module, (SM50_SHADER_METAL_VERSION)desc.MetalVersion);
  PrepareModuleStorage(module);

  dxbc::SM50ShaderInternal shader;
  PopulateShaderResourceBindings(shader.shader_info, info.resources);
  /* No D3D10 program type exists for a node; the DXIL ShaderKind stands in, which only
   * root-signature parameters visible to ALL stages resolve against (as for compute). */
  shader.shader_type = (microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE)kShaderKindNode;
  auto &signature = shader.func_signature;

  const bool thread_launch = desc.Launch == SM50_WG_LAUNCH_THREAD;
  uint32_t group[3] = {node->info.NumThreads[0], node->info.NumThreads[1], node->info.NumThreads[2]};
  if (thread_launch)
    group[0] = SM50_WG_THREAD_LAUNCH_GROUP_SIZE, group[1] = group[2] = 1;
  const uint32_t group_threads = group[0] * group[1] * group[2];
  if (!group_threads || group_threads > 1024)
    return createError("work graph node compilation: invalid thread group size");
  signature.UseMaxWorkgroupSize(group_threads);

  const uint32_t context_arg = signature.DefineInput(air::ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT,
    .array_size = 0,
    .memory_access = air::MemoryAccess::read,
    .address_space = air::AddressSpace::device,
    .type = air::MSLUint{},
    .arg_name = "work_graph_context",
    .raster_order_group = {},
  });
  const uint32_t group_arg = signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  const uint32_t local_arg = signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  const uint32_t flat_arg = signature.DefineInput(air::InputThreadIndexInThreadgroup{});
  const uint32_t grid_arg = signature.DefineInput(air::InputThreadPositionInGrid{});

  auto binding = dxbc::setup_binding_rootsig(
    &shader.shader_info, signature, module, shader.shader_type, desc.GlobalRootSignature,
    desc.GlobalRootSignatureSize, SM50_BINDING_INDEX_ROOT_ARGUMENTS, SM50_BINDING_INDEX_STATIC_SAMPLERS,
    desc.LocalRootSignature, desc.LocalRootSignature ? desc.LocalRootSignatureSize : 0, true
  );
  if (!binding)
    return createError("work graph node compilation: invalid root signature");

  info.entry->setName(Twine(desc.FunctionName) + ".dxil");
  auto [function, function_metadata] = signature.CreateFunction(desc.FunctionName, context, module, 0, false);
  auto *entry_bb = BasicBlock::Create(context, "entry", function);
  auto *epilogue_bb = BasicBlock::Create(context, "epilogue", function);
  IRBuilder<> builder(entry_bb);
  Type *i32 = builder.getInt32Ty();
  Value *ctx_words = function->getArg(context_arg);
  auto word = [&](uint32_t dword) {
    return builder.CreateLoad(i32, builder.CreateConstInBoundsGEP1_32(i32, ctx_words, dword));
  };

  dxbc::io_binding_map resource_map;
  air::AirType types(context);
  Value *tg = function->getArg(group_arg);
  Value *local = function->getArg(local_arg);
  Value *flat = function->getArg(flat_arg);
  Value *batch_start = word(kCtxBatchStart);
  Value *batch_records = word(kCtxBatchRecords);
  Value *input_first = nullptr;
  Value *input_count = builder.getInt32(1);
  auto *v3 = FixedVectorType::get(i32, 3);
  switch (desc.Launch) {
  case SM50_WG_LAUNCH_BROADCASTING: {
    Value *g = builder.CreateExtractElement(tg, uint64_t(0));
    auto record_fn = module.getOrInsertFunction(
      "d3dalloy.wg.bc_record", FunctionType::get(i32, {ctx_words->getType(), i32}, false)
    );
    auto group_fn = module.getOrInsertFunction(
      "d3dalloy.wg.bc_group", FunctionType::get(i32, {ctx_words->getType(), i32, i32, i32}, false)
    );
    Value *record = builder.CreateCall(record_fn, {ctx_words, g});
    Value *gid = UndefValue::get(v3);
    for (unsigned axis = 0; axis < 3; ++axis)
      gid = builder.CreateInsertElement(
        gid, builder.CreateCall(group_fn, {ctx_words, g, record, builder.getInt32(axis)}), axis
      );
    input_first = builder.CreateAdd(batch_start, record);
    auto *size = ConstantVector::get({builder.getInt32(group[0]), builder.getInt32(group[1]), builder.getInt32(group[2])});
    resource_map.thread_group_id_arg = gid;
    resource_map.thread_id_in_group_arg = local;
    resource_map.thread_id_in_group_flat_arg = flat;
    resource_map.thread_id_arg = builder.CreateAdd(builder.CreateMul(gid, size), local);
    break;
  }
  case SM50_WG_LAUNCH_COALESCING: {
    const uint32_t per_group = node->info.HasInput ? std::max(node->info.Input.MaxRecords, 1u) : 1u;
    Value *g = builder.CreateExtractElement(tg, uint64_t(0));
    Value *offset = builder.CreateMul(g, builder.getInt32(per_group));
    input_first = builder.CreateAdd(batch_start, offset);
    Value *left = builder.CreateSub(batch_records, offset);
    input_count = builder.CreateSelect(builder.CreateICmpULT(left, builder.getInt32(per_group)), left,
                                       builder.getInt32(per_group));
    resource_map.thread_id_in_group_arg = local;
    resource_map.thread_id_in_group_flat_arg = flat;
    break;
  }
  case SM50_WG_LAUNCH_THREAD: {
    Value *t = builder.CreateExtractElement(function->getArg(grid_arg), uint64_t(0));
    input_first = builder.CreateAdd(batch_start, t);
    /* Lanes past the batch have no record; thread launch has no group scope, so they leave. */
    auto *body = BasicBlock::Create(context, "wg.thread", function, epilogue_bb);
    builder.CreateCondBr(builder.CreateICmpULT(t, batch_records), body, epilogue_bb);
    builder.SetInsertPoint(body);
    break;
  }
  }

  if (desc.LocalRootSignature) {
    Value *low = builder.CreateZExt(word(kCtxLocalRoot), builder.getInt64Ty());
    Value *high = builder.CreateZExt(word(kCtxLocalRoot + 1), builder.getInt64Ty());
    Value *address = builder.CreateOr(low, builder.CreateShl(high, 32));
    binding->SetLocalRootArgumentPointer(builder.CreateIntToPtr(address, builder.getInt8PtrTy(1)));
  }

  llvm::air::AIRBuilder air(builder, nulls());
  dxbc::setup_temp_register(&shader.shader_info, resource_map, types, module, builder);
  dxbc::setup_immediate_constant_buffer(&shader.shader_info, resource_map, types, module, builder);
  dxbc::context ctx{
    .builder = builder,
    .air = air,
    .binding = *binding,
    .llvm = context,
    .module = module,
    .function = function,
    .resource = resource_map,
    .types = types,
    .pso_sample_mask = 0xffffffffu,
    .shader_type = shader.shader_type,
    .metal_version = (SM50_SHADER_METAL_VERSION)desc.MetalVersion,
  };

  auto body = TransplantEntryBody(*info.entry, *function, *epilogue_bb);
  if (!body)
    return body.takeError();
  builder.CreateBr(*body);
  builder.SetInsertPoint(epilogue_bb);
  builder.CreateRetVoid();

  StageSignature no_io;
  if (auto error = LowerStageIO(ctx, no_io))
    return error;

  NodeLowering lowering(module, *function, desc, *node, ctx_words, flat);
  lowering.input_first = input_first;
  lowering.input_count = input_count;
  if (auto error = lowering.run())
    return error;
  if (lowering.usesHelpers() || desc.Launch == SM50_WG_LAUNCH_BROADCASTING)
    if (auto error = linkHelpers(module)) return error;

  module.getOrInsertNamedMetadata("air.kernel")->addOperand(function_metadata);

  if (auto error = LowerResources(info, *binding))
    return error;
  if (auto error = LowerOperations(module))
    return error;
  if (auto error = VerifyLoweredModule(module))
    return error;
  StripDXILArtifacts(module);
  if (verifyModule(module, &errs()))
    return createError("work graph node compilation produced invalid IR");
  if (auto error = FinalizeShader(module, compiled, true))
    return error;
  out.ThreadgroupSize[0] = group[0];
  out.ThreadgroupSize[1] = group[1];
  out.ThreadgroupSize[2] = group[2];
  return Error::success();
}

void report(Error failure, sm50_error_t *ppError) {
  auto *error = new SM50ErrorInternal();
  raw_svector_ostream stream(error->buf);
  handleAllErrors(std::move(failure), [&](const ErrorInfoBase &base) { base.log(stream); });
  stream << '\0';
  if (ppError)
    *ppError = (sm50_error_t)error;
  else
    delete error;
}

} // namespace

} // namespace d3dalloy::dxil

using namespace d3dalloy::dxil;

AIRCONV_API int SM50WGReflectLibrary(
  const void *pBytecode, uint64_t BytecodeSize, struct SM50_WG_NODE_INFO *pNodes, uint32_t NodeCapacity,
  struct SM50_WG_NODE_IO *pOutputs, uint32_t OutputCapacity, char *pNames, uint64_t NamesSize,
  uint32_t *pNodeCount, uint32_t *pOutputCount, uint64_t *pNamesSize, sm50_error_t *ppError
) {
  if (ppError)
    *ppError = nullptr;
  auto nodes = reflectLibrary(pBytecode, BytecodeSize);
  if (!nodes) {
    report(nodes.takeError(), ppError);
    return 0;
  }
  std::string names;
  auto add = [&](const std::string &name, uint32_t &offset, uint32_t &length) {
    offset = static_cast<uint32_t>(names.size());
    length = static_cast<uint32_t>(name.size());
    names += name;
    names.push_back('\0');
  };
  std::vector<SM50_WG_NODE_INFO> infos;
  std::vector<SM50_WG_NODE_IO> outputs;
  for (auto &node : *nodes) {
    SM50_WG_NODE_INFO info = node.info;
    add(node.public_name, info.NameOffset, info.NameLength);
    add(node.entry, info.EntryPointOffset, info.EntryPointLength);
    add(node.id.name, info.NodeNameOffset, info.NodeNameLength);
    info.NodeArrayIndex = node.id.index;
    if (info.HasShareInputOf) {
      add(node.share_input.name, info.ShareInputNameOffset, info.ShareInputNameLength);
      info.ShareInputArrayIndex = node.share_input.index;
    }
    info.OutputOffset = static_cast<uint32_t>(outputs.size());
    for (auto &output : node.outputs) {
      SM50_WG_NODE_IO io = output.io;
      add(output.target.name, io.NodeNameOffset, io.NodeNameLength);
      io.NodeArrayIndex = output.target.index;
      outputs.push_back(io);
    }
    infos.push_back(info);
  }
  if (pNodeCount)
    *pNodeCount = static_cast<uint32_t>(infos.size());
  if (pOutputCount)
    *pOutputCount = static_cast<uint32_t>(outputs.size());
  if (pNamesSize)
    *pNamesSize = names.size();
  if (pNodes && pOutputs && pNames && infos.size() <= NodeCapacity && outputs.size() <= OutputCapacity &&
      names.size() <= NamesSize) {
    std::copy(infos.begin(), infos.end(), pNodes);
    std::copy(outputs.begin(), outputs.end(), pOutputs);
    std::memcpy(pNames, names.data(), names.size());
  }
  return 1;
}

AIRCONV_API int SM50WGCompileNode(
  const struct SM50_WG_NODE_COMPILE_DESC *pDesc, sm50_bitcode_t *ppBitcode, struct SM50_COMPILED_BITCODE *pMetallib,
  struct SM50_WG_NODE_COMPILED_INFO *pInfo, sm50_error_t *ppError
) {
  if (ppError)
    *ppError = nullptr;
  if (!pDesc || !ppBitcode || !pMetallib || !pInfo) {
    report(createError("SM50WGCompileNode: null argument"), ppError);
    return 0;
  }
  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (auto error = compileNode(*pDesc, *compiled, *pInfo)) {
    report(std::move(error), ppError);
    return 0;
  }
  if (pDesc->Reserved) { *ppBitcode = nullptr; *pMetallib = {}; return 1; }
  *ppBitcode = (sm50_bitcode_t)compiled.release();
  SM50GetCompiledBitcode(*ppBitcode, pMetallib);
  return 1;
}
