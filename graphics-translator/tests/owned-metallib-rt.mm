// GPU proof that the owned DXMT metallib writer emits a library Metal accepts
// and executes.
//
// The input is the AIR contract fixture graphics-translator/tests/owned-metallib-rt.metal,
// produced by the public Metal frontend. The production DXIL path never invokes
// that frontend; the serialized library under test is emitted by our own
// dxmt::metallib::MetallibWriter. Both rays are run on a real device with Metal
// API and GPU validation enabled.
//
// This is a contract proof, not a smoke test: both rays must read back exact
// bytes. Ray 0 reaches the intersection function, which accepts it, so the
// closest hit reports type=1, exactly one callback, barycentric x=0.5,
// distance=2 and the instance userID 37. Ray 1 also invokes the callback
// exactly once (barycentric x=0.25) but is rejected, so traversal reports a
// miss (type=0) while the callback count still proves the intersection function
// ran. Every failure names the stage that failed and the Metal error, because a
// silently missed ray and a rejected ray both read back as type=0.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "metallib_writer.hpp"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// Mirrors Payload/Result in owned-metallib-rt.metal.
struct Result {
  uint32_t type, calls;
  float bary, distance;
  uint32_t primitive, geometry, instance, userID;
};

static int fail(int code, const char *stage, NSString *detail) {
  fprintf(stderr, "OWNED_RT_FAIL %s: %s\n", stage, (detail ?: @"unknown").UTF8String);
  return code;
}

