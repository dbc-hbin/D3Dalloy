// OPTIONS13 public ABI and GPU oracles. The oracle never consults the translator.
// Contract: https://microsoft.github.io/DirectX-Specs/d3d/VulkanOn12.html
// Build: clang++ -std=c++20 -O2 -static <this file> -ld3d12 -ldxgi -ldxguid -ld3dcompiler
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Public ABI values from DirectX-Headers; the bundled Wine header predates OPTIONS13.
constexpr D3D12_FEATURE kOptions13 = static_cast<D3D12_FEATURE>(42);
constexpr D3D12_BLEND kAlphaFactor = static_cast<D3D12_BLEND>(20);
constexpr D3D12_BLEND kInverseAlphaFactor = static_cast<D3D12_BLEND>(21);
struct Options13 {
  BOOL UnrestrictedBufferTextureCopyPitchSupported;
  BOOL UnrestrictedVertexElementAlignmentSupported;
  BOOL InvertedViewportHeightFlipsYSupported;
  BOOL InvertedViewportDepthFlipsZSupported;
  BOOL TextureCopyBetweenDimensionsSupported;
  BOOL AlphaBlendFactorSupported;
};
static_assert(sizeof(Options13) == 24);

[[noreturn]] void fail(const char *what, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_OPTIONS13_FAIL step=%s hr=%08lx\n", what, (unsigned long)hr);
  throw std::runtime_error(what);
}
void check(HRESULT hr, const char *what) { if (FAILED(hr)) fail(what, hr); }
void require(bool value, const char *what) { if (!value) fail(what); }

void upload(ID3D12Resource *resource, const std::vector<uint8_t> &bytes) {
  void *mapped = nullptr;
  D3D12_RANGE none{};
  check(resource->Map(0, &none, &mapped), "Map upload");
  std::memcpy(mapped, bytes.data(), bytes.size());
  D3D12_RANGE written{0, bytes.size()};
  resource->Unmap(0, &written);
}
std::vector<uint8_t> readback(ID3D12Resource *resource, size_t size) {
  void *mapped = nullptr;
  D3D12_RANGE read{0, size}, none{};
  check(resource->Map(0, &read, &mapped), "Map readback");
  std::vector<uint8_t> bytes(size);
  std::memcpy(bytes.data(), mapped, size);
  resource->Unmap(0, &none);
  return bytes;
}
void equalBytes(const std::vector<uint8_t> &actual, const std::vector<uint8_t> &expected, const char *what) {
  require(actual.size() == expected.size(), "oracle length");
  for (size_t i = 0; i < actual.size(); ++i) if (actual[i] != expected[i]) {
    std::fprintf(stderr, "byte mismatch at=%zu actual=%02x expected=%02x\n", i, actual[i], expected[i]);
    fail(what);
  }
}
void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
D3D12_TEXTURE_COPY_LOCATION subresource(ID3D12Resource *resource, UINT index = 0) {
  D3D12_TEXTURE_COPY_LOCATION location{};
  location.pResource = resource;
  location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  location.SubresourceIndex = index;
  return location;
}
D3D12_TEXTURE_COPY_LOCATION footprint(ID3D12Resource *resource, DXGI_FORMAT format, UINT width, UINT height,
                                      UINT depth, UINT row_pitch, UINT64 offset) {
  D3D12_TEXTURE_COPY_LOCATION location{};
  location.pResource = resource;
  location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  location.PlacedFootprint = {offset, {format, width, height, depth, row_pitch}};
  return location;
}

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 serial = 0;
  D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
  };
  void setQueue(D3D12_COMMAND_LIST_TYPE next) {
    type = next;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    queue.Reset();
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  }
  Recording begin(ID3D12PipelineState *pso = nullptr) {
    Recording r;
    check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&r.allocator)), "CreateCommandAllocator");
    check(device->CreateCommandList(0, type, r.allocator.Get(), pso, IID_PPV_ARGS(&r.list)), "CreateCommandList");
    return r;
  }
  void submit(Recording &r, bool close = true) {
    if (close) check(r.list->Close(), "Close");
    ID3D12CommandList *lists[] = {r.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "CreateEvent");
    const HRESULT signal = queue->Signal(fence.Get(), ++serial);
    if (FAILED(signal)) { CloseHandle(event); fail("Signal", signal); }
    const HRESULT armed = fence->SetEventOnCompletion(serial, event);
    if (FAILED(armed)) { CloseHandle(event); fail("SetEventOnCompletion", armed); }
    const DWORD waited = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    require(waited == WAIT_OBJECT_0, "bounded GPU completion");
    check(device->GetDeviceRemovedReason(), "device removed");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE heap,
                                D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST) {
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&resource)), "Create buffer");
    return resource;
  }
  ComPtr<ID3D12Resource> texture(D3D12_RESOURCE_DIMENSION dimension, DXGI_FORMAT format,
                                 UINT width, UINT height, UINT depth,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST) {
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = dimension; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = UINT16(depth); desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&resource)), "Create texture");
    return resource;
  }
  ComPtr<ID3D12RootSignature> root() {
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 0, nullptr,
                                  D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> bytes, errors;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "Serialize root");
    ComPtr<ID3D12RootSignature> result;
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&result)),
          "Create root");
    return result;
  }
};

