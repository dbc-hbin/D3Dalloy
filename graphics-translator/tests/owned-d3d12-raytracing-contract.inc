// Contract cases of owned-d3d12-raytracing.cpp, driven by tests/dxil/raytracing-contract.hlsl.
// Included by owned-d3d12-raytracing.cpp after its helpers (Context, createBuffer, check, ...).
//
// Every case builds real acceleration structures, traces one ray per lane through one generic
// ray generation shader, and asserts the exact system values the hit/miss shaders observed:
// a case passes only on a fence-completed readback of the expected words, never because a call
// returned S_OK.

namespace contract {

constexpr UINT kSlotWords = 64;
constexpr UINT kMaxLanes = 48;
constexpr UINT kIntersectionLogWord = 3072;
constexpr UINT kOutputWords = 4096;
constexpr UINT kOutputBytes = kOutputWords * sizeof(uint32_t);
constexpr uint32_t kFill = 0xffffffffu;

constexpr uint32_t kKindTriangle = 1;
constexpr uint32_t kKindMiss = 2;
constexpr uint32_t kKindProcedural = 3;
constexpr uint32_t kHitKindFront = 0xfe;
constexpr uint32_t kHitKindBack = 0xff;
constexpr uint32_t kProceduralHitKind = 5;

// Shader table: record 0 raygen, misses at 64 (two records), hit groups at 256 (stride 64).
constexpr UINT kRecordStride = 64;
constexpr UINT kRayGenOffset = 0;
constexpr UINT kMissOffset = 64;
constexpr UINT kMissCount = 2;
constexpr UINT kHitGroupOffset = 256;
constexpr UINT kHitGroupCount = 8;
constexpr UINT kTableBytes = kHitGroupOffset + kHitGroupCount * kRecordStride;
// Hit-group record r carries local tag 0x1000 + r; miss record m carries 0x2000 + m.
constexpr uint32_t kHitGroupTagBase = 0x1000;
constexpr uint32_t kMissTagBase = 0x2000;

struct LaneRay {
  float origin[3];
  float tmin;
  float direction[3];
  float tmax;
  uint32_t flags;
  uint32_t mask;
  uint32_t contribution;
  uint32_t multiplier;
  uint32_t missIndex;
  uint32_t scene;
  uint32_t pad[2];
};
static_assert(sizeof(LaneRay) == 64);

// Expected values of one lane. A NaN-free exact comparison is used throughout: every geometry and
// transform below is chosen so that the reference values are exactly representable.
struct Expect {
  uint32_t kind = kKindMiss;
  uint32_t primitive = 0, instanceId = 0, instanceIndex = 0, geometry = 0, hitKind = 0;
  float t = 0;
  uint32_t rayFlags = 0;
  float objectOrigin[3]{}, objectDirection[3]{};
  float objectToWorld[3][4]{}, worldToObject[3][4]{};
  float attributes[2]{};
  uint32_t localTag = 0;
  uint32_t anyHits = 0;
  bool checkTransforms = false;
  bool checkAttributes = true;
};

void identity(float m[3][4]) {
  std::memset(m, 0, sizeof(float) * 12);
  m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

D3D12_RAYTRACING_INSTANCE_DESC instanceDesc(const float m[3][4], UINT id, UINT mask, UINT contribution,
                                            UINT flags, D3D12_GPU_VIRTUAL_ADDRESS blas) {
  D3D12_RAYTRACING_INSTANCE_DESC desc{};
  std::memcpy(desc.Transform, m, sizeof(desc.Transform));
  desc.InstanceID = id;
  desc.InstanceMask = mask;
  desc.InstanceContributionToHitGroupIndex = contribution;
  desc.Flags = flags;
  desc.AccelerationStructure = blas;
  return desc;
}

LaneRay ray(float ox, float oy, float oz, float dx, float dy, float dz, uint32_t flags = 0,
            uint32_t mask = 0xff, uint32_t contribution = 0, uint32_t multiplier = 1, uint32_t miss = 0,
            float tmin = 0.0f, float tmax = 100.0f) {
  LaneRay r{};
  r.origin[0] = ox, r.origin[1] = oy, r.origin[2] = oz;
  r.direction[0] = dx, r.direction[1] = dy, r.direction[2] = dz;
  r.tmin = tmin, r.tmax = tmax;
  r.flags = flags, r.mask = mask, r.contribution = contribution, r.multiplier = multiplier, r.missIndex = miss;
  return r;
}

uint32_t bits(float value) {
  uint32_t word;
  std::memcpy(&word, &value, sizeof(word));
  return word;
}

class Runner {
public:
  Runner(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil)
      : device5_(device5), ctx_(ctx) {
    check(device5->QueryInterface(IID_PPV_ARGS(&device_)), "contract: QueryInterface(ID3D12Device)");
    createRootSignatures();
    createStateObject(dxil);
    createCommonResources();
  }

  ID3D12Device *device() const { return device_.Get(); }
  ID3D12Device5 *device5() const { return device5_; }
  Context &ctx() { return ctx_; }
  ID3D12StateObject *stateObject() const { return stateObject_.Get(); }
  ID3D12RootSignature *globalRoot() const { return globalRoot_.Get(); }
  ID3D12RootSignature *localRoot() const { return localRoot_.Get(); }

  // A buffer in the default heap whose contents are copied from `data` through an upload buffer
  // on the next recorded list.
  ComPtr<ID3D12Resource> upload(const void *data, UINT64 size) {
    auto buffer = createBuffer(device_.Get(), std::max<UINT64>(size, 256), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = mapUpload(buffer.Get());
    std::memcpy(mapped, data, size);
    buffer->Unmap(0, nullptr);
    keep_.push_back(buffer);
    return buffer;
  }

  ComPtr<ID3D12Resource> asBuffer(UINT64 size) {
    auto buffer = createBuffer(device_.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    keep_.push_back(buffer);
    return buffer;
  }

  ComPtr<ID3D12Resource> uavBuffer(UINT64 size) {
    auto buffer = createBuffer(device_.Get(), std::max<UINT64>(size, 256), D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    keep_.push_back(buffer);
    return buffer;
  }

  // Records a build of `inputs` into a fresh destination (sized from prebuild info) and returns it.
  ComPtr<ID3D12Resource> build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
                               const char *step, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info_out = nullptr) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    require(info.ResultDataMaxSizeInBytes > 0, step);
    if (info_out)
      *info_out = info;
    auto destination = asBuffer(info.ResultDataMaxSizeInBytes);
    auto scratch = uavBuffer(std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = inputs;
    desc.DestAccelerationStructureData = destination->GetGPUVirtualAddress();
    desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    ctx_.list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    uavBarrier(ctx_.list.Get(), destination.Get());
    lastScratch_ = scratch;
    return destination;
  }

  ID3D12Resource *lastScratch() const { return lastScratch_.Get(); }

  // Trace `rays` against `scene` (and EmptyScene / NullScene) and return the readback words.
  // `stateObject` overrides the runner's own pipeline (it must accept the same global root
  // signature and shader-table layout).
  std::vector<uint32_t> trace(const char *name, D3D12_GPU_VIRTUAL_ADDRESS scene, const std::vector<LaneRay> &rays,
                              D3D12_GPU_VIRTUAL_ADDRESS emptyScene = 0, ID3D12StateObject *stateObject = nullptr) {
    require(rays.size() <= kMaxLanes, "contract: too many lanes");
    std::memcpy(raysMapped_, rays.data(), rays.size() * sizeof(LaneRay));
    writeDescriptors(scene, emptyScene);
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    ctx_.list->SetDescriptorHeaps(1, heaps);
    ctx_.list->SetComputeRootSignature(globalRoot_.Get());
    ctx_.list->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
    ctx_.list->CopyBufferRegion(output_.Get(), 0, fill_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx_.list->SetPipelineState1(stateObject ? stateObject : stateObject_.Get());
    D3D12_DISPATCH_RAYS_DESC dispatch = dispatchDesc(static_cast<UINT>(rays.size()));
    ctx_.list->DispatchRays(&dispatch);
    uavBarrier(ctx_.list.Get(), output_.Get());
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx_.list->CopyBufferRegion(readback_.Get(), 0, output_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    submit(name);
    return readback();
  }

  D3D12_DISPATCH_RAYS_DESC dispatchDesc(UINT width) const {
    const D3D12_GPU_VIRTUAL_ADDRESS table = table_->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord = {table + kRayGenOffset, kShaderIdentifierSize};
    dispatch.MissShaderTable = {table + kMissOffset, kRecordStride * kMissCount, kRecordStride};
    dispatch.HitGroupTable = {table + kHitGroupOffset, kRecordStride * kHitGroupCount, kRecordStride};
    dispatch.Width = width;
    dispatch.Height = 1;
    dispatch.Depth = 1;
    return dispatch;
  }

  void prepareDispatch(D3D12_GPU_VIRTUAL_ADDRESS scene, const std::vector<LaneRay> &rays) {
    std::memcpy(raysMapped_, rays.data(), rays.size() * sizeof(LaneRay));
    writeDescriptors(scene, 0);
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    ctx_.list->SetDescriptorHeaps(1, heaps);
    ctx_.list->SetComputeRootSignature(globalRoot_.Get());
    ctx_.list->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
    ctx_.list->CopyBufferRegion(output_.Get(), 0, fill_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  std::vector<uint32_t> finishDispatch(const char *name) {
    uavBarrier(ctx_.list.Get(), output_.Get());
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx_.list->CopyBufferRegion(readback_.Get(), 0, output_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    submit(name);
    return readback();
  }

  void submit(const char *name) {
    const HRESULT hr = ctx_.list->Close();
    if (FAILED(hr)) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_CASE_FAIL case=%s close=0x%08lx\n", name,
                   static_cast<unsigned long>(hr));
      ExitProcess(1);
    }
    ID3D12CommandList *lists[] = {ctx_.list.Get()};
    ctx_.queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++ctx_.fenceValue;
    check(ctx_.queue->Signal(ctx_.fence.Get(), value), "contract: Signal");
    if (ctx_.fence->GetCompletedValue() < value) {
      check(ctx_.fence->SetEventOnCompletion(value, ctx_.event), "contract: SetEventOnCompletion");
      require(WaitForSingleObject(ctx_.event, kFenceTimeoutMs) == WAIT_OBJECT_0, "contract: fence timeout");
    }
    check(ctx_.allocator->Reset(), "contract: allocator Reset");
    check(ctx_.list->Reset(ctx_.allocator.Get(), nullptr), "contract: list Reset");
  }

  std::vector<uint32_t> readback() {
    void *mapped = nullptr;
    D3D12_RANGE range{0, kOutputBytes};
    check(readback_->Map(0, &range, &mapped), "contract: Map(readback)");
    std::vector<uint32_t> words(kOutputWords);
    std::memcpy(words.data(), mapped, kOutputBytes);
    D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    return words;
  }

  // Compare one lane against its expectation. Returns the number of mismatching words.
  int expect(const char *name, const std::vector<uint32_t> &words, UINT lane, const Expect &e) {
    const uint32_t *slot = words.data() + lane * kSlotWords;
    int bad = 0;
    auto word = [&](UINT index, uint32_t expected, const char *what) {
      if (slot[index] != expected) {
        std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_CASE_MISMATCH case=%s lane=%u field=%s word=%u got=0x%08x want=0x%08x\n",
                     name, lane, what, index, slot[index], expected);
        ++bad;
      }
    };
    word(0, e.kind, "kind");
    word(62, 0x100u | e.kind, "payload_kind");
    word(47, e.localTag, "local_tag");
    word(7, e.rayFlags, "ray_flags");
    if (e.kind == kKindMiss) {
      word(1, kFill, "untouched_primitive");
      return bad;
    }
    word(1, e.primitive, "primitive_index");
    word(2, e.instanceId, "instance_id");
    word(3, e.instanceIndex, "instance_index");
    word(4, e.geometry, "geometry_index");
    word(5, e.hitKind, "hit_kind");
    word(6, bits(e.t), "ray_t_current");
    // The counter is InterlockedAdd'ed onto the kFill pre-fill, so N invocations read kFill + N.
    word(48, kFill + e.anyHits, "any_hit_count");
    if (e.checkAttributes) {
      word(45, bits(e.attributes[0]), "attribute_0");
      word(46, bits(e.attributes[1]), "attribute_1");
    }
    if (e.checkTransforms) {
      for (UINT i = 0; i < 3; ++i) {
        word(15 + i, bits(e.objectOrigin[i]), "object_ray_origin");
        word(18 + i, bits(e.objectDirection[i]), "object_ray_direction");
      }
      for (UINT r = 0; r < 3; ++r)
        for (UINT c = 0; c < 4; ++c) {
          word(21 + r * 4 + c, bits(e.objectToWorld[r][c]), "object_to_world_3x4");
          word(33 + r * 4 + c, bits(e.worldToObject[r][c]), "world_to_object_3x4");
          word(49 + c * 3 + r, bits(e.objectToWorld[r][c]), "object_to_world_4x3");
        }
    }
    return bad;
  }

  int reportCase(const char *name, int bad) {
    std::printf("OWNED_D3D12_RAYTRACING_CASE %s status=%s\n", name, bad ? "FAIL" : "PASS");
    std::fflush(stdout);
    return bad ? 1 : 0;
  }

  const void *identifier(const wchar_t *name) {
    ComPtr<ID3D12StateObjectProperties> properties;
    check(stateObject_.As(&properties), "contract: StateObjectProperties");
    return properties->GetShaderIdentifier(name);
  }

  ID3D12Resource *shaderTable() const { return table_.Get(); }

  void writeHitGroupRecord(UINT index, const void *identifier) {
    std::memcpy(tableMapped_ + kHitGroupOffset + index * kRecordStride, identifier, kShaderIdentifierSize);
  }

  void writeMissRecord(UINT index, const void *identifier) {
    std::memcpy(tableMapped_ + kMissOffset + index * kRecordStride, identifier, kShaderIdentifierSize);
  }

  void writeRayGenRecord(const void *identifier) {
    std::memcpy(tableMapped_ + kRayGenOffset, identifier, kShaderIdentifierSize);
  }

  // Restores the default table: record r -> group r % 2 ("ContractTriangles"/"ContractProcedural").
  void writeDefaultTable() {
    writeRayGenRecord(identifier(L"ContractRayGen"));
    for (UINT m = 0; m < kMissCount; ++m) {
      writeMissRecord(m, identifier(L"ContractMiss"));
      const uint32_t tag = kMissTagBase + m;
      std::memcpy(tableMapped_ + kMissOffset + m * kRecordStride + kShaderIdentifierSize, &tag, sizeof(tag));
    }
    for (UINT r = 0; r < kHitGroupCount; ++r) {
      writeHitGroupRecord(r, identifier(r % 2 ? L"ContractProcedural" : L"ContractTriangles"));
      const uint32_t tag = kHitGroupTagBase + r;
      std::memcpy(tableMapped_ + kHitGroupOffset + r * kRecordStride + kShaderIdentifierSize, &tag, sizeof(tag));
    }
  }

private:
  void createRootSignatures() {
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 4;
    D3D12_ROOT_PARAMETER1 global{};
    global.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    global.DescriptorTable.NumDescriptorRanges = 2;
    global.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC1 globalDesc{};
    globalDesc.NumParameters = 1;
    globalDesc.pParameters = &global;
    globalRoot_ = createRootSignature(device_.Get(), globalDesc, "contract: global root signature");

    D3D12_ROOT_PARAMETER1 local{};
    local.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    local.Constants.ShaderRegister = 0;
    local.Constants.RegisterSpace = 1;
    local.Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC1 localDesc{};
    localDesc.NumParameters = 1;
    localDesc.pParameters = &local;
    localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    localRoot_ = createRootSignature(device_.Get(), localDesc, "contract: local root signature");
  }

  void createStateObject(const std::vector<uint8_t> &dxil) {
    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary = {dxil.data(), dxil.size()};
    D3D12_HIT_GROUP_DESC triangles{};
    triangles.HitGroupExport = L"ContractTriangles";
    triangles.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    triangles.AnyHitShaderImport = L"ContractAnyHit";
    triangles.ClosestHitShaderImport = L"ContractTriangleClosestHit";
    D3D12_HIT_GROUP_DESC procedural{};
    procedural.HitGroupExport = L"ContractProcedural";
    procedural.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    procedural.IntersectionShaderImport = L"ContractIntersection";
    procedural.ClosestHitShaderImport = L"ContractProceduralClosestHit";
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{8, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
    ID3D12RootSignature *global = globalRoot_.Get();
    ID3D12RootSignature *local = localRoot_.Get();
    const WCHAR *localExports[] = {L"ContractTriangles", L"ContractProcedural", L"ContractMiss"};

    std::array<D3D12_STATE_SUBOBJECT, 8> subobjects{};
    subobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library};
    subobjects[1] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &triangles};
    subobjects[2] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &procedural};
    subobjects[3] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global};
    subobjects[4] = {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subobjects[4], 3, localExports};
    subobjects[5] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association};
    subobjects[6] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig};
    subobjects[7] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig};
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(subobjects.size()), subobjects.data()};
    check(device5_->CreateStateObject(&desc, IID_PPV_ARGS(&stateObject_)), "contract: CreateStateObject");
  }

  void createCommonResources() {
    table_ = createBuffer(device_.Get(), kTableBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
    tableMapped_ = static_cast<uint8_t *>(mapUpload(table_.Get()));
    std::memset(tableMapped_, 0, kTableBytes);
    writeDefaultTable();

    rays_ = createBuffer(device_.Get(), kMaxLanes * sizeof(LaneRay), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    raysMapped_ = mapUpload(rays_.Get());

    output_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    fill_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    void *fill = mapUpload(fill_.Get());
    std::memset(fill, 0xff, kOutputBytes);
    fill_->Unmap(0, nullptr);
    readback_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)), "contract: descriptor heap");
    increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE handle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * increment_;
    return h;
  }

  void writeDescriptors(D3D12_GPU_VIRTUAL_ADDRESS scene, D3D12_GPU_VIRTUAL_ADDRESS emptyScene) {
    D3D12_SHADER_RESOURCE_VIEW_DESC as{};
    as.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    as.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as.RaytracingAccelerationStructure.Location = scene;
    device_->CreateShaderResourceView(nullptr, &as, handle(0));
    D3D12_SHADER_RESOURCE_VIEW_DESC rays{};
    rays.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rays.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rays.Buffer.NumElements = kMaxLanes;
    rays.Buffer.StructureByteStride = sizeof(LaneRay);
    device_->CreateShaderResourceView(rays_.Get(), &rays, handle(1));
    as.RaytracingAccelerationStructure.Location = emptyScene;
    device_->CreateShaderResourceView(nullptr, &as, handle(2));
    as.RaytracingAccelerationStructure.Location = 0;
    device_->CreateShaderResourceView(nullptr, &as, handle(3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kOutputWords;
    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    device_->CreateUnorderedAccessView(output_.Get(), nullptr, &uav, handle(4));
  }

  ID3D12Device5 *device5_;
  Context &ctx_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12RootSignature> globalRoot_, localRoot_;
  ComPtr<ID3D12StateObject> stateObject_;
  ComPtr<ID3D12Resource> table_, rays_, output_, fill_, readback_, lastScratch_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  UINT increment_ = 0;
  uint8_t *tableMapped_ = nullptr;
  void *raysMapped_ = nullptr;
  std::vector<ComPtr<ID3D12Resource>> keep_;
};

} // namespace contract

