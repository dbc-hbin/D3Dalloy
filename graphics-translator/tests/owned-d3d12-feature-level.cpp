// Feature level 11_1 / 12_0 requirements, verified on the GPU with exact expected values.
// Each case prints OWNED_D3D12_FEATURE_LEVEL_CASE_PASS <name>; the final line is
// OWNED_D3D12_FEATURE_LEVEL_PASS cases=N max=0x....
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_cases = 0;

[[noreturn]] void fail(const char *step, HRESULT actual = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_FEATURE_LEVEL_FAIL step=%s hr=%08lx\n", step,
               static_cast<unsigned long>(actual));
  throw std::runtime_error(step);
}
void check(HRESULT actual, const char *step) {
  if (FAILED(actual)) fail(step, actual);
}
void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) fail(step, actual);
}
void passed(const std::string &name) {
  ++g_cases;
  std::printf("OWNED_D3D12_FEATURE_LEVEL_CASE_PASS %s\n", name.c_str());
}

ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = heap;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)),
        "buffer");
  return result;
}

ComPtr<ID3D12Resource> texture(ID3D12Device *device, DXGI_FORMAT format, UINT width, UINT height, UINT samples,
                               D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = samples;
  desc.Format = format;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)),
        "texture");
  return result;
}

std::vector<unsigned char> readback(ID3D12Resource *resource, size_t size) {
  void *mapped = nullptr;
  D3D12_RANGE range{0, size};
  check(resource->Map(0, &range, &mapped), "Map(readback)");
  std::vector<unsigned char> result(size);
  std::memcpy(result.data(), mapped, size);
  D3D12_RANGE none{};
  resource->Unmap(0, &none);
  return result;
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}

ComPtr<ID3DBlob> shader(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> bytes, errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), "owned-feature-level", nullptr, nullptr, entry, target,
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytes, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("shader", hr);
  }
  return bytes;
}

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 fence_value = 0;

  struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
  };
  List begin(ID3D12PipelineState *pso = nullptr) {
    List result;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result.allocator)),
          "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result.allocator.Get(), pso,
                                    IID_PPV_ARGS(&result.list)),
          "list");
    return result;
  }
  void execute(List &list) {
    check(list.list->Close(), "Close");
    ID3D12CommandList *lists[] = {list.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), ++fence_value), "Signal");
    check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
    const DWORD waited = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0) fail("bounded GPU completion");
    check(device->GetDeviceRemovedReason(), "device removed");
  }
  ComPtr<ID3D12RootSignature> root(const D3D12_ROOT_SIGNATURE_DESC &desc) {
    ComPtr<ID3DBlob> bytes, errors;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "serialize root");
    ComPtr<ID3D12RootSignature> result;
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&result)),
          "root");
    return result;
  }
};

D3D12_GRAPHICS_PIPELINE_STATE_DESC basePso(ID3D12RootSignature *root, ID3DBlob *vs, ID3DBlob *ps) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  if (vs) desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  if (ps) desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  for (auto &rt : desc.BlendState.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  desc.SampleMask = UINT_MAX;
  desc.SampleDesc.Count = 1;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  return desc;
}

// ---------------------------------------------------------------------------------------------------------------
// Logic ops (FS 17.7): all 16 D3D12_LOGIC_OPs, applied by RenderTarget[0] to every bound UINT RT.

uint32_t logicReference(D3D12_LOGIC_OP op, uint32_t s, uint32_t d) {
  switch (op) {
  case D3D12_LOGIC_OP_CLEAR: return 0;
  case D3D12_LOGIC_OP_SET: return ~0u;
  case D3D12_LOGIC_OP_COPY: return s;
  case D3D12_LOGIC_OP_COPY_INVERTED: return ~s;
  case D3D12_LOGIC_OP_NOOP: return d;
  case D3D12_LOGIC_OP_INVERT: return ~d;
  case D3D12_LOGIC_OP_AND: return s & d;
  case D3D12_LOGIC_OP_NAND: return ~(s & d);
  case D3D12_LOGIC_OP_OR: return s | d;
  case D3D12_LOGIC_OP_NOR: return ~(s | d);
  case D3D12_LOGIC_OP_XOR: return s ^ d;
  case D3D12_LOGIC_OP_EQUIV: return ~(s ^ d);
  case D3D12_LOGIC_OP_AND_REVERSE: return s & ~d;
  case D3D12_LOGIC_OP_AND_INVERTED: return ~s & d;
  case D3D12_LOGIC_OP_OR_REVERSE: return s | ~d;
  case D3D12_LOGIC_OP_OR_INVERTED: return ~s | d;
  }
  return 0;
}

struct UintFormat {
  DXGI_FORMAT format;
  const char *name;
  UINT components;
  UINT bits[4];
};

uint32_t packTexel(const UintFormat &f, const uint32_t value[4]) {
  // Packs up to 4 components into one little-endian texel of total width <= 32 bits (bits[] sum).
  uint32_t texel = 0, shift = 0;
  for (UINT c = 0; c < f.components; ++c) {
    const uint32_t mask = f.bits[c] == 32 ? ~0u : (1u << f.bits[c]) - 1;
    texel |= (value[c] & mask) << shift;
    shift += f.bits[c];
  }
  return texel;
}

