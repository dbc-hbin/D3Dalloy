// Link against production libairconv.a; arguments are the comparison CS and library fixtures.
#include "dxil_converter.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include <cstdio>
#include <cstring>

using namespace llvm;

static bool comparison(const char *path, unsigned expected_handles, unsigned expected_calls) {
  auto bytes = MemoryBuffer::getFile(path);
  if (!bytes) { errs() << "Cannot read " << path << '\n'; return false; }
  auto parsed = dxmt::dxil::ModuleInfo::Parse((*bytes)->getBufferStart(), (*bytes)->getBufferSize());
  if (!parsed) { logAllUnhandledErrors(parsed.takeError(), errs()); return false; }
  auto &info = **parsed;
  unsigned handles = 0, calls = 0, srvs = 0;
  for (auto &function : *info.module)
    for (auto &instruction : instructions(function))
      if (auto *call = dyn_cast<CallInst>(&instruction))
        if (auto *callee = call->getCalledFunction()) {
          handles += callee->getName().startswith("dx.op.createHandleForLib");
          calls += callee->getName().startswith("dx.op.sampleCmp");
        }
  dxmt::dxbc::ShaderInfo shader;
  dxmt::dxil::PopulateShaderResourceBindings(shader, info.resources);
  bool okay = handles == expected_handles && calls == expected_calls;
  for (const auto &resource : info.resources) {
    if (resource.klass != dxmt::dxil::ResourceClass::SRV) continue;
    ++srvs;
    const auto &srv = shader.srvMap.at(resource.declaration_id);
    okay &= resource.read && resource.sampled && resource.compared && !resource.written &&
        !resource.with_counter && srv.read && srv.sampled && srv.compared &&
        dxmt::air::to_air_resource_type(srv.resource_type, srv.compared) == llvm::air::Texture::depth2d;
  }
  okay &= srvs == 6;
  std::printf("LIBRARY_COMPARISON_%s handles=%u calls=%u srvs=%u fixture=%s\n",
      okay ? "PASS" : "FAIL", handles, calls, srvs, path);
  return okay;
}

