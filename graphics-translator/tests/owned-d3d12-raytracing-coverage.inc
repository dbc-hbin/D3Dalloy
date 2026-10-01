// Coverage cases of owned-d3d12-raytracing.cpp, driven by tests/dxil/raytracing-coverage.hlsl.
// Included after owned-d3d12-raytracing-rayquery.inc; reuses the contract geometry helpers and the
// Context / createBuffer / check helpers of the main file.
//
// One 64-lane ray table (lane L traces the ray of kind L % 16) runs through every dispatch shape,
// and every lane's 32-word slot is compared word for word against the D3D12 result:
//   grid-direct     DispatchRays 8x4x2: DispatchRaysIndex/Dimensions per lane (raygen, and the
//                   lane index the any-hit / intersection shaders derive from them)
//   grid-indirect   the same grid through ExecuteIndirect(DISPATCH_RAYS), arguments at a nonzero
//                   offset with a signature stride above sizeof(D3D12_DISPATCH_RAYS_DESC)
//   predication     DispatchRays and ExecuteIndirect(DISPATCH_RAYS) under SetPredication, both
//                   ops, predicate 0 and 1: culled dispatches leave the output untouched
//   bundle          ExecuteIndirect(DISPATCH_RAYS) recorded in a bundle; a bundle's DispatchRays and
//                   ExecuteIndirect inheriting the caller's predication
// Every dispatch uses miss, hit-group and callable tables at non-default offsets and strides, and
// every record carries a local root constant and a local descriptor table (an SRV of its own data
// word). The ray kinds cover GeometryIndex() in closest-hit / any-hit / intersection over
// multi-geometry triangle and AABB BLASes, hit-group indexing with nonzero contributions and
// multipliers, miss indices, RAY_FLAG_SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES in TraceRay,
// RayQuery<RAY_FLAG_SKIP_TRIANGLES> / <RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>, and recursive
// CallShader chains from ray generation, closest-hit and miss.