ComPtr<ID3DBlob> shader(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> bytes, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "options13-oracle", nullptr, nullptr,
                                entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytes, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("compile fixture shader", hr);
  }
  return bytes;
}
constexpr const char *raster_source = R"(
  cbuffer Constants : register(b0) { float4 color; };
  struct V { float4 position : SV_Position; float2 uv : TEXCOORD; float value : VALUE; };
  V vs(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    V v; v.position=float4(t*float2(2,-2)+float2(-1,1),.25,1); v.uv=t; v.value=1; return v;
  }
  V vs_ia(float2 position : POSITION, float value : VALUE) {
    V v; v.position=float4(position,.25,1); v.uv=(position-float2(-1,1))/float2(2,-2);
    v.value=value; return v;
  }
  float4 ps(V v) : SV_Target { return color; }
  float4 ps_viewport(V v) : SV_Target { return float4(v.uv,v.position.z,1); }
  float4 ps_ia(V v) : SV_Target { return float4(v.value,v.value,v.value,1); }
)";

D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc(ID3D12RootSignature *root, ID3DBlob *vs, ID3DBlob *ps) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.SampleMask = UINT_MAX; desc.SampleDesc.Count = 1;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  for (auto &rt : desc.BlendState.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  return desc;
}

std::vector<float> render(Context &c, ID3D12PipelineState *pso, ID3D12RootSignature *root,
                           const D3D12_VIEWPORT &viewport, const std::array<float, 4> &clear,
                           const std::function<void(ID3D12GraphicsCommandList *)> &commands,
                           bool replay = false) {
  auto texture = c.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8, 1,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create RTV heap");
  const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  c.device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
  auto result = c.buffer(256 * 8, D3D12_HEAP_TYPE_READBACK);
  auto r = c.begin(pso);
  r.list->SetGraphicsRootSignature(root);
  r.list->ClearRenderTargetView(rtv, clear.data(), 0, nullptr);
  r.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  r.list->RSSetViewports(1, &viewport);
  const D3D12_RECT scissor{0, 0, 8, 8};
  r.list->RSSetScissorRects(1, &scissor);
  r.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands(r.list.Get());
  barrier(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto src = subresource(texture.Get());
  auto dst = footprint(result.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8, 1, 256, 0);
  r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  c.submit(r);
  const auto first = readback(result.Get(), 256 * 8);
  if (replay) {
    c.submit(r, false);
    equalBytes(readback(result.Get(), 256 * 8), first, "closed list replay");
  }
  std::vector<float> pixels(8 * 8 * 4);
  for (UINT y = 0; y < 8; ++y) std::memcpy(pixels.data() + y * 32, first.data() + y * 256, 8 * 16);
  return pixels;
}
void expectFloat(float actual, float expected, const char *what) {
  if (!std::isfinite(actual) || std::fabs(actual - expected) > 0.00001f) {
    std::fprintf(stderr, "float mismatch actual=%.9g expected=%.9g\n", actual, expected); fail(what);
  }
}

void testViewport(Context &c, bool y_axis) {
  auto root = c.root();
  auto vs = shader(raster_source, "vs", "vs_5_0"), ps = shader(raster_source, "ps_viewport", "ps_5_0");
  auto desc = pipelineDesc(root.Get(), vs.Get(), ps.Get());
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "viewport PSO");
  for (UINT inverted = 0; inverted < 2; ++inverted) {
    const D3D12_VIEWPORT viewport{0, y_axis && inverted ? 8.0f : 0.0f, 8,
                                  y_axis && inverted ? -8.0f : 8.0f,
                                  !y_axis && inverted ? 1.0f : 0.0f,
                                  !y_axis && inverted ? 0.0f : 1.0f};
    const auto pixels = render(c, pso.Get(), root.Get(), viewport, {0, 0, 0, 0},
                               [](auto *list) { list->DrawInstanced(3, 1, 0, 0); }, true);
    for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
      const float wanted[] = {(x + .5f) / 8, y_axis && inverted ? 1 - (y + .5f) / 8 : (y + .5f) / 8,
                               !y_axis && inverted ? .75f : .25f, 1};
      for (UINT k = 0; k < 4; ++k) expectFloat(pixels[(y * 8 + x) * 4 + k], wanted[k], "viewport exact pixel");
    }
  }
}

