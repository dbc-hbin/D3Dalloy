// Appended RayQuery stage coverage; shares the existing scene and independent 64-word lane oracle.
namespace rayquery_stages {
using namespace rayquery;

void viewport(Context &ctx) {
  D3D12_VIEWPORT v{0, 0, 1, 1, 0, 1};
  D3D12_RECT s{0, 0, 1, 1};
  ctx.list->RSSetViewports(1, &v);
  ctx.list->RSSetScissorRects(1, &s);
}

int graphics(Runner &run, Context &ctx, const char *name, UINT tag, const std::vector<uint8_t> &vs,
             const std::vector<uint8_t> &hs, const std::vector<uint8_t> &ds, const std::vector<uint8_t> &gs) {
  auto desc = graphicsDesc(run.root());
  desc.VS = bytecode(vs);
  desc.HS = bytecode(hs);
  desc.DS = bytecode(ds);
  desc.GS = bytecode(gs);
  desc.PrimitiveTopologyType = hs.empty() ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  ComPtr<ID3D12PipelineState> pso;
  check(run.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "rayquery stages: graphics PSO");
  run.clearOutput();
  run.bindGraphics();
  ctx.list->SetPipelineState(pso.Get());
  viewport(ctx);
  ctx.list->IASetPrimitiveTopology(hs.empty() ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
  ctx.list->DrawInstanced(kLanes, 1, 0, 0);
  return run.verify(name, tag, run.finish(name));
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Value value;
};

int mesh(Runner &run, Context &ctx, const char *name, UINT tag, const std::vector<uint8_t> &as,
         const std::vector<uint8_t> &ms) {
  ComPtr<ID3D12Device2> device2;
  check(run.device()->QueryInterface(IID_PPV_ARGS(&device2)), "rayquery stages: Device2");
  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
  } stream{{.value = run.root()}, {.value = bytecode(as)}, {.value = bytecode(ms)},
           {.value = graphicsDesc(run.root()).RasterizerState}, {.value = {}}, {.value = {1, 0}}};
  D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
  ComPtr<ID3D12PipelineState> pso;
  check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "rayquery stages: mesh PSO");
  ComPtr<ID3D12GraphicsCommandList6> list6;
  check(ctx.list.As(&list6), "rayquery stages: List6");
  run.clearOutput();
  run.bindGraphics();
  ctx.list->SetPipelineState(pso.Get());
  viewport(ctx);
  list6->DispatchMesh(kLanes, 1, 1);
  return run.verify(name, tag, run.finish(name));
}

int rays(Runner &run, ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &lib) {
  D3D12_DXIL_LIBRARY_DESC library{};
  library.DXILLibrary = bytecode(lib);
  D3D12_HIT_GROUP_DESC hits[3]{};
  hits[0].HitGroupExport = L"RQSClosestGroup";
  hits[0].ClosestHitShaderImport = L"RQSClosest";
  hits[0].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  hits[1].HitGroupExport = L"RQSAnyGroup";
  hits[1].AnyHitShaderImport = L"RQSAny";
  hits[1].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  hits[2].HitGroupExport = L"RQSIntersectionGroup";
  hits[2].IntersectionShaderImport = L"RQSIntersection";
  hits[2].Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  ID3D12RootSignature *root = run.root();
  D3D12_STATE_SUBOBJECT subobjects[] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hits[0]},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hits[1]},
      {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hits[2]},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}};
  D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(std::size(subobjects)), subobjects};
  ComPtr<ID3D12StateObject> state;
  check(device5->CreateStateObject(&desc, IID_PPV_ARGS(&state)), "rayquery stages: state object");
  ComPtr<ID3D12StateObjectProperties> properties;
  check(state.As(&properties), "rayquery stages: properties");
  auto table = createBuffer(run.device(), 1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  struct Case { const char *name; UINT tag; const wchar_t *gen; const wchar_t *hit; const wchar_t *miss; };
  const Case cases[] = {
      {"closesthit", 11, L"RQSClosestGen", L"RQSClosestGroup", L"RQSNoMiss"},
      {"anyhit", 12, L"RQSAnyGen", L"RQSAnyGroup", L"RQSNoMiss"},
      {"intersection", 13, L"RQSIntersectionGen", L"RQSIntersectionGroup", L"RQSNoMiss"},
      {"miss", 14, L"RQSMissGen", L"RQSClosestGroup", L"RQSMiss"},
      {"callable", 15, L"RQSCallableGen", L"RQSClosestGroup", L"RQSNoMiss"}};
  int failures = 0;
  for (const auto &c : cases) {
    auto *mapped = static_cast<uint8_t *>(mapUpload(table.Get()));
    std::memset(mapped, 0, 1024);
    const wchar_t *exports[] = {c.gen, c.miss, L"RQSCallable"};
    for (UINT i = 0; i < 3; ++i) {
      const void *id = properties->GetShaderIdentifier(exports[i]);
      require(id != nullptr, "rayquery stages: shader identifier");
      std::memcpy(mapped + i * 64, id, kShaderIdentifierSize);
    }
    const void *hit = properties->GetShaderIdentifier(c.hit);
    require(hit != nullptr, "rayquery stages: hit identifier");
    // Existing TLAS uses instance contributions 3 and 5 and two geometries: records 0..6.
    for (UINT i = 0; i < 7; ++i) std::memcpy(mapped + 256 + i * 64, hit, kShaderIdentifierSize);
    table->Unmap(0, nullptr);
    run.clearOutput();
    run.bindCompute();
    ctx.list->SetPipelineState1(state.Get());
    const UINT64 va = table->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord = {va, 32};
    dispatch.MissShaderTable = {va + 64, 64, 64};
    dispatch.CallableShaderTable = {va + 128, 64, 64};
    dispatch.HitGroupTable = {va + 256, 7 * 64, 64};
    dispatch.Width = kLanes; dispatch.Height = dispatch.Depth = 1;
    ctx.list->DispatchRays(&dispatch);
    failures += run.verify(c.name, c.tag, run.finish(c.name)) ? 1 : 0;
  }
  return failures;
}

// Frozen shader argv[12..21]: vs,hs,hs-pass,ds,ds-pass,gs,as,ms,ms-sink,lib.
int runAll(ID3D12Device5 *device5, Context &ctx, wchar_t **argv) {
  std::array<std::vector<uint8_t>, 10> code;
  for (UINT i = 0; i < code.size(); ++i) code[i] = readFile(argv[12 + i]);
  Runner run(device5, ctx);
  int failures = 0;
  failures += rays(run, device5, ctx, code[9]);
  failures += graphics(run, ctx, "hs", 6, code[0], code[1], code[4], {}) ? 1 : 0;
  failures += graphics(run, ctx, "ds", 7, code[0], code[2], code[3], {}) ? 1 : 0;
  failures += graphics(run, ctx, "gs", 8, code[0], {}, {}, code[5]) ? 1 : 0;
  failures += mesh(run, ctx, "as", 9, code[6], code[8]) ? 1 : 0;
  failures += mesh(run, ctx, "ms", 10, {}, code[7]) ? 1 : 0;
  std::printf("OWNED_D3D12_RAYTRACING_RAYQUERY_STAGES failures=%d\n", failures);
  return failures;
}
} // namespace rayquery_stages