namespace coverage {

using contract::blasInputs;
using contract::Mat;
using contract::tlasInputs;
using contract::translateScale;
using contract::triangles;

constexpr UINT kSlotWords = 32;
constexpr UINT kWidth = 8, kHeight = 4, kDepth = 2;
constexpr UINT kLanes = kWidth * kHeight * kDepth;
constexpr UINT kOutputWords = kSlotWords * kLanes;
constexpr UINT kOutputBytes = kOutputWords * sizeof(uint32_t);
constexpr uint32_t kFill = 0xffffffffu;
constexpr uint32_t kCallEnabled = 0x80000000u;
constexpr uint32_t kRayFlagSkipTriangles = 0x100, kRayFlagSkipProcedural = 0x200;
constexpr uint32_t kTriangleInstanceId = 0x50, kBoxInstanceId = 0x51, kBoxContribution = 3;

// Shader table (one upload buffer), every table at a non-default offset and stride.
constexpr UINT kRayGenOffset = 64, kRayGenSize = 48;
constexpr UINT kMissOffset = 256, kMissStride = 128, kMissCount = 3;
constexpr UINT kHitOffset = 1024, kHitStride = 96, kHitCount = 8;
constexpr UINT kCallableOffset = 2048, kCallableStride = 160, kCallableCount = 3;
constexpr UINT kTableBytes = 4096;
// Local descriptor slot of each record (heap index kGlobalDescriptors + slot).
constexpr UINT kGlobalDescriptors = 3;
constexpr UINT kLocalMiss = 1, kLocalHit = kLocalMiss + kMissCount, kLocalCallable = kLocalHit + kHitCount;
constexpr UINT kLocalCount = kLocalCallable + kCallableCount;

enum Table : uint32_t { kRayGenTable = 0, kMissTable = 1, kHitTable = 2, kCallableTable = 3 };
// The root constant of a record; a callable's low nibble is its own table index.
uint32_t recordTag(Table table, UINT index) {
  static constexpr uint32_t base[] = {0x0001, 0x2000, 0x1000, 0x3000};
  return base[table] + index;
}
// The word the record's local descriptor table SRV holds.
uint32_t recordData(Table table, UINT index) { return 0xDA000000u | table << 8 | index; }

struct LaneRay {
  float origin[3];
  float tmin;
  float direction[3];
  float tmax;
  uint32_t flags, mask, contribution, multiplier, missIndex, callRayGen, callHit, query;
};
static_assert(sizeof(LaneRay) == 64);

uint32_t callWord(UINT start, UINT remaining) { return kCallEnabled | remaining << 8 | start; }

// What CvCallable's chain computes for a call word: (tag nibble accumulator, data sum).
std::pair<uint32_t, uint32_t> callChain(uint32_t word) {
  UINT index = word & 0xff, remaining = (word >> 8) & 0xff;
  uint32_t acc = 0, data = 0;
  for (;;) {
    acc = acc * 16 + index;
    data += recordData(kCallableTable, index);
    if (!remaining)
      break;
    --remaining;
    index = (index + 1) % kCallableCount;
  }
  return {acc, data};
}

enum class Target { Triangle, Box, Nothing };

struct LaneSpec {
  Target target = Target::Nothing;
  UINT geometry = 0;
  LaneRay ray{};
};

// The ray of lane `lane` (kind lane % 16). Triangle geometry g is the unit triangle at x 2g, z 1;
// procedural geometry g is the unit box at x 2g, y 4, z 2..3 (instance translation (0, 4, 2)).
LaneSpec laneSpec(UINT lane) {
  LaneSpec s;
  LaneRay &r = s.ray;
  r.multiplier = 1;
  r.mask = 0xff;
  r.tmax = 100.0f;
  auto aim = [&](Target target, UINT geometry) { s.target = target, s.geometry = geometry; };
  switch (lane % 16) {
  case 0: case 1: case 2: aim(Target::Triangle, lane % 16); break;
  case 3: case 4: case 5: aim(Target::Box, lane % 16 - 3); break;
  case 6: aim(Target::Triangle, 2), r.multiplier = 2, r.contribution = 2; break; // record 6
  case 7: aim(Target::Triangle, 1), r.multiplier = 3, r.contribution = 4; break; // record 7
  case 8: r.missIndex = 0; break;
  case 9: r.missIndex = 2; break;
  case 10: aim(Target::Triangle, 0), r.flags = kRayFlagSkipTriangles, r.missIndex = 1; break;
  case 11: aim(Target::Box, 1), r.flags = kRayFlagSkipProcedural, r.missIndex = 2; break;
  case 12: aim(Target::Box, 2), r.flags = kRayFlagSkipTriangles; break;
  case 13: aim(Target::Triangle, 0), r.callHit = callWord(1, 1), r.callRayGen = callWord(2, 2); break;
  case 14: r.missIndex = 1, r.callHit = callWord(0, 1), r.callRayGen = callWord(1, 0); break;
  default:
    switch ((lane / 16) % 4) {
    case 0: aim(Target::Triangle, 1), r.query = 1; break;
    case 1: aim(Target::Box, 2), r.query = 1; break;
    case 2: aim(Target::Box, 0), r.query = 2; break;
    default: aim(Target::Triangle, 2), r.query = 2; break;
    }
  }
  r.origin[0] = s.target == Target::Nothing ? -10.0f : 2.0f * s.geometry + 0.25f;
  r.origin[1] = s.target == Target::Box ? 4.25f : 0.25f;
  r.origin[2] = -1.0f;
  r.direction[2] = 1.0f;
  return s;
}

// The slot lane (x, y, z) of a w x h x d dispatch must hold.
std::array<uint32_t, kSlotWords> expectedSlot(UINT x, UINT y, UINT z, UINT w, UINT h, UINT d) {
  const UINT lane = x + w * (y + h * z);
  const LaneSpec s = laneSpec(lane);
  const LaneRay &r = s.ray;
  std::array<uint32_t, kSlotWords> want;
  want.fill(kFill);
  want[0] = x, want[1] = y, want[2] = z, want[3] = w, want[4] = h, want[5] = d;
  want[6] = recordTag(kRayGenTable, 0);
  want[7] = recordData(kRayGenTable, 0);
  if (r.callRayGen) {
    const auto [acc, data] = callChain(r.callRayGen);
    want[8] = acc, want[9] = data;
  }
  const bool hit = (s.target == Target::Triangle && !(r.flags & kRayFlagSkipTriangles)) ||
                   (s.target == Target::Box && !(r.flags & kRayFlagSkipProcedural));
  uint32_t kind = 2;
  if (hit) {
    const bool box = s.target == Target::Box;
    const UINT record = (box ? kBoxContribution : 0) + r.contribution + r.multiplier * s.geometry;
    kind = box ? 3 : 1;
    want[11] = recordTag(kHitTable, record);
    want[12] = recordData(kHitTable, record);
    want[13] = s.geometry;
    want[14] = 0;
    want[15] = box ? kBoxInstanceId : kTriangleInstanceId;
    if (!box && s.geometry != 0) { // geometries 1 and 2 are non-opaque: the any-hit runs once
      want[16] = kFill + 1;
      want[17] = 0xA00 | s.geometry;
    }
    if (box)
      want[18] = 0xB00 | s.geometry;
  } else {
    want[11] = recordTag(kMissTable, r.missIndex);
    want[12] = recordData(kMissTable, r.missIndex);
  }
  want[10] = kind;
  if (r.callHit) {
    const auto [acc, data] = callChain(r.callHit);
    want[19] = acc, want[20] = data;
  }
  want[21] = 0x100 | kind;
  if (r.query) {
    // The template flag culls the targeted primitive type; the other one is one candidate the
    // shader commits (triangles 1 and 2 are non-opaque, boxes always produce a candidate).
    const bool culled = (r.query == 1 && s.target == Target::Triangle) || (r.query == 2 && s.target == Target::Box);
    const uint32_t status = culled ? 0 : s.target == Target::Box ? 2 : 1;
    want[22] = 0x300 | status;
    want[23] = culled ? 0xff : s.geometry;
    want[24] = r.query == 1 ? kRayFlagSkipTriangles : kRayFlagSkipProcedural;
    want[25] = culled ? 0 : 1;
  }
  return want;
}

int reportCase(const char *name, int bad) {
  std::printf("OWNED_D3D12_RAYTRACING_COVERAGE_CASE %s status=%s\n", name, bad ? "FAIL" : "PASS");
  std::fflush(stdout);
  return bad ? 1 : 0;
}

class Suite {
public:
  Suite(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil, contract::Runner &geometry)
      : device5_(device5), ctx_(ctx), geometry_(geometry) {
    check(device5->QueryInterface(IID_PPV_ARGS(&device_)), "coverage: QueryInterface(ID3D12Device)");
    createRootSignatures();
    createStateObject(dxil);
    createResources();
    buildScene();
    writeTable();
  }