static bool origins() {
  LLVMContext context;
  context.setOpaquePointers(false);
  SMDiagnostic diagnostic;
  // All resources share register 0; the unused SRV shares its declaration ID
  // with the UAV. Symbol identity, not register/class guesses, must select usage.
  auto module = parseAssemblyString(R"(
%Object = type { i32 }
%Handle = type { i8* }
%Properties = type { i32, i32 }
%ResRet = type { i32, i32, i32, i32, i32 }
@raw = external global %Object
@counter = external global %Object
@array = external global [2 x %Object]
@unused = external global %Object
@"?named@@resource" = external global %Object

declare %Handle @dx.op.createHandleForLib.object(i32, %Object)
declare %Handle @dx.op.createHandleForLib.pointer(i32, %Object*)
declare %Handle @dx.op.annotateHandle(i32, %Handle, %Properties)
declare %ResRet @dx.op.rawBufferLoad.i32(i32, %Handle, i32, i32, i8, i32)
declare void @dx.op.rawBufferStore.i32(i32, %Handle, i32, i32, i32, i32, i32, i32, i8, i32)
declare i32 @dx.op.bufferUpdateCounter(i32, %Handle, i8)

define void @main(i32 %index) {
  %raw = call %Handle @dx.op.createHandleForLib.pointer(i32 160, %Object* @raw)
  %r = call %ResRet @dx.op.rawBufferLoad.i32(i32 139, %Handle %raw, i32 0, i32 0, i8 1, i32 4)
  %object = load %Object, %Object* @counter
  %counter = call %Handle @dx.op.createHandleForLib.object(i32 160, %Object %object)
  %annotated = call %Handle @dx.op.annotateHandle(i32 216, %Handle %counter, %Properties { i32 24588, i32 4 })
  %c = call i32 @dx.op.bufferUpdateCounter(i32 70, %Handle %annotated, i8 1)
  %element = getelementptr [2 x %Object], [2 x %Object]* @array, i32 0, i32 %index
  %arrayObject = load %Object, %Object* %element
  %array = call %Handle @dx.op.createHandleForLib.object(i32 160, %Object %arrayObject)
  call void @dx.op.rawBufferStore.i32(i32 140, %Handle %array, i32 0, i32 0, i32 1, i32 0, i32 0, i32 0, i8 1, i32 4)
  %named = call %Handle @dx.op.createHandleForLib.pointer(i32 160, %Object* @"?named@@resource")
  %n = call %ResRet @dx.op.rawBufferLoad.i32(i32 139, %Handle %named, i32 0, i32 0, i8 1, i32 4)
  ret void
}
!dx.shaderModel = !{!0}
!0 = !{!"lib", i32 6, i32 6}
!dx.version = !{!1}
!1 = !{i32 1, i32 6}
!dx.resources = !{!2}
!2 = !{!3, !4, null, null}
!3 = !{!5, !6, !7}
!4 = !{!8, !9}
!5 = !{i32 0, %Object* @raw, !"raw", i32 1, i32 0, i32 1, i32 11, i32 0, null}
!6 = !{i32 1, %Object* @unused, !"unused", i32 2, i32 0, i32 1, i32 11, i32 0, null}
!7 = !{i32 2, %Object* undef, !"named", i32 3, i32 0, i32 1, i32 11, i32 0, null}
!8 = !{i32 1, i8* bitcast (%Object* @counter to i8*), !"counter", i32 1, i32 0, i32 1, i32 12, i1 false, i1 true, i1 false, !10}
!9 = !{i32 2, %Object* getelementptr ([2 x %Object], [2 x %Object]* @array, i32 0, i32 1), !"array", i32 2, i32 0, i32 2, i32 11, i1 false, i1 false, i1 false, null}
!10 = !{i32 99, i32 0, i32 1, i32 4}
)", diagnostic, context);
  if (!module) { diagnostic.print("library-usage", errs()); return false; }
  if (verifyModule(*module, &errs())) return false;
  SmallVector<char, 0> bitcode;
  raw_svector_ostream stream(bitcode);
  WriteBitcodeToFile(*module, stream);
  std::vector<uint8_t> bytes(44 + bitcode.size());
  const uint32_t header[] = {0x43425844, 0, 0, 0, 0, 1, uint32_t(bytes.size()), 1, 36,
                             0x4c495844, uint32_t(bitcode.size())};
  std::memcpy(bytes.data(), header, sizeof(header));
  std::memcpy(bytes.data() + 44, bitcode.data(), bitcode.size());
  auto parsed = dxmt::dxil::ModuleInfo::Parse(bytes.data(), bytes.size());
  if (!parsed) { logAllUnhandledErrors(parsed.takeError(), errs()); return false; }
  dxmt::dxbc::ShaderInfo shader;
  dxmt::dxil::PopulateShaderResourceBindings(shader, (*parsed)->resources);
  bool okay = (*parsed)->resources.size() == 5;
  for (const auto &resource : (*parsed)->resources) {
    bool counter = resource.name == "counter", array = resource.name == "array";
    bool read = resource.name == "raw" || resource.name == "named" || counter;
    okay &= resource.read == read && resource.written == (counter || array) &&
        resource.with_counter == counter && !resource.sampled && !resource.compared &&
        resource.globally_coherent == counter && resource.rasterizer_order == counter;
  }
  const auto &counter = shader.uavMap.at(1);
  const auto &array = shader.uavMap.at(2);
  okay &= counter.read && counter.written && counter.with_counter && counter.global_coherent &&
      counter.structure_stride == 4 &&
      counter.rasterizer_order && array.written && !array.read && !array.with_counter &&
      shader.srvMap.at(0).read && !shader.srvMap.at(1).read && shader.srvMap.at(2).read;
  std::printf("LIBRARY_ORIGINS_%s resources=%zu\n", okay ? "PASS" : "FAIL", (*parsed)->resources.size());
  auto *resource = module->getNamedMetadata("dx.resources")->getOperand(0)->getOperand(0).get();
  auto *declaration = cast<MDNode>(cast<MDNode>(resource)->getOperand(0));
  declaration->replaceOperandWith(3, MDString::get(context, "invalid space"));
  bitcode.clear();
  WriteBitcodeToFile(*module, stream);
  bytes.resize(44 + bitcode.size());
  uint32_t total = bytes.size(), payload = bitcode.size();
  std::memcpy(bytes.data() + 24, &total, sizeof(total));
  std::memcpy(bytes.data() + 40, &payload, sizeof(payload));
  std::memcpy(bytes.data() + 44, bitcode.data(), bitcode.size());
  auto malformed = dxmt::dxil::ModuleInfo::Parse(bytes.data(), bytes.size());
  if (malformed) return false;
  consumeError(malformed.takeError());
  return okay;
}

int main(int argc, char **argv) {
  if (argc != 3) { std::fprintf(stderr, "Expected comparison CS and library fixture paths\n"); return 2; }
  bool okay = comparison(argv[1], 0, 13);
  okay &= comparison(argv[2], 108, 78);
  okay &= origins();
  return okay ? 0 : 1;
}
