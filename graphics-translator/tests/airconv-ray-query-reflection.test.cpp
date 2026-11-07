// Link with production libairconv.a and native LLVM; pass the frozen DXIL directory.
#include "airconv_public.h"
#include "dxil_converter.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <cstdio>
#include <filesystem>

static bool reachableQueries() {
  llvm::LLVMContext context;
  llvm::Module module("query-feature", context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  const auto function = [&](const char *name) {
    auto *result = llvm::Function::Create(type, llvm::Function::ExternalLinkage, name, module);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", result));
    builder.CreateRetVoid();
    return result;
  };
  const auto call = [&](llvm::Function *caller, llvm::Function *callee) {
    llvm::IRBuilder<> builder(caller->getEntryBlock().getTerminator());
    builder.CreateCall(callee);
  };
  // These call graphs exercise the scanner, not DXIL validation. Genuine bytecode below
  // checks the public reflection boundary separately.
  auto *trace = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
      "dx.op.rayQuery_TraceRayInline", module);
  auto *allocate = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
      "dx.op.allocateRayQuery", module);
  auto *entry = function("entry");
  auto *helper = function("helper");
  call(helper, trace);
  if (d3dalloy::dxil::HasRayQuery(*entry)) return false; // Unreachable query must not light the HUD.
  call(entry, allocate);
  if (d3dalloy::dxil::HasRayQuery(*entry)) return false; // Allocation alone does not trace.
  call(entry, helper);
  if (!d3dalloy::dxil::HasRayQuery(*entry)) return false;
  auto *cycle_a = function("cycle_a");
  auto *cycle_b = function("cycle_b");
  call(cycle_a, cycle_b);
  call(cycle_b, cycle_a);
  if (d3dalloy::dxil::HasRayQuery(*cycle_a)) return false;
  call(cycle_b, helper);
  return d3dalloy::dxil::HasRayQuery(*cycle_a);
}

int main(int argc, char **argv) {
  if (argc != 2 || !reachableQueries()) return 1;
  const struct { const char *file; bool query; } cases[] = {
      {"compute.dxil", false},
      {"raytracing-rayquery-cs.dxil", true},
      {"raytracing-rayquery-vs.dxil", true},
      {"raytracing-rayquery-stages-hs.dxil", true},
      {"raytracing-rayquery-stages-ds.dxil", true},
      {"raytracing-rayquery-stages-gs.dxil", true},
      {"raytracing-rayquery-stages-as.dxil", true},
      {"raytracing-rayquery-stages-ms.dxil", true},
  };
  for (const auto &test : cases) {
    auto bytes = llvm::MemoryBuffer::getFile((std::filesystem::path(argv[1]) / test.file).string());
    if (!bytes) return 1;
    sm50_shader_t shader = nullptr;
    sm50_error_t error = nullptr;
    MTL_SHADER_REFLECTION reflection{};
    const int result = SM50Initialize((*bytes)->getBufferStart(), (*bytes)->getBufferSize(),
                                     &shader, &reflection, &error);
    if (result || bool(reflection.HasRayQuery) != test.query) {
      std::fprintf(stderr, "%s: result=%d query=%u expected=%u %s\n", test.file, result,
          reflection.HasRayQuery, unsigned(test.query), error ? SM50GetErrorMessageString(error).c_str() : "");
    }
    if (error) SM50FreeError(error);
    if (shader) SM50Destroy(shader);
    if (result || bool(reflection.HasRayQuery) != test.query) return 1;
  }
  std::puts("RAY_QUERY_REFLECTION_PASS call_graph_cases=5 frozen_shader_cases=8");
}
