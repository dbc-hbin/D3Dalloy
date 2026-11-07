// Optional ordinary AS refit chain, replay and execution-order oracle.
// Included after the shared contract/serialization helpers.
namespace refitchain {

constexpr auto kFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION);
constexpr UINT kVisualBytes = 4096;
constexpr UINT kSerializedHeaderBytes = sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER);
constexpr UINT kMetadataStride = kVisualBytes + kSerializedHeaderBytes;
constexpr UINT kQueryOffset = 2 * kMetadataStride;
constexpr UINT kQueryBytes = 2 * 5 * sizeof(uint64_t);
constexpr UINT kObservedFields[] = {0, 1, 2, 3, 4, 5, 6, 7, 45, 46, 47, 48, 62};

struct Recording {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  explicit Recording(ID3D12Device *device) {
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "refit chain: allocator");
    ComPtr<ID3D12GraphicsCommandList> base;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&base)),
        "refit chain: list");
    check(base.As(&list), "refit chain: list4");
  }
  void close() { check(list->Close(), "refit chain: Close"); }
};

std::array<float, 18> vertices(UINT step) {
  std::array<float, 18> result{};
  for (UINT g = 0; g < 2; ++g) {
    const float x = 4.0f * g, z = 2.0f * step;
    const float triangle[] = {x, 0, z, x + 1, 0, z, x, 1, z};
    std::memcpy(result.data() + g * 9, triangle, sizeof(triangle));
  }
  return result;
}

std::array<D3D12_RAYTRACING_INSTANCE_DESC, 2> instances(UINT step, UINT64 blas) {
  std::array<D3D12_RAYTRACING_INSTANCE_DESC, 2> result{};
  for (UINT i = 0; i < 2; ++i) {
    const auto transform = contract::translateScale(1, 0, 4.0f * i, float(step));
    result[i] = contract::instanceDesc(transform.m, 0x80 + 16 * step + i, 0xff, 0, 0, blas);
  }
  return result;
}

void printWords(const uint32_t *words, UINT count) {
  for (UINT i = 0; i < count; ++i) std::printf("%s%08x", i ? "," : "", words[i]);
  std::putchar('\n');
}

std::vector<uint8_t> readback(ID3D12Resource *resource, UINT64 offset, UINT64 size) {
  require(offset <= resource->GetDesc().Width && size <= resource->GetDesc().Width - offset,
      "refit chain: readback range");
  void *mapped = nullptr;
  const D3D12_RANGE range{SIZE_T(offset), SIZE_T(offset + size)};
  check(resource->Map(0, &range, &mapped), "refit chain: Map");
  std::vector<uint8_t> bytes(size);
  std::memcpy(bytes.data(), static_cast<const uint8_t *>(mapped) + offset, size);
  const D3D12_RANGE none{0, 0};
  resource->Unmap(0, &none);
  return bytes;
}

struct Capture {
  contract::Runner &run;
  bool indexed;
  std::array<ID3D12Resource *, 2> structures;
  ComPtr<ID3D12Resource> output, fill, rays, rayReadback, metadataReadback, queries;
  std::array<ComPtr<ID3D12Resource>, 2> visual, serialized;
  ComPtr<ID3D12DescriptorHeap> heap;

