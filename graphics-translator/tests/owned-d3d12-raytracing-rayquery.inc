// Inline raytracing (RayQuery, Tier 1.1) cases of owned-d3d12-raytracing.cpp, driven by
// tests/dxil/raytracing-rayquery.hlsl (raytracing-rayquery-{cs,vs,ps,lib}.dxil).
// Included by owned-d3d12-raytracing.cpp after its helpers (Context, createBuffer, check, ...).
//
// One scene (two instances: triangles and AABBs, opaque and non-opaque geometry, scale 2 +
// translation transforms) is traced inline by the same `traceLane` from a compute, vertex, pixel
// and ray generation shader. Every lane's readback is compared word by word against values derived
// here from the scene; all of them are exactly representable, so the comparison is exact.

namespace rayquery {

constexpr UINT kSlotWords = 64;
constexpr UINT kLanes = 24;
constexpr UINT kOutputWords = kLanes * kSlotWords;
constexpr UINT kOutputBytes = kOutputWords * sizeof(uint32_t);
constexpr uint32_t kFill = 0xffffffffu;

// D3D12 RAY_FLAG / COMMITTED_STATUS / CANDIDATE_TYPE.
constexpr uint32_t kForceOpaque = 0x1, kAcceptFirst = 0x4, kCullBack = 0x10, kCullNonOpaque = 0x80,
                   kSkipTriangles = 0x100, kSkipProcedural = 0x200;
constexpr uint32_t kCommittedNothing = 0, kCommittedTriangle = 1, kCommittedProcedural = 2;
constexpr uint32_t kCandidateNonOpaqueTriangle = 0, kCandidateProcedural = 1;

struct LaneRay {
  float origin[3];
  float tmin;
  float direction[3];
  float tmax;
  uint32_t flags;
  uint32_t mask;
  uint32_t mode;
  float commitT;
  uint32_t scene;
  uint32_t pad[3];
};
static_assert(sizeof(LaneRay) == 64);

uint32_t bits(float value) {
  uint32_t word;
  std::memcpy(&word, &value, sizeof(word));
  return word;
}

// Instances: 0 = triangles at x 10 (InstanceID 100, contribution 3), 1 = AABBs at x 30 (InstanceID
// 102, contribution 5, instance mask 0x02). Both scale 2.
constexpr float kScale = 2.0f;
constexpr float kTranslation[2] = {10.0f, 30.0f};
constexpr uint32_t kInstanceId[2] = {100, 102};
constexpr uint32_t kContribution[2] = {3, 5};

struct Hit {
  uint32_t instance = 0, geometry = 0, primitive = 0;
  float t = 0;
  float objectX = 0; // object space ray origin x (y is 0.25, z -0.5; direction (0, 0, 0.5))
  bool triangle = true;
  bool nonOpaque = false;
};

struct Expect {
  bool candidate = false;
  Hit candidateHit;
  uint32_t candidates = 0;
  uint32_t status = kCommittedNothing;
  Hit committedHit;
  float tmax = 100.0f;
  uint32_t flags = 0;
  float tmin = 0.0f;
};

LaneRay ray(float ox, uint32_t flags = 0, uint32_t mode = 0, uint32_t scene = 0, uint32_t mask = 0xff,
            float tmin = 0.0f, float tmax = 100.0f) {
  LaneRay r{};
  r.origin[0] = ox, r.origin[1] = 0.5f, r.origin[2] = -1.0f;
  r.direction[0] = 0.0f, r.direction[1] = 0.0f, r.direction[2] = 1.0f;
  r.tmin = tmin, r.tmax = tmax;
  r.flags = flags, r.mask = mask, r.mode = mode, r.commitT = 3.0f, r.scene = scene;
  return r;
}

// Opaque triangle (geometry 0) is hit at world z 1 (t 2), object (0.25, 0.25).
Hit opaqueTriangle() { return Hit{0, 0, 0, 2.0f, 0.25f, true, false}; }
// Non-opaque triangle (geometry 1) at object x 2..3.
Hit nonOpaqueTriangle() { return Hit{0, 1, 0, 2.0f, 2.25f, true, true}; }
// AABBs: geometry 0 opaque at object x 0..1, geometry 1 non-opaque at object x 2..3; committed at t 3.
Hit opaqueBox() { return Hit{1, 0, 0, 3.0f, 0.25f, false, false}; }
Hit nonOpaqueBox() { return Hit{1, 1, 0, 3.0f, 2.25f, false, true}; }

struct Lane {
  LaneRay ray;
  Expect expect;
};

std::vector<Lane> lanes() {
  std::vector<Lane> l;
  auto committed = [](uint32_t status, Hit hit, uint32_t candidates = 0) {
    Expect e;
    e.status = status;
    e.committedHit = hit;
    e.candidates = candidates;
    return e;
  };
  auto withCandidate = [](Expect e, Hit hit) {
    e.candidate = true;
    e.candidateHit = hit;
    e.candidates = 1;
    return e;
  };
  // 0: opaque triangle is committed by the system, no candidate.
  l.push_back({ray(10.5f), committed(kCommittedTriangle, opaqueTriangle())});
  // 1: non-opaque triangle candidate, committed by CommitNonOpaqueTriangleHit (called twice).
  l.push_back({ray(14.5f), withCandidate(committed(kCommittedTriangle, nonOpaqueTriangle()), nonOpaqueTriangle())});
  // 2: the same candidate never committed.
  l.push_back({ray(14.5f, 0, 1), withCandidate(Expect{}, nonOpaqueTriangle())});
  // 3: Abort at the first candidate.
  l.push_back({ray(14.5f, 0, 2), withCandidate(Expect{}, nonOpaqueTriangle())});
  // 4: FORCE_OPAQUE turns the non-opaque triangle into a system-committed hit.
  {
    Expect e = committed(kCommittedTriangle, nonOpaqueTriangle());
    e.flags = kForceOpaque;
    l.push_back({ray(14.5f, kForceOpaque), e});
  }
  // 5: CULL_NON_OPAQUE culls it.
  {
    Expect e;
    e.flags = kCullNonOpaque;
    l.push_back({ray(14.5f, kCullNonOpaque), e});
  }
  // 6/7: procedural candidates (opaque and non-opaque AABB), CommitProceduralPrimitiveHit(3).
  l.push_back({ray(30.5f), withCandidate(committed(kCommittedProcedural, opaqueBox()), opaqueBox())});
  l.push_back({ray(34.5f), withCandidate(committed(kCommittedProcedural, nonOpaqueBox()), nonOpaqueBox())});
  // 8: procedural candidate never committed.
  l.push_back({ray(30.5f, 0, 1), withCandidate(Expect{}, opaqueBox())});
  // 9: SKIP_PROCEDURAL_PRIMITIVES.
  {
    Expect e;
    e.flags = kSkipProcedural;
    l.push_back({ray(30.5f, kSkipProcedural), e});
  }
  // 10: SKIP_TRIANGLES.
  {
    Expect e;
    e.flags = kSkipTriangles;
    l.push_back({ray(10.5f, kSkipTriangles), e});
  }
  // 11: miss.
  l.push_back({ray(50.5f), Expect{}});
  // 12: D3D12's null acceleration structure (descriptor Location 0).
  l.push_back({ray(10.5f, 0, 0, 1), Expect{}});
  // 13: the structure bound as a root SRV.
  l.push_back({ray(10.5f, 0, 0, 2), committed(kCommittedTriangle, opaqueTriangle())});
  // 14: template flags FORCE_OPAQUE | SKIP_PROCEDURAL_PRIMITIVES are OR'd into RayFlags().
  {
    Expect e = committed(kCommittedTriangle, nonOpaqueTriangle());
    e.flags = kForceOpaque | kSkipProcedural;
    l.push_back({ray(14.5f, 0, 0, 3), e});
  }
  // 15: template ACCEPT_FIRST_HIT_AND_END_SEARCH: the commit ends the search.
  {
    Expect e = withCandidate(committed(kCommittedTriangle, nonOpaqueTriangle()), nonOpaqueTriangle());
    e.flags = kAcceptFirst;
    l.push_back({ray(14.5f, 0, 0, 4), e});
  }
  // 16: instance mask: the AABB instance has mask 0x02.
  {
    Expect e;
    l.push_back({ray(30.5f, 0, 0, 0, 0x01), e});
  }
  // 17: the hit at t 2 is outside [0.5, 1.5].
  {
    Expect e;
    e.tmin = 0.5f;
    e.tmax = 1.5f;
    l.push_back({ray(10.5f, 0, 0, 0, 0xff, 0.5f, 1.5f), e});
  }
  // 18: the unit triangle is back facing to a +z ray: CULL_BACK_FACING_TRIANGLES culls it.
  {
    Expect e;
    e.flags = kCullBack;
    l.push_back({ray(10.5f, kCullBack), e});
  }
  while (l.size() < kLanes)
    l.push_back({ray(50.5f), Expect{}});
  return l;
}

class Runner {
public:
  Runner(ID3D12Device5 *device5, Context &ctx) : device5_(device5), ctx_(ctx) {
    check(device5->QueryInterface(IID_PPV_ARGS(&device_)), "rayquery: QueryInterface(ID3D12Device)");
    createRootSignature();
    createScene();
    createCommon();
  }