void testAlpha(Context &c) {
  auto root = c.root();
  auto vs = shader(raster_source, "vs", "vs_5_0"), ps = shader(raster_source, "ps", "ps_5_0");
  const std::array<float, 4> source{.5f, .25f, .75f, .625f}, destination{.125f, .75f, .25f, .5f};
  for (UINT inverted = 0; inverted < 2; ++inverted) {
    auto desc = pipelineDesc(root.Get(), vs.Get(), ps.Get());
    auto &blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = inverted ? kInverseAlphaFactor : kAlphaFactor;
    blend.DestBlend = blend.DestBlendAlpha = inverted ? kAlphaFactor : kInverseAlphaFactor;
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "alpha factor PSO");
    for (UINT mode = 0; mode < 6; ++mode) {
      const float alpha = mode < 4 ? mode / 4.0f : 1.0f;
      const auto pixels = render(c, pso.Get(), root.Get(), {0, 0, 8, 8, 0, 1}, destination,
        [&](auto *list) {
          list->SetGraphicsRoot32BitConstants(0, 4, source.data(), 0);
          const FLOAT factor[] = {.875f, .125f, .5f, alpha};
          // mode 4 checks the new/reset list default, mode 5 checks nullptr restoration.
          if (mode != 4) list->OMSetBlendFactor(factor);
          if (mode == 5) { const FLOAT other[] = {0, 0, 0, 0}; list->OMSetBlendFactor(other); list->OMSetBlendFactor(nullptr); }
          list->DrawInstanced(3, 1, 0, 0);
        }, true);
      for (UINT i = 0; i < 64; ++i) for (UINT k = 0; k < 4; ++k) {
        const float a = inverted ? 1 - alpha : alpha;
        expectFloat(pixels[i * 4 + k], source[k] * a + destination[k] * (1 - a), "constant alpha blend");
      }
    }
  }
}

