// Link against production libairconv.a; pass conservative-raster-ps_main.dxbc and .dxil.
#include "airconv_public.h"
#include "metallib_writer.hpp"
#include "dxil/null-descriptor-root_signature.hpp"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const auto root = null_probe::BuildRootSignatureBlob({{}, {0, 0, 0}});
  unsigned cases = 0, failures = 0;
  for (int input = 1; input < argc; ++input) {
    auto bytes = llvm::MemoryBuffer::getFile(argv[input]);
    if (!bytes) return 1;
    for (auto target : {SM50_SHADER_METAL_310, SM50_SHADER_METAL_320, SM50_SHADER_METAL_400, SM50_SHADER_METAL_410}) {
      for (unsigned variant : {0u, unsigned(SM50_MSAA_COVERAGE), unsigned(SM50_MSAA_PIXEL),
                              unsigned(SM50_MSAA_REPLAY), unsigned(SM50_MSAA_SAMPLE_APP),
                              unsigned(SM50_MSAA_COVERAGE | SM50_MSAA_ATOMIC_HELPERS),
                              unsigned(SM50_MSAA_PIXEL | SM50_MSAA_ATOMIC_HELPERS),
                              unsigned(SM50_MSAA_REPLAY | SM50_MSAA_ATOMIC_HELPERS),
                              unsigned(SM50_MSAA_SAMPLE_APP | SM50_MSAA_ATOMIC_HELPERS)}) {
        for (bool atomic : {false, true}) {
          sm50_shader_t shader = nullptr;
          sm50_bitcode_t compiled = nullptr;
          sm50_error_t error = nullptr;
          MTL_SHADER_REFLECTION reflection{};
          SM50_SHADER_COMMON_DATA common{nullptr, SM50_SHADER_COMMON, target, {}};
          SM50_SHADER_COMPILATION_ARGUMENT_DATA phase{&common, SM50_SHADER_D3D12_ATOMIC64_PHASE};
          SM50_MSAA_PIXEL_LAYOUT layout{};
          SM50_SHADER_D3D12_MSAA_PIXEL_DATA msaa{};
          msaa.type = SM50_SHADER_D3D12_MSAA_PIXEL;
          msaa.variant = variant;
          msaa.sample_count = 4;
          msaa.layout_out = &layout;
          msaa.depth_clip_enable = true;
          msaa.next = atomic ? static_cast<void *>(&phase) : &common;
          SM50_SHADER_ROOT_SIGNATURE_DATA rootsig{
            variant ? static_cast<void *>(&msaa) : msaa.next, SM50_SHADER_ROOT_SIGNATURE, root.data(), root.size()};
          const int initialized = SM50Initialize((*bytes)->getBufferStart(), (*bytes)->getBufferSize(),
                                                &shader, &reflection, &error);
          const int result = initialized ? initialized : SM50Compile(shader,
            reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&rootsig), "probe", &compiled, &error);
          if (result) {
            std::fprintf(stderr, "compile input=%d target=%d variant=%u atomic=%u: %s\n", input, target,
              variant, unsigned(atomic), error ? SM50GetErrorMessageString(error).c_str() : "unknown");
            if (error) SM50FreeError(error);
            if (shader) SM50Destroy(shader);
            return 1;
          }
          SM50_COMPILED_BITCODE data{};
          SM50GetCompiledBitcode(compiled, &data);
          dxmt::metallib::MTLBHeader header{};
          if (data.Size < sizeof(header)) return 1;
          std::memcpy(&header, data.Data, sizeof(header));
          if (header.BitcodeOffset > data.Size || header.BitcodeSize > data.Size - header.BitcodeOffset) return 1;
          llvm::LLVMContext context;
          context.setOpaquePointers(false);
          auto module = llvm::parseBitcodeFile(llvm::MemoryBufferRef(llvm::StringRef(
            static_cast<const char *>(data.Data) + header.BitcodeOffset, header.BitcodeSize), "compiled.air"), context);
          if (!module) { llvm::logAllUnhandledErrors(module.takeError(), llvm::errs()); return 1; }
          const auto field = [&](const char *name, unsigned index) -> unsigned {
            auto *metadata = (*module)->getNamedMetadata(name);
            if (!metadata || metadata->getNumOperands() != 1 || metadata->getOperand(0)->getNumOperands() <= index)
              return ~0u;
            auto *value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(metadata->getOperand(0)->getOperand(index));
            return value ? value->getZExtValue() : ~0u;
          };
          const bool promoted = variant && variant != SM50_MSAA_COVERAGE &&
            variant != (SM50_MSAA_COVERAGE | SM50_MSAA_ATOMIC_HELPERS);
          const unsigned expected = atomic || target == SM50_SHADER_METAL_410 ? 410 :
            promoted || target == SM50_SHADER_METAL_400 ? 400 : target == SM50_SHADER_METAL_320 ? 320 : 310;
          const unsigned minor = expected == 410 ? 9 : expected == 400 ? 8 : expected == 320 ? 7 : 6;
          const auto &triple = (*module)->getTargetTriple();
          const bool triple_ok = triple == (expected == 410 ? "air64-apple-macosx27.0.0" :
            expected == 400 ? "air64-apple-macosx26.0.0" :
            expected == 320 ? "air64-apple-macosx15.0.0" : "air64-apple-macosx14.0.0");
          const bool pass = field("air.version", 0) == 2 && field("air.version", 1) == minor &&
            field("air.language_version", 1) == expected / 100 &&
            field("air.language_version", 2) == (expected % 100) / 10 && triple_ok;
          std::printf("MSAA_TARGET input=%d target=%d variant=%u atomic=%u air=2.%u metal=%u.%u expected=%u %s\n",
            input, target, variant, unsigned(atomic), field("air.version", 1),
            field("air.language_version", 1), field("air.language_version", 2), expected, pass ? "PASS" : "FAIL");
          ++cases;
          failures += !pass;
          SM50DestroyBitcode(compiled);
          SM50Destroy(shader);
        }
      }
    }
  }
  std::printf("MSAA_TARGET_%s cases=%u failures=%u\n", failures ? "FAIL" : "PASS", cases, failures);
  return failures ? 1 : 0;
}
