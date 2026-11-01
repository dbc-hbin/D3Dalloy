// CPU-only trust-boundary oracle: real legacy DXR input plus deliberately invalid
// DXIL modules wrapped in a DXBC container. No GPU or external validator required.
#include "dxil_converter.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace llvm;

static int failures;

static std::vector<uint8_t> container(Module &module) {
  SmallVector<char, 0> bitcode;
  raw_svector_ostream stream(bitcode);
  WriteBitcodeToFile(module, stream);
  std::vector<uint8_t> bytes(44 + bitcode.size());
  const uint32_t header[] = {0x43425844, 0, 0, 0, 0, 1, uint32_t(bytes.size()), 1, 36,
                             0x4c495844, uint32_t(bitcode.size())};
  std::memcpy(bytes.data(), header, sizeof(header));
  std::memcpy(bytes.data() + 44, bitcode.data(), bitcode.size());
  return bytes;
}

static void check(const char *name, StringRef declarations, StringRef body,
                  bool accepted, unsigned shader_minor = 9, unsigned dxil_minor = 9,
                  bool library = false, StringRef globals = "", StringRef resources = "", bool lower = false) {
  LLVMContext context;
  context.setOpaquePointers(false);
  const std::string source = globals.str() + declarations.str() + "\ndefine void @main() {\n" +
      body.str() + "\nret void\n}\n!dx.shaderModel = !{!0}\n!0 = !{!\"" +
      (library ? "lib" : "cs") + "\", i32 6, i32 " + std::to_string(shader_minor) +
      "}\n!dx.version = !{!1}\n!1 = !{i32 1, i32 " + std::to_string(dxil_minor) +
      "}\n!dx.entryPoints = !{!2}\n!2 = !{void ()* @main, !\"main\", null, null, !3}\n"
      "!3 = !{i32 4, !4}\n!4 = !{i32 1, i32 1, i32 1}\n" + resources.str();
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(source, diagnostic, context);
  if (!module) { diagnostic.print(name, errs()); ++failures; return; }
  auto bytes = container(*module);
  auto parsed = d3dalloy::dxil::ModuleInfo::Parse(bytes.data(), bytes.size());
  const bool actual = bool(parsed);
  std::string error = actual ? "" : toString(parsed.takeError());
  if (actual != accepted) {
    std::fprintf(stderr, "VECTOR_CONTRACT_FAIL %s expected=%d actual=%d error=%s\n", name, accepted, actual, error.c_str());
    ++failures;
  } else if (actual && lower) {
    if (auto failure = d3dalloy::dxil::LowerOperations(*(*parsed)->module)) {
      std::fprintf(stderr, "VECTOR_CONTRACT_FAIL %s lowering=%s\n", name, toString(std::move(failure)).c_str());
      ++failures;
    } else if (verifyModule(*(*parsed)->module, &errs())) {
      std::fprintf(stderr, "VECTOR_CONTRACT_FAIL %s invalid lowered IR\n", name);
      ++failures;
    } else {
      bool illegal_shuffle = false;
      for (const Function &function : *(*parsed)->module)
        illegal_shuffle |= function.getName().startswith("air.simd") && function.getName().endswith("i8");
      if (illegal_shuffle) { std::fprintf(stderr, "VECTOR_CONTRACT_FAIL %s unsupported AIR i8 wave primitive\n", name); ++failures; }
      else std::printf("VECTOR_CONTRACT_CASE_PASS %s\n", name);
    }
  } else {
    std::printf("VECTOR_CONTRACT_CASE_PASS %s\n", name);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: vector-contract <raytracing-contract.dxil>\n"); return 2; }
  auto legacy = MemoryBuffer::getFile(argv[1]);
  if (!legacy) { std::fprintf(stderr, "cannot read real legacy DXR contract\n"); return 2; }
  auto parsed = d3dalloy::dxil::ModuleInfo::Parse((*legacy)->getBufferStart(), (*legacy)->getBufferSize());
  if (!parsed) { std::fprintf(stderr, "VECTOR_CONTRACT_FAIL legacy-dxr %s\n", toString(parsed.takeError()).c_str()); ++failures; }
  else std::puts("VECTOR_CONTRACT_CASE_PASS legacy-dxr");
  for (unsigned length : {2u, 4u, 5u, 8u, 1024u}) {
    const std::string type = "<" + std::to_string(length) + " x i32>";
    const std::string function = "@dx.op.vectorReduce.v" + std::to_string(length) + "i32";
    check(("length" + std::to_string(length)).c_str(), "declare i32 " + function + "(i32, " + type + ")",
          "%result = call i32 " + function + "(i32 309, " + type + " zeroinitializer)", true);
  }
  check("singleton-local", "", "%value = alloca <1 x i32>", false);
  check("too-long", "", "%value = alloca <1025 x i32>", false);
  check("scalable", "", "%value = alloca <vscale x 4 x i32>", false);
  const char *declaration = "declare i32 @dx.op.vectorReduce.v5i32(i32, <5 x i32>)";
  const char *operation = "%result = call i32 @dx.op.vectorReduce.v5i32(i32 309, <5 x i32> zeroinitializer)";
  check("old-shader-model", declaration, operation, false, 8);
  check("old-dxil-version", declaration, operation, false, 9, 8);
  check("future-shader-without-matrix-contract", "", "", false, 10, 10);
  for (unsigned opcode : {4u, 59u, 66u, 68u}) {
    const std::string declaration = "declare <5 x i32> @dx.op.illegal.v5i32(i32)";
    const std::string operation = "%result = call <5 x i32> @dx.op.illegal.v5i32(i32 " + std::to_string(opcode) + ")";
    check(("unsupported-overload" + std::to_string(opcode)).c_str(), declaration, operation, false);
  }
  check("all-equal-wrong-result", "declare i1 @dx.op.waveActiveAllEqual.v5i32(i32, <5 x i32>)",
        "%result = call i1 @dx.op.waveActiveAllEqual.v5i32(i32 115, <5 x i32> zeroinitializer)", false);
  check("all-equal-componentwise", "declare <5 x i1> @dx.op.waveActiveAllEqual.v5i32(i32, <5 x i32>)",
        "%result = call <5 x i1> @dx.op.waveActiveAllEqual.v5i32(i32 115, <5 x i32> zeroinitializer)", true);
  check("singleton-library-interface", "declare <1 x i32> @exported(<1 x i32>)", "", true, 9, 9, true);
  check("singleton-private-interface", "define internal <1 x i32> @private(<1 x i32> %arg) { ret <1 x i32> %arg }", "", false, 9, 9, true);
  check("singleton-library-helper-lowering", "define <1 x i32> @exported(<1 x i32> %arg) { ret <1 x i32> %arg }",
        "%result = call <1 x i32> @exported(<1 x i32> zeroinitializer)", true, 9, 9, true, "", "", true);
  check("i8-wave-shuffle", "declare <5 x i8> @dx.op.waveReadLaneAt.v5i8(i32, <5 x i8>, i32)",
        "%result = call <5 x i8> @dx.op.waveReadLaneAt.v5i8(i32 117, <5 x i8> zeroinitializer, i32 0)",
        true, 9, 9, false, "", "", true);
  check("i8-wave-reduction", "declare <5 x i8> @dx.op.waveActiveOp.v5i8(i32, <5 x i8>, i8, i8)",
        "%result = call <5 x i8> @dx.op.waveActiveOp.v5i8(i32 119, <5 x i8> zeroinitializer, i8 0, i8 1)",
        true, 9, 9, false, "", "", true);
  check("i1-wave-reduction", "declare <5 x i1> @dx.op.waveActiveOp.v5i1(i32, <5 x i1>, i8, i8)",
        "%result = call <5 x i1> @dx.op.waveActiveOp.v5i1(i32 119, <5 x i1> zeroinitializer, i8 0, i8 1)",
        true, 9, 9, false, "", "", true);
  check("i1-wave-prefix", "declare <5 x i1> @dx.op.wavePrefixOp.v5i1(i32, <5 x i1>, i8, i8)",
        "%result = call <5 x i1> @dx.op.wavePrefixOp.v5i1(i32 121, <5 x i1> zeroinitializer, i8 1, i8 1)",
        true, 9, 9, false, "", "", true);
  for (unsigned kind : {2u, 10u}) {
    const std::string metadata = "!dx.resources = !{!5}\n!5 = !{!6, null, null, null}\n!6 = !{!7}\n"
        "!7 = !{i32 0, <5 x i32>* @resource, !\"resource\", i32 0, i32 0, i32 1, i32 " + std::to_string(kind) + "}\n";
    check(("typed-resource" + std::to_string(kind)).c_str(), "", "", false, 9, 9, true,
          "@resource = external global <5 x i32>\n", metadata);
  }
  check("cbuffer-long-vector", "", "", false, 9, 9, true, "@resource = external global <5 x i32>\n",
        "!dx.resources = !{!5}\n!5 = !{null, null, !6, null}\n!6 = !{!7}\n"
        "!7 = !{i32 0, <5 x i32>* @resource, !\"resource\", i32 0, i32 0, i32 1, i32 32}\n");
  std::printf("VECTOR_CONTRACT_%s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