// Framebuffer fetch is sample-rate, but a pixel-frequency application's UAV effects/statistics must not be.
// Mixed non-UINT MRTs bypass the op, and late depth failure/discard must leave the stored destination intact.
void testLogicRasterEdges(Context &c) {
  const char *source = R"(
    RWStructuredBuffer<uint> records : register(u2);
    float4 vs(uint id : SV_VertexID) : SV_Position {
      float2 p = float2((id << 1) & 2, id & 2);
      return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
    }
    struct Output { uint4 a : SV_Target0; float4 b : SV_Target1; float z : SV_Depth; };
    Output body(float4 p) {
      InterlockedAdd(records[uint(p.y)*4 + uint(p.x)], 1);
      if (uint(p.x) == 3) discard;
      Output o; o.a = uint4(202,60,15,1); o.b = float4(0.25,0.5,0.75,1); o.z = 0.25; return o;
    }
    Output ps(float4 p : SV_Position) { return body(p); }
    Output psSample(float4 p : SV_Position, uint sample : SV_SampleIndex) {
      Output o = body(p); o.a.x += sample; return o;
    }
  )";
  auto vs = shader(source, "vs", "vs_5_0");
  auto ps = shader(source, "ps", "ps_5_0");
  auto ps_sample = shader(source, "psSample", "ps_5_0");
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[0].Descriptor.ShaderRegister = 2;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &range};
  auto root = c.root({2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  struct Case { UINT samples; bool sample_frequency; };
  const Case cases[] = {{1, false}, {4, false}, {4, true}};
  const UINT write_masks[] = {D3D12_COLOR_WRITE_ENABLE_ALL,
                              D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_BLUE, 0};
  UINT checked_cases = 0;
  for (const auto &current : cases) {
    const std::string compute_source = std::string(current.samples == 1 ? "#define SINGLE 1\n" : "") + R"(
      RWStructuredBuffer<uint> records : register(u2);
      #ifdef SINGLE
      Texture2D<uint4> color : register(t0); Texture2D<float4> mixed : register(t1);
      Texture2D<float> depth : register(t2);
      #else
      Texture2DMS<uint4,4> color : register(t0); Texture2DMS<float4,4> mixed : register(t1);
      Texture2DMS<float,4> depth : register(t2);
      #endif
      [numthreads(16,1,1)] void cs(uint id : SV_DispatchThreadID) {
        uint2 p = uint2(id%4,id/4);
        #ifdef SINGLE
        uint4 a = color.Load(int3(p,0)); float4 b = mixed.Load(int3(p,0)); float z = depth.Load(int3(p,0));
        uint o = 16 + id*9;
        [unroll] for(uint i=0;i<4;i++) { records[o+i]=a[i]; records[o+4+i]=asuint(b[i]); }
        records[o+8]=asuint(z);
        #else
        [unroll] for(uint s=0;s<4;s++) {
          uint4 a=color.Load(p,s); float4 b=mixed.Load(p,s); float z=depth.Load(p,s);
          uint o=16+(id*4+s)*9;
          [unroll] for(uint i=0;i<4;i++) { records[o+i]=a[i]; records[o+4+i]=asuint(b[i]); }
          records[o+8]=asuint(z);
        }
        #endif
      }
    )";
    auto cs = shader(compute_source.c_str(), "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};
    compute_desc.pRootSignature = root.Get(); compute_desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> compute;
    check(c.device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)), "logic dump PSO");
    for (auto mask : write_masks) {
      auto color = texture(c.device.Get(), DXGI_FORMAT_R10G10B10A2_UINT, 4, 4, current.samples,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
      auto mixed = texture(c.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, current.samples,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
      auto depth = texture(c.device.Get(), DXGI_FORMAT_R32_TYPELESS, 4, 4, current.samples,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      ComPtr<ID3D12DescriptorHeap> rtvs, dsvs, srvs;
      D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      check(c.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs)), "logic MRT heap");
      auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      auto rtv1 = rtv; rtv1.ptr += c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      c.device->CreateRenderTargetView(color.Get(), nullptr, rtv);
      c.device->CreateRenderTargetView(mixed.Get(), nullptr, rtv1);
      heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors = 1;
      check(c.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsvs)), "logic depth heap");
      auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
      D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{}; depth_view.Format = DXGI_FORMAT_D32_FLOAT;
      depth_view.ViewDimension = current.samples == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
      c.device->CreateDepthStencilView(depth.Get(), &depth_view, dsv);
      heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 3;
      heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      check(c.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvs)), "logic sample dump heap");
      const UINT srv_stride = c.device->GetDescriptorHandleIncrementSize(heap.Type);
      ID3D12Resource *resources[] = {color.Get(), mixed.Get(), depth.Get()};
      const DXGI_FORMAT formats[] = {DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT};
      auto srv = srvs->GetCPUDescriptorHandleForHeapStart();
      for (UINT i = 0; i < 3; ++i, srv.ptr += srv_stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = formats[i];
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = current.samples == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        if (current.samples == 1) view.Texture2D.MipLevels = 1;
        c.device->CreateShaderResourceView(resources[i], &view, srv);
      }
      const UINT bytes = (16 + 16*current.samples*9)*4;
      auto records = buffer(c.device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto zero = buffer(c.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      void *mapped = nullptr; check(zero->Map(0, nullptr, &mapped), "logic zero map");
      std::memset(mapped, 0, bytes); zero->Unmap(0, nullptr);
      auto results = buffer(c.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0};
      ComPtr<ID3D12QueryHeap> statistics;
      check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&statistics)), "logic stats heap");
      auto stats_result = buffer(c.device.Get(), sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      auto desc = basePso(root.Get(), vs.Get(), current.sample_frequency ? ps_sample.Get() : ps.Get());
      desc.NumRenderTargets = 2; desc.RTVFormats[0] = formats[0]; desc.RTVFormats[1] = formats[1];
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT; desc.SampleDesc.Count = current.samples; desc.SampleMask = 0x5;
      desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
      desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
      desc.DepthStencilState.DepthEnable = TRUE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      ComPtr<ID3D12PipelineState> pipeline;
      check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), "logic sideeffect PSO");
      auto list = c.begin(pipeline.Get());
      list.list->CopyBufferRegion(records.Get(), 0, zero.Get(), 0, bytes);
      transition(list.list.Get(), records.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list.list->SetGraphicsRootSignature(root.Get());
      list.list->SetGraphicsRootUnorderedAccessView(0, records->GetGPUVirtualAddress());
      list.list->OMSetRenderTargets(2, &rtv, TRUE, &dsv);
      const float destination[] = {166,90,63,1}, clear[] = {0,0,0,0};
      list.list->ClearRenderTargetView(rtv, destination, 0, nullptr);
      list.list->ClearRenderTargetView(rtv1, clear, 0, nullptr);
      list.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
      D3D12_VIEWPORT viewport{0,0,4,4,0,1}; D3D12_RECT scissor{0,0,4,4};
      list.list->RSSetViewports(1, &viewport); list.list->RSSetScissorRects(1, &scissor);
      list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list.list->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      // Shader-depth forces late testing: both draws execute UAV writes, only the first passes LESS.
      list.list->DrawInstanced(3,1,0,0); list.list->DrawInstanced(3,1,0,0);
      list.list->EndQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      list.list->ResolveQueryData(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, stats_result.Get(), 0);
      for (UINT i = 0; i < 3; ++i)
        transition(list.list.Get(), resources[i], i == 2 ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = records.Get();
      list.list->ResourceBarrier(1, &uav);
      list.list->SetPipelineState(compute.Get()); list.list->SetComputeRootSignature(root.Get());
      ID3D12DescriptorHeap *heaps[] = {srvs.Get()}; list.list->SetDescriptorHeaps(1, heaps);
      list.list->SetComputeRootUnorderedAccessView(0, records->GetGPUVirtualAddress());
      list.list->SetComputeRootDescriptorTable(1, srvs->GetGPUDescriptorHandleForHeapStart());
      list.list->Dispatch(1,1,1);
      transition(list.list.Get(), records.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list.list->CopyBufferRegion(results.Get(), 0, records.Get(), 0, bytes); c.execute(list);
      auto image = readback(results.Get(), bytes);
      std::vector<uint32_t> words(bytes/4); std::memcpy(words.data(), image.data(), bytes);
      const UINT invocations = current.sample_frequency ? 4 : 2;
      const uint32_t s[] = {202,60,15,1}, d[] = {166,90,63,1};
      const float normalized[] = {64/255.0f,128/255.0f,191/255.0f,1};
      for (UINT p = 0; p < 16; ++p) {
        if (words[p] != invocations) fail("logic UAV invocation multiplicity");
        for (UINT sample = 0; sample < current.samples; ++sample) {
          const bool covered = p%4 != 3 && (0x5 & (1u << sample));
          const UINT offset = 16 + (p*current.samples + sample)*9;
          for (UINT ch = 0; ch < 4; ++ch) {
            const bool written = covered && (mask & (1u << ch));
            const uint32_t value = s[ch] + (current.sample_frequency && ch == 0 ? sample : 0);
            if (words[offset+ch] != (written ? value^d[ch] : d[ch])) fail("logic per-sample UINT result");
            float got; std::memcpy(&got, &words[offset+4+ch], 4);
            const float want = written ? normalized[ch] : 0;
            if (std::abs(got-want) > 0.000001f) fail("logic mixed MRT quantization/write mask");
          }
          float got_depth; std::memcpy(&got_depth, &words[offset+8], 4);
          if (got_depth != (covered ? 0.25f : 1.0f)) fail("logic discard/late depth/sample mask");
        }
      }
      auto stats_bytes = readback(stats_result.Get(), sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
      D3D12_QUERY_DATA_PIPELINE_STATISTICS stats{}; std::memcpy(&stats, stats_bytes.data(), sizeof(stats));
      if (stats.PSInvocations != 16*invocations) fail("logic application PS statistics");
      ++checked_cases;
    }
  }
  std::printf("OWNED_D3D12_FEATURE_LEVEL_LOGIC_RASTER_EDGES cases=%u late_depth=1 discard=1 mixed_mrt=1 uav_stats=exact\n", checked_cases);
}

void testLogicOps(Context &c) {
  const char *source = R"(
    cbuffer Values : register(b0) { uint4 source_value; uint4 dest_value; };
    float4 vs(uint id : SV_VertexID) : SV_Position {
      float2 uv = float2((id << 1) & 2, id & 2);
      return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    }
    uint4 psSource() : SV_Target0 { return source_value; }
    uint4 psDest() : SV_Target0 { return dest_value; }
    struct Two { uint4 a : SV_Target0; uint4 b : SV_Target1; };
    Two psSource2() { Two r; r.a = source_value; r.b = source_value.wzyx; return r; }
    Two psDest2() { Two r; r.a = dest_value; r.b = dest_value.wzyx; return r; }
  )";
  auto vs = shader(source, "vs", "vs_5_0");
  auto ps_source = shader(source, "psSource", "ps_5_0");
  auto ps_dest = shader(source, "psDest", "ps_5_0");
  auto ps_source2 = shader(source, "psSource2", "ps_5_0");
  auto ps_dest2 = shader(source, "psDest2", "ps_5_0");
  D3D12_ROOT_PARAMETER constants{};
  constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  constants.Constants.Num32BitValues = 8;
  D3D12_ROOT_SIGNATURE_DESC root_desc{1, &constants, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  auto root = c.root(root_desc);

  // Source/destination bit patterns exercise all four (s,d) bit combinations in every component.
  const uint32_t src[4] = {0xCAu, 0x5A3Cu, 0x0Fu, 0xF00FF0F0u};
  const uint32_t dst[4] = {0xA6u, 0x3C5Au, 0xFFu, 0x0FF00FF0u};
  const UintFormat formats[] = {
      {DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", 4, {8, 8, 8, 8}},
      {DXGI_FORMAT_R16G16_UINT, "R16G16_UINT", 2, {16, 16}},
      {DXGI_FORMAT_R32_UINT, "R32_UINT", 1, {32}},
      {DXGI_FORMAT_R10G10B10A2_UINT, "R10G10B10A2_UINT", 4, {10, 10, 10, 2}},
      {DXGI_FORMAT_R8_UINT, "R8_UINT", 1, {8}},
      {DXGI_FORMAT_R16_UINT, "R16_UINT", 1, {16}},
      {DXGI_FORMAT_R8G8_UINT, "R8G8_UINT", 2, {8, 8}},
  };
  // Shader output already fits the format width (FS 17.7.1 requires applications to zero high bits).
  auto masked = [](const UintFormat &f, const uint32_t value[4], uint32_t out[4]) {
    for (UINT c = 0; c < 4; ++c)
      out[c] = c < f.components ? (f.bits[c] == 32 ? value[c] : value[c] & ((1u << f.bits[c]) - 1)) : 0;
  };
  constexpr UINT W = 4, H = 4;
  // MRT: RT1 is R32G32B32A32_UINT and shares the RT0 logic op (FS 17.7.2 (a)).
  auto wide = texture(c.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, W, H, 1,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  check(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
  const UINT rtv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtv_heap->GetCPUDescriptorHandleForHeapStart(),
                                         rtv_heap->GetCPUDescriptorHandleForHeapStart()};
  rtvs[1].ptr += rtv_stride;
  c.device->CreateRenderTargetView(wide.Get(), nullptr, rtvs[1]);
  auto wide_readback = buffer(c.device.Get(), 256 * H, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

  for (const auto &format : formats) {
    auto target = texture(c.device.Get(), format.format, W, H, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.device->CreateRenderTargetView(target.Get(), nullptr, rtvs[0]);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format.format};
    check(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)),
          "format support");
    if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP)) fail("logic-op format support bit");
    auto readback_buffer =
        buffer(c.device.Get(), 256 * H, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t s[4], d[4];
    masked(format, src, s);
    masked(format, dst, d);
    for (UINT op = D3D12_LOGIC_OP_CLEAR; op <= D3D12_LOGIC_OP_OR_INVERTED; ++op) {
      // Seed the destination with a plain draw (Clear is FLOAT-converted for UINT RTVs).
      auto seed_desc = basePso(root.Get(), vs.Get(), ps_dest2.Get());
      seed_desc.NumRenderTargets = 2;
      seed_desc.RTVFormats[0] = format.format;
      seed_desc.RTVFormats[1] = DXGI_FORMAT_R32G32B32A32_UINT;
      ComPtr<ID3D12PipelineState> seed;
      check(c.device->CreateGraphicsPipelineState(&seed_desc, IID_PPV_ARGS(&seed)), "seed PSO");
      auto logic_desc = seed_desc;
      logic_desc.PS = {ps_source2->GetBufferPointer(), ps_source2->GetBufferSize()};
      logic_desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
      logic_desc.BlendState.RenderTarget[0].LogicOp = static_cast<D3D12_LOGIC_OP>(op);
      ComPtr<ID3D12PipelineState> logic;
      check(c.device->CreateGraphicsPipelineState(&logic_desc, IID_PPV_ARGS(&logic)), "logic PSO");
      auto list = c.begin(seed.Get());
      list.list->SetGraphicsRootSignature(root.Get());
      uint32_t values[8];
      // Shader inputs are already masked to the format width.
      std::memcpy(values, s, 16);
      std::memcpy(values + 4, d, 16);
      list.list->SetGraphicsRoot32BitConstants(0, 8, values, 0);
      list.list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(W), float(H), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(W), LONG(H)};
      list.list->RSSetViewports(1, &viewport);
      list.list->RSSetScissorRects(1, &scissor);
      list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list.list->DrawInstanced(3, 1, 0, 0);
      list.list->SetPipelineState(logic.Get());
      list.list->DrawInstanced(3, 1, 0, 0);
      transition(list.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.list.Get(), wide.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      auto copy_out = [&](ID3D12Resource *from, ID3D12Resource *to, DXGI_FORMAT fmt) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{}, src_loc{};
        src_loc.pResource = from;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.pResource = to;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Footprint = {fmt, W, H, 1, 256};
        list.list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
      };
      copy_out(target.Get(), readback_buffer.Get(), format.format);
      copy_out(wide.Get(), wide_readback.Get(), DXGI_FORMAT_R32G32B32A32_UINT);
      transition(list.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      transition(list.list.Get(), wide.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      c.execute(list);
      uint32_t expected[4];
      for (UINT ch = 0; ch < 4; ++ch) expected[ch] = logicReference(static_cast<D3D12_LOGIC_OP>(op), s[ch], d[ch]);
      const uint32_t expected_texel = packTexel(format, expected);
      UINT texel_bytes = 0;
      for (UINT ch = 0; ch < format.components; ++ch) texel_bytes += format.bits[ch];
      texel_bytes /= 8;
      auto bytes = readback(readback_buffer.Get(), 256 * H);
      auto wide_bytes = readback(wide_readback.Get(), 256 * H);
      const uint32_t wide_s[4] = {s[3], s[2], s[1], s[0]};
      const uint32_t wide_d[4] = {d[3], d[2], d[1], d[0]};
      for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
          uint32_t texel = 0;
          std::memcpy(&texel, bytes.data() + y * 256 + x * texel_bytes, texel_bytes);
          if (texel != expected_texel) {
            std::fprintf(stderr, "logic op format=%s op=%u pixel=(%u,%u) got=%08x expected=%08x\n", format.name,
                         op, x, y, texel, expected_texel);
            fail("logic op RT0");
          }
          uint32_t wide_texel[4];
          std::memcpy(wide_texel, wide_bytes.data() + y * 256 + x * 16, 16);
          for (UINT ch = 0; ch < 4; ++ch) {
            const uint32_t want = logicReference(static_cast<D3D12_LOGIC_OP>(op), wide_s[ch], wide_d[ch]);
            if (wide_texel[ch] != want) {
              std::fprintf(stderr, "logic op MRT1 op=%u pixel=(%u,%u) ch=%u got=%08x expected=%08x\n", op, x, y,
                           ch, wide_texel[ch], want);
              fail("logic op RT1");
            }
          }
        }
    }
    std::printf("OWNED_D3D12_FEATURE_LEVEL_LOGIC_OP format=%s ops=16 pixels=%u mrt=R32G32B32A32_UINT\n",
                format.name, W * H);
  }
  // Invalid configurations (D3D12_BLEND_DESC remarks): blend+logic, independent blend, out-of-range op.
  auto desc = basePso(root.Get(), vs.Get(), ps_source.Get());
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
  desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
  desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
  ComPtr<ID3D12PipelineState> rejected;
  desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
  expect(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&rejected)), E_INVALIDARG, "logic+blend");
  desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
  desc.BlendState.IndependentBlendEnable = TRUE;
  expect(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&rejected)), E_INVALIDARG,
         "logic+independent blend");
  desc.BlendState.IndependentBlendEnable = FALSE;
  desc.BlendState.RenderTarget[0].LogicOp = static_cast<D3D12_LOGIC_OP>(16);
  expect(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&rejected)), E_INVALIDARG, "logic op range");
  testLogicRasterEdges(c);
  passed("logic_ops_16_uint_formats_7_mrt_and_rejections");
}