  ID3D12Device *device() const { return device_.Get(); }
  ID3D12RootSignature *root() const { return root_.Get(); }

  void bindCompute() {
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    ctx_.list->SetDescriptorHeaps(1, heaps);
    ctx_.list->SetComputeRootSignature(root_.Get());
    ctx_.list->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
    ctx_.list->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
  }

  void bindGraphics() {
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    ctx_.list->SetDescriptorHeaps(1, heaps);
    ctx_.list->SetGraphicsRootSignature(root_.Get());
    ctx_.list->SetGraphicsRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
    ctx_.list->SetGraphicsRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
  }

  void clearOutput() {
    ctx_.list->CopyBufferRegion(output_.Get(), 0, fill_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  std::vector<uint32_t> finish(const char *name) {
    uavBarrier(ctx_.list.Get(), output_.Get());
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx_.list->CopyBufferRegion(readback_.Get(), 0, output_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    const HRESULT hr = ctx_.list->Close();
    if (FAILED(hr)) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_RAYQUERY_FAIL case=%s close=0x%08lx\n", name,
                   static_cast<unsigned long>(hr));
      ExitProcess(1);
    }
    ID3D12CommandList *lists[] = {ctx_.list.Get()};
    ctx_.queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++ctx_.fenceValue;
    check(ctx_.queue->Signal(ctx_.fence.Get(), value), "rayquery: Signal");
    if (ctx_.fence->GetCompletedValue() < value) {
      check(ctx_.fence->SetEventOnCompletion(value, ctx_.event), "rayquery: SetEventOnCompletion");
      require(WaitForSingleObject(ctx_.event, kFenceTimeoutMs) == WAIT_OBJECT_0, "rayquery: fence timeout");
    }
    check(ctx_.allocator->Reset(), "rayquery: allocator Reset");
    check(ctx_.list->Reset(ctx_.allocator.Get(), nullptr), "rayquery: list Reset");
    void *mapped = nullptr;
    D3D12_RANGE range{0, kOutputBytes};
    check(readback_->Map(0, &range, &mapped), "rayquery: Map(readback)");
    std::vector<uint32_t> words(kOutputWords);
    std::memcpy(words.data(), mapped, kOutputBytes);
    D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    return words;
  }

  // Compare every lane of one stage's readback. Lanes from `activeLanes` on must be untouched.
  // Returns the number of mismatching words.
  int verify(const char *name, uint32_t tag, const std::vector<uint32_t> &words, UINT activeLanes = kLanes) {
    int bad = 0;
    const auto all = lanes();
    for (UINT lane = 0; lane < kLanes; ++lane) {
      const uint32_t *slot = words.data() + lane * kSlotWords;
      const Expect &e = all[lane].expect;
      const LaneRay &r = all[lane].ray;
      std::array<uint32_t, kSlotWords> want;
      want.fill(kFill);
      if (lane >= activeLanes) {
        for (UINT w = 0; w < kSlotWords; ++w)
          if (slot[w] != want[w]) {
            std::fprintf(stderr,
                         "OWNED_D3D12_RAYTRACING_RAYQUERY_MISMATCH case=%s lane=%u word=%u got=0x%08x want=0x%08x\n",
                         name, lane, w, slot[w], want[w]);
            ++bad;
          }
        continue;
      }
      want[0] = e.candidates;
      if (e.candidate) {
        const Hit &h = e.candidateHit;
        want[1] = h.triangle ? kCandidateNonOpaqueTriangle : kCandidateProcedural;
        want[2] = h.instance;
        want[3] = kInstanceId[h.instance];
        want[4] = kContribution[h.instance];
        want[5] = h.geometry;
        want[6] = h.primitive;
        want[7] = h.triangle ? 0u /* back facing */ : (h.nonOpaque ? 1u : 0u);
        if (h.triangle) {
          want[8] = bits(h.t);
          want[27] = bits(0.25f);
          want[28] = bits(0.25f);
        }
        objectRay(want.data() + 9, h);
        transform(want.data() + 15, h.instance);
      }
      want[29] = e.status;
      want[36] = bits(e.status == kCommittedNothing ? e.tmax : e.committedHit.t);
      if (e.status != kCommittedNothing) {
        const Hit &h = e.committedHit;
        want[30] = h.instance;
        want[31] = kInstanceId[h.instance];
        want[32] = kContribution[h.instance];
        want[33] = h.geometry;
        want[34] = h.primitive;
        objectRay(want.data() + 37, h);
        transform(want.data() + 43, h.instance);
        want[62] = bits(-kTranslation[h.instance] / kScale);
      }
      if (e.status == kCommittedTriangle) {
        want[35] = 0;
        want[55] = bits(0.25f);
        want[56] = bits(0.25f);
      }
      want[57] = e.flags;
      want[58] = bits(e.tmin);
      want[59] = bits(r.origin[0]);
      want[60] = bits(r.origin[1]);
      want[61] = bits(r.origin[2]);
      want[63] = 0x5100u | tag;
      for (UINT w = 0; w < kSlotWords; ++w)
        if (slot[w] != want[w]) {
          std::fprintf(stderr,
                       "OWNED_D3D12_RAYTRACING_RAYQUERY_MISMATCH case=%s lane=%u word=%u got=0x%08x want=0x%08x\n",
                       name, lane, w, slot[w], want[w]);
          ++bad;
        }
    }
    std::printf("OWNED_D3D12_RAYTRACING_RAYQUERY_CASE %s lanes=%u status=%s\n", name, kLanes, bad ? "FAIL" : "PASS");
    std::fflush(stdout);
    return bad;
  }

private:
  static void objectRay(uint32_t *out, const Hit &h) {
    out[0] = bits(h.objectX);
    out[1] = bits(0.25f);
    out[2] = bits(-0.5f);
    out[3] = bits(0.0f);
    out[4] = bits(0.0f);
    out[5] = bits(0.5f);
  }

  static void transform(uint32_t *out, uint32_t instance) {
    const float m[3][4] = {{kScale, 0, 0, kTranslation[instance]}, {0, kScale, 0, 0}, {0, 0, kScale, 0}};
    for (UINT r = 0; r < 3; ++r)
      for (UINT c = 0; c < 4; ++c)
        out[r * 4 + c] = bits(m[r][c]);
  }

  void createRootSignature() {
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER1 params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 3;
    D3D12_ROOT_SIGNATURE_DESC1 desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    root_ = ::createRootSignature(device_.Get(), desc, "rayquery: root signature");
  }

  ComPtr<ID3D12Resource> upload(const void *data, UINT64 size) {
    auto buffer = createBuffer(device_.Get(), std::max<UINT64>(size, 256), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = mapUpload(buffer.Get());
    std::memcpy(mapped, data, size);
    buffer->Unmap(0, nullptr);
    keep_.push_back(buffer);
    return buffer;
  }

  ComPtr<ID3D12Resource> build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    require(info.ResultDataMaxSizeInBytes > 0, "rayquery: prebuild");
    auto destination = createBuffer(device_.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto scratch = createBuffer(device_.Get(), std::max<UINT64>(info.ScratchDataSizeInBytes, 256),
                                D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = inputs;
    desc.DestAccelerationStructureData = destination->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    ctx_.list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    uavBarrier(ctx_.list.Get(), destination.Get());
    keep_.push_back(destination);
    keep_.push_back(scratch);
    return destination;
  }

  void createScene() {
    // Geometry 0: opaque unit triangle at object z 0.5, x 0..1; geometry 1: non-opaque one at x 2..3.
    const float vertices[] = {0, 0, 0.5f, 1, 0, 0.5f, 0, 1, 0.5f, 2, 0, 0.5f, 3, 0, 0.5f, 2, 1, 0.5f};
    auto vb = upload(vertices, sizeof(vertices));
    D3D12_RAYTRACING_GEOMETRY_DESC triangles[2]{};
    for (UINT g = 0; g < 2; ++g) {
      triangles[g].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
      triangles[g].Flags = g == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
      triangles[g].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
      triangles[g].Triangles.VertexCount = 3;
      triangles[g].Triangles.VertexBuffer = {vb->GetGPUVirtualAddress() + g * 36, 12};
    }
    // AABBs: geometry 0 opaque box [0,1]^3, geometry 1 non-opaque box [2,3]x[0,1]x[0,1].
    const D3D12_RAYTRACING_AABB boxes[2] = {{0, 0, 0, 1, 1, 1}, {2, 0, 0, 3, 1, 1}};
    auto bb = upload(boxes, sizeof(boxes));
    D3D12_RAYTRACING_GEOMETRY_DESC aabbs[2]{};
    for (UINT g = 0; g < 2; ++g) {
      aabbs[g].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
      aabbs[g].Flags = g == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
      aabbs[g].AABBs.AABBCount = 1;
      aabbs[g].AABBs.AABBs = {bb->GetGPUVirtualAddress() + g * sizeof(D3D12_RAYTRACING_AABB),
                              sizeof(D3D12_RAYTRACING_AABB)};
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 2;
    inputs.pGeometryDescs = triangles;
    auto triangleBlas = build(inputs);
    inputs.pGeometryDescs = aabbs;
    auto aabbBlas = build(inputs);

    D3D12_RAYTRACING_INSTANCE_DESC instances[2]{};
    for (UINT i = 0; i < 2; ++i) {
      instances[i].Transform[0][0] = instances[i].Transform[1][1] = instances[i].Transform[2][2] = kScale;
      instances[i].Transform[0][3] = kTranslation[i];
      instances[i].InstanceID = kInstanceId[i];
      instances[i].InstanceMask = i == 0 ? 0xff : 0x02;
      instances[i].InstanceContributionToHitGroupIndex = kContribution[i];
      instances[i].AccelerationStructure = (i == 0 ? triangleBlas : aabbBlas)->GetGPUVirtualAddress();
    }
    auto ib = upload(instances, sizeof(instances));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.NumDescs = 2;
    top.InstanceDescs = ib->GetGPUVirtualAddress();
    tlas_ = build(top);
  }

  void createCommon() {
    std::vector<LaneRay> rays;
    for (const Lane &lane : lanes())
      rays.push_back(lane.ray);
    rays_ = upload(rays.data(), rays.size() * sizeof(LaneRay));
    output_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> fill(kOutputBytes, 0xff);
    fill_ = upload(fill.data(), kOutputBytes);
    readback_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)), "rayquery: descriptor heap");
    const UINT increment = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto handle = [&](UINT index) {
      D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
      h.ptr += SIZE_T(index) * increment;
      return h;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC as{};
    as.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    as.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as.RaytracingAccelerationStructure.Location = tlas_->GetGPUVirtualAddress();
    device_->CreateShaderResourceView(nullptr, &as, handle(0));
    D3D12_SHADER_RESOURCE_VIEW_DESC structured{};
    structured.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    structured.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    structured.Buffer.NumElements = kLanes;
    structured.Buffer.StructureByteStride = sizeof(LaneRay);
    device_->CreateShaderResourceView(rays_.Get(), &structured, handle(1));
    as.RaytracingAccelerationStructure.Location = 0;
    device_->CreateShaderResourceView(nullptr, &as, handle(2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kOutputWords;
    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device_->CreateUnorderedAccessView(output_.Get(), nullptr, &uav, handle(3));
  }

  ID3D12Device5 *device5_;
  Context &ctx_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12Resource> tlas_, rays_, output_, fill_, readback_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  std::vector<ComPtr<ID3D12Resource>> keep_;
};

D3D12_SHADER_BYTECODE bytecode(const std::vector<uint8_t> &dxil) { return {dxil.data(), dxil.size()}; }

int caseCompute(Runner &run, Context &ctx, const std::vector<uint8_t> &dxil) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = run.root();
  desc.CS = bytecode(dxil);
  ComPtr<ID3D12PipelineState> pso;
  check(run.device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "rayquery: compute PSO");
  run.clearOutput();
  run.bindCompute();
  ctx.list->SetPipelineState(pso.Get());
  ctx.list->Dispatch(kLanes / 8, 1, 1);
  return run.verify("cs", 1, run.finish("cs"));
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc(ID3D12RootSignature *root) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.SampleDesc.Count = 1;
  return desc;
}

int caseVertex(Runner &run, Context &ctx, const std::vector<uint8_t> &dxil) {
  auto desc = graphicsDesc(run.root());
  desc.VS = bytecode(dxil);
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  ComPtr<ID3D12PipelineState> pso;
  check(run.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "rayquery: vertex PSO");
  run.clearOutput();
  run.bindGraphics();
  ctx.list->SetPipelineState(pso.Get());
  D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  D3D12_RECT scissor{0, 0, 1, 1};
  ctx.list->RSSetViewports(1, &viewport);
  ctx.list->RSSetScissorRects(1, &scissor);
  ctx.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  ctx.list->DrawInstanced(kLanes, 1, 0, 0);
  return run.verify("vs", 2, run.finish("vs"));
}

int casePixel(Runner &run, Context &ctx, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps) {
  // One pixel per lane: a kLanes x 1 target covered by a full-screen triangle.
  D3D12_HEAP_PROPERTIES props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC target{};
  target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target.Width = kLanes;
  target.Height = 1;
  target.DepthOrArraySize = 1;
  target.MipLevels = 1;
  target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target.SampleDesc.Count = 1;
  target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> rt;
  check(run.device()->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &target,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)),
        "rayquery: render target");
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
  check(run.device()->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "rayquery: rtv heap");
  run.device()->CreateRenderTargetView(rt.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

  auto desc = graphicsDesc(run.root());
  desc.VS = bytecode(vs);
  desc.PS = bytecode(ps);
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  ComPtr<ID3D12PipelineState> pso;
  check(run.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "rayquery: pixel PSO");
  run.clearOutput();
  run.bindGraphics();
  ctx.list->SetPipelineState(pso.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, float(kLanes), 1, 0, 1};
  D3D12_RECT scissor{0, 0, LONG(kLanes), 1};
  ctx.list->RSSetViewports(1, &viewport);
  ctx.list->RSSetScissorRects(1, &scissor);
  ctx.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx.list->DrawInstanced(3, 1, 0, 0);
  return run.verify("ps", 3, run.finish("ps"));
}

struct RayGenPipeline {
  ComPtr<ID3D12StateObject> stateObject;
  ComPtr<ID3D12Resource> table; // the RQRayGen record at offset 0
};

RayGenPipeline createRayGenPipeline(Runner &run, ID3D12Device5 *device5, const std::vector<uint8_t> &dxil) {
  D3D12_DXIL_LIBRARY_DESC library{};
  library.DXILLibrary = bytecode(dxil);
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  ID3D12RootSignature *global = run.root();
  D3D12_STATE_SUBOBJECT subobjects[4] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
  };
  D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 4, subobjects};
  RayGenPipeline pipeline;
  check(device5->CreateStateObject(&desc, IID_PPV_ARGS(&pipeline.stateObject)), "rayquery: CreateStateObject");
  ComPtr<ID3D12StateObjectProperties> properties;
  check(pipeline.stateObject.As(&properties), "rayquery: StateObjectProperties");
  const void *identifier = properties->GetShaderIdentifier(L"RQRayGen");
  require(identifier != nullptr, "rayquery: GetShaderIdentifier(RQRayGen)");
  pipeline.table = createBuffer(run.device(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = mapUpload(pipeline.table.Get());
  std::memcpy(mapped, identifier, kShaderIdentifierSize);
  pipeline.table->Unmap(0, nullptr);
  return pipeline;
}

int caseRayGeneration(Runner &run, Context &ctx, const RayGenPipeline &pipeline) {
  run.clearOutput();
  run.bindCompute();
  ctx.list->SetPipelineState1(pipeline.stateObject.Get());
  D3D12_DISPATCH_RAYS_DESC dispatch{};
  dispatch.RayGenerationShaderRecord = {pipeline.table->GetGPUVirtualAddress(), kShaderIdentifierSize};
  dispatch.Width = kLanes;
  dispatch.Height = 1;
  dispatch.Depth = 1;
  ctx.list->DispatchRays(&dispatch);
  return run.verify("raygen", 4, run.finish("raygen"));
}

/*
 * ExecuteIndirect(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS) whose arguments and count a compute shader
 * writes on the GPU (cs_rays_args): 4 slots, count 3. Slot 0 runs lanes 0..7, slot 1 is an invalid
 * description and runs nothing, slot 2 runs lanes 0..15, slot 3 is past the count. Lanes 16.. stay
 * untouched, which is what pins the count buffer, the per-slot widths and the GPU-side validation.
 */
int caseRayGenerationIndirect(Runner &run, Context &ctx, const RayGenPipeline &pipeline,
                              const std::vector<uint8_t> &argsDxil) {
  constexpr UINT kSlots = 4, kCountOffset = 512, kArgsBytes = 1024;
  static_assert(kSlots * sizeof(D3D12_DISPATCH_RAYS_DESC) <= kCountOffset);
  static_assert(sizeof(D3D12_DISPATCH_RAYS_DESC) == 104);

  D3D12_DESCRIPTOR_RANGE1 range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 1;
  D3D12_ROOT_PARAMETER1 params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 2;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = params;
  auto argsRoot = ::createRootSignature(run.device(), rootDesc, "rayquery: args root signature");
  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = argsRoot.Get();
  psoDesc.CS = bytecode(argsDxil);
  ComPtr<ID3D12PipelineState> argsPso;
  check(run.device()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&argsPso)), "rayquery: args PSO");