void testVertexAlignment(Context &c) {
  auto root = c.root();
  auto vs = shader(raster_source, "vs_ia", "vs_5_0"), ps = shader(raster_source, "ps_ia", "ps_5_0");
  D3D12_INPUT_ELEMENT_DESC elements[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 3, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"VALUE", 0, DXGI_FORMAT_R32_FLOAT, 0, 11, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  auto desc = pipelineDesc(root.Get(), vs.Get(), ps.Get());
  desc.InputLayout = {elements, 2};
  auto buffer = c.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  std::vector<uint8_t> bytes(256, 0);
  const float vertices[][3] = {{-1, 1, .125f}, {3, 1, .125f}, {-1, -3, .125f}};
  for (UINT i = 0; i < 3; ++i) std::memcpy(bytes.data() + 4 + i * 16, vertices[i], 12);
  upload(buffer.Get(), bytes);
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "potentially aligned input layout");
  const auto pixels = render(c, pso.Get(), root.Get(), {0, 0, 8, 8, 0, 1}, {0, 0, 0, 0},
    [&](auto *list) {
      const D3D12_VERTEX_BUFFER_VIEW view{buffer->GetGPUVirtualAddress() + 1, 255, 16};
      list->IASetVertexBuffers(0, 1, &view);
      list->DrawInstanced(3, 1, 0, 0);
    }, true);
  for (UINT i = 0; i < 64; ++i) for (UINT k = 0; k < 4; ++k)
    expectFloat(pixels[i * 4 + k], k == 3 ? 1 : .125f, "compensating vertex buffer address");
  // Both elements share a slot, so no base address can align 3 and 12 modulo four.
  elements[1].AlignedByteOffset = 12;
  pso.Reset();
  const HRESULT invalid = c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  require(invalid == E_INVALIDARG && !pso, "impossible input layout rejected");
}

struct Format { DXGI_FORMAT format; UINT bytes, block; const char *name; };
constexpr Format copy_formats[] = {
  {DXGI_FORMAT_R8_UINT, 1, 1, "r8"}, {DXGI_FORMAT_R16_UINT, 2, 1, "r16"},
  {DXGI_FORMAT_R8G8B8A8_UINT, 4, 1, "rgba8"}, {DXGI_FORMAT_R16G16B16A16_UINT, 8, 1, "rgba16"},
  {DXGI_FORMAT_R32G32B32A32_UINT, 16, 1, "rgba32"}, {DXGI_FORMAT_R32G32B32_FLOAT, 12, 1, "rgb32"},
  {DXGI_FORMAT_BC1_UNORM, 8, 4, "bc1"}, {DXGI_FORMAT_BC3_UNORM, 16, 4, "bc3"},
  {DXGI_FORMAT_BC7_UNORM, 16, 4, "bc7"},
};