// ---------------------------------------------------------------------------------------------------------------
// Target-independent rasterization (FS 3.5.6). Reference coverage: 16.8 fixed-point snapped vertices, the
// top-left fill rule, samples at the D3D standard patterns (1/16 pixel from the pixel center).

const int kX16[16] = {1, -1, -3, 4, -5, 2, 5, 3, -2, 0, -4, -6, -8, 7, 6, -7};
const int kY16[16] = {1, -3, 2, -1, -2, 5, 3, -5, 6, -7, -6, 4, 0, -4, 7, -8};
const int kX8[8] = {1, -1, 5, -3, -5, -7, 3, 7}, kY8[8] = {-3, 3, 1, -5, 5, -1, 7, -7};
const int kX4[4] = {-2, 6, -6, 2}, kY4[4] = {-6, -2, 2, 6};

bool referenceCovered(const int64_t vx[3], const int64_t vy[3], int64_t sx, int64_t sy) {
  const int64_t area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
  if (!area) return false;
  for (int e = 0; e < 3; ++e) {
    const int a = e, b = (e + 1) % 3;
    int64_t ex = vx[b] - vx[a], ey = vy[b] - vy[a];
    int64_t w = ex * (sy - vy[a]) - ey * (sx - vx[a]);
    if (area < 0) { w = -w; ex = -ex; ey = -ey; }
    if (w > 0) continue;
    if (w < 0) return false;
    if (!((ey == 0 && ex < 0) || ey < 0)) return false;  // top-left rule in y-down screen space
  }
  return true;
}