int main(int argc, char **argv) {
  if (argc != 2 || !argv[1] || !argv[1][0]) {
    fprintf(stderr, "usage: %s <contract.ll>\n", argv[0]);
    return 2;
  }
  llvm::LLVMContext context;
  context.setOpaquePointers(false);
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(argv[1], diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], llvm::errs());
    fprintf(stderr, "OWNED_RT_FAIL parse_ir: %s\n", argv[1]);
    return 3;
  }
  llvm::SmallVector<char, 0> libraryBytes;
  llvm::raw_svector_ostream stream(libraryBytes);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(*module, stream);

  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    auto data = dispatch_data_create(libraryBytes.data(), libraryBytes.size(), nullptr,
                                     DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    auto library = [device newLibraryWithData:data error:&error];
    if (!library) return fail(4, "newLibraryWithData", error.localizedDescription);
    auto kernel = [library newFunctionWithName:@"trace_contract"];
    auto intersection = [library newFunctionWithName:@"accept_triangle"];
    auto visible = [library newFunctionWithName:@"marker"];
    printf("OWNED_RT_EXPORTS kernel=%d intersection=%d visible=%d\n",
           kernel != nil, intersection != nil, visible != nil);
    if (!kernel || !intersection || !visible)
      return fail(5, "exports", @"the serialized library is missing an expected export");
    auto descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = kernel;
    descriptor.linkedFunctions = [MTLLinkedFunctions new];
    descriptor.linkedFunctions.functions = @[ intersection, visible ];
    auto pipeline = [device newComputePipelineStateWithDescriptor:descriptor
                                                         options:MTLPipelineOptionNone
                                                      reflection:nil
                                                           error:&error];
    if (!pipeline) return fail(6, "newComputePipelineState", error.localizedDescription);
    auto tableDesc = [MTLIntersectionFunctionTableDescriptor new];
    tableDesc.functionCount = 1;
    auto table = [pipeline newIntersectionFunctionTableWithDescriptor:tableDesc];
    [table setFunction:[pipeline functionHandleWithFunction:intersection] atIndex:0];

    const float vertices[] = { -1, -1, 2, 1, -1, 2, 0, 1, 2 };
    auto vertex = [device newBufferWithBytes:vertices length:sizeof(vertices)
                                     options:MTLResourceStorageModeShared];
    auto triangle = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    triangle.vertexBuffer = vertex;
    triangle.vertexStride = 12;
    triangle.vertexFormat = MTLAttributeFormatFloat3;
    triangle.triangleCount = 1;
    triangle.opaque = NO;
    triangle.allowDuplicateIntersectionFunctionInvocation = NO;
    auto primitive = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    primitive.geometryDescriptors = @[ triangle ];
    auto bsize = [device accelerationStructureSizesWithDescriptor:primitive];
    auto blas = [device newAccelerationStructureWithSize:bsize.accelerationStructureSize];
    auto bscratch = [device newBufferWithLength:bsize.buildScratchBufferSize
                                        options:MTLResourceStorageModePrivate];
    auto queue = [device newCommandQueue];
    auto build = [queue commandBuffer];
    auto bencoder = [build accelerationStructureCommandEncoder];
    [bencoder buildAccelerationStructure:blas descriptor:primitive
                            scratchBuffer:bscratch scratchBufferOffset:0];
    [bencoder endEncoding];
    [build commit];
    [build waitUntilCompleted];
    if (build.status != MTLCommandBufferStatusCompleted)
      return fail(7, "build_blas", [NSString stringWithFormat:@"status=%lu %@",
                                    (unsigned long)build.status,
                                    build.error.localizedDescription]);

    MTLAccelerationStructureUserIDInstanceDescriptor instance{};
    instance.transformationMatrix.columns[0].x = 1;
    instance.transformationMatrix.columns[1].y = 1;
    instance.transformationMatrix.columns[2].z = 1;
    instance.mask = 0xff;
    instance.userID = 37;
    auto instances = [device newBufferWithBytes:&instance length:sizeof(instance)
                                        options:MTLResourceStorageModeShared];
    auto scene = [MTLInstanceAccelerationStructureDescriptor descriptor];
    scene.instancedAccelerationStructures = @[ blas ];
    scene.instanceDescriptorBuffer = instances;
    scene.instanceCount = 1;
    scene.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    scene.instanceDescriptorStride = sizeof(instance);
    auto tsize = [device accelerationStructureSizesWithDescriptor:scene];
    auto tlas = [device newAccelerationStructureWithSize:tsize.accelerationStructureSize];
    auto tscratch = [device newBufferWithLength:tsize.buildScratchBufferSize
                                        options:MTLResourceStorageModePrivate];
    auto tbuild = [queue commandBuffer];
    auto tencoder = [tbuild accelerationStructureCommandEncoder];
    [tencoder buildAccelerationStructure:tlas descriptor:scene
                           scratchBuffer:tscratch scratchBufferOffset:0];
    [tencoder endEncoding];
    [tbuild commit];
    [tbuild waitUntilCompleted];
    if (tbuild.status != MTLCommandBufferStatusCompleted)
      return fail(8, "build_tlas", [NSString stringWithFormat:@"status=%lu %@",
                                    (unsigned long)tbuild.status,
                                    tbuild.error.localizedDescription]);

    auto output = [device newBufferWithLength:2 * sizeof(Result)
                                      options:MTLResourceStorageModeShared];
    memset(output.contents, 0xff, 2 * sizeof(Result));
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setAccelerationStructure:tlas atBufferIndex:0];
    [encoder setIntersectionFunctionTable:table atBufferIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    [encoder useResource:blas usage:MTLResourceUsageRead];
    [encoder dispatchThreadgroups:MTLSizeMake(2, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
      return fail(9, "trace_contract", [NSString stringWithFormat:@"status=%lu %@",
                                        (unsigned long)command.status,
                                        command.error.localizedDescription]);

    auto result = static_cast<const Result *>(output.contents);
    for (unsigned i = 0; i < 2; ++i)
      printf("OWNED_RT_RAY %u type=%u calls=%u bary=%.6f distance=%.6f primitive=%u "
             "geometry=%u instance=%u userID=%u\n",
             i, result[i].type, result[i].calls, result[i].bary, result[i].distance,
             result[i].primitive, result[i].geometry, result[i].instance, result[i].userID);
    bool hit = result[0].type == 1 && result[0].calls == 1 &&
               fabs(result[0].bary - 0.5f) < 1e-5 && fabs(result[0].distance - 2.0f) < 1e-5 &&
               result[0].primitive == 0 && result[0].geometry == 0 && result[0].instance == 0 &&
               result[0].userID == 37;
    bool rejected = result[1].type == 0 && result[1].calls == 1 &&
                    fabs(result[1].bary - 0.25f) < 1e-5;
    if (!hit || !rejected)
      return fail(10, "readback", [NSString stringWithFormat:@"hit=%d rejected=%d", hit, rejected]);
    printf("OWNED_RT_PASS\n");
    return 0;
  }
}