  ID3D12Device *device() const { return device_.Get(); }
  ID3D12StateObject *stateObject() const { return stateObject_.Get(); }
  ID3D12DescriptorHeap *heap() const { return heap_.Get(); }

  D3D12_DISPATCH_RAYS_DESC dispatchDesc(UINT w, UINT h, UINT d) const {
    const D3D12_GPU_VIRTUAL_ADDRESS table = table_->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC desc{};
    desc.RayGenerationShaderRecord = {table + kRayGenOffset, kRayGenSize};
    desc.MissShaderTable = {table + kMissOffset, kMissStride * kMissCount, kMissStride};
    desc.HitGroupTable = {table + kHitOffset, kHitStride * kHitCount, kHitStride};
    desc.CallableShaderTable = {table + kCallableOffset, kCallableStride * kCallableCount, kCallableStride};
    desc.Width = w, desc.Height = h, desc.Depth = d;
    return desc;
  }

  // Global bindings, the output reset, `body` (the dispatch), then the readback.
  template <typename Body> std::vector<uint32_t> run(const char *name, Body &&body) {
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    auto *list = ctx_.list.Get();
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(globalRoot_.Get());
    list->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
    list->CopyBufferRegion(output_.Get(), 0, fill_.Get(), 0, kOutputBytes);
    transition(list, output_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    body(list);
    uavBarrier(list, output_.Get());
    transition(list, output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback_.Get(), 0, output_.Get(), 0, kOutputBytes);
    transition(list, output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    geometry_.submit(name);
    void *mapped = nullptr;
    D3D12_RANGE range{0, kOutputBytes};
    check(readback_->Map(0, &range, &mapped), "coverage: Map(readback)");
    std::vector<uint32_t> words(kOutputWords);
    std::memcpy(words.data(), mapped, kOutputBytes);
    D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    return words;
  }

  // Compare every lane: lanes of a w x h x d grid hold their expected slot, the rest stay at the
  // fill pattern (w = 0: the dispatch was culled). `laneBad`, when given, receives per-lane counts.
  int verify(const char *name, const std::vector<uint32_t> &words, UINT w, UINT h, UINT d,
             std::vector<int> *laneBad = nullptr) {
    int bad = 0, printed = 0;
    if (laneBad)
      laneBad->assign(kLanes, 0);
    const UINT lanes = w * h * d;
    for (UINT lane = 0; lane < kLanes; ++lane) {
      std::array<uint32_t, kSlotWords> want;
      want.fill(kFill);
      if (lane < lanes)
        want = expectedSlot(lane % w, (lane / w) % h, lane / (w * h), w, h, d);
      for (UINT i = 0; i < kSlotWords; ++i) {
        const uint32_t got = words[lane * kSlotWords + i];
        if (got == want[i])
          continue;
        ++bad;
        if (laneBad)
          ++(*laneBad)[lane];
        if (printed++ < 24)
          std::fprintf(stderr,
                       "OWNED_D3D12_RAYTRACING_CASE_MISMATCH case=coverage-%s lane=%u word=%u got=0x%08x want=0x%08x\n",
                       name, lane, i, got, want[i]);
      }
    }
    return bad;
  }

  // An upload buffer holding one D3D12_DISPATCH_RAYS_DESC at kIndirectOffset.
  static constexpr UINT kIndirectOffset = 16, kIndirectStride = 112;
  ComPtr<ID3D12Resource> indirectArguments(UINT w, UINT h, UINT d) {
    auto buffer = createBuffer(device_.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    auto *mapped = static_cast<uint8_t *>(mapUpload(buffer.Get()));
    std::memset(mapped, 0xcd, 256);
    const auto desc = dispatchDesc(w, h, d);
    std::memcpy(mapped + kIndirectOffset, &desc, sizeof(desc));
    buffer->Unmap(0, nullptr);
    return buffer;
  }

  ID3D12CommandSignature *signature() {
    if (!signature_) {
      D3D12_INDIRECT_ARGUMENT_DESC argument{};
      argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
      D3D12_COMMAND_SIGNATURE_DESC desc{};
      desc.ByteStride = kIndirectStride;
      desc.NumArgumentDescs = 1;
      desc.pArgumentDescs = &argument;
      check(device_->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature_)),
            "coverage: DISPATCH_RAYS command signature");
    }
    return signature_.Get();
  }

  // Predicate words: 0 at offset 0, 1 at offset 8.
  ID3D12Resource *predicate() const { return predicate_.Get(); }

private:
  D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * increment_;
    return h;
  }