  auto args = createBuffer(run.device(), kArgsBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComPtr<ID3D12DescriptorHeap> argsHeap;
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  check(run.device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&argsHeap)), "rayquery: args heap");
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = kArgsBytes / 4;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  run.device()->CreateUnorderedAccessView(args.Get(), nullptr, &uav, argsHeap->GetCPUDescriptorHandleForHeapStart());

  D3D12_INDIRECT_ARGUMENT_DESC argument{};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
  signatureDesc.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
  signatureDesc.NumArgumentDescs = 1;
  signatureDesc.pArgumentDescs = &argument;
  ComPtr<ID3D12CommandSignature> signature;
  check(run.device()->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&signature)),
        "rayquery: DISPATCH_RAYS command signature");

  run.clearOutput();
  ID3D12DescriptorHeap *heaps[] = {argsHeap.Get()};
  ctx.list->SetDescriptorHeaps(1, heaps);
  ctx.list->SetComputeRootSignature(argsRoot.Get());
  const uint64_t record = pipeline.table->GetGPUVirtualAddress();
  const uint32_t recordWords[2] = {uint32_t(record), uint32_t(record >> 32)};
  ctx.list->SetComputeRoot32BitConstants(0, 2, recordWords, 0);
  ctx.list->SetComputeRootDescriptorTable(1, argsHeap->GetGPUDescriptorHandleForHeapStart());
  ctx.list->SetPipelineState(argsPso.Get());
  ctx.list->Dispatch(1, 1, 1);
  transition(ctx.list.Get(), args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

  run.bindCompute();
  ctx.list->SetPipelineState1(pipeline.stateObject.Get());
  ctx.list->ExecuteIndirect(signature.Get(), kSlots, args.Get(), 0, args.Get(), kCountOffset);
  return run.verify("raygen-indirect", 4, run.finish("raygen-indirect"), 16);
}

// argv order: cs, vs, fs (full-screen VS of the pixel case), ps, lib, args.
int runAll(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &cs, const std::vector<uint8_t> &vs,
           const std::vector<uint8_t> &fs, const std::vector<uint8_t> &ps, const std::vector<uint8_t> &lib,
           const std::vector<uint8_t> &args) {
  Runner run(device5, ctx);
  int failures = 0;
  failures += caseCompute(run, ctx, cs) ? 1 : 0;
  failures += caseVertex(run, ctx, vs) ? 1 : 0;
  failures += casePixel(run, ctx, fs, ps) ? 1 : 0;
  const RayGenPipeline raygen = createRayGenPipeline(run, device5, lib);
  failures += caseRayGeneration(run, ctx, raygen) ? 1 : 0;
  failures += caseRayGenerationIndirect(run, ctx, raygen, args) ? 1 : 0;
  std::printf("OWNED_D3D12_RAYTRACING_RAYQUERY failures=%d\n", failures);
  std::fflush(stdout);
  return failures;
}

} // namespace rayquery
