#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
constexpr GUID key = {0x585aab71, 0x1729, 0x421f, {0xac, 0x28, 0x64, 0x31, 0x51, 0x09, 0x61, 0x12}};

static void check(HRESULT hr) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "D3D11_SHADER_CACHE_FAIL hr=0x%08lx\n", static_cast<unsigned long>(hr));
    ExitProcess(1);
  }
}

template <typename Legacy, typename Shader>
static void checkForwarding(Shader *shader, ID3D11Device *device) {
  ComPtr<Legacy> legacy;
  check(shader->QueryInterface(IID_PPV_ARGS(&legacy)));
  ComPtr<IUnknown> modernIdentity, legacyIdentity;
  check(shader->QueryInterface(IID_PPV_ARGS(&modernIdentity)));
  check(legacy->QueryInterface(IID_PPV_ARGS(&legacyIdentity)));
  assert(modernIdentity.Get() == legacyIdentity.Get());
  ComPtr<Shader> roundtrip;
  check(legacy->QueryInterface(IID_PPV_ARGS(&roundtrip)));
  assert(roundtrip.Get() == shader);
  const ULONG references = legacy->AddRef();
  assert(legacy->Release() == references - 1);
  ComPtr<ID3D10Device> legacyDevice;
  legacy->GetDevice(&legacyDevice);
  ComPtr<ID3D11Device> modernDevice;
  check(legacyDevice.As(&modernDevice));
  assert(modernDevice.Get() == device);
  UINT value = 0x12345678, size = sizeof(value), actual = 0;
  check(legacy->SetPrivateData(key, size, &value));
  check(legacy->GetPrivateData(key, &size, &actual));
  assert(size == sizeof(value) && actual == value);
  check(legacy->SetPrivateDataInterface(key, device));
  IUnknown *retained = nullptr;
  size = sizeof(retained);
  check(shader->GetPrivateData(key, &size, &retained));
  assert(retained == static_cast<IUnknown *>(device));
  retained->Release();
  check(legacy->SetPrivateDataInterface(key, nullptr));
}

int main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &device, nullptr, &context));
  const char source[] =
    "struct Vertex { float4 position:SV_Position; };"
    "float4 vs(uint id:SV_VertexID):SV_Position { return float4(id==1?3:-1,id==2?3:-1,0,1); }"
    "[maxvertexcount(3)] void gs(triangle Vertex v[3],inout TriangleStream<Vertex> s)"
    "{s.Append(v[0]);s.Append(v[1]);s.Append(v[2]);}"
    "float4 ps():SV_Target {return float4(1,0,0,1);}";
  ComPtr<ID3DBlob> vsCode, gsCode, psCode;
  check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsCode, nullptr));
  check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "gs", "gs_5_0", 0, 0, &gsCode, nullptr));
  check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psCode, nullptr));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 4;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target, readback;
  check(device->CreateTexture2D(&desc, nullptr, &target));
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  check(device->CreateTexture2D(&desc, nullptr, &readback));
  ComPtr<ID3D11RenderTargetView> rtv;
  check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
  context->RSSetViewports(1, &viewport);
  D3D11_RASTERIZER_DESC rasterDesc{};
  rasterDesc.FillMode = D3D11_FILL_SOLID;
  rasterDesc.CullMode = D3D11_CULL_NONE;
  rasterDesc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  check(device->CreateRasterizerState(&rasterDesc, &raster));
  context->RSSetState(raster.Get());
  auto view = rtv.Get();
  context->OMSetRenderTargets(1, &view, nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (unsigned replay = 0; replay < 2; ++replay) {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
    check(device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, &gs));
    check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
    checkForwarding<ID3D10VertexShader>(vs.Get(), device.Get());
    checkForwarding<ID3D10GeometryShader>(gs.Get(), device.Get());
    checkForwarding<ID3D10PixelShader>(ps.Get(), device.Get());
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(rtv.Get(), clear);
    context->Draw(3, 0);
    context->CopyResource(readback.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (unsigned y = 0; y < 4; ++y)
      for (unsigned x = 0; x < 4; ++x) {
        const auto pixel = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 4;
        if (pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255)
          std::fprintf(stderr, "D3D11_SHADER_CACHE_PIXEL replay=%u x=%u y=%u rgba=%u,%u,%u,%u\n",
                       replay, x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
        assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
      }
    context->Unmap(readback.Get(), 0);
  }
  context->ClearState();
  std::puts("D3D11_SHADER_CACHE_PASS forwarding=3 identity=1 private_data=1 replay=2 pixels=32");
}