  void createRootSignatures() {
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER1 global{};
    global.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    global.DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC1 globalDesc{};
    globalDesc.NumParameters = 1;
    globalDesc.pParameters = &global;
    globalRoot_ = createRootSignature(device_.Get(), globalDesc, "coverage: global root signature");

    D3D12_DESCRIPTOR_RANGE1 localRange{};
    localRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    localRange.NumDescriptors = 1;
    localRange.BaseShaderRegister = 0;
    localRange.RegisterSpace = 1;
    D3D12_ROOT_PARAMETER1 local[2]{};
    local[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    local[0].Constants = {0, 1, 1};
    local[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    local[1].DescriptorTable = {1, &localRange};
    D3D12_ROOT_SIGNATURE_DESC1 localDesc{};
    localDesc.NumParameters = 2;
    localDesc.pParameters = local;
    localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    localRoot_ = createRootSignature(device_.Get(), localDesc, "coverage: local root signature");
  }

  void createStateObject(const std::vector<uint8_t> &dxil) {
    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary = {dxil.data(), dxil.size()};
    D3D12_HIT_GROUP_DESC triangleGroup{};
    triangleGroup.HitGroupExport = L"CvTriangleGroup";
    triangleGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    triangleGroup.AnyHitShaderImport = L"CvAnyHit";
    triangleGroup.ClosestHitShaderImport = L"CvClosestHit";
    D3D12_HIT_GROUP_DESC boxGroup{};
    boxGroup.HitGroupExport = L"CvBoxGroup";
    boxGroup.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    boxGroup.IntersectionShaderImport = L"CvIntersection";
    boxGroup.ClosestHitShaderImport = L"CvClosestHit";
    // The callable parameter (16 bytes) is bounded by the payload size too.
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{16, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
    ID3D12RootSignature *global = globalRoot_.Get();
    ID3D12RootSignature *local = localRoot_.Get();
    const WCHAR *localExports[] = {L"CvRayGen", L"CvMiss", L"CvCallable", L"CvTriangleGroup", L"CvBoxGroup"};
    std::array<D3D12_STATE_SUBOBJECT, 8> subobjects{};
    subobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library};
    subobjects[1] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &triangleGroup};
    subobjects[2] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &boxGroup};
    subobjects[3] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global};
    subobjects[4] = {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subobjects[4], UINT(std::size(localExports)), localExports};
    subobjects[5] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association};
    subobjects[6] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig};
    subobjects[7] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig};
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(subobjects.size()), subobjects.data()};
    check(device5_->CreateStateObject(&desc, IID_PPV_ARGS(&stateObject_)), "coverage: CreateStateObject");
  }

  void createResources() {
    table_ = createBuffer(device_.Get(), kTableBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
    rays_ = createBuffer(device_.Get(), kLanes * sizeof(LaneRay), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    {
      auto *mapped = static_cast<LaneRay *>(mapUpload(rays_.Get()));
      for (UINT lane = 0; lane < kLanes; ++lane)
        mapped[lane] = laneSpec(lane).ray;
      rays_->Unmap(0, nullptr);
    }
    localData_ = createBuffer(device_.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
    {
      auto *mapped = static_cast<uint32_t *>(mapUpload(localData_.Get()));
      mapped[0] = recordData(kRayGenTable, 0);
      for (UINT i = 0; i < kMissCount; ++i)
        mapped[kLocalMiss + i] = recordData(kMissTable, i);
      for (UINT i = 0; i < kHitCount; ++i)
        mapped[kLocalHit + i] = recordData(kHitTable, i);
      for (UINT i = 0; i < kCallableCount; ++i)
        mapped[kLocalCallable + i] = recordData(kCallableTable, i);
      localData_->Unmap(0, nullptr);
    }
    predicate_ = createBuffer(device_.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
    {
      auto *mapped = static_cast<uint64_t *>(mapUpload(predicate_.Get()));
      mapped[0] = 0;
      mapped[1] = 1;
      predicate_->Unmap(0, nullptr);
    }
    output_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    fill_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    void *fill = mapUpload(fill_.Get());
    std::memset(fill, 0xff, kOutputBytes);
    fill_->Unmap(0, nullptr);
    readback_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGlobalDescriptors + kLocalCount,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)), "coverage: descriptor heap");
    increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kLanes;
    srv.Buffer.StructureByteStride = sizeof(LaneRay);
    device_->CreateShaderResourceView(rays_.Get(), &srv, cpuHandle(1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kOutputWords;
    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device_->CreateUnorderedAccessView(output_.Get(), nullptr, &uav, cpuHandle(2));
    // Each record's local table is one SRV whose only element is that record's data word.
    for (UINT slot = 0; slot < kLocalCount; ++slot) {
      D3D12_SHADER_RESOURCE_VIEW_DESC data{};
      data.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      data.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      data.Buffer.FirstElement = slot;
      data.Buffer.NumElements = 1;
      data.Buffer.StructureByteStride = sizeof(uint32_t);
      device_->CreateShaderResourceView(localData_.Get(), &data, cpuHandle(kGlobalDescriptors + slot));
    }
  }

  void buildScene() {
    // Triangle BLAS: geometry g is the unit triangle at x 2g, z 1; geometry 0 opaque, 1 and 2 not.
    float vertices[27];
    for (UINT g = 0; g < 3; ++g) {
      const float x = 2.0f * g;
      const float tri[9] = {x, 0, 1, x + 1, 0, 1, x, 1, 1};
      std::memcpy(vertices + g * 9, tri, sizeof(tri));
    }
    auto vb = geometry_.upload(vertices, sizeof(vertices));
    D3D12_RAYTRACING_GEOMETRY_DESC tris[3];
    for (UINT g = 0; g < 3; ++g)
      tris[g] = triangles(vb->GetGPUVirtualAddress() + g * 36, 12, 3, DXGI_FORMAT_R32G32B32_FLOAT,
                          g == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE);
    auto triangleBlas = geometry_.build(blasInputs(tris, 3), "coverage: triangle BLAS");
    // AABB BLAS: geometry g is the unit box at object x 2g (CvIntersection tests that same box).
    D3D12_RAYTRACING_AABB boxes[3] = {{0, 0, 0, 1, 1, 1}, {2, 0, 0, 3, 1, 1}, {4, 0, 0, 5, 1, 1}};
    auto ab = geometry_.upload(boxes, sizeof(boxes));
    D3D12_RAYTRACING_GEOMETRY_DESC aabbs[3]{};
    for (UINT g = 0; g < 3; ++g) {
      aabbs[g].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
      aabbs[g].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
      aabbs[g].AABBs.AABBCount = 1;
      aabbs[g].AABBs.AABBs = {ab->GetGPUVirtualAddress() + g * sizeof(D3D12_RAYTRACING_AABB),
                              sizeof(D3D12_RAYTRACING_AABB)};
    }
    auto boxBlas = geometry_.build(blasInputs(aabbs, 3), "coverage: AABB BLAS");
    const Mat identity = translateScale(1, 0, 0, 0);
    const Mat boxPlace = translateScale(1, 0, 4, 2);
    D3D12_RAYTRACING_INSTANCE_DESC instances[2] = {
        contract::instanceDesc(identity.m, kTriangleInstanceId, 0xff, 0, 0, triangleBlas->GetGPUVirtualAddress()),
        contract::instanceDesc(boxPlace.m, kBoxInstanceId, 0xff, kBoxContribution, 0, boxBlas->GetGPUVirtualAddress()),
    };
    auto ib = geometry_.upload(instances, sizeof(instances));
    tlas_ = geometry_.build(tlasInputs(ib->GetGPUVirtualAddress(), 2), "coverage: TLAS");
    D3D12_SHADER_RESOURCE_VIEW_DESC as{};
    as.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    as.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as.RaytracingAccelerationStructure.Location = tlas_->GetGPUVirtualAddress();
    device_->CreateShaderResourceView(nullptr, &as, cpuHandle(0));
  }

  // Record layout (D3D12 local root layout): identifier, root constant at 32, table handle at 40.
  void writeRecord(uint8_t *table, UINT offset, const wchar_t *name, Table kind, UINT index, UINT slot) {
    ComPtr<ID3D12StateObjectProperties> properties;
    check(stateObject_.As(&properties), "coverage: StateObjectProperties");
    const void *id = properties->GetShaderIdentifier(name);
    require(id != nullptr, "coverage: GetShaderIdentifier");
    std::memcpy(table + offset, id, kShaderIdentifierSize);
    const uint32_t tag = recordTag(kind, index);
    std::memcpy(table + offset + 32, &tag, sizeof(tag));
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64(kGlobalDescriptors + slot) * increment_;
    std::memcpy(table + offset + 40, &handle.ptr, sizeof(handle.ptr));
  }

  void writeTable() {
    auto *table = static_cast<uint8_t *>(mapUpload(table_.Get()));
    std::memset(table, 0xee, kTableBytes);
    writeRecord(table, kRayGenOffset, L"CvRayGen", kRayGenTable, 0, 0);
    for (UINT i = 0; i < kMissCount; ++i)
      writeRecord(table, kMissOffset + i * kMissStride, L"CvMiss", kMissTable, i, kLocalMiss + i);
    for (UINT i = 0; i < kHitCount; ++i)
      writeRecord(table, kHitOffset + i * kHitStride, i >= 3 && i <= 5 ? L"CvBoxGroup" : L"CvTriangleGroup",
                  kHitTable, i, kLocalHit + i);
    for (UINT i = 0; i < kCallableCount; ++i)
      writeRecord(table, kCallableOffset + i * kCallableStride, L"CvCallable", kCallableTable, i, kLocalCallable + i);
    table_->Unmap(0, nullptr);
  }

  ID3D12Device5 *device5_;
  Context &ctx_;
  contract::Runner &geometry_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12RootSignature> globalRoot_, localRoot_;
  ComPtr<ID3D12StateObject> stateObject_;
  ComPtr<ID3D12Resource> table_, rays_, localData_, predicate_, output_, fill_, readback_, tlas_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  ComPtr<ID3D12CommandSignature> signature_;
  UINT increment_ = 0;
};

// The ray kinds grouped by the feature they cover, for the per-feature evidence lines.
struct Feature {
  const char *name;
  bool (*covers)(UINT lane);
};
constexpr Feature kFeatures[] = {
    {"geometry-index", [](UINT lane) { return lane % 16 <= 5; }},
    {"hit-group-indexing", [](UINT lane) { return lane % 16 == 6 || lane % 16 == 7; }},
    {"miss-index", [](UINT lane) { return lane % 16 == 8 || lane % 16 == 9; }},
    {"traceray-skip-flags", [](UINT lane) { return lane % 16 >= 10 && lane % 16 <= 12; }},
    {"callable-chains", [](UINT lane) { return lane % 16 == 13 || lane % 16 == 14; }},
    {"rayquery-template-skip", [](UINT lane) { return lane % 16 == 15; }},
};

int caseGridDirect(Suite &suite) {
  auto words = suite.run("coverage grid-direct", [&](ID3D12GraphicsCommandList4 *list) {
    list->SetPipelineState1(suite.stateObject());
    const auto desc = suite.dispatchDesc(kWidth, kHeight, kDepth);
    list->DispatchRays(&desc);
  });
  std::vector<int> laneBad;
  const int bad = suite.verify("grid-direct", words, kWidth, kHeight, kDepth, &laneBad);
  for (const auto &feature : kFeatures) {
    UINT lanes = 0;
    int featureBad = 0;
    for (UINT lane = 0; lane < kLanes; ++lane)
      if (feature.covers(lane))
        ++lanes, featureBad += laneBad[lane];
    std::printf("OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE %s lanes=%u status=%s\n", feature.name, lanes,
                featureBad ? "FAIL" : "PASS");
  }
  return reportCase("grid-direct", bad);
}

int caseGridIndirect(Suite &suite) {
  auto arguments = suite.indirectArguments(kWidth, kHeight, kDepth);
  auto words = suite.run("coverage grid-indirect", [&](ID3D12GraphicsCommandList4 *list) {
    list->SetPipelineState1(suite.stateObject());
    list->ExecuteIndirect(suite.signature(), 1, arguments.Get(), Suite::kIndirectOffset, nullptr, 0);
  });
  return reportCase("grid-indirect", suite.verify("grid-indirect", words, kWidth, kHeight, kDepth));
}

// D3D12 culls a predicated operation when the snapshot word matches the op: EQUAL_ZERO culls on 0,
// NOT_EQUAL_ZERO culls on non-zero.
bool culled(D3D12_PREDICATION_OP op, uint64_t value) {
  return op == D3D12_PREDICATION_OP_EQUAL_ZERO ? value == 0 : value != 0;
}

int casePredication(Suite &suite) {
  constexpr UINT w = 4, h = 2, d = 2;
  auto arguments = suite.indirectArguments(w, h, d);
  int bad = 0;
  for (const auto op : {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}) {
    for (const uint64_t value : {uint64_t(0), uint64_t(1)}) {
      for (const bool indirect : {false, true}) {
        char name[96];
        std::snprintf(name, sizeof(name), "predication-%s-%s-%u", indirect ? "indirect" : "direct",
                      op == D3D12_PREDICATION_OP_EQUAL_ZERO ? "eq0" : "ne0", unsigned(value));
        auto words = suite.run(name, [&](ID3D12GraphicsCommandList4 *list) {
          list->SetPipelineState1(suite.stateObject());
          list->SetPredication(suite.predicate(), value * 8, op);
          if (indirect) {
            list->ExecuteIndirect(suite.signature(), 1, arguments.Get(), Suite::kIndirectOffset, nullptr, 0);
          } else {
            const auto desc = suite.dispatchDesc(w, h, d);
            list->DispatchRays(&desc);
          }
          list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
        const bool skip = culled(op, value);
        const int caseBad = suite.verify(name, words, skip ? 0 : w, h, d);
        std::printf("OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION %s culled=%d status=%s\n", name, skip ? 1 : 0,
                    caseBad ? "FAIL" : "PASS");
        bad += caseBad;
      }
    }
  }
  return reportCase("predication", bad);
}

int caseBundle(Suite &suite) {
  constexpr UINT w = 4, h = 2, d = 2;
  auto fullArguments = suite.indirectArguments(kWidth, kHeight, kDepth);
  ComPtr<ID3D12CommandAllocator> allocator;
  check(suite.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&allocator)),
        "coverage: bundle allocator");
  auto record = [&](auto &&body) {
    ComPtr<ID3D12GraphicsCommandList> base;
    check(suite.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&base)),
          "coverage: bundle list");
    ComPtr<ID3D12GraphicsCommandList4> bundle;
    check(base.As(&bundle), "coverage: bundle list4");
    ID3D12DescriptorHeap *heaps[] = {suite.heap()};
    bundle->SetDescriptorHeaps(1, heaps);
    bundle->SetPipelineState1(suite.stateObject());
    body(bundle.Get());
    check(bundle->Close(), "coverage: bundle Close");
    return bundle;
  };
  auto indirectBundle = record([&](ID3D12GraphicsCommandList4 *bundle) {
    bundle->ExecuteIndirect(suite.signature(), 1, fullArguments.Get(), Suite::kIndirectOffset, nullptr, 0);
  });
  auto directBundle = record([&](ID3D12GraphicsCommandList4 *bundle) {
    const auto desc = suite.dispatchDesc(w, h, d);
    bundle->DispatchRays(&desc);
  });

  int bad = 0;
  auto execute = [&](const char *name, ID3D12GraphicsCommandList4 *bundle, bool predicated,
                     D3D12_PREDICATION_OP op, uint64_t value, UINT gw, UINT gh, UINT gd) {
    auto words = suite.run(name, [&](ID3D12GraphicsCommandList4 *list) {
      if (predicated)
        list->SetPredication(suite.predicate(), value * 8, op);
      list->ExecuteBundle(bundle);
      if (predicated)
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    });
    const bool skip = predicated && culled(op, value);
    const int caseBad = suite.verify(name, words, skip ? 0 : gw, gh, gd);
    std::printf("OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE %s culled=%d status=%s\n", name, skip ? 1 : 0,
                caseBad ? "FAIL" : "PASS");
    bad += caseBad;
  };
  execute("bundle-indirect", indirectBundle.Get(), false, D3D12_PREDICATION_OP_EQUAL_ZERO, 0, kWidth, kHeight, kDepth);
  execute("bundle-indirect-predicated-culled", indirectBundle.Get(), true, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO, 1,
          kWidth, kHeight, kDepth);
  execute("bundle-indirect-predicated-run", indirectBundle.Get(), true, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO, 0,
          kWidth, kHeight, kDepth);
  execute("bundle-direct-predicated-culled", directBundle.Get(), true, D3D12_PREDICATION_OP_EQUAL_ZERO, 0, w, h, d);
  execute("bundle-direct-predicated-run", directBundle.Get(), true, D3D12_PREDICATION_OP_EQUAL_ZERO, 1, w, h, d);
  return reportCase("bundle", bad);
}

int runAll(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil, contract::Runner &geometry) {
  Suite suite(device5, ctx, dxil, geometry);
  int failures = 0;
  failures += caseGridDirect(suite);
  failures += caseGridIndirect(suite);
  failures += casePredication(suite);
  failures += caseBundle(suite);
  std::printf("OWNED_D3D12_RAYTRACING_COVERAGE failures=%d\n", failures);
  std::fflush(stdout);
  return failures;
}

} // namespace coverage
