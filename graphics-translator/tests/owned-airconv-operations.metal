#include <metal_stdlib>
using namespace metal;

// Metal supplies only the native kernel ABI. The consumer replaces this body
// with DXIL calls and runs the production LowerOperations and metallib writer.
kernel void arithmetic(device const uint *input [[buffer(0)]],
                       device uint *output [[buffer(1)]],
                       uint index [[thread_position_in_grid]]) {
  output[index] = input[index];
}
