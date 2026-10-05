// Public device64 producer-once acceptance. Genuine DXIL is built by build-atomic64-producers.sh.
#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain

namespace {
struct ProducerCase {
  const char *name, *vs, *hs, *ds, *gs, *as, *ms;
  UINT grid[3], child[3], counts[6];
};
constexpr ProducerCase producer_cases[] = {
  {"vs", "vs_atomic", nullptr, nullptr, nullptr, nullptr, nullptr, {1,1,1}, {1,1,1}, {6,0,0,0,0,0}},
  {"hs", "vs_plain", "hs_atomic", "ds_plain", nullptr, nullptr, nullptr, {1,1,1}, {1,1,1}, {0,6,0,0,0,0}},
  {"ds", "vs_plain", "hs_plain", "ds_atomic", nullptr, nullptr, nullptr, {1,1,1}, {1,1,1}, {0,0,6,0,0,0}},
  {"gs", "vs_plain", nullptr, nullptr, "gs_atomic", nullptr, nullptr, {1,1,1}, {1,1,1}, {0,0,0,4,0,0}},
  {"ds_gs", "vs_plain", "hs_plain", "ds_atomic", "gs_atomic", nullptr, nullptr, {1,1,1}, {1,1,1}, {0,0,6,4,0,0}},
  {"ms_grid", nullptr, nullptr, nullptr, nullptr, nullptr, "ms_atomic", {2,2,1}, {1,1,1}, {0,0,0,0,0,16}},
  {"as_child1", nullptr, nullptr, nullptr, nullptr, "as_atomic", "ms_payload", {2,1,1}, {1,1,1}, {0,0,0,0,8,8}},
  {"as_child2", nullptr, nullptr, nullptr, nullptr, "as_atomic", "ms_payload", {1,2,1}, {2,1,1}, {0,0,0,0,8,16}},
  {"as_child3d", nullptr, nullptr, nullptr, nullptr, "as_atomic", "ms_payload", {2,1,1}, {1,2,2}, {0,0,0,0,8,32}},
};
std::vector<uint8_t> producerShader(const char *entry) {
  if (!entry) return {};
  const std::string name = std::string("atomic64-producers-") + entry + ".dxil";
  return readFile(std::wstring(name.begin(), name.end()).c_str());
}
void runProducer(Gpu &gpu, ID3D12RootSignature *root, const ProducerCase &c, UINT samples) {
  begin();
  const auto vs = producerShader(c.vs), hs = producerShader(c.hs), ds = producerShader(c.ds),
             gs = producerShader(c.gs), as = producerShader(c.as), ms = producerShader(c.ms), ps = producerShader("ps_sample");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = root;
  d.VS = {vs.data(),vs.size()}; d.HS = {hs.data(),hs.size()}; d.DS = {ds.data(),ds.size()};
  d.GS = {gs.data(),gs.size()}; d.PS = {ps.data(),ps.size()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.BlendState.RenderTarget[0].BlendEnable = TRUE;
  d.BlendState.RenderTarget[0].SrcBlend = d.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  d.BlendState.RenderTarget[0].SrcBlendAlpha = d.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
  d.BlendState.RenderTarget[0].BlendOp = d.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  d.SampleMask = UINT32_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.RasterizerState.MultisampleEnable = TRUE;
  d.PrimitiveTopologyType = c.hs ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = samples;
  ComPtr<ID3D12PipelineState> pso;
  if (c.ms) {
    ComPtr<ID3D12Device2> device2; check(gpu.device.As(&device2), "QueryInterface(Device2)");
    D3D12_RT_FORMAT_ARRAY formats{}; formats.NumRenderTargets = 1; formats.RTFormats[0] = d.RTVFormats[0];
    struct {
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    } stream{{.value=root}, {.value={as.data(),as.size()}}, {.value={ms.data(),ms.size()}}, {.value=d.PS},
             {.value=d.BlendState}, {.value=d.SampleMask}, {.value=d.RasterizerState}, {.value=d.DepthStencilState},
             {.value=formats}, {.value=d.SampleDesc}};
    const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "CreatePipelineState(producer mesh)");
  } else check(gpu.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(producer)");
  constexpr UINT records = 6 * 64;
  std::vector<uint8_t> initial_cells(records * 8), initial_logs(records * 32, 0xcd);
  for (UINT key = 0; key < records; ++key) {
    const uint64_t initial = 0x1234567800000000ULL + uint64_t(key) * 256;
    std::memcpy(initial_cells.data() + key * 8, &initial, 8);
    std::memset(initial_logs.data() + key * 32 + 16, 0, 4);
  }
  auto logs = gpu.uavBuffer(initial_logs), cells = gpu.uavBuffer(initial_cells);
  gpu.rawUav(0, logs.Get()); gpu.structuredUav(1, cells.Get(), 8);
  D3D12_RESOURCE_DESC rt{}; rt.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rt.Width = rt.Height = 8; rt.DepthOrArraySize = rt.MipLevels = 1;
  rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rt.SampleDesc.Count = samples; rt.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> target, resolved;
  check(gpu.device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rt,D3D12_RESOURCE_STATE_RENDER_TARGET,
      nullptr,IID_PPV_ARGS(&target)), "CreateCommittedResource(producer RT)");
  auto rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart(); gpu.device->CreateRenderTargetView(target.Get(),nullptr,rtv);
  const FLOAT clear[4] = {0,0,0,0}; gpu.list->ClearRenderTargetView(rtv,clear,0,nullptr);
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()}; gpu.list->SetDescriptorHeaps(1,heaps);
  gpu.list->SetPipelineState(pso.Get()); gpu.list->SetGraphicsRootSignature(root);
  const UINT control[] = {c.child[0],c.child[1],c.child[2],c.grid[0],c.grid[1],0,0,0};
  gpu.list->SetGraphicsRoot32BitConstants(0,8,control,0); gpu.list->SetGraphicsRootDescriptorTable(1,gpu.gpu(0));
  gpu.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
  const D3D12_VIEWPORT viewport{0,0,8,8,0,1}; const D3D12_RECT scissor{0,0,8,8};
  gpu.list->RSSetViewports(1,&viewport); gpu.list->RSSetScissorRects(1,&scissor);
  if (c.ms) { ComPtr<ID3D12GraphicsCommandList6> list6; check(gpu.list.As(&list6),"QueryInterface(List6)");
    list6->DispatchMesh(c.grid[0],c.grid[1],c.grid[2]);
  } else { gpu.list->IASetPrimitiveTopology(c.hs ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.list->DrawInstanced(3,2,0,0); }
  ID3D12Resource *copy_source = target.Get();
  if (samples > 1) {
    rt.SampleDesc.Count = 1;
    check(gpu.device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&rt,D3D12_RESOURCE_STATE_RESOLVE_DEST,
        nullptr,IID_PPV_ARGS(&resolved)),"CreateCommittedResource(resolve)");
    gpu.transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    gpu.list->ResolveSubresource(resolved.Get(),0,target.Get(),0,rt.Format);
    gpu.transition(resolved.Get(),D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE); copy_source = resolved.Get();
  } else gpu.transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
  rt.SampleDesc.Count = 1; gpu.device->GetCopyableFootprints(&rt,0,1,0,&footprint,nullptr,nullptr,&bytes);
  auto image = gpu.buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=image.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=footprint;
  D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=copy_source; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  gpu.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
  const auto result = gpu.finish({logs.Get(),cells.Get()},nullptr);
  for (UINT stage=0; stage<6; ++stage) for (UINT lane=0; lane<64; ++lane) {
    const UINT key=stage*64+lane; const bool active=lane<c.counts[stage];
    const uint64_t initial=0x1234567800000000ULL+uint64_t(key)*256;
    const uint64_t old=load<uint64_t>(result[0],key*32), cas=load<uint64_t>(result[0],key*32+8), cell=load<uint64_t>(result[1],key*8);
    const UINT count=load<UINT>(result[0],key*32+16);
    EXPECT_EQ(c.name,"old",key,old,active?initial:0xcdcdcdcdcdcdcdcdULL);
    EXPECT_EQ(c.name,"cas",key,cas,active?initial+1:0xcdcdcdcdcdcdcdcdULL);
    EXPECT_EQ(c.name,"count",key,count,active?1:0); EXPECT_EQ(c.name,"cell",key,cell,initial+(active?9:0));
    for (UINT offset=20; offset<32; offset+=4) EXPECT_EQ(c.name,"guard",key,load<UINT>(result[0],key*32+offset),kSentinel);
    std::printf("OWNED_D3D12_ATOMIC64_PRODUCERS_RECORD case=%s samples=%u stage=%u lane=%u old=%016llx cas=%016llx count=%u cell=%016llx guard=%08x,%08x,%08x\n",c.name,samples,stage,lane,
      (unsigned long long)old,(unsigned long long)cas,count,(unsigned long long)cell,load<UINT>(result[0],key*32+20),load<UINT>(result[0],key*32+24),load<UINT>(result[0],key*32+28));
  }
  void *mapped=nullptr; const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)}; check(image->Map(0,&range,&mapped),"Map(image)");
  const UINT primitives = c.ms ? c.counts[5] / 4 : c.gs ? 4 : 2;
  const UINT expected_pixel = (primitives * 16 << 24) | (primitives * (samples == 4 ? 10 : 4) << 8);
  for (UINT y=0;y<8;++y) for (UINT x=0;x<8;++x) {
    UINT pixel; std::memcpy(&pixel,static_cast<uint8_t *>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4,4);
    EXPECT_EQ(c.name,"render producer output and primitive count",y*8+x,pixel,expected_pixel);
    std::printf("OWNED_D3D12_ATOMIC64_PRODUCERS_PIXEL case=%s samples=%u index=%u rgba=%08x\n",c.name,samples,y*8+x,pixel);
  }
  image->Unmap(0,nullptr);
  std::printf("OWNED_D3D12_ATOMIC64_PRODUCERS_CASE case=%s samples=%u status=%s\n",c.name,samples,g_case_mismatches?"FAIL":"PASS");
  if (g_case_mismatches) ++g_failures;
}
}
int wmain() {
  setvbuf(stdout,nullptr,_IONBF,0);
  const auto dll=GetModuleHandleW(L"d3d12.dll"); using BackendIdFn=const char *(WINAPI *)();
  const auto backend=dll?reinterpret_cast<BackendIdFn>(GetProcAddress(dll,"OwnedGraphicsBackendId")):nullptr;
  if (!backend||!backend()||std::strcmp(backend(),kExpectedBackendId)) die("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_ATOMIC64_PRODUCERS_BACKEND id=%s\n",backend());
  ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter; check(factory->EnumAdapters1(0,&adapter),"EnumAdapters1");
  ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12CreateDevice");
  Gpu gpu(device.Get());
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE|D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,0};
  D3D12_ROOT_PARAMETER1 parameters[2]{}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants={0,0,8}; parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={1,&range};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version=D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1.NumParameters=2; desc.Desc_1_1.pParameters=parameters;
  auto root=rootSignature(device.Get(),desc);
  for (const auto &c:producer_cases) for (UINT samples:{1u,4u}) runProducer(gpu,root.Get(),c,samples);
  std::printf("OWNED_D3D12_ATOMIC64_PRODUCERS_%s cases=18 stages=VS,HS,DS,GS,AS,MS\n",g_failures?"FAIL":"PASS");
  return g_failures?1:0;
}
