// Native owned-compiler GPU proof for the D3D12 null / out-of-range descriptor
// contract.
//
// Drives the owned DXIL front end through the public SM50 API (the same calls
// the runtime makes), loads the produced metallib on the real Metal device and
// executes the fixture against a root signature whose CBV/SRV/UAV heap holds
// production-valid null views. Nothing falls back to a foreign compiler: if the
// owned front end cannot compile the shader the probe fails with its error text.
//
// The descriptor heap is filled exactly as src/d3d12/d3d12_descriptor_heap.cpp
// fills its shader-visible storage (32 bytes per slot):
//
//   +0  handle / device address    +8  metadata
//   +16 aux (UAV counter)          +24 padding
//
// usage: null-descriptor-probe <null-descriptor.dxil>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>

#include "airconv_public.h"

#include "null-descriptor-root_signature.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

using namespace null_probe;

constexpr uint32_t kRootArgumentsSlot = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
constexpr uint32_t kDescriptorSlots = 6;
constexpr uint32_t kDescriptorBytes = 32;
constexpr uint64_t kOobBufferBytes = 64 * 1024;
constexpr uint64_t kOobViewBytes = 16;   // the view covers 16 bytes of the 64 KiB allocation
constexpr uint64_t kOobStoreOffset = 4096;
constexpr uint64_t kResultBytes = 64;
constexpr uint32_t kSentinel = 0xaaaaaaaau;

/* Result words the fixture writes, in dword order. */
enum ResultWord : unsigned {
  kNullCbv = 0,
  kNullRaw = 1,
  kNullStructured = 2,
  kControlEarly = 3,
  kNullCounter = 4,
  kOobCounter = 5,
  kControlLate = 6,
  kRootDescriptor = 7,
};

int fails = 0;

void fail(const char *message) {
  std::fprintf(stderr, "NULL_DESCRIPTOR_PROBE_FAIL %s\n", message);
  fails += 1;
}

std::vector<uint8_t> load(const char *path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/* One shader-visible descriptor, layout-compatible with the runtime's
 * ShaderVisibleDescriptorGPUStorage. */
struct DescriptorSlot {
  uint64_t pointer;
  uint64_t metadata;
  uint64_t aux;
  uint64_t padding;
};
static_assert(sizeof(DescriptorSlot) == kDescriptorBytes);

/* A D3D12 null view: no device address and no metadata, so the descriptor
 * declares a zero-byte resource. */
constexpr DescriptorSlot kNullDescriptor{0, 0, 0, 0};

struct CompiledShader {
  id<MTLFunction> function = nil;
  MTL_SHADER_REFLECTION reflection{};
};

bool CompileShader(
    id<MTLDevice> device, const std::vector<uint8_t> &bytecode, const std::vector<uint8_t> &root_signature,
    CompiledShader &out
) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  if (SM50Initialize(bytecode.data(), bytecode.size(), &shader, &out.reflection, &error)) {
    std::fprintf(
        stderr, "NULL_DESCRIPTOR_PROBE_FAIL SM50Initialize: %s\n",
        error ? SM50GetErrorMessageString(error).c_str() : "no diagnostic"
    );
    if (error)
      SM50FreeError(error);
    fails += 1;
    return false;
  }

  SM50_SHADER_COMMON_DATA common{};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = SM50_SHADER_METAL_310;

  SM50_SHADER_ROOT_SIGNATURE_DATA rootsig{};
  rootsig.next = &common;
  rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
  rootsig.bytecode = root_signature.data();
  rootsig.bytecode_length = root_signature.size();

  sm50_bitcode_t bitcode = nullptr;
  if (SM50Compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig, "cs_main", &bitcode, &error)) {
    std::fprintf(
        stderr, "NULL_DESCRIPTOR_PROBE_FAIL SM50Compile: %s\n",
        error ? SM50GetErrorMessageString(error).c_str() : "no diagnostic"
    );
    if (error)
      SM50FreeError(error);
    SM50Destroy(shader);
    fails += 1;
    return false;
  }

  SM50_COMPILED_BITCODE compiled{};
  SM50GetCompiledBitcode(bitcode, &compiled);
  std::printf("NULL_DESCRIPTOR_PROBE_COMPILE metallib_bytes=%llu\n",
              static_cast<unsigned long long>(compiled.Size));
  dispatch_data_t data =
      dispatch_data_create(compiled.Data, compiled.Size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError *metal_error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:data error:&metal_error];
  SM50DestroyBitcode(bitcode);
  SM50Destroy(shader);
  if (!library) {
    std::fprintf(stderr, "NULL_DESCRIPTOR_PROBE_FAIL metal library rejected: %s\n",
                 metal_error ? metal_error.localizedDescription.UTF8String : "unknown");
    fails += 1;
    return false;
  }
  out.function = [library newFunctionWithName:@"cs_main"];
  if (!out.function) {
    fail("metallib has no entry named cs_main");
    return false;
  }
  return true;
}