  Capture(contract::Runner &owner, ID3D12Resource *blas, ID3D12Resource *tlas, UINT steps, bool indices = false)
      : run(owner), indexed(indices), structures{blas, tlas} {
    auto *device = run.device();
    auto uav = [&](UINT64 size, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      return createBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, state);
    };
    output = uav(contract::kOutputBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    fill = createBuffer(device, contract::kOutputBytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    std::memset(mapUpload(fill.Get()), 0xff, contract::kOutputBytes);
    fill->Unmap(0, nullptr);
    std::array<contract::LaneRay, 4> laneRays{};
    for (UINT i = 0; i < 2; ++i)
      for (UINT g = 0; g < 2; ++g)
        laneRays[2 * i + g] = contract::ray(4.0f * g + 0.25f, 4.0f * i + 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0);
    rays = run.upload(laneRays.data(), sizeof(laneRays));
    rayReadback = createBuffer(device, UINT64(steps) * contract::kOutputBytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    metadataReadback = createBuffer(device, kQueryOffset + kQueryBytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    queries = uav(kQueryBytes);
    for (UINT i = 0; i < 2; ++i) visual[i] = uav(kVisualBytes), serialized[i] = uav(kVisualBytes);
    const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "refit chain: heap");
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto handle = [&](UINT i) {
      auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * increment; return h;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.RaytracingAccelerationStructure.Location = tlas->GetGPUVirtualAddress();
    device->CreateShaderResourceView(nullptr, &srv, handle(0));
    srv.RaytracingAccelerationStructure.Location = 0;
    device->CreateShaderResourceView(nullptr, &srv, handle(2));
    device->CreateShaderResourceView(nullptr, &srv, handle(3));
    srv = {}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Buffer.NumElements = 4; srv.Buffer.StructureByteStride = sizeof(contract::LaneRay);
    device->CreateShaderResourceView(rays.Get(), &srv, handle(1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = contract::kOutputWords; view.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateUnorderedAccessView(output.Get(), nullptr, &view, handle(4));
  }

  void ray(ID3D12GraphicsCommandList4 *list, UINT slot) {
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(run.globalRoot());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->CopyBufferRegion(output.Get(), 0, fill.Get(), 0, contract::kOutputBytes);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState1(run.stateObject());
    const auto dispatch = run.dispatchDesc(4);
    list->DispatchRays(&dispatch);
    uavBarrier(list, output.Get());
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(rayReadback.Get(), UINT64(slot) * contract::kOutputBytes,
        output.Get(), 0, contract::kOutputBytes);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }

  void metadata(ID3D12GraphicsCommandList4 *list) {
    constexpr UINT offsets[] = {0, 8, 16, 32};
    for (UINT i = 0; i < 2; ++i) {
      const auto address = structures[i]->GetGPUVirtualAddress();
      for (UINT type = 0; type < 4; ++type) {
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{
            queries->GetGPUVirtualAddress() + i * 40 + offsets[type],
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE(type)};
        list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &address);
      }
      list->CopyRaytracingAccelerationStructure(visual[i]->GetGPUVirtualAddress(), address,
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
      list->CopyRaytracingAccelerationStructure(serialized[i]->GetGPUVirtualAddress(), address,
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
      for (UINT which = 0; which < 2; ++which) {
        auto *resource = which ? serialized[i].Get() : visual[i].Get();
        const UINT size = which ? kSerializedHeaderBytes : kVisualBytes;
        uavBarrier(list, resource);
        transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(metadataReadback.Get(), i * kMetadataStride + (which ? kVisualBytes : 0),
            resource, 0, size);
        transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
    }
    uavBarrier(list, queries.Get());
    transition(list, queries.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(metadataReadback.Get(), kQueryOffset, queries.Get(), 0, kQueryBytes);
    transition(list, queries.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  int verify(const char *name, const std::vector<UINT> &steps, const std::array<UINT64, 2> &prebuild) {
    int bad = 0;
    for (UINT slot = 0; slot < steps.size(); ++slot) {
      const auto bytes = readback(rayReadback.Get(), UINT64(slot) * contract::kOutputBytes, contract::kOutputBytes);
      std::vector<uint32_t> words(contract::kOutputWords);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      for (UINT lane = 0; lane < 4; ++lane) {
        const UINT instance = lane / 2, geometry = lane % 2, step = steps[slot];
        bad += run.expect(name, words, lane, contract::triangleHit(float(3 * step + 1), 0.25f, 0.25f,
            contract::kHitGroupTagBase, 0x80 + 16 * step + instance, instance, geometry));
        std::array<uint32_t, std::size(kObservedFields)> observed{};
        for (UINT i = 0; i < observed.size(); ++i) observed[i] = words[lane * contract::kSlotWords + kObservedFields[i]];
        std::printf("OWNED_D3D12_REFIT_CHAIN_RAY case=%s step=%u lane=%u words=", name, step, lane);
        printWords(observed.data(), UINT(observed.size()));
      }
    }
    const auto bytes = readback(metadataReadback.Get(), 0, kQueryOffset + kQueryBytes);
    for (UINT i = 0; i < 2; ++i) {
      std::array<uint64_t, 5> q{};
      std::memcpy(q.data(), bytes.data() + kQueryOffset + i * sizeof(q), sizeof(q));
      D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header{};
      std::memcpy(&header, bytes.data() + i * kMetadataStride + kVisualBytes, sizeof(header));
      const bool identifier = run.device5()->CheckDriverMatchingIdentifier(
          D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, &header.DriverMatchingIdentifier) ==
          D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
      bad += !q[0] || q[0] > q[4] || q[0] % 256 || q[1] != (i ? 136 : indexed ? 328 : 296) ||
          q[2] != (i ? 264 : indexed ? 440 : 408) ||
          q[3] != (i ? 2 : 0) || q[4] != prebuild[i] || header.SerializedSizeInBytesIncludingHeader != q[2] ||
          header.DeserializedSizeInBytes != prebuild[i] || header.NumBottomLevelAccelerationStructurePointersAfterHeader != q[3] ||
          !identifier;
      std::printf("OWNED_D3D12_REFIT_CHAIN_SIZE case=%s kind=%s compacted=%llu visualization=%llu serialized=%llu "
          "pointers=%llu current=%llu header_serialized=%llu deserialized=%llu header_pointers=%llu identifier=%u\n",
          name, i ? "tlas" : "blas", static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
          static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]), static_cast<unsigned long long>(q[4]),
          static_cast<unsigned long long>(header.SerializedSizeInBytesIncludingHeader),
          static_cast<unsigned long long>(header.DeserializedSizeInBytes),
          static_cast<unsigned long long>(header.NumBottomLevelAccelerationStructurePointersAfterHeader), UINT(identifier));
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER vis{};
      const auto *payload = bytes.data() + i * kMetadataStride;
      std::memcpy(&vis, payload, sizeof(vis));
      int descriptorBad = vis.Type != (i ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL :
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) || vis.NumDescs != 2;
      if (!i) {
        std::array<uint32_t, 18> observed{};
        for (UINT g = 0; g < 2; ++g) {
          D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
          std::memcpy(&geometry, payload + 8 + g * sizeof(geometry), sizeof(geometry));
          const auto &tri = geometry.Triangles;
          descriptorBad += geometry.Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES ||
              geometry.Flags != D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE || tri.VertexFormat != DXGI_FORMAT_R32G32B32_FLOAT ||
              tri.VertexCount != 3 || tri.VertexBuffer.StrideInBytes != 12 || tri.Transform3x4 ||
              tri.IndexCount != (indexed ? 3u : 0u) ||
              tri.IndexFormat != (indexed ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN) ||
              bool(tri.IndexBuffer) != indexed;
          const auto base = visual[i]->GetGPUVirtualAddress(), address = tri.VertexBuffer.StartAddress;
          if (q[1] > kVisualBytes || address < base || address - base > q[1] || 36 > q[1] - (address - base)) {
            ++descriptorBad; continue;
          }
          std::memcpy(observed.data() + 9 * g, payload + (address - base), 36);
          if (indexed) {
            const auto indexAddress = tri.IndexBuffer;
            const uint32_t expectedIndices[] = {0, 1, 2};
            if (indexAddress < base || indexAddress - base > q[1] || 12 > q[1] - (indexAddress - base))
              ++descriptorBad;
            else
              descriptorBad += std::memcmp(payload + (indexAddress - base), expectedIndices, 12) != 0;
          }
        }
        const auto expected = vertices(steps.back());
        bad += std::memcmp(observed.data(), expected.data(), sizeof(expected)) != 0;
        std::printf("OWNED_D3D12_REFIT_CHAIN_SHADOW case=%s kind=blas type=%u descriptors=%u descriptor_mismatches=%d words=",
            name, UINT(vis.Type), vis.NumDescs, descriptorBad);
        printWords(observed.data(), UINT(observed.size()));
      } else {
        std::array<uint32_t, 32> observed{};
        std::memcpy(observed.data(), payload + 8, sizeof(observed));
        const auto expected = instances(steps.back(), structures[0]->GetGPUVirtualAddress());
        bad += std::memcmp(observed.data(), expected.data(), sizeof(expected)) != 0;
        std::printf("OWNED_D3D12_REFIT_CHAIN_SHADOW case=%s kind=tlas type=%u descriptors=%u descriptor_mismatches=%d words=",
            name, UINT(vis.Type), vis.NumDescs, descriptorBad);
        printWords(observed.data(), UINT(observed.size()));
      }
      bad += descriptorBad;
    }
    std::printf("OWNED_D3D12_REFIT_CHAIN_CASE case=%s steps=%u status=%s\n", name, UINT(steps.size()), bad ? "FAIL" : "PASS");
    std::fflush(stdout);
    return bad;
  }
};

void wait(Context &ctx) {
  if (ctx.fence->GetCompletedValue() >= ctx.fenceValue) return;
  check(ctx.fence->SetEventOnCompletion(ctx.fenceValue, ctx.event), "refit chain: fence event");
  require(WaitForSingleObject(ctx.event, kFenceTimeoutMs) == WAIT_OBJECT_0, "refit chain: fence timeout");
}

// Two disjoint placed BLAS updates may overlap. A later update of A must complete after
// its first update, and the single TLAS consumes the final geometry of both BLAS.
int runBatch(ID3D12Device5 *device, Context &ctx, contract::Runner &run) {
  // Two independent refits with more than 4,096 potential range pairs.
  // Only the first two geometries intersect the fixture rays.
  constexpr UINT geometry_count = 65;
  std::array<std::array<D3D12_RAYTRACING_GEOMETRY_DESC, geometry_count>, 3> geometry{};
  const float distant[] = {128,0,0, 129,0,0, 128,1,0};
  const auto far_vb = run.upload(distant, sizeof(distant));
  for (UINT step = 0; step < geometry.size(); ++step) {
    const auto v = vertices(step);
    auto vb = run.upload(v.data(), sizeof(v));
    for (UINT g = 0; g < 2; ++g)
      geometry[step][g] = contract::triangles(vb->GetGPUVirtualAddress() + g * 36, 12, 3,
          DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
    std::fill(geometry[step].begin() + 2, geometry[step].end(),
        contract::triangles(far_vb->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT,
            D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE));
  }
  auto inputs = contract::blasInputs(geometry[0].data(), geometry_count, kFlags);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
  auto desc = bufferDesc(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
      D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE);
  const auto storage = device->GetResourceAllocationInfo(0, 1, &desc);
  D3D12_HEAP_DESC heap_desc{};
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.SizeInBytes = storage.SizeInBytes * 2;
  ComPtr<ID3D12Heap> heap;
  check(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)), "batch: mixed heap");
  std::array<ComPtr<ID3D12Resource>, 2> blas, scratch;
  for (UINT i : {1u, 0u}) {
    check(device->CreatePlacedResource(heap.Get(), i * storage.SizeInBytes, &desc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&blas[i])), "batch: BLAS");
    scratch[i] = run.uavBuffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes));
  }
  Recording first(device), second(device);
  ID3D12GraphicsCommandList4 *recording = first.list.Get();
  auto command = [&](UINT member, UINT step, ID3D12Resource *source = nullptr, ID3D12Resource *dest = nullptr) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = contract::blasInputs(geometry[step].data(), geometry_count, kFlags);
    build.DestAccelerationStructureData = (dest ? dest : blas[member].Get())->GetGPUVirtualAddress();
    if (step) {
      build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
      build.SourceAccelerationStructureData = (source ? source : blas[member].Get())->GetGPUVirtualAddress();
    }
    build.ScratchAccelerationStructureData = scratch[member]->GetGPUVirtualAddress();
    recording->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  };
  command(0, 0); command(1, 0); uavBarrier(recording, nullptr);
  command(0, 1);
  first.close();
  recording = second.list.Get();
  command(1, 1);
  second.close();
  recording = ctx.list.Get();
  uavBarrier(recording, nullptr);
  auto alternate = run.asBuffer(info.ResultDataMaxSizeInBytes);
  command(0, 1, blas[0].Get(), alternate.Get()); uavBarrier(ctx.list.Get(), nullptr);
  command(0, 2, alternate.Get(), blas[0].Get()); uavBarrier(ctx.list.Get(), nullptr);
  auto instances_data = instances(0, blas[0]->GetGPUVirtualAddress());
  instances_data[1].AccelerationStructure = blas[1]->GetGPUVirtualAddress();
  auto instance_input = run.upload(instances_data.data(), sizeof(instances_data));
  auto tlas = run.build(contract::tlasInputs(instance_input->GetGPUVirtualAddress(), 2), "batch: TLAS");
  std::vector<contract::LaneRay> rays;
  for (UINT i = 0; i < 4; ++i)
    rays.push_back(contract::ray(4.0f * (i % 2) + 0.25f, 4.0f * (i / 2) + 0.25f,
        -1, 0, 0, 1, 0, 0xff, 0, 0));
  auto check_rays = [&](const char *name, ID3D12Resource *top, float first_distance) {
    std::fprintf(stderr, "OWNED_D3D12_REFIT_CONTROL case=%s begin\n", name); std::fflush(stderr);
    if (!std::strcmp(name, "refit-batch")) {
      ID3D12CommandList *lists[] = {first.list.Get(), second.list.Get()};
      ctx.queue->ExecuteCommandLists(2, lists);
    }
    const auto words = run.trace(name, top->GetGPUVirtualAddress(), rays);
    std::fprintf(stderr, "OWNED_D3D12_REFIT_CONTROL case=%s end\n", name); std::fflush(stderr);
    int failures = 0;
    for (UINT i = 0; i < 4; ++i)
      failures += run.expect(name, words, i, contract::triangleHit(i < 2 ? first_distance : 3.f, .25f, .25f,
          contract::kHitGroupTagBase, 0x80 + i / 2, i / 2, i % 2));
    return failures;
  };
  int bad = check_rays("refit-batch", tlas.Get(), 5.f);
  std::printf("OWNED_D3D12_REFIT_BATCH lanes=4 failures=%d\n", bad);
  // The overlapping owner is never activated. Even after it dies the original
  // resource must keep working, but the allocation-history proof stays revoked.
  ComPtr<ID3D12Resource> overlapping;
  check(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
      nullptr, IID_PPV_ARGS(&overlapping)), "batch: overlapping placement");
  overlapping.Reset();
  command(0, 1); uavBarrier(ctx.list.Get(), nullptr);
  auto alias_tlas = run.build(contract::tlasInputs(instance_input->GetGPUVirtualAddress(), 2), "alias: TLAS");
  const int alias_bad = check_rays("refit-revoked", alias_tlas.Get(), 3.f);
  std::printf("OWNED_D3D12_REFIT_ALIAS_REVOKED lanes=4 failures=%d\n", alias_bad);
  bad += alias_bad;
  auto abandon_then_update = [&](const char *name, UINT unpublished_geometries, UINT step, float distance) {
    Recording abandoned(run.device());
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC unused{};
    unused.Inputs = contract::blasInputs(geometry[0].data(), unpublished_geometries, kFlags);
    unused.DestAccelerationStructureData = blas[0]->GetGPUVirtualAddress();
    unused.ScratchAccelerationStructureData = scratch[0]->GetGPUVirtualAddress();
    abandoned.list->BuildRaytracingAccelerationStructure(&unused, 0, nullptr);
    abandoned.close(); // Newest at record time only; its native BLAS has never been built.
    command(0, step); uavBarrier(ctx.list.Get(), nullptr);
    auto top = run.build(contract::tlasInputs(instance_input->GetGPUVirtualAddress(), 2), name);
    return check_rays(name, top.Get(), distance);
  };
  const int same_shape = abandon_then_update("refit-abandoned-same", geometry_count, 2, 5.f);
  const int other_shape = abandon_then_update("refit-abandoned-shape", 1, 1, 3.f);
  std::printf("OWNED_D3D12_REFIT_ABANDONED same_shape_failures=%d other_shape_failures=%d rays=8\n",
      same_shape, other_shape);
  return bad + same_shape + other_shape;
}

int runAll(ID3D12Device5 *device, Context &ctx, const std::vector<uint8_t> &dxil) {
  char indexedEnv[2]{};
  const bool indexed = GetEnvironmentVariableA("OWNED_D3D12_REFIT_INDEXED", indexedEnv, sizeof(indexedEnv)) == 1 &&
      indexedEnv[0] == '1';
  contract::Runner run(device, ctx, dxil);
  Context second(run.device());
  Recording initial(run.device()), repeated(run.device()), cross(run.device()), unpublished(run.device());
  auto vertexInput = createBuffer(run.device(), 3 * 36, D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  auto instanceInput = createBuffer(run.device(), 3 * 64, D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> indexInput;
  if (indexed) indexInput = createBuffer(run.device(), 32, D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<D3D12_RAYTRACING_GEOMETRY_DESC, 3> geometry{};
  for (UINT g = 0; g < geometry.size(); ++g)
    geometry[g] = contract::triangles(vertexInput->GetGPUVirtualAddress() + g * 36, 12, 3,
        DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
  if (indexed) {
    for (UINT g = 0; g < 2; ++g) {
      geometry[g].Triangles.IndexBuffer = indexInput->GetGPUVirtualAddress() + g * 16;
      geometry[g].Triangles.IndexCount = 3;
      geometry[g].Triangles.IndexFormat = g ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    }
    const uint32_t indexBytes[] = {0x00010000u, 2, 0, 0, 0, 1, 2, 0};
    auto upload = run.upload(indexBytes, sizeof(indexBytes));
    initial.list->CopyBufferRegion(indexInput.Get(), 0, upload.Get(), 0, sizeof(indexBytes));
    transition(initial.list.Get(), indexInput.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  auto bottomInputs = contract::blasInputs(geometry.data(), 2, kFlags);
  auto topInputs = contract::tlasInputs(instanceInput->GetGPUVirtualAddress(), 2, D3D12_ELEMENTS_LAYOUT_ARRAY, kFlags);
  const auto otherFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
  auto otherBottom = contract::blasInputs(geometry.data(), 3, otherFlags);
  auto otherTop = contract::tlasInputs(instanceInput->GetGPUVirtualAddress(), 3, D3D12_ELEMENTS_LAYOUT_ARRAY, otherFlags);
  std::array<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO, 2> info{}, otherInfo{};
  const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs[] = {bottomInputs, topInputs};
  const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS others[] = {otherBottom, otherTop};
  std::array<ComPtr<ID3D12Resource>, 2> structures, scratch;
  std::array<UINT64, 2> prebuild{};
  for (UINT i = 0; i < 2; ++i) {
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs[i], &info[i]);
    device->GetRaytracingAccelerationStructurePrebuildInfo(&others[i], &otherInfo[i]);
    require(info[i].ResultDataMaxSizeInBytes && otherInfo[i].ResultDataMaxSizeInBytes, "refit chain: prebuild");
    prebuild[i] = info[i].ResultDataMaxSizeInBytes;
    structures[i] = run.asBuffer(std::max(prebuild[i], otherInfo[i].ResultDataMaxSizeInBytes));
    scratch[i] = run.uavBuffer(std::max({info[i].ScratchDataSizeInBytes, info[i].UpdateScratchDataSizeInBytes,
        otherInfo[i].ScratchDataSizeInBytes}));
  }
  auto build = [&](ID3D12GraphicsCommandList4 *list, UINT i,
                   D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS description, bool update) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = description;
    if (update) desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(
        desc.Inputs.Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
    desc.DestAccelerationStructureData = structures[i]->GetGPUVirtualAddress();
    desc.SourceAccelerationStructureData = update ? desc.DestAccelerationStructureData : 0;
    desc.ScratchAccelerationStructureData = scratch[i]->GetGPUVirtualAddress();
    // Order the preceding DispatchRays AS reads before an in-place write.
    if (update) uavBarrier(list, structures[i].Get());
    list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    uavBarrier(list, structures[i].Get());
    uavBarrier(list, scratch[i].Get());
  };
  auto copyInputs = [&](ID3D12GraphicsCommandList4 *list, UINT step, bool first) {
    const auto vb = vertices(step);
    const auto ib = instances(step, structures[0]->GetGPUVirtualAddress());
    auto verticesUpload = run.upload(vb.data(), sizeof(vb)), instancesUpload = run.upload(ib.data(), sizeof(ib));
    ID3D12Resource *destinations[] = {vertexInput.Get(), instanceInput.Get()};
    ID3D12Resource *sources[] = {verticesUpload.Get(), instancesUpload.Get()};
    const UINT sizes[] = {sizeof(vb), sizeof(ib)};
    for (UINT i = 0; i < 2; ++i) {
      if (!first) transition(list, destinations[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(destinations[i], 0, sources[i], 0, sizes[i]);
      transition(list, destinations[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  };
  copyInputs(initial.list.Get(), 0, true);
  for (UINT i = 0; i < 2; ++i) build(initial.list.Get(), i, inputs[i], false);
  initial.close();
  Capture primary(run, structures[0].Get(), structures[1].Get(), 2, indexed);
  for (UINT step = 1; step <= 2; ++step) {
    copyInputs(repeated.list.Get(), step, false);
    for (UINT i = 0; i < 2; ++i) build(repeated.list.Get(), i, inputs[i], true);
    primary.ray(repeated.list.Get(), step - 1);
  }
  primary.metadata(repeated.list.Get());
  repeated.close();
  // R2 is recorded before B too: it captures the same native sidecars as R.
  Capture secondary(run, structures[0].Get(), structures[1].Get(), 1, indexed);
  copyInputs(cross.list.Get(), 3, false);
  // This private vertex input is unchanged until the immediately following BLAS refit.
  build(cross.list.Get(), 0, inputs[0], true);
  for (UINT i = 0; i < 2; ++i) build(cross.list.Get(), i, inputs[i], true);
  secondary.ray(cross.list.Get(), 0);
  secondary.metadata(cross.list.Get());
  cross.close();
  for (UINT i = 0; i < 2; ++i) build(unpublished.list.Get(), i, others[i], false);
  unpublished.close(); // Deliberately never submitted or reset during the oracle.
  std::printf("OWNED_D3D12_REFIT_CHAIN_SOURCE blas_address=%016llx tlas_address=%016llx blas_prebuild=%llu tlas_prebuild=%llu\n",
      static_cast<unsigned long long>(structures[0]->GetGPUVirtualAddress()),
      static_cast<unsigned long long>(structures[1]->GetGPUVirtualAddress()),
      static_cast<unsigned long long>(prebuild[0]), static_cast<unsigned long long>(prebuild[1]));
  std::printf("OWNED_D3D12_REFIT_CHAIN_RECORDINGS initial_lists=2 r_executions=3 r2_executions=1 unpublished_builds=2 cross_queue=1 indexed=%u\n", indexed);
  std::fflush(stdout);
  int bad = 0;
  auto submit = [&](const char *name, ID3D12CommandList **lists, UINT count) {
    std::fprintf(stderr, "OWNED_D3D12_REFIT_CHAIN_EXECUTE case=%s begin\n", name); std::fflush(stderr);
    ctx.queue->ExecuteCommandLists(count, lists);
    check(ctx.queue->Signal(ctx.fence.Get(), ++ctx.fenceValue), "refit chain: Signal");
    wait(ctx);
    std::fprintf(stderr, "OWNED_D3D12_REFIT_CHAIN_EXECUTE case=%s end\n", name); std::fflush(stderr);
  };
  ID3D12CommandList *first[] = {initial.list.Get(), repeated.list.Get()};
  ID3D12CommandList *again[] = {repeated.list.Get()};
  submit("chain", first, 2);
  bad += primary.verify("chain", {1, 2}, prebuild);
  submit("replay", again, 1);
  bad += primary.verify("replay", {1, 2}, prebuild);
  std::fprintf(stderr, "OWNED_D3D12_REFIT_CHAIN_EXECUTE case=cross-return begin\n"); std::fflush(stderr);
  check(second.queue->Wait(ctx.fence.Get(), ctx.fenceValue), "refit chain: Q2 waits Q1");
  ID3D12CommandList *other[] = {cross.list.Get()};
  second.queue->ExecuteCommandLists(1, other);
  check(second.queue->Signal(second.fence.Get(), ++second.fenceValue), "refit chain: Q2 Signal");
  check(ctx.queue->Wait(second.fence.Get(), second.fenceValue), "refit chain: Q1 waits Q2");
  ctx.queue->ExecuteCommandLists(1, again);
  check(ctx.queue->Signal(ctx.fence.Get(), ++ctx.fenceValue), "refit chain: Q1 return Signal");
  wait(ctx); wait(second);
  std::fprintf(stderr, "OWNED_D3D12_REFIT_CHAIN_EXECUTE case=cross-return end\n"); std::fflush(stderr);
  bad += secondary.verify("cross-queue", {3}, prebuild);
  bad += primary.verify("after-cross", {1, 2}, prebuild);
  bad += runBatch(device, ctx, run);
  std::printf("OWNED_D3D12_REFIT_CHAIN_RESULT cases=4 refits=15 ray_lanes=28 shadow_snapshots=8 unpublished_builds=2 failures=%d\n", bad);
  if (!bad) std::puts("OWNED_D3D12_REFIT_CHAIN_PASS");
  return bad ? 1 : 0;
}

} // namespace refitchain