// Coverage of pixel (px, py) by an NDC triangle on a size x size viewport, for the given pattern.
uint32_t referenceMask(const float *ndc, UINT size, int px, int py, UINT count, const int *xs, const int *ys) {
  int64_t vx[3], vy[3];
  for (int i = 0; i < 3; ++i) {
    vx[i] = llround((ndc[2 * i] * 0.5 + 0.5) * size * 256.0);
    vy[i] = llround((0.5 - ndc[2 * i + 1] * 0.5) * size * 256.0);
  }
  uint32_t mask = 0;
  for (UINT s = 0; s < count; ++s) {
    const int64_t sx = px * 256 + 128 + (count == 1 ? 0 : xs[s] * 16);
    const int64_t sy = py * 256 + 128 + (count == 1 ? 0 : ys[s] * 16);
    if (referenceCovered(vx, vy, sx, sy)) mask |= 1u << s;
  }
  return mask;
}

// SV_Coverage needs DXC: shaders are frozen DXIL (dxil/feature-level.hlsl, build-feature-level.sh) read beside
// the executable.
std::vector<uint8_t> dxil(const char *entry) {
  std::wstring name = L"feature-level-";
  for (const char *ch = entry; *ch; ++ch) name += static_cast<wchar_t>(*ch);
  name += L".dxil";
  HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail("open frozen DXIL", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 24)) {
    CloseHandle(file);
    fail("frozen DXIL size");
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != data.size()) fail("read frozen DXIL");
  return data;
}