namespace contract {

// ---- Geometry helpers -----------------------------------------------------------------------

uint16_t halfBits(float value) {
  // Exact for the 0 / 1 / -1 values the fixture encodes.
  if (value == 0.0f)
    return 0x0000;
  if (value == 1.0f)
    return 0x3c00;
  if (value == -1.0f)
    return 0xbc00;
  require(false, "contract: halfBits value");
  return 0;
}

// One vertex format case: DXGI format, stride, and an encoder writing the (x, y, z) position
// (components outside the format are not written, and the padding the stride leaves is filled
// with a garbage pattern that D3D12 requires the build to ignore).
struct FormatCase {
  DXGI_FORMAT format;
  UINT stride;
  bool threeComponents;
  void (*encode)(uint8_t *out, float x, float y, float z);
  const char *name;
};

void encodeF32x3(uint8_t *o, float x, float y, float z) { float v[3] = {x, y, z}; std::memcpy(o, v, 12); }
void encodeF32x2(uint8_t *o, float x, float y, float) { float v[2] = {x, y}; std::memcpy(o, v, 8); }
void encodeF16x2(uint8_t *o, float x, float y, float) { uint16_t v[2] = {halfBits(x), halfBits(y)}; std::memcpy(o, v, 4); }
void encodeF16x4(uint8_t *o, float x, float y, float z) {
  uint16_t v[3] = {halfBits(x), halfBits(y), halfBits(z)};
  std::memcpy(o, v, 6);
}
template <typename T, int Scale> void encodeNorm2(uint8_t *o, float x, float y, float) {
  T v[2] = {T(x * Scale), T(y * Scale)};
  std::memcpy(o, v, sizeof(v));
}
template <typename T, int Scale> void encodeNorm3(uint8_t *o, float x, float y, float z) {
  T v[3] = {T(x * Scale), T(y * Scale), T(z * Scale)};
  std::memcpy(o, v, sizeof(v));
}
void encodeUnorm1010102(uint8_t *o, float x, float y, float z) {
  // The A2 bits are set: D3D12 ignores them.
  const uint32_t w = uint32_t(x * 1023) | uint32_t(y * 1023) << 10 | uint32_t(z * 1023) << 20 | 3u << 30;
  std::memcpy(o, &w, 4);
}

// Tier 1.0 formats first, then the Tier 1.1 additions.
const FormatCase kFormatCases[] = {
    {DXGI_FORMAT_R32G32B32_FLOAT, 12, true, encodeF32x3, "r32g32b32_float"},
    {DXGI_FORMAT_R32G32_FLOAT, 8, false, encodeF32x2, "r32g32_float"},
    {DXGI_FORMAT_R16G16_FLOAT, 4, false, encodeF16x2, "r16g16_float"},
    // The A16 component is ignored, so a 6-byte stride packs vertices without it.
    {DXGI_FORMAT_R16G16B16A16_FLOAT, 6, true, encodeF16x4, "r16g16b16a16_float"},
    {DXGI_FORMAT_R16G16_SNORM, 4, false, encodeNorm2<int16_t, 32767>, "r16g16_snorm"},
    {DXGI_FORMAT_R16G16B16A16_SNORM, 8, true, encodeNorm3<int16_t, 32767>, "r16g16b16a16_snorm"},
    {DXGI_FORMAT_R16G16B16A16_UNORM, 8, true, encodeNorm3<uint16_t, 65535>, "r16g16b16a16_unorm"},
    {DXGI_FORMAT_R16G16_UNORM, 4, false, encodeNorm2<uint16_t, 65535>, "r16g16_unorm"},
    {DXGI_FORMAT_R10G10B10A2_UNORM, 4, true, encodeUnorm1010102, "r10g10b10a2_unorm"},
    // The A8 component is ignored, so a 3-byte stride packs vertices without it.
    {DXGI_FORMAT_R8G8B8A8_UNORM, 3, true, encodeNorm3<uint8_t, 255>, "r8g8b8a8_unorm"},
    {DXGI_FORMAT_R8G8_UNORM, 2, false, encodeNorm2<uint8_t, 255>, "r8g8_unorm"},
    {DXGI_FORMAT_R8G8B8A8_SNORM, 4, true, encodeNorm3<int8_t, 127>, "r8g8b8a8_snorm"},
    {DXGI_FORMAT_R8G8_SNORM, 2, false, encodeNorm2<int8_t, 127>, "r8g8_snorm"},
};
constexpr UINT kFormatCount = UINT(std::size(kFormatCases));

struct Mat {
  float m[3][4];
};

Mat translateScale(float scale, float tx, float ty, float tz) {
  Mat r{};
  r.m[0][0] = r.m[1][1] = r.m[2][2] = scale;
  r.m[0][3] = tx, r.m[1][3] = ty, r.m[2][3] = tz;
  return r;
}

// Inverse of translateScale.
Mat inverseTranslateScale(float scale, float tx, float ty, float tz) {
  return translateScale(1.0f / scale, -tx / scale, -ty / scale, -tz / scale);
}

D3D12_RAYTRACING_GEOMETRY_DESC triangles(D3D12_GPU_VIRTUAL_ADDRESS vertices, UINT stride, UINT count,
                                         DXGI_FORMAT format, D3D12_RAYTRACING_GEOMETRY_FLAGS flags,
                                         D3D12_GPU_VIRTUAL_ADDRESS transform = 0,
                                         D3D12_GPU_VIRTUAL_ADDRESS indices = 0, UINT indexCount = 0,
                                         DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN) {
  D3D12_RAYTRACING_GEOMETRY_DESC g{};
  g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  g.Flags = flags;
  g.Triangles.Transform3x4 = transform;
  g.Triangles.VertexFormat = format;
  g.Triangles.VertexCount = count;
  g.Triangles.VertexBuffer = {vertices, stride};
  g.Triangles.IndexBuffer = indices;
  g.Triangles.IndexCount = indexCount;
  g.Triangles.IndexFormat = indexFormat;
  return g;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs(const D3D12_RAYTRACING_GEOMETRY_DESC *geometries,
                                                                UINT count,
                                                                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags =
                                                                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) {
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  inputs.Flags = flags;
  inputs.NumDescs = count;
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.pGeometryDescs = geometries;
  return inputs;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs(D3D12_GPU_VIRTUAL_ADDRESS instances, UINT count,
                                                                D3D12_ELEMENTS_LAYOUT layout = D3D12_ELEMENTS_LAYOUT_ARRAY,
                                                                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags =
                                                                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) {
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  inputs.Flags = flags;
  inputs.NumDescs = count;
  inputs.DescsLayout = layout;
  inputs.InstanceDescs = instances;
  return inputs;
}

// The unit triangle (0,0,z) (1,0,z) (0,1,z), as float3.
std::vector<float> unitTriangle(float z) { return {0, 0, z, 1, 0, z, 0, 1, z}; }

Expect triangleHit(float t, float u, float v, uint32_t tag, uint32_t instanceId, uint32_t instanceIndex,
                   uint32_t geometry = 0, uint32_t primitive = 0) {
  Expect e;
  e.kind = kKindTriangle;
  e.t = t;
  e.attributes[0] = u, e.attributes[1] = v;
  e.localTag = tag;
  e.instanceId = instanceId;
  e.instanceIndex = instanceIndex;
  e.geometry = geometry;
  e.primitive = primitive;
  // Seen from -z travelling +z, the unit triangle's vertices run counter-clockwise, so D3D12's
  // default (clockwise = front) reports it back facing.
  e.hitKind = kHitKindBack;
  return e;
}

Expect miss(uint32_t missIndex = 0, uint32_t flags = 0) {
  Expect e;
  e.kind = kKindMiss;
  e.localTag = kMissTagBase + missIndex;
  e.rayFlags = flags;
  return e;
}

void setTransforms(Expect &e, const Mat &objectToWorld, const Mat &worldToObject, const float origin[3],
                   const float direction[3]) {
  e.checkTransforms = true;
  std::memcpy(e.objectToWorld, objectToWorld.m, sizeof(e.objectToWorld));
  std::memcpy(e.worldToObject, worldToObject.m, sizeof(e.worldToObject));
  std::memcpy(e.objectOrigin, origin, sizeof(e.objectOrigin));
  std::memcpy(e.objectDirection, direction, sizeof(e.objectDirection));
}

// ---- Cases ------------------------------------------------------------------------------------

// Every D3D12 vertex format (Tier 1.0 and the Tier 1.1 additions), one geometry each, displaced by
// a per-geometry Transform3x4 (row-major, translation in column 3). Geometry i sits at x = 4i, so
// lane i's ray enters exactly geometry i and GeometryIndex, the reported distance (z = 1 for the
// three-component formats, 0 for the two-component ones whose z is defined as 0) and the
// barycentrics are all exact.
int caseVertexFormats(Runner &run) {
  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
  std::vector<Mat> transforms;
  std::vector<LaneRay> rays;
  std::vector<Expect> expects;
  std::vector<ComPtr<ID3D12Resource>> buffers;
  for (UINT i = 0; i < kFormatCount; ++i)
    transforms.push_back(translateScale(1.0f, 4.0f * i, 0.0f, 0.0f));
  auto transformBuffer = run.upload(transforms.data(), transforms.size() * sizeof(Mat));
  for (UINT i = 0; i < kFormatCount; ++i) {
    const auto &format = kFormatCases[i];
    std::vector<uint8_t> bytes(format.stride * 3, 0xcd);
    const float z = format.threeComponents ? 1.0f : 0.0f;
    format.encode(bytes.data() + 0 * format.stride, 0, 0, z);
    format.encode(bytes.data() + 1 * format.stride, 1, 0, z);
    format.encode(bytes.data() + 2 * format.stride, 0, 1, z);
    auto vertices = run.upload(bytes.data(), bytes.size());
    buffers.push_back(vertices);
    geometries.push_back(triangles(vertices->GetGPUVirtualAddress(), format.stride, 3, format.format,
                                   D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,
                                   transformBuffer->GetGPUVirtualAddress() + i * sizeof(Mat)));
    // Multiplier 0: every geometry selects hit-group record 0.
    rays.push_back(ray(4.0f * i + 0.25f, 0.25f, -1.0f, 0, 0, 1, 0, 0xff, 0, 0));
    expects.push_back(triangleHit(z + 1.0f, 0.25f, 0.25f, kHitGroupTagBase, 0, 0, i));
  }
  auto blas = run.build(blasInputs(geometries.data(), kFormatCount), "contract: formats BLAS");
  Mat identityMatrix = translateScale(1, 0, 0, 0);
  auto instance = instanceDesc(identityMatrix.m, 0, 0xff, 0, 0, blas->GetGPUVirtualAddress());
  auto instances = run.upload(&instance, sizeof(instance));
  auto tlas = run.build(tlasInputs(instances->GetGPUVirtualAddress(), 1), "contract: formats TLAS");
  auto words = run.trace("vertex-formats", tlas->GetGPUVirtualAddress(), rays);
  int bad = 0;
  for (UINT i = 0; i < kFormatCount; ++i) {
    const int lane_bad = run.expect(kFormatCases[i].name, words, i, expects[i]);
    std::printf("OWNED_D3D12_RAYTRACING_FORMAT %s status=%s\n", kFormatCases[i].name, lane_bad ? "FAIL" : "PASS");
    bad += lane_bad;
  }
  return run.reportCase("vertex-formats", bad);
}

// An asymmetric row-major geometry transform with translation must intersect exactly like
// independently baked vertices. The off-diagonal terms distinguish a transpose/layout error
// from an identity, diagonal-scale, or translation-only implementation.
int caseGeometryTransformLayout(Runner &run) {
  // Determinant 2: both the transform and its inverse have exactly representable values.
  const Mat transform{{{2, 0, 0, 4}, {0.5f, 1, 0, -2}, {0.25f, 0.5f, 1, 3}}};
  const float original[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const float baked[] = {4, -2, 3, 6, -1.5f, 3.25f, 4, -1, 3.5f};
  auto matrix = run.upload(&transform, sizeof(transform));
  auto originalVertices = run.upload(original, sizeof(original));
  auto bakedVertices = run.upload(baked, sizeof(baked));
  const auto transformedGeometry = triangles(originalVertices->GetGPUVirtualAddress(), 12, 3,
      DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE, matrix->GetGPUVirtualAddress());
  const auto bakedGeometry = triangles(bakedVertices->GetGPUVirtualAddress(), 12, 3,
      DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
  auto transformedBlas = run.build(blasInputs(&transformedGeometry, 1), "contract: row-major geometry BLAS");
  auto bakedBlas = run.build(blasInputs(&bakedGeometry, 1), "contract: baked geometry BLAS");
  const Mat identityMatrix = translateScale(1, 0, 0, 0);
  const D3D12_RAYTRACING_INSTANCE_DESC instances[] = {
      instanceDesc(identityMatrix.m, 0x31, 1, 0, 0, transformedBlas->GetGPUVirtualAddress()),
      instanceDesc(identityMatrix.m, 0x32, 2, 0, 0, bakedBlas->GetGPUVirtualAddress()),
  };
  auto instanceBuffer = run.upload(instances, sizeof(instances));
  auto tlas = run.build(tlasInputs(instanceBuffer->GetGPUVirtualAddress(), 2), "contract: geometry layout TLAS");
  const std::vector<LaneRay> rays = {
      ray(4.5f, -1.625f, 2.1875f, 0, 0, 1, 0, 1),
      ray(4.5f, -1.625f, 2.1875f, 0, 0, 1, 0, 2),
  };
  auto words = run.trace("geometry-transform-row-major", tlas->GetGPUVirtualAddress(), rays);
  int bad = run.expect("geometry-transform-row-major", words, 0,
      triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase, 0x31, 0));
  bad += run.expect("geometry-transform-baked", words, 1,
      triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase, 0x32, 1));
  return run.reportCase("geometry-transform-row-major", bad);
}

// Indexed geometry (R16 and R32 indices), an inactive (NaN x) triangle that still occupies its
// PrimitiveIndex, a geometry with zero triangles that still occupies its GeometryIndex, an
// AABB-free BLAS with no geometry at all, and the D3D12 inactive-instance and empty / NULL
// acceleration structure rules.
int caseGeometryRules(Runner &run) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  // primitive 0 inactive (x is NaN), primitive 1 the unit triangle at z = 0, primitive 2 at z = 2.
  const float vertices[] = {nan, 0, 0, nan, 0, 0, nan, 0, 0,
                            0, 0, 0, 1, 0, 0, 0, 1, 0,
                            0, 0, 2, 1, 0, 2, 0, 1, 2};
  auto vb = run.upload(vertices, sizeof(vertices));
  const uint16_t indices16[] = {3, 4, 5};
  const uint32_t indices32[] = {6, 7, 8, 3, 4, 5};
  auto ib16 = run.upload(indices16, sizeof(indices16));
  auto ib32 = run.upload(indices32, sizeof(indices32));
  D3D12_RAYTRACING_GEOMETRY_DESC geometries[4] = {
      // geometry 0: no triangles at all (VertexCount 0).
      triangles(vb->GetGPUVirtualAddress(), 12, 0, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE),
      // geometry 1: non-indexed, primitive 0 inactive, primitive 1 active at z = 0, primitive 2 at z = 2.
      triangles(vb->GetGPUVirtualAddress(), 12, 9, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE),
      // geometry 2: R16 indices naming the z = 0 triangle.
      triangles(vb->GetGPUVirtualAddress(), 12, 9, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,
                0, ib16->GetGPUVirtualAddress(), 3, DXGI_FORMAT_R16_UINT),
      // geometry 3: R32 indices: primitive 0 at z = 2, primitive 1 at z = 0.
      triangles(vb->GetGPUVirtualAddress(), 12, 9, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,
                0, ib32->GetGPUVirtualAddress(), 6, DXGI_FORMAT_R32_UINT),
  };
  // Each geometry lives in its own BLAS so a ray meets exactly one of them; instance i is placed
  // at x = 10 i. The zero-triangle geometry is geometry 0 of the BLAS that also holds geometry 1.
  auto blasA = run.build(blasInputs(geometries, 2), "contract: geometry BLAS A");
  auto blasB = run.build(blasInputs(geometries + 2, 1), "contract: geometry BLAS B");
  auto blasC = run.build(blasInputs(geometries + 3, 1), "contract: geometry BLAS C");
  auto blasEmpty = run.build(blasInputs(nullptr, 0), "contract: empty BLAS");
  const Mat place[] = {translateScale(1, 0, 0, 0), translateScale(1, 10, 0, 0), translateScale(1, 20, 0, 0),
                       translateScale(1, 30, 0, 0), translateScale(1, 40, 0, 0)};
  D3D12_RAYTRACING_INSTANCE_DESC instances[5] = {
      instanceDesc(place[0].m, 0x10, 0xff, 0, 0, blasA->GetGPUVirtualAddress()),
      // A NULL bottom-level structure: legal, inactive, but it still occupies InstanceIndex 1.
      instanceDesc(place[1].m, 0x11, 0xff, 0, 0, 0),
      instanceDesc(place[2].m, 0x12, 0xff, 0, 0, blasB->GetGPUVirtualAddress()),
      instanceDesc(place[3].m, 0x13, 0xff, 0, 0, blasC->GetGPUVirtualAddress()),
      // A BLAS with no geometry: degenerate, never intersected.
      instanceDesc(place[4].m, 0x14, 0xff, 0, 0, blasEmpty->GetGPUVirtualAddress()),
  };
  auto ib = run.upload(instances, sizeof(instances));
  auto tlas = run.build(tlasInputs(ib->GetGPUVirtualAddress(), 5), "contract: geometry TLAS");
  auto emptyTlas = run.build(tlasInputs(0, 0), "contract: empty TLAS");
  std::vector<LaneRay> rays = {
      // 0: the z = 0 triangle of geometry 1 is primitive 1 (the NaN one before it still counts).
      ray(0.25f, 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0),
      // 1: the NULL instance at x = 10 is never hit.
      ray(10.25f, 0.25f, -1, 0, 0, 1),
      // 2: R16-indexed geometry, primitive 0; GeometryIndex 0 of its own BLAS.
      ray(20.25f, 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0),
      // 3: R32-indexed geometry: the nearest is primitive 1 (z = 0).
      ray(30.25f, 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0),
      // 4: the empty-geometry BLAS instance is never hit.
      ray(40.25f, 0.25f, -1, 0, 0, 1),
      // 5: an empty TLAS (NumDescs 0) misses; miss index 1.
      ray(0.25f, 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0, 1),
      // 6: a NULL acceleration structure (descriptor Location 0) misses.
      ray(0.25f, 0.25f, -1, 0, 0, 1),
      // 7: starting past the z = 0 triangle, geometry 1's z = 2 primitive 2 is what is hit.
      ray(0.25f, 0.25f, 1, 0, 0, 1, 0, 0xff, 0, 0),
  };
  rays[5].scene = 1;
  rays[6].scene = 2;
  auto words = run.trace("geometry-rules", tlas->GetGPUVirtualAddress(), rays, emptyTlas->GetGPUVirtualAddress());
  int bad = 0;
  bad += run.expect("geometry-rules", words, 0, triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase, 0x10, 0, 1, 1));
  bad += run.expect("geometry-rules", words, 1, miss());
  bad += run.expect("geometry-rules", words, 2, triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase, 0x12, 2, 0, 0));
  bad += run.expect("geometry-rules", words, 3, triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase, 0x13, 3, 0, 1));
  bad += run.expect("geometry-rules", words, 4, miss());
  bad += run.expect("geometry-rules", words, 5, miss(1));
  bad += run.expect("geometry-rules", words, 6, miss());
  bad += run.expect("geometry-rules", words, 7, triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase, 0x10, 0, 1, 2));
  return run.reportCase("geometry-rules", bad);
}

} // namespace contract