void testCopyPitch(Context &c) {
  UINT verified = 0;
  for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY}) {
    c.setQueue(type);
    for (const auto &format : copy_formats) for (UINT dims = 1; dims <= 3; ++dims) {
      // Test BC in its universally supported 2D shape; scalar formats cover 1D and 3D.
      if (format.block > 1 && dims != 2) continue;
      const auto dimension = static_cast<D3D12_RESOURCE_DIMENSION>(dims + 1);
      const UINT width = format.block * 3, height = dims == 1 ? 1 : format.block * 2, depth = dims == 3 ? 3 : 1;
      const UINT rows = (height + format.block - 1) / format.block;
      const UINT src_pitch = format.bytes * 5, dst_pitch = format.bytes * 7;
      const UINT src_offset = format.bytes, dst_offset = format.bytes * 3;
      const UINT src_slice = src_pitch * rows, dst_slice = dst_pitch * rows;
      const UINT size = 512 + dst_offset + dst_slice * depth;
      std::vector<uint8_t> input(size, 0x42), expected(size, 0xcc), sentinel(size, 0xcc);
      for (UINT z = 0; z < depth; ++z) for (UINT y = 0; y < rows; ++y) for (UINT x = 0; x < 3 * format.bytes; ++x) {
        const uint8_t value = uint8_t(1 + ((z * 37 + y * 19 + x * 3) % 100));
        input[src_offset + z * src_slice + y * src_pitch + x] = value;
        expected[dst_offset + z * dst_slice + y * dst_pitch + x] = value;
      }
      auto source = c.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto guard = c.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto result = c.buffer(size, D3D12_HEAP_TYPE_READBACK);
      // COPY queues require COMMON ownership, not a 3D/compute COPY_DEST state.
      // Fixed-state upload/readback buffers are the exception. Keep the same byte oracle.
      auto texture = c.texture(dimension, format.format, width, height, depth, D3D12_RESOURCE_FLAG_NONE,
          type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST);
      upload(source.Get(), input); upload(guard.Get(), sentinel);
      auto r = c.begin();
      r.list->CopyBufferRegion(result.Get(), 0, guard.Get(), 0, size);
      auto src = footprint(source.Get(), format.format, width, height, depth, src_pitch, src_offset);
      auto tex = subresource(texture.Get());
      auto dst = footprint(result.Get(), format.format, width, height, depth, dst_pitch, dst_offset);
      r.list->CopyTextureRegion(&tex, 0, 0, 0, &src, nullptr);
      barrier(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      r.list->CopyTextureRegion(&dst, 0, 0, 0, &tex, nullptr);
      barrier(r.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
          type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST);
      c.submit(r);
      equalBytes(readback(result.Get(), size), expected, "texel-aligned pitch/offset with row guards");
      c.submit(r, false);
      equalBytes(readback(result.Get(), size), expected, "replayed pitch/offset with row guards");
      ++verified;
      std::printf("OWNED_D3D12_OPTIONS13_COPY format=%s dims=%u queue=%u bytes=%u guards=1\n",
                   format.name, dims, UINT(type), format.bytes);
    }
  }
  c.setQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
  require(verified == 63, "copy matrix count");
  // Invalid footprint addresses/pitches, extents and backing-buffer bounds are
  // rejected at Close before any command can reach the GPU. No device removal.
  auto data = c.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto texture = c.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R16_UINT, 3, 2, 1);
  auto tex = subresource(texture.Get());
  for (UINT invalid = 0; invalid < 6; ++invalid) {
    auto r = c.begin();
    auto src = footprint(data.Get(), DXGI_FORMAT_R16_UINT, 3, 2, 1, 10, 2);
    if (invalid == 0) src.PlacedFootprint.Offset = 1;
    if (invalid == 1) src.PlacedFootprint.Footprint.RowPitch = 7;
    if (invalid == 2) src.PlacedFootprint.Footprint.RowPitch = 4;
    if (invalid == 3) src.PlacedFootprint.Offset = 252;
    const D3D12_BOX outside{0, 0, 0, 4, 2, 1};
    const D3D12_BOX full{0, 0, 0, 3, 2, 1};
    r.list->CopyTextureRegion(&tex, invalid == 5 ? 1 : 0, 0, 0, &src,
        invalid == 4 ? &outside : invalid == 5 ? &full : nullptr);
    require(r.list->Close() == E_INVALIDARG, "invalid relaxed-copy footprint rejected");
    check(c.device->GetDeviceRemovedReason(), "invalid copy recording preserves device");
  }
  std::printf("OWNED_D3D12_OPTIONS13_COPY_BOUNDS rejected=6 replay=63\n");
}

