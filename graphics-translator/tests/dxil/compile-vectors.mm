// Native AIR legality check: compile genuine DXC inputs through the production API,
// then ask Metal to build the compute PSO (LLVM verification alone is insufficient).
// usage: compile-vectors <input.dxil> <output.metallib> <root-signature.dxbc>
#include "airconv_public.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 4) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (bytes.empty()) return 2;
  sm50_shader_t shader{};
  sm50_bitcode_t bitcode{};
  sm50_error_t error{};
  SM50_SHADER_COMMON_DATA common{};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = SM50_SHADER_METAL_310;
  std::ifstream roots(argv[3], std::ios::binary);
  std::vector<char> rootBytes{std::istreambuf_iterator<char>(roots), std::istreambuf_iterator<char>()};
  if (rootBytes.empty()) return 2;
  SM50_SHADER_ROOT_SIGNATURE_DATA root{};
  root.next = &common;
  root.type = SM50_SHADER_ROOT_SIGNATURE;
  root.bytecode = rootBytes.data();
  root.bytecode_length = rootBytes.size();
  int failed = SM50Initialize(bytes.data(), bytes.size(), &shader, nullptr, &error);
  if (!failed)
    failed = SM50Compile(shader, reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                         "vectors_main", &bitcode, &error);
  if (failed) {
    char message[4096]{};
    if (error) { SM50GetErrorMessage(error, message, sizeof(message)); SM50FreeError(error); }
    std::fprintf(stderr, "VECTOR_COMPILE_FAIL %s: %s\n", argv[1], message);
    if (shader) SM50Destroy(shader);
    return 1;
  }
  SM50_COMPILED_BITCODE result{};
  SM50GetCompiledBitcode(bitcode, &result);
  std::ofstream output(argv[2], std::ios::binary);
  output.write(static_cast<const char *>(result.Data), result.Size);
  bool okay = output.good();
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    dispatch_data_t data = dispatch_data_create(result.Data, result.Size,
        dispatch_get_main_queue(), ^{});
    NSError *nativeError = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&nativeError];
    id<MTLFunction> function = [library newFunctionWithName:@"vectors_main"];
    id<MTLComputePipelineState> pipeline = function
        ? [device newComputePipelineStateWithFunction:function error:&nativeError] : nil;
    if (!device || !library || !pipeline) {
      std::fprintf(stderr, "VECTOR_METAL_FAIL %s: %s\n", argv[1],
          nativeError ? nativeError.localizedDescription.UTF8String : "missing device/function/pipeline");
      okay = false;
    } else {
      std::printf("VECTOR_METAL_PASS input=%s bytes=%llu threads=%lu\n", argv[1],
          static_cast<unsigned long long>(result.Size),
          static_cast<unsigned long>(pipeline.maxTotalThreadsPerThreadgroup));
    }
    [pipeline release]; [function release]; [library release]; [device release];
    dispatch_release(data);
  }
  SM50DestroyBitcode(bitcode);
  SM50Destroy(shader);
  return okay ? 0 : 1;
}