namespace contract {

// Instance transforms and every system value that depends on them, for a triangle and a
// procedural instance: ObjectToWorld3x4 / 4x3, WorldToObject3x4, ObjectRayOrigin /
// ObjectRayDirection, WorldRayOrigin / Direction inside the intersection shader, and the
// reported distance in the parametric space shared by both rays. Scale 2 and power-of-two
// translations keep every reference value exact.
int caseTransforms(Runner &run) {
  auto vb = run.upload(unitTriangle(0.5f).data(), 9 * sizeof(float));
  auto triangleGeometry = triangles(vb->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT,
                                    D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
  auto triangleBlas = run.build(blasInputs(&triangleGeometry, 1), "contract: transform triangle BLAS");
  const D3D12_RAYTRACING_AABB box{0, 0, 0, 1, 1, 1};
  auto aabbs = run.upload(&box, sizeof(box));
  D3D12_RAYTRACING_GEOMETRY_DESC aabbGeometry{};
  aabbGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
  aabbGeometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  aabbGeometry.AABBs.AABBCount = 1;
  aabbGeometry.AABBs.AABBs = {aabbs->GetGPUVirtualAddress(), sizeof(D3D12_RAYTRACING_AABB)};
  auto boxBlas = run.build(blasInputs(&aabbGeometry, 1), "contract: transform AABB BLAS");

  const Mat triangleXf = translateScale(2.0f, 8.0f, 0.0f, 0.0f);
  const Mat boxXf = translateScale(2.0f, 16.0f, 0.0f, 4.0f);
  // Contribution 1 selects hit-group record 1 = "ContractProcedural".
  D3D12_RAYTRACING_INSTANCE_DESC instances[2] = {
      instanceDesc(triangleXf.m, 0x21, 0xff, 0, 0, triangleBlas->GetGPUVirtualAddress()),
      instanceDesc(boxXf.m, 0x22, 0xff, 1, 0, boxBlas->GetGPUVirtualAddress()),
  };
  auto ib = run.upload(instances, sizeof(instances));
  auto tlas = run.build(tlasInputs(ib->GetGPUVirtualAddress(), 2), "contract: transform TLAS");
  std::vector<LaneRay> rays = {
      // World (9, 0.5, -1) + t (0, 0, 2): object origin (0.5, 0.25, -0.5), direction (0, 0, 1);
      // the triangle at object z 0.5 is hit at t = 0.5 with barycentrics (0.5, 0.25).
      ray(9.0f, 0.5f, -1.0f, 0, 0, 2.0f, 0, 0xff, 0, 0),
      // World (17, 1, 2) + t (0, 0, 2): object origin (0.5, 0.5, -1), direction (0, 0, 1); the
      // unit box is entered at t = 1.
      ray(17.0f, 1.0f, 2.0f, 0, 0, 2.0f, 0, 0xff, 0, 0),
  };
  auto words = run.trace("transforms", tlas->GetGPUVirtualAddress(), rays);
  int bad = 0;
  // World z -1 + 2t meets the triangle at world z 1 (object z 0.5 scaled by 2): t = 1.
  Expect tri = triangleHit(1.0f, 0.5f, 0.25f, kHitGroupTagBase, 0x21, 0);
  const float triOrigin[3] = {0.5f, 0.25f, -0.5f}, dir[3] = {0, 0, 1};
  setTransforms(tri, triangleXf, inverseTranslateScale(2.0f, 8.0f, 0.0f, 0.0f), triOrigin, dir);
  bad += run.expect("transforms", words, 0, tri);

  Expect box_hit;
  box_hit.kind = kKindProcedural;
  box_hit.t = 1.0f;
  box_hit.hitKind = kProceduralHitKind;
  box_hit.instanceId = 0x22;
  box_hit.instanceIndex = 1;
  box_hit.localTag = kHitGroupTagBase + 1;
  box_hit.attributes[0] = 0.125f, box_hit.attributes[1] = 0.625f;
  const float boxOrigin[3] = {0.5f, 0.5f, -1.0f};
  setTransforms(box_hit, boxXf, inverseTranslateScale(2.0f, 16.0f, 0.0f, 4.0f), boxOrigin, dir);
  bad += run.expect("transforms", words, 1, box_hit);
  // The intersection shader ran exactly once for this ray and saw the WORLD-space origin.
  const uint32_t *log = words.data() + kIntersectionLogWord + 0x22 * 4;
  // The count is InterlockedAdd'ed onto the kFill pre-fill: one invocation reads kFill + 1.
  const uint32_t want[4] = {kFill + 1u, bits(17.0f), bits(1.0f), bits(2.0f)};
  for (UINT i = 0; i < 4; ++i) {
    if (log[i] != want[i]) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_CASE_MISMATCH case=transforms intersection_log[%u] got=0x%08x want=0x%08x\n",
                   i, log[i], want[i]);
      ++bad;
    }
  }
  return run.reportCase("transforms", bad);
}