void testCrossDimension(Context &c) {
  for (UINT from = 1; from <= 3; ++from) for (UINT to = 1; to <= 3; ++to) {
    if (from == to) continue;
    const auto src_dim = static_cast<D3D12_RESOURCE_DIMENSION>(from + 1);
    const auto dst_dim = static_cast<D3D12_RESOURCE_DIMENSION>(to + 1);
    const UINT src_height = from > 1 ? 4 : 1, src_depth = from == 3 ? 3 : 1;
    const UINT dst_height = to > 1 ? 4 : 1, dst_depth = to == 3 ? 3 : 1;
    const UINT src_slice = 256 * src_height, dst_slice = 256 * dst_height;
    const UINT size = 256 * 4 * 3;
    std::vector<uint8_t> input(size, 0x11), initial(size, 0x44), expected(size, 0xcc), guard(size, 0xcc);
    for (UINT z = 0; z < src_depth; ++z) for (UINT y = 0; y < src_height; ++y) for (UINT x = 0; x < 8; ++x)
      input[z * src_slice + y * 256 + x] = uint8_t(1 + z * 32 + y * 8 + x);
    for (UINT z = 0; z < dst_depth; ++z) for (UINT y = 0; y < dst_height; ++y)
      std::fill_n(expected.data() + z * dst_slice + y * 256, 8, 0x44);
    const UINT sy = from > 1 ? 1 : 0, sz = from == 3 ? 1 : 0;
    const UINT dy = to > 1 ? 2 : 0, dz = to == 3 ? 2 : 0;
    const UINT copy_height = from > 1 && to > 1 ? 2 : 1;
    for (UINT y = 0; y < copy_height; ++y)
      std::copy_n(input.data() + sz * src_slice + (sy + y) * 256 + 1, 4,
                   expected.data() + dz * dst_slice + (dy + y) * 256 + 2);
    auto upload_src = c.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto upload_dst = c.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto upload_guard = c.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto result = c.buffer(size, D3D12_HEAP_TYPE_READBACK);
    auto source = c.texture(src_dim, DXGI_FORMAT_R8_UINT, 8, src_height, src_depth);
    auto destination = c.texture(dst_dim, DXGI_FORMAT_R8_UINT, 8, dst_height, dst_depth);
    upload(upload_src.Get(), input); upload(upload_dst.Get(), initial); upload(upload_guard.Get(), guard);
    auto r = c.begin();
    auto src = subresource(source.Get()), dst = subresource(destination.Get());
    auto src_upload = footprint(upload_src.Get(), DXGI_FORMAT_R8_UINT, 8, src_height, src_depth, 256, 0);
    auto dst_upload = footprint(upload_dst.Get(), DXGI_FORMAT_R8_UINT, 8, dst_height, dst_depth, 256, 0);
    auto rb = footprint(result.Get(), DXGI_FORMAT_R8_UINT, 8, dst_height, dst_depth, 256, 0);
    r.list->CopyBufferRegion(result.Get(), 0, upload_guard.Get(), 0, size);
    r.list->CopyTextureRegion(&src, 0, 0, 0, &src_upload, nullptr);
    r.list->CopyTextureRegion(&dst, 0, 0, 0, &dst_upload, nullptr);
    barrier(r.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_BOX box{1, sy, sz, 5, sy + copy_height, sz + 1};
    r.list->CopyTextureRegion(&dst, 2, dy, dz, &src, &box);
    barrier(r.list.Get(), destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    r.list->CopyTextureRegion(&rb, 0, 0, 0, &dst, nullptr);
    barrier(r.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(r.list.Get(), destination.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    c.submit(r);
    equalBytes(readback(result.Get(), size), expected, "cross-dimensional inner region");
    c.submit(r, false);
    equalBytes(readback(result.Get(), size), expected, "replayed cross-dimensional inner region");
    for (UINT invalid = 0; invalid < 3; ++invalid) {
      auto rejected = c.begin();
      auto invalid_box = box;
      if (invalid == 0) invalid_box.right = 9;
      rejected.list->CopyTextureRegion(&dst, invalid == 1 ? 5 : 2, dy,
          invalid == 2 ? dst_depth : dz, &src, &invalid_box);
      require(rejected.list->Close() == E_INVALIDARG, "cross-dimensional bounds rejected");
      check(c.device->GetDeviceRemovedReason(), "cross-dimensional rejection preserves device");
    }
    std::printf("OWNED_D3D12_OPTIONS13_CROSS from=%u to=%u offsets=1 guards=1\n", from, to);
  }
  std::printf("OWNED_D3D12_OPTIONS13_CROSS_BOUNDS rejected=18 replay=6\n");
}

void testCapabilities(Context &c) {
  Options13 options{};
  check(c.device->CheckFeatureSupport(kOptions13, &options, sizeof(options)), "OPTIONS13 query");
  require(options.UnrestrictedBufferTextureCopyPitchSupported && options.UnrestrictedVertexElementAlignmentSupported &&
           options.InvertedViewportHeightFlipsYSupported && options.InvertedViewportDepthFlipsZSupported &&
           options.TextureCopyBetweenDimensionsSupported && options.AlphaBlendFactorSupported, "OPTIONS13 six flags");
  require(c.device->CheckFeatureSupport(kOptions13, &options, sizeof(options) - 1) == E_INVALIDARG,
           "OPTIONS13 structure size");
  struct Options15 { BOOL TriangleFanSupported, DynamicIndexBufferStripCutSupported; } fan{};
  constexpr auto options15 = static_cast<D3D12_FEATURE>(44);
  check(c.device->CheckFeatureSupport(options15, &fan, sizeof(fan)), "OPTIONS15 query");
  require(fan.TriangleFanSupported, "OPTIONS15 triangle fan advertised after behavioral proof");
  require(c.device->CheckFeatureSupport(options15, &fan, sizeof(fan) - 1) == E_INVALIDARG,
          "OPTIONS15 structure size");
}

#include "owned-d3d12-render-p4.inc"

int run() {
  const auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  require(backend && !std::strcmp(backend(), "owned-graphics-d3d12"), "owned backend identity");
  std::printf("OWNED_D3D12_OPTIONS13_BACKEND id=%s\n", backend());
  Context c;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)), "Create device");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "Create fence");
  c.setQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
  UINT passed = 0, failed = 0;
  const char *only = std::getenv("OWNED_OPTIONS13_ONLY");
  const std::pair<const char *, std::function<void()>> cases[] = {
    {"alpha_blend", [&] { testAlpha(c); }},
    {"viewport_z", [&] { testViewport(c, false); }},
    {"viewport_y", [&] { testViewport(c, true); }},
    {"vertex_alignment", [&] { testVertexAlignment(c); }},
    {"copy_pitch", [&] { testCopyPitch(c); }},
    {"cross_dimension", [&] { c.setQueue(D3D12_COMMAND_LIST_TYPE_DIRECT); testCrossDimension(c); }},
    {"triangle_fan", [&] { testTriangleFan(c); }},
    {"manual_write_tracking", [&] { testManualWriteTracking(c); }},
    {"capabilities", [&] { testCapabilities(c); }},
  };
  for (const auto &[name, body] : cases) {
    if (only && std::strcmp(only, name)) continue;
    std::printf("OWNED_D3D12_OPTIONS13_CASE_BEGIN %s\n", name); std::fflush(stdout);
    try { body(); ++passed; std::printf("OWNED_D3D12_OPTIONS13_CASE_PASS %s\n", name); }
    catch (const std::exception &error) {
      ++failed;
      std::fprintf(stderr, "OWNED_D3D12_OPTIONS13_CASE_FAIL case=%s exception=%s\n", name, error.what());
    }
    std::fflush(stdout);
  }
  if (failed || !passed) return 1;
  std::printf("OWNED_D3D12_OPTIONS13_PASS cases=%u\n", passed);
  return 0;
}
} // namespace

int main() {
  try { return run(); }
  catch (const std::exception &error) {
    std::fprintf(stderr, "OWNED_D3D12_OPTIONS13_FAIL case=setup exception=%s\n", error.what());
    return 1;
  }
}
