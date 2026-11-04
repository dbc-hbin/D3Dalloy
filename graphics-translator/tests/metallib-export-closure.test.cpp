// Link the production metallib writer and LLVM 15; no Metal device is required.
#include "metallib_writer.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace llvm;
using namespace d3dalloy::metallib;

static void require(bool okay, const char *reason) {
  if (!okay) { std::fprintf(stderr, "METALLIB_EXPORT_CLOSURE_FAIL %s\n", reason); std::exit(1); }
}

template <class T> static T read(StringRef bytes, size_t at) {
  require(at <= bytes.size() && sizeof(T) <= bytes.size() - at, "record bounds");
  T result;
  std::memcpy(&result, bytes.data() + at, sizeof(T));
  return result;
}

static SmallVector<char, 0> write(const Module &module) {
  SmallVector<char, 0> bytes;
  raw_svector_ostream stream(bytes);
  MetallibWriter writer;
  if (auto error = writer.Write(module, stream)) {
    logAllUnhandledErrors(std::move(error), errs());
    require(false, "writer");
  }
  return bytes;
}

static bool defined(const Module &module, StringRef name) {
  auto *value = module.getNamedValue(name);
  return value && !value->isDeclaration();
}

static void check(bool baseline, unsigned mode) {
  LLVMContext context;
  context.setOpaquePointers(false);
  SMDiagnostic diagnostic;
  auto module = parseAssemblyString(R"(
@table = internal constant [2 x i32*] [i32* @bias, i32* @other]
@bias = internal global i32 7
@other = internal global i32 9
@prefix_value = internal global i32 17
@prologue_value = internal global i32 19
@fc = addrspace(2) externally_initialized global i32 undef, section "air.fc_initializer"
@llvm.used = appending global [1 x i8*] [i8* bitcast (i32 ()* @retained to i8*)], section "llvm.metadata"
define internal i32 @retained() { ret i32 5 }
define i32 @unused_external() { ret i32 999 }
define i32 @metadata_helper() { ret i32 23 }
define i32 @personality(...) { ret i32 0 }
define internal i32 @shared() {
  %p = load i32*, i32** getelementptr ([2 x i32*], [2 x i32*]* @table, i32 0, i32 0)
  %v = load i32, i32* %p
  ret i32 %v
}
define internal i32 @helper_a() { %v = call i32 @shared() ret i32 %v }
define internal i32 @helper_b() { %v = call i32 @shared() ret i32 %v }
define internal i32 @helper_c() personality i32 (...)* @personality {
  ret i32 31
}
define i32 @a() {
  %x = call i32 @helper_a(), !owned !6
  %y = call i32 @b()
  %z = add i32 %x, %y
  ret i32 %z
}
define i32 @b() { %v = call i32 @helper_b() ret i32 %v }
define i32 @c() { %v = call i32 @helper_c() ret i32 %v }
!air.visible = !{!0, !1, !2}
!0 = !{i32 ()* @a}
!1 = !{i32 ()* @b}
!2 = !{i32 ()* @c}
!air.function_constants = !{!3}
!3 = !{i32 addrspace(2)* @fc, !"uint", !"choice", i32 0}
!owned.roots = !{!6}
!6 = !{i32 ()* @metadata_helper}
)", diagnostic, context);
  if (!module) diagnostic.print("export-closure", errs());
  require(bool(module), "parse fixture");
  if (mode == 1) {
    auto *target = module->getFunction("retained");
    GlobalAlias::create(target->getFunctionType(), 0, GlobalValue::ExternalLinkage, "retained_alias", target, module.get());
  } else if (mode == 2) {
    auto *calleeType = llvm::FunctionType::get(Type::getInt32Ty(context), false);
    auto *type = llvm::FunctionType::get(Type::getInt32Ty(context), {calleeType->getPointerTo()}, false);
    auto *function = Function::Create(type, GlobalValue::ExternalLinkage, "indirect", *module);
    IRBuilder<> builder(BasicBlock::Create(context, "entry", function));
    builder.CreateRet(builder.CreateCall(calleeType, function->getArg(0)));
    module->getOrInsertNamedMetadata("air.visible")->addOperand(MDNode::get(context, ValueAsMetadata::get(function)));
  } else if (mode == 3) {
    module->getFunction("helper_c")->setPrefixData(module->getNamedGlobal("prefix_value"));
    module->getFunction("helper_c")->setPrologueData(module->getNamedGlobal("prologue_value"));
  }
  require(!verifyModule(*module, &errs()), "input verifier");
  std::string before;
  raw_string_ostream original(before);
  module->print(original, nullptr);
  auto output = write(*module);
  std::string after;
  raw_string_ostream unchanged(after);
  module->print(unchanged, nullptr);
  require(before == after, "source module mutated");
  require(output == write(*module), "nondeterministic output");

  StringRef bytes(output.data(), output.size());
  auto header = read<MTLBHeader>(bytes, 0);
  require(header.Magic == MTLB_Magic && header.FileSize == bytes.size(), "MTLB header");
  const auto count = read<uint32_t>(bytes, header.FunctionListOffset);
  require(count == (mode == 2 ? 4u : 3u), "export count");
  size_t record = header.FunctionListOffset + 4;
  size_t definitions = 0;
  for (unsigned i = 0; i < count; ++i) {
    const auto length = read<uint32_t>(bytes, record);
    require(length >= 8 && length <= bytes.size() - record, "function record size");
    StringRef name;
    uint64_t offset = UINT64_MAX, size = 0;
    for (size_t field = record + 4; field < record + length;) {
      const auto tag = read<MTLBFourCC>(bytes, field);
      if (tag == MTLBFourCC::EndTag) break;
      const auto n = read<uint16_t>(bytes, field + 4);
      require(field + 6 + n <= record + length, "tag bounds");
      if (tag == MTLBFourCC::Name) name = bytes.substr(field + 6, n - 1);
      if (tag == MTLBFourCC::Offset) offset = read<uint64_t>(bytes, field + 22);
      if (tag == MTLBFourCC::Size) size = read<uint64_t>(bytes, field + 6);
      field += 6 + n;
    }
    require(offset <= header.BitcodeSize && size <= header.BitcodeSize - offset, "AIR bounds");
    LLVMContext isolatedContext;
    isolatedContext.setOpaquePointers(false);
    auto parsed = parseBitcodeFile(MemoryBufferRef(bytes.substr(header.BitcodeOffset + offset, size), name), isolatedContext);
    if (!parsed) { logAllUnhandledErrors(parsed.takeError(), errs()); require(false, "parse emitted AIR"); }
    auto &isolated = **parsed;
    require(!verifyModule(isolated, &errs()), "emitted AIR verifier");
    require(defined(isolated, name), "entry definition");
    for (StringRef entry : {"a", "b", "c", "indirect"})
      if (entry != name) require(!defined(isolated, entry), "foreign export defined twice");
    auto *entries = isolated.getNamedMetadata("air.visible");
    require(entries && entries->getNumOperands() == 1, "entry metadata isolation");
    require(defined(isolated, "fc") && defined(isolated, "retained") && defined(isolated, "metadata_helper"),
        "constant/used/metadata roots");
    if (name == "a") require(isolated.getFunction("b") && isolated.getFunction("b")->isDeclaration(), "linked export declaration");
    if (name == "a" || name == "b")
      require(defined(isolated, "shared") && defined(isolated, "table") && defined(isolated, "bias") && defined(isolated, "other"),
          "transitive constant aggregate");
    if (name == "c") require(defined(isolated, "personality"), "personality operand");
    if (name == "c" && mode == 3)
      require(defined(isolated, "prefix_value") && defined(isolated, "prologue_value"), "prefix/prologue operands");
    const bool fallback = baseline || mode == 1 || name == "indirect";
    require(defined(isolated, "unused_external") == fallback, "unrelated external definition or fallback");
    if (!fallback) {
      require(defined(isolated, "helper_a") == (name == "a"), "helper a closure");
      require(defined(isolated, "helper_b") == (name == "b"), "export boundary closure");
      require(defined(isolated, "helper_c") == (name == "c"), "helper c closure");
    }
    for (const auto &function : isolated) definitions += !function.isDeclaration();
    record += length;
  }
  std::printf("METALLIB_EXPORT_CLOSURE_PASS mode=%u baseline=%u exports=%u definitions=%zu bytes=%zu source_unchanged=1\n",
      mode, baseline, count, definitions, bytes.size());
}

int main(int argc, char **argv) {
  const bool baseline = argc == 2 && std::strcmp(argv[1], "--baseline") == 0;
  for (unsigned mode = 0; mode < (baseline ? 3u : 4u); ++mode) check(baseline, mode);
}
