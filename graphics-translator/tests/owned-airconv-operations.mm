#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "airconv_context.hpp"
#include "metallib_writer.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>

// dxil_converter.hpp includes Windows BOOL, which conflicts with Objective-C.
namespace d3dalloy::dxil {
llvm::Error LowerOperations(llvm::Module &module);
}

// The public Metal frontend supplies the kernel ABI only. Every result below
// comes from a real dx.op call lowered by the production compiler, serialized
// by its metallib writer and executed on the GPU.
int main(int argc, char **argv) {
  if (argc != 3) return 2; // contract.ll, bits|wide|carry
  const bool bits = std::strcmp(argv[2], "bits") == 0;
  const bool carry = std::strcmp(argv[2], "carry") == 0;
  if (!bits && !carry && std::strcmp(argv[2], "wide") != 0) return 2;
  llvm::LLVMContext context;
  context.setOpaquePointers(false);
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(argv[1], diagnostic, context);
  if (!module) { diagnostic.print(argv[0], llvm::errs()); return 1; }
  auto *function = module->getFunction("arithmetic");
  if (!function || function->arg_size() != 3) return 1;
  function->deleteBody();
  function->setLinkage(llvm::GlobalValue::ExternalLinkage);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", function));
  auto *i32 = b.getInt32Ty();
  auto *index = function->getArg(2);
  std::array<llvm::Value *, 4> input;
  for (unsigned i = 0; i < input.size(); ++i)
    input[i] = b.CreateLoad(i32, b.CreateGEP(i32, function->getArg(0),
        b.CreateAdd(b.CreateMul(index, b.getInt32(input.size())), b.getInt32(i))));
  auto call = [&](const char *name, unsigned opcode, llvm::Type *result, llvm::ArrayRef<llvm::Value *> args) {
    llvm::SmallVector<llvm::Type *, 5> types(args.size() + 1, i32);
    llvm::SmallVector<llvm::Value *, 5> values{b.getInt32(opcode)};
    values.append(args.begin(), args.end());
    return b.CreateCall(module->getOrInsertFunction(name, llvm::FunctionType::get(result, types, false)), values);
  };
  llvm::SmallVector<llvm::Value *, 6> results;
  if (bits) {
    results.push_back(call("dx.op.tertiary.i32", 51, i32, {input[2], input[3], input[0]}));
    results.push_back(call("dx.op.tertiary.i32", 52, i32, {input[2], input[3], input[0]}));
    results.push_back(call("dx.op.quaternary.i32", 53, i32, {input[2], input[3], input[0], input[1]}));
  } else {
    auto *type = llvm::StructType::get(context, {i32, carry ? b.getInt1Ty() : i32});
    for (unsigned opcode : carry ? std::vector<unsigned>{44, 45} : std::vector<unsigned>{41, 42, 43}) {
      auto *value = call(carry ? "dx.op.binaryWithCarryOrBorrow.i32" : "dx.op.binaryWithTwoOuts.i32",
                         opcode, type, {input[0], input[1]});
      results.push_back(b.CreateExtractValue(value, 0));
      results.push_back(b.CreateZExtOrTrunc(b.CreateExtractValue(value, 1), i32));
    }
  }
  for (unsigned i = 0; i < results.size(); ++i)
    b.CreateStore(results[i], b.CreateGEP(i32, function->getArg(1),
        b.CreateAdd(b.CreateMul(index, b.getInt32(results.size())), b.getInt32(i))));
  b.CreateRetVoid();
  if (auto error = d3dalloy::dxil::LowerOperations(*module)) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "LOWER_FAIL: "); return 1;
  }
  d3dalloy::runOptimizationPasses(*module);
  if (llvm::verifyModule(*module, &llvm::errs())) return 1;
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  d3dalloy::metallib::MetallibWriter writer;
  if (auto error = writer.Write(*module, stream)) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs(), "WRITE_FAIL: "); return 1;
  }
  constexpr std::array<uint32_t, 8> values{0, 1, 0x80000000, 0xffffffff, 0x12345678, 0xfedcba98, 0x7fffffff, 0x55555555};
  std::vector<std::array<uint32_t, 4>> inputs;
  std::vector<uint32_t> expected;
  for (auto x : values) for (unsigned width = 0; width < (bits ? 64u : values.size()); ++width)
    for (unsigned offset = 0; offset < (bits ? 64u : 1u); ++offset) {
      const uint32_t y = bits ? ~x : values[width];
      if (!bits && !carry && y == 0) continue; // division by zero is not the contract under test
      inputs.push_back({x, y, width, offset});
      if (bits) {
        const unsigned shift = offset & 31, count = std::min(width & 31, 32 - shift);
        const uint32_t mask = count ? uint32_t((uint64_t(1) << count) - 1) : 0;
        const uint32_t u = (x >> shift) & mask;
        const uint32_t s = count && (u & (1u << (count - 1))) ? u | ~mask : u;
        expected.insert(expected.end(), {s, u, (y & ~(mask << shift)) | ((x & mask) << shift)});
      } else if (carry) {
        expected.insert(expected.end(), {x + y, uint32_t(uint64_t(x) + y > UINT32_MAX), x - y, uint32_t(x < y)});
      } else {
        const auto signed_product = uint64_t(int64_t(std::bit_cast<int32_t>(x)) * std::bit_cast<int32_t>(y));
        const auto unsigned_product = uint64_t(x) * y;
        expected.insert(expected.end(), {uint32_t(signed_product >> 32), uint32_t(signed_product),
            uint32_t(unsigned_product >> 32), uint32_t(unsigned_product), x / y, x % y});
      }
    }
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    auto data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    auto library = [device newLibraryWithData:data error:&error];
    auto kernel = [library newFunctionWithName:@"arithmetic"];
    auto pipeline = kernel ? [device newComputePipelineStateWithFunction:kernel error:&error] : nil;
    if (!pipeline) { fprintf(stderr, "PIPELINE_FAIL %s\n", error.localizedDescription.UTF8String); return 1; }
    auto in = [device newBufferWithBytes:inputs.data() length:inputs.size() * sizeof(inputs[0]) options:MTLResourceStorageModeShared];
    auto out = [device newBufferWithLength:expected.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    std::memset(out.contents, 0xcd, out.length);
    auto queue = [device newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:in offset:0 atIndex:0];
    [encoder setBuffer:out offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(inputs.size(), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      fprintf(stderr, "GPU_FAIL %s\n", command.error.localizedDescription.UTF8String); return 1;
    }
    auto *actual = static_cast<const uint32_t *>(out.contents);
    unsigned mismatches = 0;
    for (unsigned i = 0; i < expected.size(); ++i) if (actual[i] != expected[i]) {
      if (mismatches < 8) fprintf(stderr, "MISMATCH mode=%s case=%u field=%u actual=%08x expected=%08x\n",
          argv[2], unsigned(i / results.size()), unsigned(i % results.size()), actual[i], expected[i]);
      ++mismatches;
    }
    printf("AIRCONV_OPERATIONS mode=%s cases=%zu values=%zu mismatches=%u %s\n", argv[2], inputs.size(),
        expected.size(), mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
  }
}