struct ForcedHarness {
  ComPtr<ID3D12RootSignature> root;
  std::vector<uint8_t> vs, ps_coverage, ps_color;
  ComPtr<ID3D12Resource> result, zero, readback;
  static constexpr UINT kSize = 16, kBytes = kSize * kSize * 8;

  explicit ForcedHarness(Context &c) {
    vs = dxil("vs_main");
    ps_coverage = dxil("ps_coverage");
    ps_color = dxil("ps_color");
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    root = c.root(root_desc);
    result = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    zero = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(zero->Map(0, nullptr, &mapped), "Map(zero)");
    std::memset(mapped, 0, kBytes);
    zero->Unmap(0, nullptr);
    readback = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso(const std::vector<uint8_t> &ps, UINT forced) {
    auto desc = basePso(root.Get(), nullptr, nullptr);
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.RasterizerState.ForcedSampleCount = forced;
    return desc;
  }
  // Clears the UAV, then (via `body`) records draws; resolves the UAV into `readback`.
  template <typename Body> void record(Context::List &list, Body &&body) {
    list.list->CopyBufferRegion(result.Get(), 0, zero.Get(), 0, kBytes);
    transition(list.list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list.list->SetGraphicsRootSignature(root.Get());
    list.list->SetGraphicsRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
    list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
    list.list->RSSetViewports(1, &viewport);
    list.list->RSSetScissorRects(1, &scissor);
    body();
    transition(list.list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, kBytes);
    transition(list.list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }
};

// Deterministic triangles: a large one, a sliver, and seeded random ones spanning the viewport edges.
std::vector<std::array<float, 6>> forcedTriangles(UINT count) {
  std::vector<std::array<float, 6>> result = {{-0.9f, -0.85f, 0.95f, -0.2f, -0.1f, 0.9f},
                                              {-1.0f, -0.1f, 1.0f, 0.05f, -1.0f, 0.2f}};
  uint32_t seed = 7;
  auto next = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (int((seed >> 8) % 20001) - 10000) / 10000.0f * 1.1f;
  };
  while (result.size() < count) {
    std::array<float, 6> t;
    for (auto &v : t) v = next();
    result.push_back(t);
  }
  return result;
}

// UAV-only rendering with ForcedSampleCount 1/4/8/16 (FS 3.5.6.4): pixel-frequency invocation exactly where the
// forced pattern is covered, SV_Coverage carrying the full forced-pattern mask (16 bits at 16x), SampleMask
// ignored, plus an occlusion query counting forced samples.
void testUavOnlyForced(Context &c) {
  ForcedHarness h(c);
  const auto triangles = forcedTriangles(40);
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
  ComPtr<ID3D12QueryHeap> queries;
  check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)), "occlusion heap");
  auto query_result = buffer(c.device.Get(), 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  struct Pattern { UINT count; const int *xs; const int *ys; };
  const Pattern patterns[] = {{1, nullptr, nullptr}, {4, kX4, kY4}, {8, kX8, kY8}, {16, kX16, kY16}};
  for (const auto &pattern : patterns) {
    auto desc = h.pso(h.ps_coverage, pattern.count);
    // FS 3.5.6.4: SampleMask does not affect UAV-only forced rasterization.
    desc.SampleMask = 0x5u;
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "UAV-only forced PSO");
    UINT checked = 0, partial = 0;
    for (const auto &tri : triangles) {
      auto list = c.begin(pso.Get());
      h.record(list, [&] {
        float constants[8] = {tri[0], tri[1], tri[2], tri[3], tri[4], tri[5], 0, 0};
        list.list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
        list.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list.list->DrawInstanced(3, 1, 0, 0);
        list.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, query_result.Get(), 0);
      });
      c.execute(list);
      auto bytes = readback(h.readback.Get(), ForcedHarness::kBytes);
      auto samples = readback(query_result.Get(), 8);
      uint64_t visible = 0, expected_visible = 0;
      std::memcpy(&visible, samples.data(), 8);
      for (int py = 0; py < int(ForcedHarness::kSize); ++py)
        for (int px = 0; px < int(ForcedHarness::kSize); ++px) {
          uint32_t got[2];
          std::memcpy(got, bytes.data() + (py * ForcedHarness::kSize + px) * 8, 8);
          const uint32_t want = referenceMask(tri.data(), ForcedHarness::kSize, px, py, pattern.count,
                                              pattern.xs, pattern.ys);
          const uint32_t want_invocations = want ? 1 : 0;
          expected_visible += __builtin_popcount(want);
          if (want && want != (1u << pattern.count) - 1u) ++partial;
          if (got[0] != want || got[1] != want_invocations) {
            std::fprintf(stderr, "forced=%u pixel=(%d,%d) coverage=%04x invocations=%u expected=%04x/%u\n",
                         pattern.count, px, py, got[0], got[1], want, want_invocations);
            fail("UAV-only forced coverage/invocation");
          }
          ++checked;
        }
      if (visible != expected_visible) {
        std::fprintf(stderr, "forced=%u occlusion=%llu expected=%llu\n", pattern.count,
                     static_cast<unsigned long long>(visible), static_cast<unsigned long long>(expected_visible));
        fail("UAV-only forced occlusion samples");
      }
    }
    std::printf("OWNED_D3D12_FEATURE_LEVEL_UAV_ONLY_FORCED samples=%u triangles=%zu pixels=%u partial=%u\n",
                pattern.count, triangles.size(), checked, partial);
  }
  // FS 3.5.6.2 TIR: ForcedSampleCount 16 with a single-sample RTV (the Direct2D path). Same coverage and
  // invocation; the RTV pixel takes the PS color where any forced sample is covered; occlusion counts the
  // covered forced samples.
  {
    const UINT S = ForcedHarness::kSize;
    auto target = texture(c.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, S, S, 1,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    check(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    c.device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto image = buffer(c.device.Get(), 256 * S, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto desc = h.pso(h.ps_color, 16);
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "TIR 16x 1x RTV PSO");
    const float clear[4] = {0, 0, 0, 0};
    UINT partial = 0;
    for (const auto &tri : triangles) {
      auto list = c.begin(pso.Get());
      h.record(list, [&] {
        list.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        float constants[8] = {tri[0], tri[1], tri[2], tri[3], tri[4], tri[5], 0, 0};
        list.list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
        list.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list.list->DrawInstanced(3, 1, 0, 0);
        list.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, query_result.Get(), 0);
        transition(list.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = image.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, S, S, 1, 256};
        list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
      });
      c.execute(list);
      auto bytes = readback(h.readback.Get(), ForcedHarness::kBytes);
      auto pixels = readback(image.Get(), 256 * S);
      auto samples = readback(query_result.Get(), 8);
      uint64_t visible = 0, expected_visible = 0;
      std::memcpy(&visible, samples.data(), 8);
      for (int py = 0; py < int(S); ++py)
        for (int px = 0; px < int(S); ++px) {
          uint32_t got[2];
          std::memcpy(got, bytes.data() + (py * S + px) * 8, 8);
          const uint32_t want = referenceMask(tri.data(), S, px, py, 16, kX16, kY16);
          expected_visible += __builtin_popcount(want);
          if (want && want != 0xffffu) ++partial;
          const UINT color[4] = {64, 128, 191, 255};
          bool color_ok = true;
          for (UINT ch = 0; ch < 4; ++ch) {
            const int value = pixels[py * 256 + px * 4 + ch], expect_value = want ? int(color[ch]) : 0;
            color_ok &= value >= expect_value - 1 && value <= expect_value + 1;
          }
          if (got[0] != want || got[1] != (want ? 1u : 0u) || !color_ok) {
            std::fprintf(stderr, "TIR16 pixel=(%d,%d) coverage=%04x invocations=%u expected=%04x rgba=%u,%u,%u,%u\n",
                         px, py, got[0], got[1], want, pixels[py * 256 + px * 4], pixels[py * 256 + px * 4 + 1],
                         pixels[py * 256 + px * 4 + 2], pixels[py * 256 + px * 4 + 3]);
            fail("TIR 16x 1x RTV coverage/invocation/color");
          }
        }
      if (visible != expected_visible) {
        std::fprintf(stderr, "TIR16 occlusion=%llu expected=%llu\n", static_cast<unsigned long long>(visible),
                     static_cast<unsigned long long>(expected_visible));
        fail("TIR 16x 1x RTV occlusion samples");
      }
    }
    std::printf("OWNED_D3D12_FEATURE_LEVEL_TIR16_RTV triangles=%zu partial=%u\n", triangles.size(), partial);
  }
  // FS 3.5.6.4: sample-frequency invocation is invalid with UAV-only forced rendering; other counts are rejected.
  auto bad = h.pso(h.ps_coverage, 2);
  ComPtr<ID3D12PipelineState> rejected;
  expect(c.device->CreateGraphicsPipelineState(&bad, IID_PPV_ARGS(&rejected)), E_INVALIDARG, "forced 2");
  bad.RasterizerState.ForcedSampleCount = 32;
  expect(c.device->CreateGraphicsPipelineState(&bad, IID_PPV_ARGS(&rejected)), E_INVALIDARG, "forced 32");
  passed("uav_only_forced_1_4_8_16_and_tir16_rtv_coverage_invocation_occlusion_exact");
}

