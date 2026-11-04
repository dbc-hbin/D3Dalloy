// Link against production libairconv.a; pass conservative-raster-ps_main.dxbc and .dxil.
#include "airconv_public.h"
#include "airconv_context.hpp"
#include "air_signature.hpp"
#include "metallib_writer.hpp"
#include "msaa_vertex.hpp"
#include "dxil/null-descriptor-root_signature.hpp"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <cstring>

// Task batching must preserve the entire legacy replay geometry, including all eight native clip planes.
// Identical AIR also excludes new wireframe edges and changes to the native interpolation plane.
static bool vertexTasks() {
  using namespace llvm;
  unsigned cases = 0, equivalent = 0;
  for (unsigned clips : {0u, 4u, 5u, 8u}) for (unsigned geometry = 0; geometry < 5; ++geometry) {
    std::string legacy;
    for (bool task : {false, true}) {
      LLVMContext context;
      context.setOpaquePointers(false);
      Module module("msaa-task-vertex", context);
      d3dalloy::initializeModule(module);
      d3dalloy::air::FunctionSignatureBuilder signature;
      signature.DefineInput(d3dalloy::air::InputVertexID{});
      signature.DefineInput(d3dalloy::air::InputBaseVertex{});
      signature.DefineInput(d3dalloy::air::InputInstanceID{});
      signature.DefineInput(d3dalloy::air::InputBaseInstance{});
      signature.DefineOutput(d3dalloy::air::OutputPosition{.type = d3dalloy::air::msl_float4});
      if (clips) signature.DefineOutput(d3dalloy::air::OutputClipDistance{clips});
      auto [body, metadata] = signature.CreateFunction("probe", context, module, 0, false);
      IRBuilder<> builder(BasicBlock::Create(context, "entry", body));
      builder.CreateCall(module.getOrInsertFunction("app.vertex.side_effect",
        FunctionType::get(builder.getVoidTy(), {}, false)));
      builder.CreateRet(Constant::getNullValue(body->getReturnType()));
      module.getOrInsertNamedMetadata("air.vertex")->addOperand(metadata);
      const unsigned originalWords = module.getDataLayout().getTypeAllocSize(body->getReturnType()).getFixedValue() / 4;
      SM50_MSAA_INTERPOLATION_LAYOUT interpolation{};
      SM50_MSAA_VERTEX_LAYOUT layout{};
      SM50_SHADER_D3D12_MSAA_VERTEX_DATA options{};
      options.variant = SM50_MSAA_VERTEX_REPLAY | (task ? SM50_MSAA_GPU_TILE_TASKS : 0);
      options.layout_out = &layout;
      options.line_mode = geometry < 4 ? geometry : SM50_MSAA_LINE_NONE;
      options.interpolation = &interpolation;
      options.depth_clip_enable = true;
      auto error = d3dalloy::msaa::transformVertex(module, options, geometry == 4);
      if (error) { logAllUnhandledErrors(std::move(error), errs()); return false; }
      if (verifyModule(module, &errs()) || layout.record_dwords != originalWords ||
          layout.clip_distance_count != clips ||
          !(layout.flags & SM50_MSAA_VERTEX_GPU_TILE_TASKS_SUPPORTED) ||
          !module.getFunction("app.vertex.side_effect")->use_empty()) return false;
      auto *entry = module.getNamedMetadata("air.vertex")->getOperand(0);
      unsigned nativePlanes = 0, clipArrays = 0;
      for (const auto &operand : cast<MDNode>(entry->getOperand(1))->operands()) {
        auto *field = cast<MDNode>(operand.get());
        if (cast<MDString>(field->getOperand(0))->getString() != "air.clip_distance") continue;
        ++clipArrays;
        nativePlanes += mdconst::extract<ConstantInt>(field->getOperand(2))->getZExtValue();
      }
      bool taskBinding = false;
      for (const auto &operand : cast<MDNode>(entry->getOperand(2))->operands()) {
        auto *field = cast<MDNode>(operand.get());
        for (unsigned i = 0; i + 1 < field->getNumOperands(); ++i) {
          auto *tag = dyn_cast<MDString>(field->getOperand(i));
          if (!tag || tag->getString() != "air.location_index") continue;
          taskBinding |= mdconst::extract<ConstantInt>(field->getOperand(i + 1))->getZExtValue() == 12;
        }
      }
      if (nativePlanes != clips || clipArrays != unsigned(clips != 0) || taskBinding)
        return false;
      std::string ir;
      raw_string_ostream stream(ir);
      module.print(stream, nullptr);
      stream.flush();
      if (!task) legacy = ir;
      else {
        if (ir != legacy) {
          std::fprintf(stderr, "task replay changed native geometry: clips=%u geometry=%u\n", clips, geometry);
          return false;
        }
        ++equivalent;
      }
      ++cases;
    }
  }
  std::printf("MSAA_VERTEX_TASK_PASS cases=%u identical_legacy_pairs=%u clip_max=8 task_binding12=0\n",
              cases, equivalent);
  return true;
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  if (!vertexTasks()) return 1;
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
                              unsigned(SM50_MSAA_SAMPLE_APP | SM50_MSAA_ATOMIC_HELPERS),
                              unsigned(SM50_MSAA_COVERAGE | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_PIXEL | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_REPLAY | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_SAMPLE_APP | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_COVERAGE | SM50_MSAA_ATOMIC_HELPERS | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_PIXEL | SM50_MSAA_ATOMIC_HELPERS | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_REPLAY | SM50_MSAA_ATOMIC_HELPERS | SM50_MSAA_GPU_TILE_TASKS),
                              unsigned(SM50_MSAA_SAMPLE_APP | SM50_MSAA_ATOMIC_HELPERS | SM50_MSAA_GPU_TILE_TASKS)}) {
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
          d3dalloy::metallib::MTLBHeader header{};
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
          const unsigned role = variant & ~(SM50_MSAA_ATOMIC_HELPERS | SM50_MSAA_GPU_TILE_TASKS);
          const bool promoted = role && role != SM50_MSAA_COVERAGE;
          const unsigned expected = atomic || target == SM50_SHADER_METAL_410 ? 410 :
            promoted || target == SM50_SHADER_METAL_400 ? 400 : target == SM50_SHADER_METAL_320 ? 320 : 310;
          const unsigned minor = expected == 410 ? 9 : expected == 400 ? 8 : expected == 320 ? 7 : 6;
          const auto &triple = (*module)->getTargetTriple();
          const bool triple_ok = triple == (expected == 410 ? "air64-apple-macosx27.0.0" :
            expected == 400 ? "air64-apple-macosx26.0.0" :
            expected == 320 ? "air64-apple-macosx15.0.0" : "air64-apple-macosx14.0.0");
          const bool task = variant & SM50_MSAA_GPU_TILE_TASKS;
          const bool task_layout = bool(layout.flags & SM50_MSAA_PIXEL_GPU_TILE_TASKS) == task;
          const bool array_coverage = !task || role == SM50_MSAA_COVERAGE ||
            (*module)->getFunction("air.read_texture_2d_ms_array.u.v4i32");
          const bool verified = !llvm::verifyModule(**module, &llvm::errs());
          const bool pass = verified && task_layout && array_coverage &&
            field("air.version", 0) == 2 && field("air.version", 1) == minor &&
            field("air.language_version", 1) == expected / 100 &&
            field("air.language_version", 2) == (expected % 100) / 10 && triple_ok;
          std::printf("MSAA_TARGET input=%d target=%d variant=%u atomic=%u air=2.%u metal=%u.%u expected=%u task=%u ir=%u array=%u %s\n",
            input, target, variant, unsigned(atomic), field("air.version", 1),
            field("air.language_version", 1), field("air.language_version", 2), expected,
            unsigned(task), unsigned(verified), unsigned(array_coverage), pass ? "PASS" : "FAIL");
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
