// Public format-list oracle. No private bridge entrypoints or CPU pixel fallback.
#define wmain depth_casting_base_main
#include "owned-d3d12-dxil.cpp"
#undef wmain
#include <bit>
#include <cwchar>

namespace {
constexpr UINT snapshotBytes = 1024;
constexpr UINT patterns32[] = {0x00000001, 0x007fffff, 0x7fc12345, 0x3f000000,
                               0x80000001, 0xffc12345, 0x3f800000, 0x00000000};
constexpr UINT patterns16[] = {0x0001, 0x03ff, 0x7e55, 0x8000, 0x8001, 0x7c00, 0xffff, 0x3c00};
constexpr UINT samples32[] = {0x3e000000, 0x3ec00000, 0x3f200000, 0x3f600000};
constexpr UINT samples16[] = {1, 0x03ff, 0x7e55, 0xffff};
UINT halfFloatBits(UINT value) {
  const UINT sign = (value & 0x8000) << 16;
  UINT mantissa = value & 0x3ff;
  const UINT exponent = (value >> 10) & 31;
  if (exponent == 31) return sign | 0x7f800000 | (mantissa << 13);
  if (exponent) return sign | ((exponent + 112) << 23) | (mantissa << 13);
  if (!mantissa) return sign;
  int power = -14;
  while (!(mantissa & 0x400)) { mantissa <<= 1; --power; }
  return sign | (UINT(power + 127) << 23) | ((mantissa & 0x3ff) << 13);
}
void exact(UINT actual, UINT expected, UINT bits, UINT phase, UINT word) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_D3D12_DEPTH_CASTING_FAIL bits=%u phase=%u word=%u actual=%08x expected=%08x\n",
                 bits, phase, word, actual, expected);
    ExitProcess(1);
  }
}
void runDepth(ID3D12Device10 *device, UINT bits, bool multisampled = false) {
  Context context(device);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = multisampled ? 2 : 8;
  desc.DepthOrArraySize = 3; desc.MipLevels = multisampled ? 1 : 4;
  desc.Format = bits == 32 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D16_UNORM;
  desc.SampleDesc.Count = multisampled ? 4 : 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_RESOURCE_DESC1 extended{};
  extended.Dimension = desc.Dimension; extended.Width = desc.Width; extended.Height = desc.Height;
  extended.DepthOrArraySize = desc.DepthOrArraySize; extended.MipLevels = desc.MipLevels;
  extended.Format = desc.Format; extended.SampleDesc = desc.SampleDesc;
  extended.Layout = desc.Layout; extended.Flags = desc.Flags;
  DXGI_FORMAT formats[] = {
      bits == 32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
      bits == 32 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16_FLOAT,
      bits == 32 ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8G8_UINT,
      bits == 32 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16_UNORM};
  auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &extended,
                                         D3D12_BARRIER_LAYOUT_COMMON,
                                         nullptr, nullptr, 4, formats, IID_PPV_ARGS(&texture)), "depth cast list");
  transition(context.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON,
             multisampled ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> upload;
  void *mapped = nullptr;
  if (!multisampled) {
    constexpr UINT count = 12;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, count> footprints{};
    std::array<UINT, count> rows{};
    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), nullptr, &uploadBytes);
    upload = createBuffer(device, uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    check(upload->Map(0, nullptr, &mapped), "depth upload map");
    std::memset(mapped, 0, size_t(uploadBytes));
    for (UINT sub = 0; sub < count; ++sub) {
      const UINT mip = sub % 4, slice = sub / 4;
      for (UINT y = 0; y < rows[sub]; ++y) for (UINT x = 0; x < footprints[sub].Footprint.Width; ++x) {
        const UINT index = (x + 3 * y + 5 * slice + 7 * mip) & 7;
        const UINT value = bits == 32 ? patterns32[index] : patterns16[index];
        auto *target = static_cast<uint8_t *>(mapped) + footprints[sub].Offset +
                       size_t(y) * footprints[sub].Footprint.RowPitch + x * (bits / 8);
        std::memcpy(target, &value, bits / 8);
      }
    }
    upload->Unmap(0, nullptr);
    for (UINT sub = 0; sub < count; ++sub) {
      D3D12_TEXTURE_COPY_LOCATION source{}, target{};
      source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      source.PlacedFootprint = footprints[sub];
      target.pResource = texture.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      target.SubresourceIndex = sub;
      context.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    }
    transition(context.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> views;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&views)), "depth view heap");
  auto handle = views->GetCPUDescriptorHandleForHeapStart();
  const UINT increment = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
  for (UINT index = 0; index < 4; ++index) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = formats[index];
    view.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (multisampled) view.Texture2DMSArray = {1, 2};
    else view.Texture2DArray = {1, 3, 1, 2, 0, 0};
    device->CreateShaderResourceView(texture.Get(), &view, handle); handle.ptr += increment;
  }
  heapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, multisampled ? 3u : 1u, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> depthViews;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&depthViews)), "depth DSV heap");
  const UINT depthIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  auto depthHandle = depthViews->GetCPUDescriptorHandleForHeapStart();
  for (UINT slice = 0; slice < (multisampled ? 3u : 1u); ++slice) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = desc.Format;
    dsv.ViewDimension = multisampled ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    if (multisampled) dsv.Texture2DMSArray = {slice, 1};
    else dsv.Texture2DArray = {2, 2, 1};
    device->CreateDepthStencilView(texture.Get(), &dsv, depthHandle);
    depthHandle.ptr += depthIncrement;
  }
  depthHandle = depthViews->GetCPUDescriptorHandleForHeapStart();
  if (multisampled) depthHandle.ptr += 2 * depthIncrement;
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor.ShaderRegister = 0;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[2].Constants = {0, 0, 1};
  StaticSamplerDesc1 staticSampler{};
  staticSampler.Flags = 1; // D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR, required by GatherRaw.
  auto &sampler = staticSampler.base;
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(3);
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  RootSignatureDesc2 rootDesc{3, parameters, 1, &staticSampler,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  auto root = createRootSignature(device, rootDesc);
  wchar_t path[64];
  std::swprintf(path, 64, multisampled ? L"depth-casting-%u-msaa-cs.dxil" : L"depth-casting-%u.dxil", bits);
  auto shader = readFile(path);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
  pipelineDesc.pRootSignature = root.Get(); pipelineDesc.CS = {shader.data(), shader.size()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)), "depth pipeline");
  auto output = createBuffer(device, snapshotBytes, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  auto seed = createBuffer(device, snapshotBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = createBuffer(device, snapshotBytes * 2, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  check(seed->Map(0, nullptr, &mapped), "depth seed map");
  std::memset(mapped, 0xcd, snapshotBytes); seed->Unmap(0, nullptr);
  context.list->CopyBufferRegion(output.Get(), 0, seed.Get(), 0, snapshotBytes);
  transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComPtr<ID3D12PipelineState> paintPipeline;
  if (multisampled) {
    auto vertex = readFile(L"depth-casting-msaa-vs.dxil");
    std::swprintf(path, 64, L"depth-casting-%u-msaa-ps.dxil", bits);
    auto pixel = readFile(path);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC paint{};
    paint.pRootSignature = root.Get();
    paint.VS = {vertex.data(), vertex.size()}; paint.PS = {pixel.data(), pixel.size()};
    paint.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    paint.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    paint.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    paint.RasterizerState.DepthClipEnable = TRUE;
    paint.RasterizerState.MultisampleEnable = TRUE;
    paint.DepthStencilState.DepthEnable = TRUE;
    paint.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    paint.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    paint.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    paint.DSVFormat = desc.Format; paint.SampleDesc = desc.SampleDesc;
    check(device->CreateGraphicsPipelineState(&paint, IID_PPV_ARGS(&paintPipeline)), "depth paint pipeline");
    context.list->SetPipelineState(paintPipeline.Get());
    context.list->SetGraphicsRootSignature(root.Get());
    context.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, 2, 2, 0, 1};
    const D3D12_RECT scissor{0, 0, 2, 2};
    context.list->RSSetViewports(1, &viewport); context.list->RSSetScissorRects(1, &scissor);
    auto target = depthViews->GetCPUDescriptorHandleForHeapStart();
    for (UINT slice = 0; slice < 3; ++slice) {
      context.list->OMSetRenderTargets(0, nullptr, FALSE, &target);
      context.list->SetGraphicsRoot32BitConstant(2, slice, 0);
      context.list->DrawInstanced(3, 1, 0, 0); target.ptr += depthIncrement;
    }
    transition(context.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  for (UINT phase = 0; phase < 2; ++phase) {
    if (phase) {
      transition(context.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
      context.list->ClearDepthStencilView(depthHandle, D3D12_CLEAR_FLAG_DEPTH,
                                         bits == 32 ? .75f : 1.f, 0, 0, nullptr);
      transition(context.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    context.list->SetDescriptorHeaps(1, heaps); context.list->SetPipelineState(pipeline.Get());
    context.list->SetComputeRootSignature(root.Get());
    context.list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    context.list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    context.list->Dispatch(1, 1, 1);
    transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.list->CopyBufferRegion(readback.Get(), phase * snapshotBytes, output.Get(), 0, snapshotBytes);
    transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  context.submitAndWait(); check(device->GetDeviceRemovedReason(), "depth device status");
  D3D12_RANGE readRange{0, snapshotBytes * 2};
  check(readback->Map(0, &readRange, &mapped), "depth readback map");
  const auto *values = static_cast<const UINT *>(mapped);
  for (UINT phase = 0; phase < 2; ++phase) {
    const UINT *data = values + phase * snapshotBytes / 4;
    const UINT mipCount = multisampled ? 1 : 3;
    for (UINT mip = 0; mip < mipCount; ++mip) for (UINT view = 0; view < 3; ++view) {
      const UINT offset = mip * 12 + view * 4;
      exact(data[offset], multisampled ? 2 : 4 >> mip, bits, phase, offset);
      exact(data[offset + 1], multisampled ? 2 : 4 >> mip, bits, phase, offset + 1);
      exact(data[offset + 2], 2, bits, phase, offset + 2);
      exact(data[offset + 3], multisampled ? 4 : 3, bits, phase, offset + 3);
    }
    UINT word = mipCount * 12;
    for (UINT mip = 0; mip < mipCount; ++mip) for (UINT layer = 0; layer < 2; ++layer)
      for (UINT sample = 0; sample < desc.SampleDesc.Count; ++sample)
      for (UINT y = 0; y < (multisampled ? 2 : 4u >> mip); ++y)
      for (UINT x = 0; x < (multisampled ? 2 : 4u >> mip); ++x) {
        const UINT index = (x + 3 * y + 5 * (layer + 1) + 7 * (mip + 1)) & 7;
        UINT input = bits == 32 ? patterns32[index] : patterns16[index];
        if (multisampled) input = bits == 32 ? samples32[(sample + layer + 1) & 3] : samples16[(sample + layer + 1) & 3];
        if (phase && (multisampled || mip == 1) && layer == 1) input = bits == 32 ? 0x3f400000 : 0xffff;
        exact(data[word], input, bits, phase, word);
        if (bits == 16 && (input & 0x7c00) == 0x7c00 && (input & 0x3ff)) {
          if (!std::isnan(std::bit_cast<float>(data[word + 1]))) {
            std::fprintf(stderr, "OWNED_D3D12_DEPTH_CASTING_FAIL half_NaN bits=%u phase=%u word=%u actual=%08x\n",
                         bits, phase, word + 1, data[word + 1]); ExitProcess(1);
          }
        } else exact(data[word + 1], bits == 32 ? input : halfFloatBits(input), bits, phase, word + 1);
        exact(data[word + 2], input, bits, phase, word + 2);
        if (bits == 32) exact(data[word + 3], input, bits, phase, word + 3);
        else if (std::fabs(std::bit_cast<float>(data[word + 3]) - float(input) / 65535.f) > 0.000001f ||
                 !std::isfinite(std::bit_cast<float>(data[word + 3]))) {
          std::fprintf(stderr, "OWNED_D3D12_DEPTH_CASTING_FAIL bits=16 phase=%u word=%u unorm=%08x input=%04x\n",
                       phase, word + 3, data[word + 3], input); ExitProcess(1);
        }
        word += 4;
      }
    if (!multisampled) {
      const UINT xs[] = {1, 2, 2, 1}, ys[] = {2, 2, 1, 1};
      for (UINT layer = 0; layer < 2; ++layer) for (UINT lane = 0; lane < 4; ++lane) {
        const UINT index = (xs[lane] + 3 * ys[lane] + 5 * (layer + 1) + 7) & 7;
        exact(data[word], bits == 32 ? patterns32[index] : patterns16[index], bits, phase, word); ++word;
      }
      exact(data[word], 1, bits, phase, word); ++word;
      exact(data[word], 1, bits, phase, word); ++word;
      exact(data[word], 0x13579bdf, bits, phase, word); ++word;
      exact(data[word], bits, bits, phase, word); ++word;
    }
    for (; word < snapshotBytes / 4; ++word) exact(data[word], 0xcdcdcdcd, bits, phase, word);
    std::printf("OWNED_D3D12_DEPTH_CASTING_CASE_PASS d%u_%s%s\n", bits,
                multisampled ? "msaa_" : "", phase ? "clear" : multisampled ? "draw" : "upload");
    std::fflush(stdout);
  }
  D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
}
}
int wmain() {
  using Backend = const char *(WINAPI *)();
  auto backend = reinterpret_cast<Backend>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId)) return fail("depth casting backend");
  std::printf("OWNED_D3D12_DEPTH_CASTING_BACKEND id=%s\n", backend()); std::fflush(stdout);
  ComPtr<ID3D12Device10> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "depth casting device10");
  runDepth(device.Get(), 16); runDepth(device.Get(), 32);
  runDepth(device.Get(), 16, true); runDepth(device.Get(), 32, true);
  std::puts("OWNED_D3D12_DEPTH_CASTING_PASS cases=8");
  return 0;
}