// ForcedSampleCount 1 with multisampled RTVs (MultisampleRTVWithForcedSampleCountOne, FS 3.5.6.2): center-sample
// rasterization, one PS invocation per covered pixel, its output written to every RTV sample enabled by
// SampleMask (and SV_Coverage), the occlusion query counting forced (1x) samples, 0 when every output is masked.
void testForcedOneMultisampleRtv(Context &c) {
  ForcedHarness h(c);
  const auto triangles = forcedTriangles(6);
  const float clear[4] = {0, 0, 0, 0};
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
  ComPtr<ID3D12QueryHeap> queries;
  check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)), "occlusion heap");
  D3D12_QUERY_HEAP_DESC stats_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0};
  ComPtr<ID3D12QueryHeap> stats;
  check(c.device->CreateQueryHeap(&stats_desc, IID_PPV_ARGS(&stats)), "statistics heap");
  auto query_result = buffer(c.device.Get(), 8 + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                             D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  check(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  struct Case { UINT samples; UINT mask; };
  const Case cases[] = {{4, 0xffffffffu}, {4, 0x5u}, {8, 0xffffffffu}, {8, 0x81u}, {4, 0x0u}};
  for (const auto &current : cases) {
    const UINT S = ForcedHarness::kSize;
    auto target = texture(c.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, S, S, current.samples,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto desc = h.pso(h.ps_color, 1);
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = current.samples;
    desc.SampleMask = current.mask;
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "forced1 MSAA RTV PSO");
    // Per-sample read-back through a resolve-free copy into a sample-indexed compute dump would need another
    // shader; instead draw every triangle into its own cleared target and check samples via a 1x resolve
    // with exact UNORM averages (each sample is either the PS color or the clear value).
    for (const auto &tri : triangles) {
      auto resolved = texture(c.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, S, S, 1, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_RESOLVE_DEST);
      auto image = buffer(c.device.Get(), 256 * S, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      auto list = c.begin(pso.Get());
      h.record(list, [&] {
        list.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        float constants[8] = {tri[0], tri[1], tri[2], tri[3], tri[4], tri[5], 0, 0};
        list.list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
        list.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list.list->BeginQuery(stats.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        list.list->DrawInstanced(3, 1, 0, 0);
        list.list->EndQuery(stats.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        list.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, query_result.Get(), 0);
        list.list->ResolveQueryData(stats.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, query_result.Get(), 8);
        transition(list.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        list.list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        transition(list.list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = resolved.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = image.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, S, S, 1, 256};
        list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      });
      c.execute(list);
      auto bytes = readback(h.readback.Get(), ForcedHarness::kBytes);
      auto pixels = readback(image.Get(), 256 * S);
      auto query = readback(query_result.Get(), 8 + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
      uint64_t visible = 0;
      D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
      std::memcpy(&visible, query.data(), 8);
      std::memcpy(&statistics, query.data() + 8, sizeof(statistics));
      const uint32_t enabled = current.mask & ((1u << current.samples) - 1u);
      const UINT written = __builtin_popcount(enabled);
      uint64_t expected_visible = 0, expected_invocations = 0;
      for (int py = 0; py < int(S); ++py)
        for (int px = 0; px < int(S); ++px) {
          const bool covered = referenceMask(tri.data(), S, px, py, 1, nullptr, nullptr) != 0;
          uint32_t got[2];
          std::memcpy(got, bytes.data() + (py * S + px) * 8, 8);
          // FS 3.5.6.2: an invocation happens for a covered pixel only if an output sample is in SampleMask.
          const bool invoked = covered && enabled;
          expected_invocations += invoked;
          expected_visible += invoked;
          if (got[1] != (invoked ? 1u : 0u) || (invoked && got[0] != 1u)) {
            std::fprintf(stderr, "forced1 rtv=%ux mask=%x pixel=(%d,%d) coverage=%x invocations=%u covered=%d\n",
                         current.samples, current.mask, px, py, got[0], got[1], covered);
            fail("forced1 MSAA RTV invocation and SV_Coverage");
          }
          // Resolve = average over samples; written samples hold (64,128,191,255), others 0.
          const UINT color[4] = {64, 128, 191, 255};
          for (UINT ch = 0; ch < 4; ++ch) {
            const UINT want = invoked ? (color[ch] * written + current.samples / 2) / current.samples : 0;
            const UINT value = pixels[py * 256 + px * 4 + ch];
            if (value + 1 < want || value > want + 1) {
              std::fprintf(stderr, "forced1 rtv=%ux mask=%x pixel=(%d,%d) ch=%u got=%u expected=%u\n",
                           current.samples, current.mask, px, py, ch, value, want);
              fail("forced1 MSAA RTV sample writes");
            }
          }
        }
      if (visible != expected_visible || statistics.PSInvocations != expected_invocations ||
          statistics.CInvocations != 1 || statistics.IAPrimitives != 1) {
        std::fprintf(stderr, "forced1 rtv=%ux mask=%x occlusion=%llu/%llu ps=%llu/%llu c=%llu ia=%llu\n",
                     current.samples, current.mask, static_cast<unsigned long long>(visible),
                     static_cast<unsigned long long>(expected_visible),
                     static_cast<unsigned long long>(statistics.PSInvocations),
                     static_cast<unsigned long long>(expected_invocations),
                     static_cast<unsigned long long>(statistics.CInvocations),
                     static_cast<unsigned long long>(statistics.IAPrimitives));
        fail("forced1 MSAA RTV occlusion/pipeline statistics");
      }
    }
    std::printf("OWNED_D3D12_FEATURE_LEVEL_FORCED1_MSAA_RTV samples=%u mask=0x%x triangles=%zu\n",
                current.samples, current.mask, triangles.size());
  }
  passed("forced1_multisample_rtv_4x_8x_samplemask_occlusion_statistics");
}

// ---------------------------------------------------------------------------------------------------------------
// Typed UAV loads (FS / D3D12 "Typed unordered access view loads"): R32_FLOAT/UINT/SINT plus the 15-format set
// reported by TypedUAVLoadAdditionalFormats. Each format: a CS reads every texel of a typed RWTexture2D (and a
// typed RWBuffer) and writes the raw loaded value to a structured buffer; values are exact per component.

struct TypedLoadFormat {
  DXGI_FORMAT format;
  const char *name;
  const char *hlsl;  // element type
  UINT texel_bytes;
  UINT kind;         // 0 float, 1 uint, 2 sint, 3 unorm
  UINT components;
  UINT bits;         // per component
};

void testTypedUavLoads(Context &c) {
  const TypedLoadFormat formats[] = {
      {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", "float", 4, 0, 1, 32},
      {DXGI_FORMAT_R32_UINT, "R32_UINT", "uint", 4, 1, 1, 32},
      {DXGI_FORMAT_R32_SINT, "R32_SINT", "int", 4, 2, 1, 32},
      {DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT", "float4", 16, 0, 4, 32},
      {DXGI_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", "uint4", 16, 1, 4, 32},
      {DXGI_FORMAT_R32G32B32A32_SINT, "R32G32B32A32_SINT", "int4", 16, 2, 4, 32},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", "float4", 8, 0, 4, 16},
      {DXGI_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", "uint4", 8, 1, 4, 16},
      {DXGI_FORMAT_R16G16B16A16_SINT, "R16G16B16A16_SINT", "int4", 8, 2, 4, 16},
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", "unorm float4", 4, 3, 4, 8},
      {DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", "uint4", 4, 1, 4, 8},
      {DXGI_FORMAT_R8G8B8A8_SINT, "R8G8B8A8_SINT", "int4", 4, 2, 4, 8},
      {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT", "float", 2, 0, 1, 16},
      {DXGI_FORMAT_R16_UINT, "R16_UINT", "uint", 2, 1, 1, 16},
      {DXGI_FORMAT_R16_SINT, "R16_SINT", "int", 2, 2, 1, 16},
      {DXGI_FORMAT_R8_UNORM, "R8_UNORM", "unorm float", 1, 3, 1, 8},
      {DXGI_FORMAT_R8_UINT, "R8_UINT", "uint", 1, 1, 1, 8},
      {DXGI_FORMAT_R8_SINT, "R8_SINT", "int", 1, 2, 1, 8},
  };
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "options");
  if (!options.TypedUAVLoadAdditionalFormats) fail("TypedUAVLoadAdditionalFormats");
  constexpr UINT W = 8, H = 4, N = W * H;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].Descriptor.ShaderRegister = 2;
  D3D12_ROOT_SIGNATURE_DESC root_desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  auto root = c.root(root_desc);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> heap;
  check(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "uav heap");
  const UINT stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  // Raw texel bytes: deterministic patterns covering sign bits, NaN-free floats, 0 and max values.
  auto texel = [](const TypedLoadFormat &f, UINT i, UINT comp) -> uint32_t {
    const uint32_t seed = (i * 4 + comp) * 2654435761u;
    if (f.kind == 0 && f.bits == 32) {
      const float values[] = {0.0f, -1.5f, 3.25f, 65504.0f, -0.0078125f, 1e-3f, 123456.75f, -2.0f};
      float v = values[(i + comp) % 8] + float(i);
      uint32_t bits;
      std::memcpy(&bits, &v, 4);
      return bits;
    }
    if (f.kind == 0) {  // half: finite, non-NaN bit patterns (exponent < 31)
      uint32_t h = seed >> 16;
      if (((h >> 10) & 31) == 31) h &= ~(1u << 14);
      return h & 0xffff;
    }
    return f.bits == 32 ? seed : (seed >> (32 - f.bits));
  };
  // Expected 32-bit result of the HLSL load, per component (raw bits for float/int, float bits for unorm/half).
  auto expected = [](const TypedLoadFormat &f, uint32_t raw) -> uint32_t {
    if (f.kind == 1 || (f.kind == 0 && f.bits == 32)) return raw;
    if (f.kind == 2) return f.bits == 32 ? raw : uint32_t(int32_t(raw << (32 - f.bits)) >> (32 - f.bits));
    float v;
    if (f.kind == 3) {
      v = float(raw) / float((1u << f.bits) - 1);
    } else {  // half to float, exact
      const uint32_t sign = (raw >> 15) & 1, exponent = (raw >> 10) & 31, mantissa = raw & 1023;
      v = exponent ? std::ldexp(float(mantissa | 1024), int(exponent) - 25) : std::ldexp(float(mantissa), -24);
      if (sign) v = -v;
    }
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return bits;
  };
  UINT verified = 0;
  for (const auto &f : formats) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f.format};
    check(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "format support");
    if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
      std::fprintf(stderr, "typed UAV load format=%s support1=%08x support2=%08x\n", f.name, support.Support1,
                   support.Support2);
      fail("typed UAV load support bit");
    }
    std::string source = std::string("RWTexture2D<") + f.hlsl + "> tex : register(u0);\nRWBuffer<" + f.hlsl +
                         "> buf : register(u1);\nRWStructuredBuffer<uint4> out_values : register(u2);\n"
                         "[numthreads(8, 4, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
                         "  uint i = id.y * 8 + id.x;\n"
                         "  out_values[i] = asuint(tex[id.xy])" + (f.components == 1 ? ".xxxx" : "") + ";\n"
                         "  out_values[32 + i] = asuint(buf[i])" + (f.components == 1 ? ".xxxx" : "") + ";\n}\n";
    auto cs = shader(source.c_str(), "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{};
    cs_desc.pRootSignature = root.Get();
    cs_desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)), "typed load CS");
    auto tex = texture(c.device.Get(), f.format, W, H, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    auto buf = buffer(c.device.Get(), N * f.texel_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto out = buffer(c.device.Get(), 2 * N * 16, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto upload = buffer(c.device.Get(), 256 * H + N * f.texel_bytes, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    auto result = buffer(c.device.Get(), 2 * N * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint32_t> raw(N * 4, 0);
    unsigned char *mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "Map(upload)");
    std::memset(mapped, 0, 256 * H + N * f.texel_bytes);
    const UINT comp_bytes = f.bits / 8;
    for (UINT i = 0; i < N; ++i)
      for (UINT comp = 0; comp < f.components; ++comp) {
        raw[i * 4 + comp] = texel(f, i, comp);
        std::memcpy(mapped + (i / W) * 256 + (i % W) * f.texel_bytes + comp * comp_bytes, &raw[i * 4 + comp],
                    comp_bytes);
        std::memcpy(mapped + 256 * H + i * f.texel_bytes + comp * comp_bytes, &raw[i * 4 + comp], comp_bytes);
      }
    upload->Unmap(0, nullptr);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC tex_uav{};
    tex_uav.Format = f.format;
    tex_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    c.device->CreateUnorderedAccessView(tex.Get(), nullptr, &tex_uav, cpu);
    cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC buf_uav{};
    buf_uav.Format = f.format;
    buf_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    buf_uav.Buffer.NumElements = N;
    c.device->CreateUnorderedAccessView(buf.Get(), nullptr, &buf_uav, cpu);
    auto list = c.begin(pso.Get());
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {f.format, W, H, 1, 256};
    dst.pResource = tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list.list->CopyBufferRegion(buf.Get(), 0, upload.Get(), 256 * H, N * f.texel_bytes);
    transition(list.list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list.list.Get(), buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list.list->SetDescriptorHeaps(1, heaps);
    list.list->SetComputeRootSignature(root.Get());
    list.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list.list->Dispatch(1, 1, 1);
    transition(list.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.list->CopyBufferRegion(result.Get(), 0, out.Get(), 0, 2 * N * 16);
    c.execute(list);
    auto bytes = readback(result.Get(), 2 * N * 16);
    for (UINT view = 0; view < 2; ++view)
      for (UINT i = 0; i < N; ++i)
        for (UINT comp = 0; comp < 4; ++comp) {
          uint32_t got;
          std::memcpy(&got, bytes.data() + (view * N + i) * 16 + comp * 4, 4);
          // Single-component views are replicated by .xxxx; absent components of multi-component formats
          // (none in this list) would default.
          const UINT source_comp = f.components == 1 ? 0 : comp;
          const uint32_t want = expected(f, raw[i * 4 + source_comp]);
          if (got != want) {
            std::fprintf(stderr, "typed UAV load format=%s view=%s texel=%u comp=%u raw=%08x got=%08x want=%08x\n",
                         f.name, view ? "buffer" : "texture2d", i, comp, raw[i * 4 + source_comp], got, want);
            fail("typed UAV load value");
          }
        }
    std::printf("OWNED_D3D12_FEATURE_LEVEL_TYPED_UAV_LOAD format=%s texels=%u views=texture2d,buffer\n", f.name, N);
    ++verified;
  }
  if (verified != 18) fail("typed UAV load format count");
  passed("typed_uav_load_18_required_formats_texture2d_and_buffer_exact");
}

int run() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) {
    std::fprintf(stderr, "OWNED_D3D12_FEATURE_LEVEL_FAIL step=backend\n");
    return 3;
  }
  std::printf("OWNED_D3D12_FEATURE_LEVEL_BACKEND id=%s\n", backend());
  Context c;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)), "D3D12CreateDevice(11_0)");
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  check(c.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&c.queue)), "queue");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");
  testLogicOps(c);
  testUavOnlyForced(c);
  testForcedOneMultisampleRtv(c);
  testTypedUavLoads(c);
  std::printf("OWNED_D3D12_FEATURE_LEVEL_PASS cases=%d\n", g_cases);
  return 0;
}

} // namespace

int main() {
  try {
    return run();
  } catch (const std::exception &) {
    return 1;
  }
}
