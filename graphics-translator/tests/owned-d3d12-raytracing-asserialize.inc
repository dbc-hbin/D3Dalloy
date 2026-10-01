// Ordered AS operations: read back public DXR layouts and trace the resulting structures.
namespace asserialize {
using namespace contract;
constexpr auto kFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION);

int report(const char *name, int failures) {
  std::printf("RAYTRACING_AS case=%s failures=%d status=%s\n", name, failures, failures ? "FAIL" : "PASS");
  return failures;
}

std::vector<uint8_t> read(Runner &run, ID3D12Resource *buffer, UINT64 size, UINT64 offset = 0) {
  auto readback = createBuffer(run.device(), size, D3D12_HEAP_TYPE_READBACK,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  transition(run.ctx().list.Get(), buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  run.ctx().list->CopyBufferRegion(readback.Get(), 0, buffer, offset, size);
  transition(run.ctx().list.Get(), buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  run.submit("AS readback");
  void *mapped = nullptr;
  D3D12_RANGE range{0, SIZE_T(size)};
  check(readback->Map(0, &range, &mapped), "AS Map readback");
  std::vector<uint8_t> result(size);
  std::memcpy(result.data(), mapped, size);
  D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  return result;
}

struct Scene {
  ComPtr<ID3D12Resource> blas, tlas, vertices, indices, transform, instances;
  D3D12_RAYTRACING_GEOMETRY_DESC geometry[2]{};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo{}, tlasInfo{};
};

Scene scene(Runner &run) {
  Scene s;
  auto vertices = unitTriangle(0);
  s.vertices = run.upload(vertices.data(), vertices.size() * sizeof(float));
  const uint16_t indices[] = {0, 1, 2};
  s.indices = run.upload(indices, sizeof(indices));
  auto xf = translateScale(1, 0, 0, 2);
  s.transform = run.upload(&xf, sizeof(xf));
  s.geometry[0] = triangles(0, 12, 0, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
  s.geometry[1] = triangles(s.vertices->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT,
      D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE, s.transform->GetGPUVirtualAddress(),
      s.indices->GetGPUVirtualAddress(), 3, DXGI_FORMAT_R16_UINT);
  s.blas = run.build(blasInputs(s.geometry, 2, kFlags), "AS BLAS", &s.blasInfo);
  float identityMatrix[3][4]; identity(identityMatrix);
  D3D12_RAYTRACING_INSTANCE_DESC instances[] = {
      instanceDesc(identityMatrix, 99, 0xff, 0, 0, 0),
      instanceDesc(identityMatrix, 37, 0xff, 0, 0, s.blas->GetGPUVirtualAddress())};
  s.instances = run.upload(instances, sizeof(instances));
  s.tlas = run.build(tlasInputs(s.instances->GetGPUVirtualAddress(), 2, D3D12_ELEMENTS_LAYOUT_ARRAY, kFlags),
      "AS TLAS", &s.tlasInfo);
  return s;
}

ComPtr<ID3D12Resource> top(Runner &run, ID3D12Resource *blas, float z = 0) {
  auto xf = translateScale(1, 0, 0, z);
  auto instance = instanceDesc(xf.m, 37, 0xff, 0, 0, blas->GetGPUVirtualAddress());
  auto ib = run.upload(&instance, sizeof(instance));
  return run.build(tlasInputs(ib->GetGPUVirtualAddress(), 1), "AS copied BLAS TLAS");
}

int hit(Runner &run, const char *name, ID3D12Resource *tlas, float t, UINT instanceIndex) {
  auto words = run.trace(name, tlas->GetGPUVirtualAddress(), {ray(0.25f, 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0)});
  return run.expect(name, words, 0, triangleHit(t, 0.25f, 0.25f, kHitGroupTagBase, 37, instanceIndex, 1));
}

UINT64 query(Runner &run, ID3D12Resource *as, UINT type) {
  auto output = run.uavBuffer(256);
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{
      output->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE(type)};
  const auto address = as->GetGPUVirtualAddress();
  run.ctx().list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &address);
  uavBarrier(run.ctx().list.Get(), output.Get());
  auto bytes = read(run, output.Get(), type == 2 ? 16 : 8);
  UINT64 value; std::memcpy(&value, bytes.data(), 8);
  return value;
}

int postbuild(Runner &run, Scene &s) {
  auto output = run.uavBuffer(256);
  auto scratch = run.uavBuffer(s.blasInfo.ScratchDataSizeInBytes);
  auto destination = run.asBuffer(s.blasInfo.ResultDataMaxSizeInBytes);
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  build.Inputs = blasInputs(s.geometry, 2, kFlags);
  build.DestAccelerationStructureData = destination->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
  const UINT offsets[] = {0, 8, 16, 32};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC inlineInfo[4];
  for (UINT i = 0; i < 4; ++i)
    inlineInfo[i] = {output->GetGPUVirtualAddress() + offsets[i], D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE(i)};
  run.ctx().list->BuildRaytracingAccelerationStructure(&build, 4, inlineInfo);
  uavBarrier(run.ctx().list.Get(), destination.Get());
  const auto source = destination->GetGPUVirtualAddress();
  for (UINT i = 0; i < 4; ++i) {
    auto info = inlineInfo[i]; info.DestBuffer += 64;
    run.ctx().list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &source);
  }
  const D3D12_GPU_VIRTUAL_ADDRESS sources[] = {source, s.tlas->GetGPUVirtualAddress()};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC arrayInfo{
      output->GetGPUVirtualAddress() + 128, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION};
  run.ctx().list->EmitRaytracingAccelerationStructurePostbuildInfo(&arrayInfo, 2, sources);
  uavBarrier(run.ctx().list.Get(), output.Get());
  auto bytes = read(run, output.Get(), 160);
  const auto *words = reinterpret_cast<const UINT64 *>(bytes.data());
  int a = (words[4] != s.blasInfo.ResultDataMaxSizeInBytes) + (words[0] > words[4]) +
      (words[0] % 256 != 0) + (words[1] < 8 + 2 * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC)) +
      (words[2] < 56 + 64) + (words[3] != 0);
  int b = std::memcmp(bytes.data(), bytes.data() + 64, 40) != 0;
  b += words[16] != words[2] || words[17] != 0 || words[19] != 2;
  return report("postbuild-inline", a) + report("postbuild-emit", b);
}

int copy(Runner &run, Scene &s, bool tlas, bool compact) {
  const char *name = compact ? (tlas ? "compact-tlas" : "compact-blas") : (tlas ? "clone-tlas" : "clone-blas");
  auto *source = tlas ? s.tlas.Get() : s.blas.Get();
  const UINT64 size = compact ? query(run, source, 0) : (tlas ? s.tlasInfo.ResultDataMaxSizeInBytes : s.blasInfo.ResultDataMaxSizeInBytes);
  auto destination = run.asBuffer(size);
  run.ctx().list->CopyRaytracingAccelerationStructure(destination->GetGPUVirtualAddress(), source->GetGPUVirtualAddress(),
      compact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
  uavBarrier(run.ctx().list.Get(), destination.Get());
  const auto current = query(run, destination.Get(), 3);
  auto scene = tlas ? destination : top(run, destination.Get());
  int bad = (current != size) + hit(run, name, scene.Get(), 3, tlas ? 1 : 0);
  if (compact) {
    auto scratch = run.uavBuffer(tlas ? s.tlasInfo.UpdateScratchDataSizeInBytes : s.blasInfo.UpdateScratchDataSizeInBytes);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC update{};
    update.Inputs = tlas ? tlasInputs(s.instances->GetGPUVirtualAddress(), 2, D3D12_ELEMENTS_LAYOUT_ARRAY, kFlags) : blasInputs(s.geometry, 2, kFlags);
    update.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(update.Inputs.Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
    update.SourceAccelerationStructureData = update.DestAccelerationStructureData = destination->GetGPUVirtualAddress();
    update.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    run.ctx().list->BuildRaytracingAccelerationStructure(&update, 0, nullptr);
    uavBarrier(run.ctx().list.Get(), destination.Get());
    auto updated = tlas ? destination : top(run, destination.Get());
    bad += hit(run, name, updated.Get(), 3, tlas ? 1 : 0);
    bad += query(run, destination.Get(), 3) != size;
  }
  return report(name, bad);
}

int serialize(Runner &run, Scene &s, bool tlas) {
  const char *name = tlas ? "serialize-tlas-relocate" : "serialize-blas";
  auto *source = tlas ? s.tlas.Get() : s.blas.Get();
  const UINT64 serializedSize = query(run, source, 2);
  constexpr UINT64 blobOffset = 256;
  // The serialized range need not own the whole backing resource. Its offsets and
  // sizes must remain correct when relocated into a larger application buffer.
  auto blob = run.uavBuffer(blobOffset + serializedSize + 1024 * 1024);
  run.ctx().list->CopyRaytracingAccelerationStructure(blob->GetGPUVirtualAddress() + blobOffset, source->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
  uavBarrier(run.ctx().list.Get(), blob.Get());
  auto bytes = read(run, blob.Get(), serializedSize, blobOffset);
  D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  int bad = header.NumBottomLevelAccelerationStructurePointersAfterHeader != (tlas ? 2 : 0);
  bad += header.DeserializedSizeInBytes != (tlas ? s.tlasInfo.ResultDataMaxSizeInBytes : s.blasInfo.ResultDataMaxSizeInBytes);
  bad += header.SerializedSizeInBytesIncludingHeader != serializedSize;
  bad += run.device5()->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
      &header.DriverMatchingIdentifier) != D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
  auto wrong = header.DriverMatchingIdentifier; wrong.DriverOpaqueVersioningData[15] ^= 1;
  bad += run.device5()->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
      &wrong) != D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
  wrong = header.DriverMatchingIdentifier; wrong.DriverOpaqueGUID.Data1 ^= 1;
  bad += run.device5()->CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
      &wrong) != D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
  // Relocation is authored on the GPU. Changing only the public pointer array must move the
  // TLAS reference; the opaque body still contains the original address.
  ComPtr<ID3D12Resource> relocated;
  if (tlas) {
    auto xf = translateScale(1, 0, 0, 4);
    auto transform = run.upload(&xf, sizeof(xf));
    auto geometry = s.geometry[1]; geometry.Triangles.Transform3x4 = transform->GetGPUVirtualAddress();
    relocated = run.build(blasInputs(&geometry, 1, kFlags), "AS relocated BLAS");
    UINT64 pointers[] = {0, relocated->GetGPUVirtualAddress()};
    auto upload = run.upload(pointers, sizeof(pointers));
    transition(run.ctx().list.Get(), blob.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    run.ctx().list->CopyBufferRegion(blob.Get(), blobOffset + 56, upload.Get(), 0, sizeof(pointers));
    transition(run.ctx().list.Get(), blob.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  auto destination = run.asBuffer(header.DeserializedSizeInBytes);
  run.ctx().list->CopyRaytracingAccelerationStructure(destination->GetGPUVirtualAddress(), blob->GetGPUVirtualAddress() + blobOffset,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
  uavBarrier(run.ctx().list.Get(), destination.Get());
  auto scene = tlas ? destination : top(run, destination.Get());
  auto words = run.trace(name, scene->GetGPUVirtualAddress(), {ray(0.25f, 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0)});
  bad += run.expect(name, words, 0, triangleHit(tlas ? 5 : 3, 0.25f, 0.25f, kHitGroupTagBase, 37, tlas ? 1 : 0, tlas ? 0 : 1));
  return report(name, bad);
}

int specialSerialization(Runner &run, bool empty) {
  const char *name = empty ? "serialize-empty" : "serialize-aabb";
  D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
  geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
  geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  const D3D12_RAYTRACING_AABB box{0, 0, 0, 1, 1, 1};
  auto boxes = run.upload(&box, sizeof(box));
  geometry.AABBs.AABBCount = 1; geometry.AABBs.AABBs = {boxes->GetGPUVirtualAddress(), sizeof(box)};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
  auto source = run.build(blasInputs(empty ? nullptr : &geometry, empty ? 0 : 1), name, &info);
  auto blob = run.uavBuffer(query(run, source.Get(), 2));
  auto destination = run.asBuffer(info.ResultDataMaxSizeInBytes);
  run.ctx().list->CopyRaytracingAccelerationStructure(blob->GetGPUVirtualAddress(), source->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
  uavBarrier(run.ctx().list.Get(), blob.Get());
  run.ctx().list->CopyRaytracingAccelerationStructure(destination->GetGPUVirtualAddress(), blob->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
  uavBarrier(run.ctx().list.Get(), destination.Get());
  auto xf = translateScale(1, 0, 0, 0);
  auto instance = instanceDesc(xf.m, 37, 0xff, 1, 0, destination->GetGPUVirtualAddress());
  auto ib = run.upload(&instance, sizeof(instance));
  auto tlas = run.build(tlasInputs(ib->GetGPUVirtualAddress(), 1), name);
  auto words = run.trace(name, tlas->GetGPUVirtualAddress(), {ray(0.5f, 0.5f, -1, 0, 0, 1)});
  Expect expected = miss();
  if (!empty) {
    expected.kind = kKindProcedural; expected.t = 1; expected.instanceId = 37;
    expected.localTag = kHitGroupTagBase + 1; expected.hitKind = kProceduralHitKind;
    expected.attributes[0] = 0.125f; expected.attributes[1] = 0.625f;
  }
  int bad = run.expect(name, words, 0, expected);
  if (empty) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    auto emptyTop = run.build(tlasInputs(0, 0), "AS empty TLAS", &topInfo);
    auto topBlob = run.uavBuffer(query(run, emptyTop.Get(), 2));
    auto topRoundtrip = run.asBuffer(topInfo.ResultDataMaxSizeInBytes);
    run.ctx().list->CopyRaytracingAccelerationStructure(topBlob->GetGPUVirtualAddress(), emptyTop->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
    uavBarrier(run.ctx().list.Get(), topBlob.Get());
    run.ctx().list->CopyRaytracingAccelerationStructure(topRoundtrip->GetGPUVirtualAddress(), topBlob->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
    uavBarrier(run.ctx().list.Get(), topRoundtrip.Get());
    auto emptyWords = run.trace(name, topRoundtrip->GetGPUVirtualAddress(), {ray(0.5f, 0.5f, -1, 0, 0, 1)});
    bad += run.expect(name, emptyWords, 0, miss());
  }
  return report(name, bad);
}

int visualize(Runner &run, Scene &s, bool tlas) {
  const char *name = tlas ? "visualize-tlas" : "visualize-blas";
  auto *source = tlas ? s.tlas.Get() : s.blas.Get();
  auto output = run.uavBuffer(query(run, source, 1));
  run.ctx().list->CopyRaytracingAccelerationStructure(output->GetGPUVirtualAddress(), source->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
  uavBarrier(run.ctx().list.Get(), output.Get());
  auto bytes = read(run, output.Get(), output->GetDesc().Width);
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  int bad = header.Type != (tlas ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) || header.NumDescs != 2;
  if (tlas) {
    const auto *instances = reinterpret_cast<const D3D12_RAYTRACING_INSTANCE_DESC *>(bytes.data() + 8);
    bad += instances[0].AccelerationStructure != 0 || instances[1].AccelerationStructure != s.blas->GetGPUVirtualAddress() || instances[1].InstanceID != 37;
    const auto xf = translateScale(1, 0, 0, 0);
    bad += std::memcmp(instances[1].Transform, xf.m, 48) != 0;
  } else {
    D3D12_RAYTRACING_GEOMETRY_DESC geometries[2];
    std::memcpy(geometries, bytes.data() + 8, sizeof(geometries));
    bad += geometries[0].Triangles.VertexCount != 0 || geometries[1].Triangles.IndexCount != 3 ||
        geometries[1].Triangles.IndexFormat != DXGI_FORMAT_R32_UINT || geometries[1].Triangles.VertexFormat != DXGI_FORMAT_R32G32B32_FLOAT;
    auto checkData = [&](UINT64 address, const void *expected, size_t size) {
      if (address < output->GetGPUVirtualAddress() || address - output->GetGPUVirtualAddress() > bytes.size() ||
          size > bytes.size() - (address - output->GetGPUVirtualAddress())) return 1;
      return int(std::memcmp(bytes.data() + address - output->GetGPUVirtualAddress(), expected, size) != 0);
    };
    const auto vertices = unitTriangle(0); const UINT indices[] = {0, 1, 2}; const auto xf = translateScale(1, 0, 0, 2);
    bad += checkData(geometries[1].Triangles.VertexBuffer.StartAddress, vertices.data(), 36);
    bad += checkData(geometries[1].Triangles.IndexBuffer, indices, 12);
    bad += checkData(geometries[1].Triangles.Transform3x4, &xf, 48);
  }
  return report(name, bad);
}

int update(Runner &run, bool tlas, bool inplace) {
  const char *name = tlas ? (inplace ? "update-tlas-inplace" : "update-tlas-copy") : (inplace ? "update-blas-inplace" : "update-blas-copy");
  auto s = scene(run);
  auto *source = tlas ? s.tlas.Get() : s.blas.Get();
  const auto info = tlas ? s.tlasInfo : s.blasInfo;
  run.submit("AS initial update build");
  auto destination = inplace ? ComPtr<ID3D12Resource>(source) : run.asBuffer(info.ResultDataMaxSizeInBytes);
  auto scratch = run.uavBuffer(info.UpdateScratchDataSizeInBytes);
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
  if (tlas) {
    auto xf = translateScale(1, 0, 0, 4);
    D3D12_RAYTRACING_INSTANCE_DESC instances[] = {instanceDesc(xf.m, 99, 0xff, 0, 0, 0), instanceDesc(xf.m, 37, 0xff, 0, 0, s.blas->GetGPUVirtualAddress())};
    auto ib = run.upload(instances, sizeof(instances));
    build.Inputs = tlasInputs(ib->GetGPUVirtualAddress(), 2, D3D12_ELEMENTS_LAYOUT_ARRAY, kFlags);
  } else {
    const auto vertices = unitTriangle(4);
    auto vb = run.upload(vertices.data(), 36);
    s.geometry[1].Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    build.Inputs = blasInputs(s.geometry, 2, kFlags);
  }
  build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(build.Inputs.Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
  build.DestAccelerationStructureData = destination->GetGPUVirtualAddress();
  build.SourceAccelerationStructureData = source->GetGPUVirtualAddress();
  build.ScratchAccelerationStructureData = info.UpdateScratchDataSizeInBytes ? scratch->GetGPUVirtualAddress() : 0;
  run.ctx().list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  uavBarrier(run.ctx().list.Get(), destination.Get());
  // A BLAS update invalidates TLASes that reference it: rebuild the TLAS before tracing.
  auto scene = tlas ? destination : top(run, destination.Get());
  int bad = hit(run, name, scene.Get(), 7, tlas ? 1 : 0);
  // Serialization after update must contain the new positions/instances, not the original build.
  auto blob = run.uavBuffer(query(run, destination.Get(), 2));
  auto roundtrip = run.asBuffer(info.ResultDataMaxSizeInBytes);
  run.ctx().list->CopyRaytracingAccelerationStructure(blob->GetGPUVirtualAddress(), destination->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
  uavBarrier(run.ctx().list.Get(), blob.Get());
  run.ctx().list->CopyRaytracingAccelerationStructure(roundtrip->GetGPUVirtualAddress(), blob->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
  uavBarrier(run.ctx().list.Get(), roundtrip.Get());
  auto roundtripScene = tlas ? roundtrip : top(run, roundtrip.Get());
  bad += hit(run, name, roundtripScene.Get(), 7, tlas ? 1 : 0);
  // The deserialized source was unknown at record time. Its update descriptor and
  // compatibility must be resolved from the GPU shadow, not a stale CPU build cache.
  build.SourceAccelerationStructureData = roundtrip->GetGPUVirtualAddress();
  build.DestAccelerationStructureData = roundtrip->GetGPUVirtualAddress();
  build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(
      build.Inputs.Flags & ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
  run.ctx().list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  uavBarrier(run.ctx().list.Get(), roundtrip.Get());
  auto updatedRoundtrip = tlas ? roundtrip : top(run, roundtrip.Get());
  bad += hit(run, name, updatedRoundtrip.Get(), 7, tlas ? 1 : 0);
  return report(name, bad);
}

int runAll(ID3D12Device5 *device, Context &context, const std::vector<uint8_t> &dxil) {
  Runner run(device, context, dxil);
  auto s = scene(run);
  int bad = postbuild(run, s);
  bad += copy(run, s, false, false); bad += copy(run, s, true, false);
  bad += copy(run, s, false, true); bad += copy(run, s, true, true);
  bad += serialize(run, s, false); bad += serialize(run, s, true);
  bad += specialSerialization(run, false); bad += specialSerialization(run, true);
  bad += visualize(run, s, false); bad += visualize(run, s, true);
  bad += update(run, false, true); bad += update(run, false, false);
  bad += update(run, true, true); bad += update(run, true, false);
  std::printf("RAYTRACING_AS_SUMMARY cases=16 failures=%d\n", bad);
  return bad;
}
} // namespace asserialize
