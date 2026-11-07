// Pipeline transitions share the main DXR fixture's device, list, fence and rootargs library.
// Atomic additions commute, so C -> RT -> C has no UAV dependency requiring a barrier.
namespace rttransition {

int run(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil,
        const std::vector<uint8_t> &computeDxil) {
  ComPtr<ID3D12Device> device;
  check(device5->QueryInterface(IID_PPV_ARGS(&device)), "transitions: QueryInterface(ID3D12Device)");
  D3D12_ROOT_PARAMETER1 parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
  rootDesc.NumParameters = 1;
  rootDesc.pParameters = &parameter;
  auto root = createRootSignature(device.Get(), rootDesc, "transitions: root signature");

  D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};
  computeDesc.pRootSignature = root.Get();
  computeDesc.CS = {computeDxil.data(), computeDxil.size()};
  ComPtr<ID3D12PipelineState> compute;
  check(device->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&compute)),
        "transitions: compute pipeline");

  D3D12_EXPORT_DESC exportDesc{L"TransitionRayGen", nullptr, D3D12_EXPORT_FLAG_NONE};
  D3D12_DXIL_LIBRARY_DESC library{{dxil.data(), dxil.size()}, 1, &exportDesc};
  ID3D12RootSignature *global = root.Get();
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  D3D12_STATE_SUBOBJECT subobjects[] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
  };
  const D3D12_STATE_OBJECT_DESC stateDesc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                                        static_cast<UINT>(std::size(subobjects)), subobjects};
  ComPtr<ID3D12StateObject> state;
  check(device5->CreateStateObject(&stateDesc, IID_PPV_ARGS(&state)), "transitions: state object");
  ComPtr<ID3D12StateObjectProperties> properties;
  check(state.As(&properties), "transitions: state properties");
  const void *identifier = properties->GetShaderIdentifier(L"TransitionRayGen");
  require(identifier != nullptr, "transitions: TransitionRayGen identifier");
  auto table = createBuffer(device.Get(), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memcpy(mapUpload(table.Get()), identifier, kShaderIdentifierSize);
  table->Unmap(0, nullptr);
  D3D12_DISPATCH_RAYS_DESC rays{};
  rays.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), kShaderIdentifierSize};
  rays.Width = 8;
  rays.Height = 8;
  rays.Depth = 1;

  constexpr UINT kBytes = 2 * sizeof(uint32_t);
  auto zero = createBuffer(device.Get(), kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memset(mapUpload(zero.Get()), 0, kBytes);
  zero->Unmap(0, nullptr);
  auto arguments = createBuffer(device.Get(), sizeof(rays), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memcpy(mapUpload(arguments.Get()), &rays, sizeof(rays));
  arguments->Unmap(0, nullptr);
  auto count = createBuffer(device.Get(), sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  auto *countMapped = static_cast<uint32_t *>(mapUpload(count.Get()));
  D3D12_INDIRECT_ARGUMENT_DESC argument{};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
  const D3D12_COMMAND_SIGNATURE_DESC signatureDesc{sizeof(rays), 1, &argument, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&signature)),
        "transitions: indirect signature");

  enum class Kind {
    BarrierControl, Direct, PredicatedOff, IndirectReplay, ZeroWidth, ZeroHeight, ZeroDepth, ZeroDepthLarge
  };
  const struct {
    Kind kind;
    const char *name;
  } cases[] = {
      {Kind::BarrierControl, "barrier-control"},
      {Kind::Direct, "compute-raygen-same-compute"},
      {Kind::PredicatedOff, "predicated-off-raygen-then-compute"},
      {Kind::IndirectReplay, "closed-indirect-list-count-one-then-zero"},
      {Kind::ZeroWidth, "zero-width-then-compute"},
      {Kind::ZeroHeight, "zero-height-then-compute"},
      {Kind::ZeroDepth, "zero-depth-then-compute"},
      {Kind::ZeroDepthLarge, "zero-depth-large-width-height-then-compute"},
  };
  int failures = 0;
  UINT executionsChecked = 0;
  for (const auto &test : cases) {
    auto dispatch = rays;
    bool suppressed = false;
    switch (test.kind) {
    case Kind::ZeroWidth: dispatch.Width = 0; suppressed = true; break;
    case Kind::ZeroHeight: dispatch.Height = 0; suppressed = true; break;
    case Kind::ZeroDepth: dispatch.Depth = 0; suppressed = true; break;
    case Kind::ZeroDepthLarge:
      dispatch.Width = 1u << 30;
      dispatch.Height = 1u << 30;
      dispatch.Depth = 0;
      suppressed = true;
      break;
    case Kind::PredicatedOff: suppressed = true; break;
    case Kind::BarrierControl:
    case Kind::Direct:
    case Kind::IndirectReplay: break;
    }
    // COPY_SOURCE at both list boundaries makes the very same closed list replayable.
    auto output = createBuffer(device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto readback = createBuffer(device.Get(), kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    auto *list = ctx.list.Get();
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(output.Get(), 0, zero.Get(), 0, kBytes);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->SetPipelineState(compute.Get());
    list->Dispatch(1, 1, 1);
    if (test.kind == Kind::PredicatedOff)
      list->SetPredication(zero.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    list->SetPipelineState1(state.Get());
    if (test.kind == Kind::IndirectReplay)
      list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, count.Get(), 0);
    else
      list->DispatchRays(&dispatch);
    if (test.kind == Kind::PredicatedOff)
      list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    // Only the control interrupts the tested transition. All other barriers bracket the workload.
    if (test.kind == Kind::BarrierControl)
      uavBarrier(list, output.Get());
    list->SetPipelineState(compute.Get());
    list->Dispatch(1, 1, 1);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, kBytes);
    const HRESULT close = list->Close();
    std::printf("OWNED_D3D12_RAYTRACING_TRANSITION_CLOSE case=%s hr=0x%08lx\n",
                test.name, static_cast<unsigned long>(close));
    check(close, test.name);
    const UINT executions = test.kind == Kind::IndirectReplay ? 2 : 1;
    for (UINT execution = 0; execution < executions; ++execution) {
      // The fence in executeAndWait completes before the CPU changes this GPU-read count.
      // Do not Reset, re-record, or Close between these ordered submissions.
      *countMapped = execution == 0 ? 1u : 0u;
      ctx.executeAndWait();
      void *mapped = nullptr;
      const D3D12_RANGE range{0, kBytes};
      check(readback->Map(0, &range, &mapped), "transitions: readback");
      const auto *words = static_cast<const uint32_t *>(mapped);
      const UINT expectedRays = suppressed || execution == 1 ? 0u : rays.Width * rays.Height * rays.Depth;
      const bool valid = words[0] == expectedRays && words[1] == 2u;
      std::printf("OWNED_D3D12_RAYTRACING_TRANSITION_CASE case=%s execution=%u count=%u "
                  "raygen=%u expected_raygen=%u compute=%u expected_compute=2 status=%s\n",
                  test.name, execution, *countMapped, words[0], expectedRays, words[1], valid ? "PASS" : "FAIL");
      failures += !valid;
      ++executionsChecked;
      const D3D12_RANGE none{0, 0};
      readback->Unmap(0, &none);
    }
    check(ctx.allocator->Reset(), "transitions: allocator reset");
    check(ctx.list->Reset(ctx.allocator.Get(), nullptr), "transitions: list reset");
  }
  count->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_RAYTRACING_TRANSITIONS_RESULT cases=%u failures=%d\n", executionsChecked, failures);
  if (!failures)
    std::puts("OWNED_D3D12_RAYTRACING_TRANSITIONS_PASS");
  std::fflush(stdout);
  return failures ? 1 : 0;
}

} // namespace rttransition