// Instance mask, InstanceContributionToHitGroupIndex, RayContributionToHitGroupIndex,
// MultiplierForGeometryContributionToHitGroupIndex, the miss index, and every instance flag:
// TRIANGLE_CULL_DISABLE, TRIANGLE_FRONT_COUNTERCLOCKWISE, FORCE_OPAQUE and FORCE_NON_OPAQUE, each
// observed through the hit group record the hit ran (its local tag), the any-hit count and the
// facing HitKind reports.
int caseInstancesAndHitGroups(Runner &run) {
  auto vb = run.upload(unitTriangle(0.0f).data(), 9 * sizeof(float));
  // Two geometries of one BLAS: geometry 0 opaque, geometry 1 non-opaque, at x 0 and x 2.
  const float shifted[] = {2, 0, 0, 3, 0, 0, 2, 1, 0};
  auto vb2 = run.upload(shifted, sizeof(shifted));
  D3D12_RAYTRACING_GEOMETRY_DESC geometries[2] = {
      triangles(vb->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE),
      triangles(vb2->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_NONE),
  };
  auto blas = run.build(blasInputs(geometries, 2), "contract: instance BLAS");
  const UINT flags[] = {
      D3D12_RAYTRACING_INSTANCE_FLAG_NONE,
      D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE,
      D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE,
      D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE,
      D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE,
      D3D12_RAYTRACING_INSTANCE_FLAG_NONE,
  };
  // Instance i sits at y = 10 i. Instance 5 has mask 0x02 only.
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
  std::vector<Mat> places;
  for (UINT i = 0; i < 6; ++i)
    places.push_back(translateScale(1, 0, 10.0f * i, 0));
  for (UINT i = 0; i < 6; ++i)
    instances.push_back(instanceDesc(places[i].m, 0x30 + i, i == 5 ? 0x02 : 0x01, i == 0 ? 2 : 0, flags[i],
                                     blas->GetGPUVirtualAddress()));
  auto ib = run.upload(instances.data(), instances.size() * sizeof(instances[0]));
  auto tlas = run.build(tlasInputs(ib->GetGPUVirtualAddress(), 6), "contract: instance TLAS");

  constexpr uint32_t kCullBack = 0x10, kCullFront = 0x20, kCullOpaque = 0x40, kCullNonOpaque = 0x80;
  constexpr uint32_t kForceOpaque = 0x1, kForceNonOpaque = 0x2;
  // Every ray travels +z, so it sees the triangle's vertices counter-clockwise: back facing by
  // default, front facing on the FRONT_COUNTERCLOCKWISE instance.
  auto at = [](UINT instance, bool second, uint32_t rayFlags, uint32_t mask, uint32_t contribution,
               uint32_t multiplier, uint32_t missIndex = 0) {
    return ray(second ? 2.25f : 0.25f, 10.0f * instance + 0.25f, -1, 0, 0, 1, rayFlags, mask, contribution,
               multiplier, missIndex);
  };
  std::vector<LaneRay> rays = {
      // 0: instance 0, geometry 0: record = 2 (instance) + 0 (ray) + 0*3 -> tag 0x1002.
      at(0, false, 0, 0x01, 0, 3),
      // 1: instance 0, geometry 1: record = 2 + 1 + 1*3 = 6; non-opaque, so the any-hit runs once.
      at(0, true, 0, 0x01, 1, 3),
      // 2: CULL_BACK_FACING on a back-facing triangle: culled, miss index 1.
      at(0, false, kCullBack, 0x01, 0, 0, 1),
      // 3: FRONT_COUNTERCLOCKWISE instance: the same triangle is now front facing, so CULL_BACK
      //    keeps it and HitKind reports FRONT.
      at(1, false, kCullBack, 0x01, 0, 0),
      // 4: FRONT_COUNTERCLOCKWISE + CULL_FRONT: culled.
      at(1, false, kCullFront, 0x01, 0, 0),
      // 5: TRIANGLE_CULL_DISABLE ignores CULL_BACK.
      at(2, false, kCullBack, 0x01, 0, 0),
      // 6: FORCE_OPAQUE instance on the non-opaque geometry: no any-hit.
      at(3, true, 0, 0x01, 0, 0),
      // 7: FORCE_OPAQUE instance + CULL_OPAQUE ray flag: culled.
      at(3, true, kCullOpaque, 0x01, 0, 0),
      // 8: FORCE_NON_OPAQUE instance on the opaque geometry: the any-hit runs.
      at(4, false, 0, 0x01, 0, 0),
      // 9: FORCE_NON_OPAQUE instance + CULL_NON_OPAQUE ray flag: culled.
      at(4, false, kCullNonOpaque, 0x01, 0, 0),
      // 10: RAY_FLAG_FORCE_OPAQUE overrides the FORCE_NON_OPAQUE instance: no any-hit.
      at(4, false, kForceOpaque, 0x01, 0, 0),
      // 11: RAY_FLAG_FORCE_NON_OPAQUE on the opaque geometry: the any-hit runs.
      at(0, false, kForceNonOpaque, 0x01, 0, 0),
      // 12: mask 0x01 against instance 5's mask 0x02: no intersection.
      at(5, false, 0, 0x01, 0, 0),
      // 13: mask 0x02 hits instance 5.
      at(5, false, 0, 0x02, 0, 0),
      // 14: inclusion mask 0 never hits anything.
      at(0, false, 0, 0x00, 0, 0),
  };
  auto words = run.trace("instances-hit-groups", tlas->GetGPUVirtualAddress(), rays);
  auto hit = [&](UINT instance, bool second, uint32_t record, uint32_t hitKind, uint32_t anyHits,
                 uint32_t rayFlags) {
    Expect e = triangleHit(1, 0.25f, 0.25f, kHitGroupTagBase + record, 0x30 + instance, instance, second ? 1 : 0);
    e.hitKind = hitKind;
    e.anyHits = anyHits;
    e.rayFlags = rayFlags;
    return e;
  };
  int bad = 0;
  const char *name = "instances-hit-groups";
  bad += run.expect(name, words, 0, hit(0, false, 2, kHitKindBack, 0, 0));
  bad += run.expect(name, words, 1, hit(0, true, 6, kHitKindBack, 1, 0));
  bad += run.expect(name, words, 2, miss(1, kCullBack));
  bad += run.expect(name, words, 3, hit(1, false, 0, kHitKindFront, 0, kCullBack));
  bad += run.expect(name, words, 4, miss(0, kCullFront));
  bad += run.expect(name, words, 5, hit(2, false, 0, kHitKindBack, 0, kCullBack));
  bad += run.expect(name, words, 6, hit(3, true, 0, kHitKindBack, 0, 0));
  bad += run.expect(name, words, 7, miss(0, kCullOpaque));
  bad += run.expect(name, words, 8, hit(4, false, 0, kHitKindBack, 1, 0));
  bad += run.expect(name, words, 9, miss(0, kCullNonOpaque));
  bad += run.expect(name, words, 10, hit(4, false, 0, kHitKindBack, 0, kForceOpaque));
  bad += run.expect(name, words, 11, hit(0, false, 2, kHitKindBack, 1, kForceNonOpaque));
  bad += run.expect(name, words, 12, miss());
  bad += run.expect(name, words, 13, hit(5, false, 0, kHitKindBack, 0, 0));
  bad += run.expect(name, words, 14, miss());
  return run.reportCase(name, bad);
}

} // namespace contract

namespace contract {

int runAll(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil) {
  Runner run(device5, ctx, dxil);
  int failures = 0;
  failures += caseVertexFormats(run);
  failures += caseGeometryTransformLayout(run);
  failures += caseGeometryRules(run);
  failures += caseTransforms(run);
  failures += caseInstancesAndHitGroups(run);
  std::printf("OWNED_D3D12_RAYTRACING_CONTRACT failures=%d\n", failures);
  return failures;
}

} // namespace contract
