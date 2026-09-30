#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT kSize = 32;
constexpr char kMarker[] = "OWNED_D3D12_MSAA_LINES";

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=%08lx\n", kMarker, step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool condition, const char *step) { if (!condition) fail(step); }
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type,
                              D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  auto h = heap(type);
  ComPtr<ID3D12Resource> r;
  const HRESULT hr = device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state,
                                                     nullptr, IID_PPV_ARGS(&r));
  if (FAILED(hr)) {
    std::fprintf(stderr, "buffer bytes=%llu type=%u state=%u\n",
                 static_cast<unsigned long long>(bytes), unsigned(type), unsigned(state));
    fail("buffer", hr);
  }
  return r;
}
ComPtr<ID3DBlob> shader(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "msaa-lines", nullptr, nullptr,
                                entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("D3DCompile", hr);
  }
  return code;
}
std::vector<uint8_t> shader_file(const char *name) {
  FILE *f = std::fopen(name, "rb");
  if (!f) fail(name);
  require(std::fseek(f, 0, SEEK_END) == 0, "shader seek");
  const long size = std::ftell(f);
  require(size > 0 && std::fseek(f, 0, SEEK_SET) == 0, "shader size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  require(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size(), "shader read");
  std::fclose(f);
  return bytes;
}
void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
struct CaseResult {
  std::array<uint8_t, kSize * kSize * 4> rgba{};
  UINT solid = 0, partial = 0, lit = 0;
  UINT64 visible = 0;
  UINT uav_calls = 0;
};
struct GateResult {
  std::array<uint8_t, kSize * kSize * 4> rgba{};
  UINT64 first_visible = 0, second_visible = 0;
};
struct Fixture {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3DBlob> vs, vs_far, vs_extreme, vs_extreme_reverse, ps, ps_extreme;
  ComPtr<ID3DBlob> vs_depth_prepass, vs_depth_line, ps_gate_red, ps_gate_green;
  std::vector<uint8_t> vs_line_oracle, vs_line_edge, ps_line_eval, ps_line_sample;
  std::vector<uint8_t> ps_unused_sample_derivative;
  std::vector<uint8_t> ps_sample_derivative;
  ComPtr<ID3D12DescriptorHeap> rtv;
  ComPtr<ID3D12DescriptorHeap> dsv;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fence_value = 0;
  Fixture() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                            IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_ROOT_PARAMETER uav_param{};
    uav_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    uav_param.Descriptor.ShaderRegister = 0;
    uav_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_desc.NumParameters = 1; root_desc.pParameters = &uav_param;
    ComPtr<ID3DBlob> bytes, errors;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &bytes, &errors), "serialize root");
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "root");
    D3D12_DESCRIPTOR_HEAP_DESC h{};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; h.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&rtv)), "RTV heap");
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&dsv)), "DSV heap");
    constexpr char source[] = R"(
      static const float2 points[5] = {
        float2(-0.84, 0.65), float2(0.83, -0.57), float2(-0.65, -0.76),
        float2(-0.7, 0.17), float2(0.75, 0.35)
      };
      struct V {
        float4 position : SV_Position;
        noperspective float2 value : TEXCOORD0;
        nointerpolation float leading : TEXCOORD1;
        float projective : TEXCOORD2;
      };
      V vs(uint id : SV_VertexID) {
        V v; v.position = float4(points[id], 0.3, 1);
        v.value = points[id]; v.leading = id == 0 || id == 3 ? 0.25 : 0.75;
        v.projective = id * 0.25;
        return v;
      }
      V vs_far(uint id : SV_VertexID) {
        V v = vs(id); v.position.z = 1.3; return v;
      }
      V extreme(uint id, bool reverse_w) {
        V v = vs(id);
        float w = (id == 0) == reverse_w ? 1000.0 : 1.0;
        float ndc_x = (16.5 + (id == 0 ? -0.08 : 0.08)) / 16.0 - 1.0;
        float ndc_y = 1.0 - 16.5 / 16.0;
        v.position = float4(ndc_x * w, ndc_y * w, (id == 0 ? 0.2 : 0.8) * w, w);
        v.value = float2(id, 0);
        v.projective = id;
        v.leading = 0.25;
        return v;
      }
      V vs_extreme(uint id : SV_VertexID) { return extreme(id, false); }
      V vs_extreme_reverse(uint id : SV_VertexID) { return extreme(id, true); }
      float4 ps(V v) : SV_Target {
        return float4(saturate(v.value.x * 0.5 + 0.5), v.leading, 1, 1);
      }
      float4 ps_extreme(V v, float4 screen : SV_Position) : SV_Target {
        return float4(v.value.x, v.projective, screen.z, 1);
      }
    )";
    vs = shader(source, "vs", "vs_5_0");
    vs_far = shader(source, "vs_far", "vs_5_0");
    vs_extreme = shader(source, "vs_extreme", "vs_5_0");
    vs_extreme_reverse = shader(source, "vs_extreme_reverse", "vs_5_0");
    ps = shader(source, "ps", "ps_5_0");
    ps_extreme = shader(source, "ps_extreme", "ps_5_0");
    vs_line_oracle = shader_file("msaa-lines-vsLine.dxil");
    vs_line_edge = shader_file("msaa-lines-vsLineEdge.dxil");
    ps_line_eval = shader_file("msaa-lines-psEval.dxil");
    ps_line_sample = shader_file("msaa-lines-psSample.dxil");
    ps_unused_sample_derivative = shader_file("msaa-lines-psUnusedSampleDerivative.dxil");
    ps_sample_derivative = shader_file("msaa-lines-psSampleDerivative.dxil");
    constexpr char gate_source[] = R"(
      float4 vs_prepass(uint id : SV_VertexID) : SV_Position {
        float2 xy = float2(id & 1 ? 1 : -1, id & 2 ? -1 : 1);
        // DSV contains 0.65 - 0.02*x, independent of the line's depth.
        return float4(xy, 0.33 - 0.32*xy.x, 1);
      }
      float4 vs_line(uint id : SV_VertexID) : SV_Position {
        return float4(id ? 0.625 : -0.625, -0.03125, id ? 0.8 : 0.2, 1);
      }
      float4 ps_red() : SV_Target { return float4(1, 0, 0, 1); }
      float4 ps_green() : SV_Target { return float4(0, 1, 0, 1); }
    )";
    vs_depth_prepass = shader(gate_source, "vs_prepass", "vs_5_0");
    vs_depth_line = shader(gate_source, "vs_line", "vs_5_0");
    ps_gate_red = shader(gate_source, "ps_red", "ps_5_0");
    ps_gate_green = shader(gate_source, "ps_green", "ps_5_0");
  }
  ~Fixture() { if (event) CloseHandle(event); }
  void drain() {
    check(queue->Signal(fence.Get(), ++fence_value), "signal");
    check(fence->SetEventOnCompletion(fence_value, event), "fence event");
    require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0 &&
            fence->GetCompletedValue() >= fence_value, "GPU completion");
  }
  CaseResult draw(UINT samples, bool multisample, bool antialias,
                  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                  bool indexed = false, bool indirect = false, bool restart = false,
                  bool beyond_far_plane = false, bool depth_clip = true,
                  UINT sample_mask = UINT_MAX, bool extreme = false,
                  bool reverse_w = false, UINT line_oracle = 0) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.Get();
    ID3DBlob *vertex = extreme ? (reverse_w ? vs_extreme_reverse.Get() : vs_extreme.Get())
                              : beyond_far_plane ? vs_far.Get() : vs.Get();
    p.VS = line_oracle == 3 ? D3D12_SHADER_BYTECODE{vs_line_edge.data(), vs_line_edge.size()} :
           line_oracle ? D3D12_SHADER_BYTECODE{vs_line_oracle.data(), vs_line_oracle.size()} :
                           D3D12_SHADER_BYTECODE{vertex->GetBufferPointer(), vertex->GetBufferSize()};
    ID3DBlob *pixel = extreme ? ps_extreme.Get() : ps.Get();
    p.PS = line_oracle == 1 ? D3D12_SHADER_BYTECODE{ps_line_eval.data(), ps_line_eval.size()} :
           line_oracle == 4 ? D3D12_SHADER_BYTECODE{ps_unused_sample_derivative.data(),
                                                    ps_unused_sample_derivative.size()} :
           line_oracle == 5 ? D3D12_SHADER_BYTECODE{ps_sample_derivative.data(),
                                                    ps_sample_derivative.size()} :
           line_oracle >= 2 ? D3D12_SHADER_BYTECODE{ps_line_sample.data(), ps_line_sample.size()} :
                              D3D12_SHADER_BYTECODE{pixel->GetBufferPointer(), pixel->GetBufferSize()};
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = depth_clip;
    p.RasterizerState.MultisampleEnable = multisample;
    p.RasterizerState.AntialiasedLineEnable = antialias;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = sample_mask;
    p.SampleDesc.Count = samples;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    if (restart) p.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)), "line PSO");
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = tex.Height = kSize;
    tex.DepthOrArraySize = tex.MipLevels = 1;
    tex.SampleDesc.Count = samples;
    tex.Format = p.RTVFormats[0];
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target, resolved;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex,
          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "line target");
    auto resolved_desc = tex;
    resolved_desc.SampleDesc.Count = 1;
    resolved_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (samples > 1)
      check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &resolved_desc,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)), "resolved");
    device->CreateRenderTargetView(target.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 image_bytes = 0;
    device->GetCopyableFootprints(&resolved_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
    auto readback = buffer(device.Get(), image_bytes, D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; qd.Count = 1;
    ComPtr<ID3D12QueryHeap> query;
    check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&query)), "occlusion heap");
    auto query_out = buffer(device.Get(), 8, D3D12_HEAP_TYPE_READBACK,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> counter, counter_zero, counter_out;
    if (line_oracle == 5) {
      counter = buffer(device.Get(), 4, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      counter_zero = buffer(device.Get(), 4, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
      counter_out = buffer(device.Get(), 4, D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST);
      void *zero = nullptr; D3D12_RANGE no_read{};
      check(counter_zero->Map(0, &no_read, &zero), "map counter clear");
      std::memset(zero, 0, 4); counter_zero->Unmap(0, nullptr);
    }
    const uint16_t list_indices[4] = {0, 1, 3, 4};
    const uint16_t strip_indices[6] = {0, 1, 2, 0xffff, 3, 4};
    const UINT index_count = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ? 6 : 4;
    auto index = indexed ? buffer(device.Get(), sizeof(strip_indices), D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ) : ComPtr<ID3D12Resource>{};
    if (index) {
      void *data = nullptr; D3D12_RANGE no_read{};
      check(index->Map(0, &no_read, &data), "map indices");
      std::memcpy(data, topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ?
                  static_cast<const void *>(strip_indices) : static_cast<const void *>(list_indices),
                  topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ? sizeof(strip_indices) : sizeof(list_indices));
      index->Unmap(0, nullptr);
    }
    D3D12_DRAW_INDEXED_ARGUMENTS draw_args{index_count, 1, 0, 0, 0};
    auto args = indirect ? buffer(device.Get(), sizeof(draw_args), D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ) : ComPtr<ID3D12Resource>{};
    if (args) {
      void *data = nullptr; D3D12_RANGE no_read{};
      check(args->Map(0, &no_read, &data), "map indirect");
      std::memcpy(data, &draw_args, sizeof(draw_args)); args->Unmap(0, nullptr);
    }
    ComPtr<ID3D12CommandSignature> signature;
    if (indirect) {
      D3D12_INDIRECT_ARGUMENT_DESC argument{};
      argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
      D3D12_COMMAND_SIGNATURE_DESC command{};
      command.ByteStride = sizeof(draw_args);
      command.NumArgumentDescs = 1; command.pArgumentDescs = &argument;
      check(device->CreateCommandSignature(&command, nullptr, IID_PPV_ARGS(&signature)), "signature");
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                    pso.Get(), IID_PPV_ARGS(&list)), "command list");
    const auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
    list->SetGraphicsRootSignature(root.Get());
    if (counter) {
      list->CopyBufferRegion(counter.Get(), 0, counter_zero.Get(), 0, 4);
      barrier(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetGraphicsRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
    }
    list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    const float clear[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(handle, clear, 0, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(topology);
    if (indexed) {
      D3D12_INDEX_BUFFER_VIEW ib{};
      ib.BufferLocation = index->GetGPUVirtualAddress();
      ib.SizeInBytes = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ?
                       sizeof(strip_indices) : sizeof(list_indices);
      ib.Format = DXGI_FORMAT_R16_UINT;
      list->IASetIndexBuffer(&ib);
    }
    list->BeginQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    if (indirect) list->ExecuteIndirect(signature.Get(), 1, args.Get(), 0, nullptr, 0);
    else if (indexed) list->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    else list->DrawInstanced(2, 1, 0, 0);
    list->EndQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1,
                           query_out.Get(), 0);
    if (counter) {
      D3D12_RESOURCE_BARRIER uav{};
      uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = counter.Get();
      list->ResourceBarrier(1, &uav);
      barrier(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(counter_out.Get(), 0, counter.Get(), 0, 4);
    }
    ID3D12Resource *source = target.Get();
    if (samples > 1) {
      barrier(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
              D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, tex.Format);
      barrier(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
      source = resolved.Get();
    } else {
      barrier(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "close");
    ID3D12CommandList *submitted = list.Get();
    queue->ExecuteCommandLists(1, &submitted);
    drain();
    auto image = std::vector<uint8_t>(image_bytes);
    void *data = nullptr; D3D12_RANGE read{0, SIZE_T(image_bytes)};
    check(readback->Map(0, &read, &data), "map image");
    std::memcpy(image.data(), data, image.size()); readback->Unmap(0, nullptr);
    CaseResult result;
    for (UINT y = 0; y < kSize; ++y)
      for (UINT x = 0; x < kSize; ++x) {
        const auto *pixel = image.data() + y * footprint.Footprint.RowPitch + x * 4;
        const UINT index = (y * kSize + x) * 4;
        std::memcpy(result.rgba.data() + index, pixel, 4);
        const uint8_t alpha = pixel[3];
        result.solid += alpha == 255;
        result.partial += alpha > 0 && alpha < 255;
        result.lit += alpha > 0;
      }
    read = {0, 8};
    check(query_out->Map(0, &read, &data), "map occlusion");
    std::memcpy(&result.visible, data, 8); query_out->Unmap(0, nullptr);
    if (counter_out) {
      read = {0, 4};
      check(counter_out->Map(0, &read, &data), "map line UAV count");
      std::memcpy(&result.uav_calls, data, 4); counter_out->Unmap(0, nullptr);
    }
    return result;
  }
  GateResult depth_stencil_gate(UINT mode) {
    // mode 0/1: LESS/GREATER against the gradient; 2: stencil read-mask miss;
    // mode 3: stencil write-mask followed by a full-byte stencil comparison.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.Get();
    p.SampleDesc.Count = 4;
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.StencilEnable = TRUE;
    p.DepthStencilState.StencilReadMask = 0xff;
    p.DepthStencilState.StencilWriteMask = 0x0f;
    p.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    p.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    p.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.VS = {vs_depth_prepass->GetBufferPointer(), vs_depth_prepass->GetBufferSize()};
    ComPtr<ID3D12PipelineState> prepass, line, verify_stencil;
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&prepass)), "gradient prepass PSO");
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    p.VS = {vs_depth_line->GetBufferPointer(), vs_depth_line->GetBufferSize()};
    p.PS = {ps_gate_red->GetBufferPointer(), ps_gate_red->GetBufferSize()};
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    p.DepthStencilState.DepthFunc = mode == 1 ? D3D12_COMPARISON_FUNC_GREATER :
                                     D3D12_COMPARISON_FUNC_LESS;
    if (mode == 3) p.DepthStencilState.DepthEnable = FALSE;
    p.DepthStencilState.StencilReadMask = 0x0f;
    p.DepthStencilState.StencilWriteMask = 0xf0;
    p.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    p.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&line)), "DSV line PSO");
    if (mode == 3) {
      p.PS = {ps_gate_green->GetBufferPointer(), ps_gate_green->GetBufferSize()};
      p.DepthStencilState.StencilReadMask = 0xff;
      p.DepthStencilState.StencilWriteMask = 0;
      p.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
      p.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
      check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&verify_stencil)),
            "full-byte stencil compare PSO");
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = kSize;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 4;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target, resolved, depth;
    check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "gate target");
    auto resolved_desc = desc;
    resolved_desc.SampleDesc.Count = 1; resolved_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &resolved_desc,
          D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)), "gate resolve");
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear_depth{};
    clear_depth.Format = desc.Format; clear_depth.DepthStencil = {1, 0xa0};
    check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_depth, IID_PPV_ARGS(&depth)), "gate DSV");
    const auto rt = rtv->GetCPUDescriptorHandleForHeapStart();
    const auto ds = dsv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rt);
    device->CreateDepthStencilView(depth.Get(), nullptr, ds);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 image_bytes = 0;
    device->GetCopyableFootprints(&resolved_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
    auto image = buffer(device.Get(), image_bytes, D3D12_HEAP_TYPE_READBACK,
                        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; qd.Count = 2;
    ComPtr<ID3D12QueryHeap> queries;
    check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "gate query");
    auto query_out = buffer(device.Get(), 16, D3D12_HEAP_TYPE_READBACK,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&allocator)), "gate allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                    prepass.Get(), IID_PPV_ARGS(&list)), "gate list");
    list->SetGraphicsRootSignature(root.Get());
    const float clear[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(rt, clear, 0, nullptr);
    list->ClearDepthStencilView(ds, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                1, 0xa0, 0, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(0, nullptr, FALSE, &ds);
    list->OMSetStencilRef(0xa5);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list->DrawInstanced(4, 1, 0, 0);
    list->OMSetRenderTargets(1, &rt, FALSE, &ds);
    list->SetPipelineState(line.Get());
    list->OMSetStencilRef(mode == 2 ? 0x36 : 0x35);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->DrawInstanced(2, 1, 0, 0);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    if (mode == 3) {
      list->SetPipelineState(verify_stencil.Get());
      list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
      list->DrawInstanced(2, 1, 0, 0);
      list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
    }
    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0,
                           mode == 3 ? 2 : 1, query_out.Get(), 0);
    barrier(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, resolved_desc.Format);
    barrier(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = resolved.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = image.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "close gate");
    ID3D12CommandList *submitted = list.Get(); queue->ExecuteCommandLists(1, &submitted);
    drain();
    GateResult result;
    void *data = nullptr; D3D12_RANGE read{0, SIZE_T(image_bytes)};
    check(image->Map(0, &read, &data), "map gate image");
    for (UINT y = 0; y < kSize; ++y)
      for (UINT x = 0; x < kSize; ++x)
        std::memcpy(result.rgba.data() + (y * kSize + x) * 4,
                    static_cast<const uint8_t *>(data) + y * footprint.Footprint.RowPitch + x * 4, 4);
    image->Unmap(0, nullptr);
    read = {0, mode == 3 ? 16u : 8u};
    check(query_out->Map(0, &read, &data), "map gate occlusion");
    std::memcpy(&result.first_visible, data, 8);
    if (mode == 3) std::memcpy(&result.second_visible, static_cast<const uint8_t *>(data) + 8, 8);
    query_out->Unmap(0, nullptr);
    return result;
  }
};
void equivalent(const CaseResult &a, const CaseResult &b, const char *step) {
  require(a.rgba == b.rgba && a.visible == b.visible, step);
}
} // namespace
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12 module");
  using BackendId = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backend && backend() && std::strcmp(backend(), "owned-graphics-d3d12") == 0,
          "owned backend identity");
  std::printf("%s_BACKEND id=%s\n", kMarker, backend());
  Fixture f;
  const auto aliased = f.draw(4, false, false);
  const auto alpha = f.draw(4, false, true);
  const auto quad = f.draw(4, true, false);
  const auto both = f.draw(4, true, true);
  require(aliased.solid > 0 && aliased.partial == 0, "aliased line has partial samples");
  require(aliased.visible == UINT64(aliased.solid) * 4,
          "aliased line must cover every sample of each lit pixel");
  const auto masked = f.draw(4, false, false, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                             false, false, false, false, true, 0x5);
  require(masked.solid == 0 && masked.partial > 0 &&
          masked.visible == UINT64(masked.lit) * 2,
          "aliased line must honor two-of-four pipeline sample mask");
  require(alpha.partial > 0 && alpha.rgba != aliased.rgba, "alpha-AA failed to modulate coverage");
  require(quad.partial > 0 && quad.rgba != aliased.rgba, "quad failed per-sample coverage");
  require(quad.lit > aliased.lit && quad.visible > aliased.visible,
          "quadrilateral line must rasterize its width at sample locations");
  equivalent(quad, both, "MSAA flag must override alpha-AA flag");
  for (UINT mode = 0; mode < 3; ++mode) {
    const bool ms = mode == 2, aa = mode == 1;
    const auto clipped = f.draw(4, ms, aa, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                                false, false, false, true, true);
    const auto clamped = f.draw(4, ms, aa, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                                false, false, false, true, false);
    require(clipped.lit == 0 && clipped.visible == 0, "far line bypassed depth clipping");
    require(clamped.lit > 0 && clamped.visible > 0, "disabled depth clip lost far line");
  }
  UINT interpolated = 0, flat = 0;
  for (UINT i = 0; i < kSize * kSize; ++i) {
    const UINT at = i * 4;
    if (!quad.rgba[at + 3]) continue;
    interpolated += quad.rgba[at] > 40 && quad.rgba[at] < 215;
    flat += quad.rgba[at + 1] == 64 || quad.rgba[at + 1] == 191;
  }
  require(interpolated > 20 && flat > 20, "line interpolation or leading vertex lost");
  // Independently derive the expected DSV comparison: the fullscreen prepass
  // writes 0.65-0.02*x and the line writes 0.2+0.6*(x-6)/20. Neither depth
  // comes from SV_Position.z in the line color shader. At x=8 and x=24 the
  // signs are opposite for every sample in a pixel.
  const auto line_depth = [](float x) { return 0.2f + 0.6f * (x - 6.f) / 20.f; };
  const auto pre_depth = [](float x) { return 0.65f - 0.02f * x; };
  require(line_depth(9) < pre_depth(8) && line_depth(24) > pre_depth(25),
          "independent depth oracle anchors");
  for (UINT mode = 0; mode < 4; ++mode) {
    const auto gate = f.depth_stencil_gate(mode);
    const auto pixel = [&](UINT x, UINT channel) { return gate.rgba[(16 * kSize + x) * 4 + channel]; };
    UINT64 color_samples = 0;
    for (UINT i = 0; i < kSize * kSize; ++i)
      color_samples += (UINT(gate.rgba[i * 4 + 3]) * 4 + 127) / 255;
    std::printf("%s_CASE DSV mode=%u left=%u,%u,%u right=%u,%u,%u occlusion=%llu,%llu color-samples=%llu\n",
                kMarker, mode, pixel(8, 0), pixel(8, 1), pixel(8, 3),
                pixel(24, 0), pixel(24, 1), pixel(24, 3),
                static_cast<unsigned long long>(gate.first_visible),
                static_cast<unsigned long long>(gate.second_visible),
                static_cast<unsigned long long>(color_samples));
    require(gate.first_visible == color_samples, "line DSV occlusion disagrees with resolved samples");
    if (mode == 0)
      require(pixel(8, 0) == 255 && pixel(24, 3) == 0 && gate.first_visible > 0,
              "LESS line DSV gradient comparison");
    if (mode == 1)
      require(pixel(8, 3) == 0 && pixel(24, 0) == 255 && gate.first_visible > 0,
              "GREATER line DSV gradient comparison");
    if (mode == 2)
      require(pixel(8, 3) == 0 && pixel(24, 3) == 0 && gate.first_visible == 0,
              "stencil read mask or reference did not reject line");
    if (mode == 3)
      require(pixel(8, 1) == 255 && pixel(24, 1) == 255 &&
              gate.second_visible == gate.first_visible && gate.first_visible > 0,
              "stencil write mask/full-byte reference did not gate second line");
  }
  // Four D3D standard-pattern sample x positions are 3/8, 7/8, 1/8, 5/8.
  // The 20-pixel line carries value=(screen_x-6)/20, so the expected
  // EvaluateAtSample/Snapped, sample-qualified, centroid and ddx results are
  // computed from screen coordinates, not from another shader output.
  const float sample_x[4] = {0.375f, 0.875f, 0.125f, 0.625f};
  const auto encoded = [](float v) { return UINT(v * 255 + 0.5f); };
  const auto near_byte = [](uint8_t actual, UINT expected) {
    return actual + 2 >= expected && actual <= expected + 2;
  };
  for (UINT which : {2u, 1u}) {
    const auto result = f.draw(4, false, false, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                               false, false, false, false, true, UINT_MAX, false, false, which);
    const UINT at = (16 * kSize + 16) * 4;
    std::printf("%s_CASE line-input=%u center=%u,%u,%u,%u occlusion=%llu\n", kMarker,
                which, UINT(result.rgba[at]), UINT(result.rgba[at + 1]),
                UINT(result.rgba[at + 2]), UINT(result.rgba[at + 3]),
                static_cast<unsigned long long>(result.visible));
    require(result.visible == 76, "line input oracle sample coverage");
    if (which == 1)
      require(near_byte(result.rgba[at], encoded((16 + sample_x[0] - 6)/20)) &&
              near_byte(result.rgba[at + 1], encoded((16 + sample_x[3] - 6)/20)) &&
              near_byte(result.rgba[at + 2], encoded((16.5f - 6)/20)) &&
              near_byte(result.rgba[at + 3], encoded(4.f/20)),
              "line EvaluateAtSample/Snapped or ddx slope");
    else
      require(near_byte(result.rgba[at], encoded((0.5f +
                   ((16 + sample_x[0] - 6)/20 - 0.525f)*32)/4)) &&
              near_byte(result.rgba[at + 1], encoded((0.5f +
                   ((16 + sample_x[3] - 6)/20 - 0.525f)*32)/4)) &&
              near_byte(result.rgba[at + 2], encoded(0.5f)) &&
              result.rgba[at + 3] == 255,
              "line sample-qualified/centroid input");
  }
  const auto centroid_edge = f.draw(4, true, false, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                                      false, false, false, false, true, UINT_MAX, false, false, 3);
  const UINT cm = (16 * kSize + 16) * 4;
  std::printf("%s_CASE line-centroid-edge center=%u,%u,%u,%u occlusion=%llu\n", kMarker,
              UINT(centroid_edge.rgba[cm]), UINT(centroid_edge.rgba[cm + 1]),
              UINT(centroid_edge.rgba[cm + 2]), UINT(centroid_edge.rgba[cm + 3]),
              static_cast<unsigned long long>(centroid_edge.visible));
  const UINT sample0_encoded = encoded((0.5f + ((16 + sample_x[0] - 6)/20 - 0.525f)*32)/4);
  // Two of four geometric samples cover this edge pixel. Both invocations
  // must evaluate centroid at a covered point (sample 0), not pixel center.
  require(near_byte(centroid_edge.rgba[cm], sample0_encoded) &&
          centroid_edge.rgba[cm + 1] == 0 &&
          near_byte(centroid_edge.rgba[cm + 2], sample0_encoded * 2) &&
          near_byte(centroid_edge.rgba[cm + 3], encoded(0.5f)) &&
          centroid_edge.visible > 0,
          "line centroid stayed at uncovered center");
  const auto unused_sample = f.draw(4, false, false, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                                    false, false, false, false, true, UINT_MAX, false, false, 4);
  std::printf("%s_CASE unused-sample-derivative center=%u,%u,%u,%u occlusion=%llu\n", kMarker,
              UINT(unused_sample.rgba[cm]), UINT(unused_sample.rgba[cm + 1]),
              UINT(unused_sample.rgba[cm + 2]), UINT(unused_sample.rgba[cm + 3]),
              static_cast<unsigned long long>(unused_sample.visible));
  const auto sample_derivative = f.draw(4, false, false, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                                        false, false, false, false, true, UINT_MAX, false, false, 5);
  std::printf("%s_CASE sample-derivative center=%u,%u,%u,%u occlusion=%llu uav=%u\n", kMarker,
              UINT(sample_derivative.rgba[cm]), UINT(sample_derivative.rgba[cm + 1]),
              UINT(sample_derivative.rgba[cm + 2]), UINT(sample_derivative.rgba[cm + 3]),
              static_cast<unsigned long long>(sample_derivative.visible), sample_derivative.uav_calls);
  require(near_byte(unused_sample.rgba[cm], encoded((16.5f - 6)/20)) &&
          near_byte(unused_sample.rgba[cm + 1], encoded((16.5f - 6)/20)) &&
          unused_sample.rgba[cm + 3] == 0 && unused_sample.visible == 76,
          "unused sample-qualified field corrupted line ddy");
  require(near_byte(sample_derivative.rgba[cm], sample0_encoded) &&
          near_byte(sample_derivative.rgba[cm + 1], encoded((0.5f +
                    ((16 + sample_x[3] - 6)/20 - 0.525f)*32)/4)) &&
          near_byte(sample_derivative.rgba[cm + 2], encoded(0.5f)) &&
          near_byte(sample_derivative.rgba[cm + 3], encoded(4.f/20)) &&
          sample_derivative.visible == 76 && sample_derivative.uav_calls == 76,
          "sample-frequency line derivative slope");
  for (bool reverse_w : {false, true}) {
    const auto result = f.draw(4, false, true, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                               false, false, false, false, true, UINT_MAX, true, reverse_w);
    const UINT at = (16 * kSize + 16) * 4;
    std::printf("%s_CASE extreme-reverse=%u center=%u,%u,%u,%u visible=%llu\n", kMarker,
                UINT(reverse_w), UINT(result.rgba[at]), UINT(result.rgba[at + 1]),
                UINT(result.rgba[at + 2]), UINT(result.rgba[at + 3]),
                static_cast<unsigned long long>(result.visible));
    require(result.rgba[at + 3] > 0 && result.rgba[at + 3] < 255,
            "subpixel line alpha coverage lost at extreme W ratio");
    require(result.rgba[at] >= 120 && result.rgba[at] <= 136,
            "noperspective center interpolant stretched by line cap");
    require(reverse_w ? result.rgba[at + 1] >= 253 : result.rgba[at + 1] <= 2,
            "perspective center interpolant stretched by line cap");
    require(result.rgba[at + 2] >= 120 && result.rgba[at + 2] <= 136,
            "extreme line depth changed with cap carrier");
  }
  const auto one = f.draw(1, false, false);
  equivalent(one, f.draw(1, false, true), "1x alpha flag changed rasterization");
  equivalent(one, f.draw(1, true, false), "1x multisample flag changed rasterization");
  equivalent(one, f.draw(1, true, true), "1x combined flags changed rasterization");
  for (UINT mode = 0; mode < 3; ++mode) {
    const bool ms = mode == 2, aa = mode == 1;
    const auto direct = f.draw(4, ms, aa, D3D_PRIMITIVE_TOPOLOGY_LINELIST, true);
    const auto indirect = f.draw(4, ms, aa, D3D_PRIMITIVE_TOPOLOGY_LINELIST, true, true);
    equivalent(direct, indirect, "indexed line indirect differs from direct");
    const auto strip = f.draw(4, ms, aa, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, false, true);
    const auto strip_indirect = f.draw(4, ms, aa, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, true, true);
    equivalent(strip, strip_indirect, "indexed restart strip indirect differs from direct");
    require(strip.lit >= direct.lit && strip.visible >= direct.visible,
            "restart strip lost its first and last segments");
    std::printf("%s_CASE_PASS mode=%u list=%u,%u,%llu strip=%u,%u,%llu\n", kMarker, mode,
                direct.solid, direct.partial, static_cast<unsigned long long>(direct.visible),
                strip.solid, strip.partial, static_cast<unsigned long long>(strip.visible));
  }
  std::printf("%s_CASE_PASS modes alias=%u,%u,%llu alpha=%u,%u,%llu quad=%u,%u,%llu\n", kMarker,
              aliased.solid, aliased.partial, static_cast<unsigned long long>(aliased.visible),
              alpha.solid, alpha.partial, static_cast<unsigned long long>(alpha.visible),
              quad.solid, quad.partial, static_cast<unsigned long long>(quad.visible));
  std::printf("%s_PASS\n", kMarker);
  return 0;
}
