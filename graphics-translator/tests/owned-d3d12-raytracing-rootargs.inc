// Root-argument-changing ExecuteIndirect(DISPATCH_RAYS) for owned-d3d12-raytracing.cpp (included after the
// rayquery block). The command signature sets two root constants and the root UAV before every dispatch; each
// slot's ray generation shader writes its own row with its own constants (tests/dxil/raytracing-rootargs.hlsl).

namespace rootargs {

constexpr UINT kSlots = 4, kRows = 4, kWidth = 16, kExecuted = 3;

int run(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil) {
  ComPtr<ID3D12Device> device;
  check(device5->QueryInterface(IID_PPV_ARGS(&device)), "rootargs: QueryInterface(ID3D12Device)");

  D3D12_ROOT_PARAMETER1 params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 2;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = params;
  auto root = ::createRootSignature(device.Get(), rootDesc, "rootargs: root signature");

  D3D12_DXIL_LIBRARY_DESC library{};
  library.DXILLibrary = {dxil.data(), dxil.size()};
  D3D12_EXPORT_DESC exportDesc{L"RARayGen", nullptr, D3D12_EXPORT_FLAG_NONE};
  library.NumExports = 1;
  library.pExports = &exportDesc;
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  ID3D12RootSignature *global = root.Get();
  D3D12_STATE_SUBOBJECT subobjects[4] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
  };
  const D3D12_STATE_OBJECT_DESC stateDesc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 4, subobjects};
  ComPtr<ID3D12StateObject> stateObject;
  check(device5->CreateStateObject(&stateDesc, IID_PPV_ARGS(&stateObject)), "rootargs: CreateStateObject");
  ComPtr<ID3D12StateObjectProperties> properties;
  check(stateObject.As(&properties), "rootargs: StateObjectProperties");
  const void *identifier = properties->GetShaderIdentifier(L"RARayGen");
  require(identifier != nullptr, "rootargs: GetShaderIdentifier(RARayGen)");
  auto table = createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memcpy(mapUpload(table.Get()), identifier, kShaderIdentifierSize);
  table->Unmap(0, nullptr);

  // Two output buffers; slots alternate between them through the root UAV argument.
  constexpr UINT64 kBytes = kRows * kWidth * 4;
  ComPtr<ID3D12Resource> outputs[2];
  for (auto &output : outputs)
    output = createBuffer(device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<uint32_t> fillWords(kRows * kWidth, 0xffffffffu);
  auto fill = createBuffer(device.Get(), kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memcpy(mapUpload(fill.Get()), fillWords.data(), kBytes);
  fill->Unmap(0, nullptr);

  // One command: 2 root constants, the root UAV address, then the dispatch description.
  struct Command {
    uint32_t k[2];
    uint64_t uav;
    D3D12_DISPATCH_RAYS_DESC dispatch;
  };
  static_assert(sizeof(Command) == 120);
  D3D12_INDIRECT_ARGUMENT_DESC arguments[3]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  arguments[0].Constant = {0, 0, 2};
  arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
  arguments[1].UnorderedAccessView.RootParameterIndex = 1;
  arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
  const D3D12_COMMAND_SIGNATURE_DESC signatureDesc{sizeof(Command), 3, arguments, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(device->CreateCommandSignature(&signatureDesc, root.Get(), IID_PPV_ARGS(&signature)),
        "rootargs: argument-changing DISPATCH_RAYS command signature");

  // Slot s: row s (k.x = s), base 100 * (s + 1), width 4 * (s + 1), into outputs[s & 1]. Slot 3 is past the count.
  Command commands[kSlots]{};
  for (UINT s = 0; s < kSlots; ++s) {
    commands[s].k[0] = s;
    commands[s].k[1] = 100 * (s + 1);
    commands[s].uav = outputs[s & 1]->GetGPUVirtualAddress();
    commands[s].dispatch.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), kShaderIdentifierSize};
    commands[s].dispatch.Width = 4 * (s + 1);
    commands[s].dispatch.Height = 1;
    commands[s].dispatch.Depth = 1;
  }
  auto argumentBuffer = createBuffer(device.Get(), sizeof(commands) + 16, D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto *mapped = static_cast<uint8_t *>(mapUpload(argumentBuffer.Get()));
  std::memcpy(mapped, commands, sizeof(commands));
  const uint32_t count = kExecuted;
  std::memcpy(mapped + sizeof(commands), &count, sizeof(count));
  argumentBuffer->Unmap(0, nullptr);

  auto readback = createBuffer(device.Get(), 2 * kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  // The previous case leaves the list open on a fresh allocator (the runners reset after each submit).
  for (auto &output : outputs) {
    ctx.list->CopyBufferRegion(output.Get(), 0, fill.Get(), 0, kBytes);
    transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  ctx.list->SetComputeRootSignature(root.Get());
  // The application's own values: a direct dispatch with them before and after would write row 3 of outputs[0].
  const uint32_t own[2] = {3, 7};
  ctx.list->SetComputeRoot32BitConstants(0, 2, own, 0);
  ctx.list->SetComputeRootUnorderedAccessView(1, outputs[0]->GetGPUVirtualAddress());
  ctx.list->SetPipelineState1(stateObject.Get());
  ctx.list->ExecuteIndirect(signature.Get(), kSlots, argumentBuffer.Get(), 0, argumentBuffer.Get(), sizeof(commands));
  for (auto &output : outputs)
    transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(readback.Get(), 0, outputs[0].Get(), 0, kBytes);
  ctx.list->CopyBufferRegion(readback.Get(), kBytes, outputs[1].Get(), 0, kBytes);
  ctx.submitAndWait();
  check(ctx.allocator->Reset(), "rootargs: Allocator::Reset");
  check(ctx.list->Reset(ctx.allocator.Get(), nullptr), "rootargs: List::Reset");

  void *data = nullptr;
  check(readback->Map(0, nullptr, &data), "rootargs: Map(readback)");
  const auto *words = static_cast<const uint32_t *>(data);
  int bad = 0;
  for (UINT buffer = 0; buffer < 2; ++buffer)
    for (UINT row = 0; row < kRows; ++row)
      for (UINT lane = 0; lane < kWidth; ++lane) {
        // Row `row` of buffer `buffer` is written only by the slot row == s, s < count, s & 1 == buffer.
        const bool written = row < kExecuted && (row & 1) == buffer && lane < 4 * (row + 1);
        const uint32_t expected = written ? (row << 16) | (100 * (row + 1) + lane) : 0xffffffffu;
        const uint32_t actual = words[buffer * kRows * kWidth + row * kWidth + lane];
        if (actual != expected && bad++ < 16)
          std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_ROOTARGS_MISMATCH buffer=%u row=%u lane=%u actual=0x%08x "
                               "expected=0x%08x\n", buffer, row, lane, actual, expected);
      }
  readback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_RAYTRACING_ROOTARGS_CASE dispatch-rays-root-arguments slots=%u status=%s\n", kSlots,
              bad ? "FAIL" : "PASS");
  std::fflush(stdout);
  return bad ? 1 : 0;
}

} // namespace rootargs