/** The fixture's root signature: one CBV/SRV/UAV table plus a root UAV. */
SignatureSpec BuildSignature() {
  SignatureSpec spec;
  TableSpec table;
  table.visibility = kVisibilityAll;
  table.ranges = {
      {kRangeCbv, /*base_shader_register=*/0, /*register_space=*/0, /*num_descriptors=*/1},
      {kRangeSrv, 0, 0, 2},
      {kRangeUav, 0, 0, 3},
  };
  spec.tables.push_back(table);
  spec.root_uav = {kVisibilityAll, /*shader_register=*/3, /*register_space=*/0};
  return spec;
}

/** Table slots, in the order the ranges append them. */
enum TableSlot : unsigned {
  kSlotCbv = 0,
  kSlotNullRaw = 1,
  kSlotNullStructured = 2,
  kSlotNullUav = 3,
  kSlotOobUav = 4,
  kSlotResult = 5,
};

bool ExpectWord(const char *what, uint32_t expected, uint32_t actual) {
  std::printf("NULL_DESCRIPTOR_PROBE_WORD %s expected=0x%08x actual=0x%08x\n", what, expected, actual);
  if (expected != actual) {
    fail(what);
    return false;
  }
  return true;
}

int RunProbe(id<MTLDevice> device, id<MTLCommandQueue> queue, const std::vector<uint8_t> &bytecode) {
  const auto root_signature = BuildRootSignatureBlob(BuildSignature());

  CompiledShader shader;
  if (!CompileShader(device, bytecode, root_signature, shader))
    return 1;

  NSError *error = nil;
  id<MTLComputePipelineState> pipeline =
      [device newComputePipelineStateWithFunction:shader.function error:&error];
  if (!pipeline) {
    std::fprintf(stderr, "NULL_DESCRIPTOR_PROBE_FAIL compute pipeline: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown");
    fails += 1;
    return 1;
  }

  id<MTLBuffer> heap = [device newBufferWithLength:kDescriptorSlots * kDescriptorBytes
                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> oob = [device newBufferWithLength:kOobBufferBytes options:MTLResourceStorageModeShared];
  id<MTLBuffer> result = [device newBufferWithLength:kResultBytes options:MTLResourceStorageModeShared];
  id<MTLBuffer> root_uav = [device newBufferWithLength:kDescriptorBytes options:MTLResourceStorageModeShared];

  std::memset([heap contents], 0, kDescriptorSlots * kDescriptorBytes);
  auto *slots = static_cast<DescriptorSlot *>([heap contents]);
  slots[kSlotCbv] = kNullDescriptor;
  slots[kSlotNullRaw] = kNullDescriptor;
  slots[kSlotNullStructured] = kNullDescriptor;
  slots[kSlotNullUav] = kNullDescriptor;
  /* Bound to a 64 KiB allocation, but the view covers only its first 16 bytes. */
  slots[kSlotOobUav] = {oob.gpuAddress, kOobViewBytes, 0, 0};
  slots[kSlotResult] = {result.gpuAddress, kResultBytes, 0, 0};

  std::memset([result contents], 0, kResultBytes);
  std::memset([root_uav contents], 0, kDescriptorBytes);
  for (uint64_t i = 0; i < kOobBufferBytes / sizeof(uint32_t); i++)
    static_cast<uint32_t *>([oob contents])[i] = kSentinel;

  uint64_t root_arguments[2] = {heap.gpuAddress, root_uav.gpuAddress};
  id<MTLBuffer> arguments = [device newBufferWithBytes:root_arguments
                                               length:sizeof(root_arguments)
                                              options:MTLResourceStorageModeShared];

  const uint32_t threadgroup_x = shader.reflection.ThreadgroupSize[0];
  const uint32_t threadgroup_y = shader.reflection.ThreadgroupSize[1];
  const uint32_t threadgroup_z = shader.reflection.ThreadgroupSize[2];
  if (!threadgroup_x || !threadgroup_y || !threadgroup_z) {
    fail("compute reflection reported an empty thread group");
    return 1;
  }

  id<MTLCommandBuffer> command = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:arguments offset:0 atIndex:kRootArgumentsSlot];
  [encoder useResource:heap usage:MTLResourceUsageRead];
  [encoder useResource:oob usage:MTLResourceUsageRead | MTLResourceUsageWrite];
  [encoder useResource:result usage:MTLResourceUsageRead | MTLResourceUsageWrite];
  [encoder useResource:root_uav usage:MTLResourceUsageRead | MTLResourceUsageWrite];
  [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threadgroup_x, threadgroup_y, threadgroup_z)];
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted) {
    std::fprintf(stderr, "NULL_DESCRIPTOR_PROBE_FAIL command buffer status=%ld error=%s\n",
                 static_cast<long>(command.status),
                 command.error ? command.error.localizedDescription.UTF8String : "none");
    fails += 1;
    return 1;
  }

  const auto *words = static_cast<const uint32_t *>([result contents]);
  ExpectWord("null_cbv_read", 0x00000000u, words[kNullCbv]);
  ExpectWord("null_raw_srv_read", 0x00000000u, words[kNullRaw]);
  ExpectWord("null_structured_srv_read", 0x00000000u, words[kNullStructured]);
  ExpectWord("control_early", 0x11223344u, words[kControlEarly]);
  ExpectWord("null_uav_atomic_returns_zero", 0x00000000u, words[kNullCounter]);
  ExpectWord("out_of_range_uav_atomic_returns_zero", 0x00000000u, words[kOobCounter]);
  ExpectWord("control_late", 0xa5a5a5a5u, words[kControlLate]);
  ExpectWord("root_descriptor_roundtrip", 0xcafef00du, words[kRootDescriptor]);

  const auto *oob_words = static_cast<const uint32_t *>([oob contents]);
  ExpectWord("out_of_range_store_dropped", kSentinel,
             oob_words[kOobStoreOffset / sizeof(uint32_t)]);
  ExpectWord("out_of_range_store_not_redirected", kSentinel, oob_words[0]);
  ExpectWord("root_descriptor_store_landed", 0xcafef00du,
             static_cast<const uint32_t *>([root_uav contents])[0]);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: null-descriptor-probe <null-descriptor.dxil>\n");
    return 2;
  }
  const auto bytecode = load(argv[1]);
  if (bytecode.empty()) {
    std::fprintf(stderr, "NULL_DESCRIPTOR_PROBE_FAIL empty shader input\n");
    return 2;
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "NULL_DESCRIPTOR_PROBE_FAIL no Metal device\n");
      return 2;
    }
    std::printf("NULL_DESCRIPTOR_PROBE_DEVICE name=%s\n", device.name.UTF8String);
    RunProbe(device, [device newCommandQueue], bytecode);
  }

  std::printf("NULL_DESCRIPTOR_PROBE_RESULT pass=%d\n", fails == 0 ? 1 : 0);
  if (fails == 0)
    std::puts("NULL_DESCRIPTOR_PROBE_PASS");
  return fails == 0 ? 0 : 1;
}
