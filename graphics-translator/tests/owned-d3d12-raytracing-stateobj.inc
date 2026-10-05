// State-object cases of owned-d3d12-raytracing.cpp, driven by tests/dxil/raytracing-stateobj.hlsl.
// Included by owned-d3d12-raytracing.cpp after owned-d3d12-raytracing-contract.inc (reuses its
// geometry helpers and the Context / createBuffer / check helpers of the main file).
//
// Each case asserts exact GPU readback words or exact API results:
//   dxil-subobjects       a pipeline made only of the library (root signatures, associations,
//                         configs, hit groups, state object config all from RDAT), plus a
//                         D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION override that moves a miss
//                         shader from the CBV local root signature to the constant one
//   config-flags          STATE_OBJECT_CONFIG dependency flags on collections, imported into a
//                         pipeline whose own associations resolve the collection's dependencies
//   recursion-depth       MaxTraceRecursionDepth 0 (accepted; a raygen without TraceRay runs) and
//                         31 (accepted), 32 rejected
//   stack-size            GetShaderStackSize "hitGroup::closesthit|anyhit|intersection" syntax
//                         and its 0xffffffff results; SetPipelineStackSize drop rule
//   bundle                SetPipelineState1 + DispatchRays recorded in a bundle and executed
//   miss-ray-values       miss shader WorldRayOrigin/Direction, RayTMin, RayTCurrent = TMax, RayFlags
//   geometry-skip-flags   GeometryIndex over three geometries; RAY_FLAG_SKIP_TRIANGLES /
//                         SKIP_PROCEDURAL_PRIMITIVES; pipeline config1 SKIP_* flags (not visible
//                         through RayFlags())
//   add-to-state-object   AddToStateObject appends an export; the parent's identifiers stay
//                         byte-identical and valid in the child; the addition opt-in rules

namespace stateobj {

using contract::blasInputs;
using contract::Mat;
using contract::tlasInputs;
using contract::translateScale;
using contract::triangles;
using contract::bits;

constexpr UINT kSlotWords = 32;
constexpr UINT kMaxLanes = 16;
constexpr UINT kOutputWords = kSlotWords * kMaxLanes;
constexpr UINT kOutputBytes = kOutputWords * sizeof(uint32_t);
constexpr uint32_t kFill = 0xffffffffu;
constexpr uint32_t kKindTriangle = 1, kKindMiss = 2, kKindProcedural = 3, kKindNoTrace = 4, kKindAddedMiss = 5;
constexpr uint32_t kHitKindBack = 0xff;

// Records: raygen at 0, misses at 64 (stride 64, 4 records), hit groups at 512 (stride 64, 4).
constexpr UINT kRecordStride = 64;
constexpr UINT kMissOffset = 64;
constexpr UINT kMissCount = 4;
constexpr UINT kHitGroupOffset = 512;
constexpr UINT kHitGroupCount = 4;
constexpr UINT kTableBytes = kHitGroupOffset + kHitGroupCount * kRecordStride;

struct LaneRay {
  float origin[3];
  float tmin;
  float direction[3];
  float tmax;
  uint32_t flags, mask, contribution, multiplier, missIndex, pad[3];
};
static_assert(sizeof(LaneRay) == 64);

LaneRay ray(float ox, float oy, float oz, float dx, float dy, float dz, uint32_t flags = 0, uint32_t contribution = 0,
            uint32_t multiplier = 1, uint32_t miss = 0, float tmin = 0.0f, float tmax = 100.0f) {
  LaneRay r{};
  r.origin[0] = ox, r.origin[1] = oy, r.origin[2] = oz;
  r.direction[0] = dx, r.direction[1] = dy, r.direction[2] = dz;
  r.tmin = tmin, r.tmax = tmax;
  r.flags = flags, r.mask = 0xff, r.contribution = contribution, r.multiplier = multiplier, r.missIndex = miss;
  return r;
}

int mismatch(const char *name, const char *what, uint32_t got, uint32_t want, UINT lane = 0, UINT word = 0) {
  if (got == want)
    return 0;
  std::fprintf(stderr,
               "OWNED_D3D12_RAYTRACING_CASE_MISMATCH case=%s lane=%u field=%s word=%u got=0x%08x want=0x%08x\n", name,
               lane, what, word, got, want);
  return 1;
}

int reportCase(const char *name, int bad) {
  std::printf("OWNED_D3D12_RAYTRACING_CASE %s status=%s\n", name, bad ? "FAIL" : "PASS");
  std::fflush(stdout);
  return bad ? 1 : 0;
}

class Runner {
public:
  Runner(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil, contract::Runner &geometry,
         UINT outputStride = sizeof(uint32_t))
      : device5_(device5), ctx_(ctx), dxil_(dxil), geometry_(geometry) {
    check(device5->QueryInterface(IID_PPV_ARGS(&device_)), "stateobj: QueryInterface(ID3D12Device)");
    check(device5->QueryInterface(IID_PPV_ARGS(&device7_)), "stateobj: QueryInterface(ID3D12Device7)");
    // The global root signature the library declares (SoGlobalRS), built through the API with
    // the same layout; the command list must bind a signature the pipeline accepts.
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_ROOT_PARAMETER1 table{};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &table;
    globalRoot_ = createRootSignature(device_.Get(), rootDesc, "stateobj: global root signature");
    createResources(outputStride);
  }

  ID3D12Device *device() const { return device_.Get(); }
  ID3D12Device5 *device5() const { return device5_; }
  ID3D12Device7 *device7() const { return device7_.Get(); }
  contract::Runner &geometry() { return geometry_; }
  Context &ctx() { return ctx_; }
  const std::vector<uint8_t> &dxil() const { return dxil_; }

  D3D12_DXIL_LIBRARY_DESC library(UINT count = 0, D3D12_EXPORT_DESC *exports = nullptr) const {
    D3D12_DXIL_LIBRARY_DESC desc{};
    desc.DXILLibrary = {dxil_.data(), dxil_.size()};
    desc.NumExports = count;
    desc.pExports = exports;
    return desc;
  }

  HRESULT create(std::vector<D3D12_STATE_SUBOBJECT> subobjects, ComPtr<ID3D12StateObject> &out,
                 D3D12_STATE_OBJECT_TYPE type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
    D3D12_STATE_OBJECT_DESC desc{type, UINT(subobjects.size()), subobjects.data()};
    out.Reset();
    return device5_->CreateStateObject(&desc, IID_PPV_ARGS(&out));
  }

  static const void *identifier(ID3D12StateObject *object, const wchar_t *name) {
    ComPtr<ID3D12StateObjectProperties> properties;
    check(object->QueryInterface(IID_PPV_ARGS(&properties)), "stateobj: StateObjectProperties");
    return properties->GetShaderIdentifier(name);
  }

  // Shader table records. Tags are written after the identifier as the record's local root
  // constant (SoLocalRS) or as the GPU VA of a CBV holding the tag (SoLocalRSCbv).
  void writeRecord(UINT offset, ID3D12StateObject *object, const wchar_t *name, uint32_t tag, bool cbv = false) {
    const void *id = identifier(object, name);
    require(id != nullptr, "stateobj: GetShaderIdentifier");
    std::memcpy(table_ + offset, id, kShaderIdentifierSize);
    std::memset(table_ + offset + kShaderIdentifierSize, 0, kRecordStride - kShaderIdentifierSize);
    if (cbv) {
      cbvMapped_[cbvNext_ * 64] = tag;
      const UINT64 address = cbvs_->GetGPUVirtualAddress() + cbvNext_ * 256;
      cbvNext_ = (cbvNext_ + 1) % 8;
      std::memcpy(table_ + offset + kShaderIdentifierSize, &address, sizeof(address));
    } else {
      std::memcpy(table_ + offset + kShaderIdentifierSize, &tag, sizeof(tag));
    }
  }
  void raygen(ID3D12StateObject *object, const wchar_t *name) { writeRecord(0, object, name, 0); }
  void miss(UINT index, ID3D12StateObject *object, const wchar_t *name, uint32_t tag, bool cbv = false) {
    writeRecord(kMissOffset + index * kRecordStride, object, name, tag, cbv);
  }
  void hitGroup(UINT index, ID3D12StateObject *object, const wchar_t *name, uint32_t tag) {
    writeRecord(kHitGroupOffset + index * kRecordStride, object, name, tag);
  }

  D3D12_DISPATCH_RAYS_DESC dispatchDesc(UINT width) const {
    const D3D12_GPU_VIRTUAL_ADDRESS table = tableResource_->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord = {table, kShaderIdentifierSize};
    dispatch.MissShaderTable = {table + kMissOffset, kRecordStride * kMissCount, kRecordStride};
    dispatch.HitGroupTable = {table + kHitGroupOffset, kRecordStride * kHitGroupCount, kRecordStride};
    dispatch.Width = width;
    dispatch.Height = 1;
    dispatch.Depth = 1;
    return dispatch;
  }

  // Record the binding state and the output reset; `body` issues the dispatch itself.
  template <typename Body>
  std::vector<uint32_t> run(const char *name, D3D12_GPU_VIRTUAL_ADDRESS scene, const std::vector<LaneRay> &rays,
                            Body &&body) {
    require(rays.size() <= kMaxLanes, "stateobj: too many lanes");
    std::memcpy(raysMapped_, rays.data(), rays.size() * sizeof(LaneRay));
    D3D12_SHADER_RESOURCE_VIEW_DESC as{};
    as.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    as.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as.RaytracingAccelerationStructure.Location = scene;
    device_->CreateShaderResourceView(nullptr, &as, handle(0));
    ID3D12DescriptorHeap *heaps[] = {heap_.Get()};
    ctx_.list->SetDescriptorHeaps(1, heaps);
    ctx_.list->SetComputeRootSignature(globalRoot_.Get());
    ctx_.list->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
    ctx_.list->CopyBufferRegion(output_.Get(), 0, fill_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    body(ctx_.list.Get(), dispatchDesc(static_cast<UINT>(rays.size())));
    uavBarrier(ctx_.list.Get(), output_.Get());
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx_.list->CopyBufferRegion(readback_.Get(), 0, output_.Get(), 0, kOutputBytes);
    transition(ctx_.list.Get(), output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    geometry_.submit(name);
    void *mapped = nullptr;
    D3D12_RANGE range{0, kOutputBytes};
    check(readback_->Map(0, &range, &mapped), "stateobj: Map(readback)");
    std::vector<uint32_t> words(kOutputWords);
    std::memcpy(words.data(), mapped, kOutputBytes);
    D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    return words;
  }

  std::vector<uint32_t> trace(const char *name, ID3D12StateObject *object, D3D12_GPU_VIRTUAL_ADDRESS scene,
                              const std::vector<LaneRay> &rays) {
    return run(name, scene, rays, [&](ID3D12GraphicsCommandList4 *list, const D3D12_DISPATCH_RAYS_DESC &dispatch) {
      list->SetPipelineState1(object);
      list->DispatchRays(&dispatch);
    });
  }

  ID3D12RootSignature *globalRoot() const { return globalRoot_.Get(); }
  ID3D12DescriptorHeap *heap() const { return heap_.Get(); }

private:
  D3D12_CPU_DESCRIPTOR_HANDLE handle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * increment_;
    return h;
  }

  void createResources(UINT outputStride) {
    tableResource_ = createBuffer(device_.Get(), kTableBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    table_ = static_cast<uint8_t *>(mapUpload(tableResource_.Get()));
    std::memset(table_, 0, kTableBytes);
    cbvs_ = createBuffer(device_.Get(), 8 * 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    cbvMapped_ = static_cast<uint32_t *>(mapUpload(cbvs_.Get()));
    rays_ = createBuffer(device_.Get(), kMaxLanes * sizeof(LaneRay), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    raysMapped_ = mapUpload(rays_.Get());
    output_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    fill_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    void *fill = mapUpload(fill_.Get());
    std::memset(fill, 0xff, kOutputBytes);
    fill_->Unmap(0, nullptr);
    readback_ = createBuffer(device_.Get(), kOutputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)), "stateobj: descriptor heap");
    increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kMaxLanes;
    srv.Buffer.StructureByteStride = sizeof(LaneRay);
    device_->CreateShaderResourceView(rays_.Get(), &srv, handle(1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kOutputBytes / outputStride;
    uav.Buffer.StructureByteStride = outputStride;
    device_->CreateUnorderedAccessView(output_.Get(), nullptr, &uav, handle(2));
  }

  ID3D12Device5 *device5_;
  Context &ctx_;
  const std::vector<uint8_t> &dxil_;
  contract::Runner &geometry_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12Device7> device7_;
  ComPtr<ID3D12RootSignature> globalRoot_;
  ComPtr<ID3D12Resource> tableResource_, cbvs_, rays_, output_, fill_, readback_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  UINT increment_ = 0;
  uint8_t *table_ = nullptr;
  uint32_t *cbvMapped_ = nullptr;
  UINT cbvNext_ = 0;
  void *raysMapped_ = nullptr;
};

// ---- Scene ------------------------------------------------------------------------------------

// Triangle BLAS: geometry 0 at x 0 (opaque), geometry 1 at x 2 (non-opaque: the any-hit runs);
// AABB BLAS: the procedural unit box at x 4. Both instances use InstanceID 0x40; the box instance
// contributes 2, so record 2 (the procedural group) serves it.
struct Scene {
  ComPtr<ID3D12Resource> tlas;
};

Scene buildScene(Runner &run) {
  auto &g = run.geometry();
  const float tri0[] = {0, 0, 1, 1, 0, 1, 0, 1, 1};
  const float tri1[] = {2, 0, 1, 3, 0, 1, 2, 1, 1};
  auto vb0 = g.upload(tri0, sizeof(tri0));
  auto vb1 = g.upload(tri1, sizeof(tri1));
  // SoIntersection tests the object-space unit box; the instance transform places it at x 4.
  const D3D12_RAYTRACING_AABB box{0, 0, 0, 1, 1, 1};
  auto aabbs = g.upload(&box, sizeof(box));
  D3D12_RAYTRACING_GEOMETRY_DESC geometries[2] = {
      triangles(vb0->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE),
      triangles(vb1->GetGPUVirtualAddress(), 12, 3, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_NONE),
  };
  // D3D12 forbids mixing triangle and AABB geometries in one BLAS, so the box has its own.
  D3D12_RAYTRACING_GEOMETRY_DESC box_geometry{};
  box_geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
  box_geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  box_geometry.AABBs.AABBCount = 1;
  box_geometry.AABBs.AABBs = {aabbs->GetGPUVirtualAddress(), sizeof(D3D12_RAYTRACING_AABB)};
  auto blas = g.build(blasInputs(geometries, 2), "stateobj: triangle BLAS");
  auto boxBlas = g.build(blasInputs(&box_geometry, 1), "stateobj: AABB BLAS");
  const Mat identity = translateScale(1, 0, 0, 0);
  const Mat boxPlace = translateScale(1, 4, 0, 0);
  // Instance 1 (the box) contributes 2: its geometry 0 selects record 2, the procedural group.
  D3D12_RAYTRACING_INSTANCE_DESC instance[2] = {
      contract::instanceDesc(identity.m, 0x40, 0xff, 0, 0, blas->GetGPUVirtualAddress()),
      contract::instanceDesc(boxPlace.m, 0x40, 0xff, 2, 0, boxBlas->GetGPUVirtualAddress()),
  };
  auto instances = g.upload(instance, sizeof(instance));
  Scene scene;
  scene.tlas = g.build(tlasInputs(instances->GetGPUVirtualAddress(), 2), "stateobj: TLAS");
  return scene;
}

constexpr uint32_t kRayFlagSkipTriangles = 0x100, kRayFlagSkipProcedural = 0x200, kRayFlagCullBack = 0x10;

// Ray hitting geometry g (0, 1 triangles; 2 procedural box) from z = -1 travelling +z.
LaneRay atGeometry(UINT geometry, uint32_t flags = 0, uint32_t miss = 0) {
  return ray(2.0f * geometry + 0.25f, 0.25f, -1.0f, 0, 0, 1, flags, 0, 1, miss);
}

struct Expect {
  uint32_t kind = kKindMiss;
  uint32_t geometry = 0, primitive = 0, hitKind = 0;
  uint32_t rayFlags = 0;
  float tmin = 0, tcurrent = 0;
  float origin[3]{}, direction[3]{};
  uint32_t tag = 0;
  uint32_t anyHits = 0;
};

Expect hit(UINT geometry, uint32_t tag, uint32_t flags = 0, uint32_t anyHits = 0) {
  Expect e;
  e.kind = geometry == 2 ? kKindProcedural : kKindTriangle;
  // "geometry 2" is the box, the only geometry (index 0) of its own BLAS.
  e.geometry = geometry == 2 ? 0 : geometry;
  e.hitKind = geometry == 2 ? 5 : kHitKindBack;
  e.rayFlags = flags;
  e.tcurrent = 2.0f; // z 1 for the triangles, the box's z 0 face is entered at t 1: see below
  if (geometry == 2)
    e.tcurrent = 1.0f;
  e.origin[0] = 2.0f * geometry + 0.25f, e.origin[1] = 0.25f, e.origin[2] = -1.0f;
  e.direction[2] = 1.0f;
  e.tag = tag;
  e.anyHits = anyHits;
  return e;
}

Expect missOf(const LaneRay &r, uint32_t tag, uint32_t kind = kKindMiss) {
  Expect e;
  e.kind = kind;
  e.rayFlags = r.flags;
  e.tmin = r.tmin;
  e.tcurrent = r.tmax;
  std::memcpy(e.origin, r.origin, sizeof(e.origin));
  std::memcpy(e.direction, r.direction, sizeof(e.direction));
  e.tag = tag;
  return e;
}

int expect(const char *name, const std::vector<uint32_t> &words, UINT lane, const Expect &e) {
  const uint32_t *slot = words.data() + lane * kSlotWords;
  int bad = 0;
  auto word = [&](UINT index, uint32_t want, const char *what) { bad += mismatch(name, what, slot[index], want, lane, index); };
  word(0, e.kind, "kind");
  word(15, 0x100u | e.kind, "payload_kind");
  word(5, e.rayFlags, "ray_flags");
  word(6, bits(e.tmin), "ray_t_min");
  word(7, bits(e.tcurrent), "ray_t_current");
  for (UINT i = 0; i < 3; ++i) {
    word(8 + i, bits(e.origin[i]), "world_ray_origin");
    word(11 + i, bits(e.direction[i]), "world_ray_direction");
  }
  word(14, e.tag, "local_tag");
  if (e.kind == kKindTriangle || e.kind == kKindProcedural) {
    word(1, e.geometry, "geometry_index");
    word(2, e.primitive, "primitive_index");
    word(3, 0x40, "instance_id");
    word(4, e.hitKind, "hit_kind");
  } else {
    word(1, kFill, "untouched_geometry");
  }
  word(16, kFill + e.anyHits, "any_hit_count");
  return bad;
}

// The default table of a pipeline holding the library's exports.
void writeTable(Runner &run, ID3D12StateObject *object) {
  run.raygen(object, L"SoRayGen");
  run.miss(0, object, L"SoMiss", 0x2000);
  run.miss(1, object, L"SoMiss", 0x2001);
  run.hitGroup(0, object, L"SoHitGroup", 0x1000);
  run.hitGroup(1, object, L"SoHitGroup", 0x1001);
  run.hitGroup(2, object, L"SoProcGroup", 0x1002);
  run.hitGroup(3, object, L"SoHitGroup", 0x1003);
}

// ---- Cases ------------------------------------------------------------------------------------

// A pipeline made of the DXIL library alone: every subobject comes from its RDAT table. Then the
// same library with a DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION at state object scope moving SoMissCbv
// from SoLocalRSCbv to SoLocalRS, which D3D12 lets override the library's own association.
int caseDxilSubobjects(Runner &run, const Scene &scene) {
  const char *name = "dxil-subobjects";
  int bad = 0;
  auto library = run.library();
  ComPtr<ID3D12StateObject> object;
  HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object);
  bad += mismatch(name, "create_library_only_hr", uint32_t(hr), uint32_t(S_OK));
  if (FAILED(hr))
    return reportCase(name, bad);
  writeTable(run, object.Get());
  // Miss record 2 runs SoMissCbv through its CBV local root signature.
  run.miss(2, object.Get(), L"SoMissCbv", 0x2c02, true);
  std::vector<LaneRay> rays = {atGeometry(0), atGeometry(1), atGeometry(2), ray(-10, 0, -1, 0, 0, 1),
                               ray(-10, 0, -1, 0, 0, 1, 0, 0, 1, 2)};
  auto words = run.trace(name, object.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
  bad += expect(name, words, 0, hit(0, 0x1000));
  bad += expect(name, words, 1, hit(1, 0x1001, 0, 1));
  bad += expect(name, words, 2, hit(2, 0x1002));
  bad += expect(name, words, 3, missOf(rays[3], 0x2000));
  bad += expect(name, words, 4, missOf(rays[4], 0x2c02));

  // Override: SoMissCbv now takes SoLocalRS, so its record carries the tag as a root constant.
  const WCHAR *missCbv[] = {L"SoMissCbv"};
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION override{L"SoLocalRS", 1, missCbv};
  ComPtr<ID3D12StateObject> overridden;
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                   {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &override}},
                  overridden);
  bad += mismatch(name, "create_override_hr", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    writeTable(run, overridden.Get());
    run.miss(2, overridden.Get(), L"SoMissCbv", 0x2c12, false);
    std::vector<LaneRay> missRays = {ray(-10, 0, -1, 0, 0, 1, 0, 0, 1, 2)};
    auto overrideWords = run.trace(name, overridden.Get(), scene.tlas->GetGPUVirtualAddress(), missRays);
    bad += expect(name, overrideWords, 0, missOf(missRays[0], 0x2c12));
  }

  // An association naming a DXIL subobject the description does not define is rejected.
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION missing{L"NoSuchSubobject", 1, missCbv};
  ComPtr<ID3D12StateObject> rejected;
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                   {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &missing}},
                  rejected);
  bad += mismatch(name, "unknown_dxil_subobject_hr", uint32_t(hr), uint32_t(E_INVALIDARG));
  // An export list that leaves out the library's pipeline config leaves the exports without one.
  D3D12_EXPORT_DESC partial[] = {{L"SoRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoLocalAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto partialLibrary = run.library(UINT(std::size(partial)), partial);
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &partialLibrary}}, rejected);
  bad += mismatch(name, "export_list_without_pipeline_config_hr", uint32_t(hr), uint32_t(E_INVALIDARG));
  // ... and with an API pipeline config added it is complete again.
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &partialLibrary},
                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}},
                  rejected);
  bad += mismatch(name, "export_list_plus_api_pipeline_config_hr", uint32_t(hr), uint32_t(S_OK));
  return reportCase(name, bad);
}

// STATE_OBJECT_CONFIG dependency flags on collections.
int caseConfigFlags(Runner &run, const Scene &scene) {
  const char *name = "config-flags";
  int bad = 0;
  // Collection C1: the miss shader alone, with ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS so
  // it may leave its shader/pipeline config and local root signature to the containing pipeline,
  // and an explicit association to a DXIL subobject the collection does not contain.
  D3D12_EXPORT_DESC missOnly[] = {{L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto missLibrary = run.library(1, missOnly);
  D3D12_STATE_OBJECT_CONFIG deferred{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
                                     D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS};
  const WCHAR *missExport[] = {L"SoMiss"};
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION external{L"SoLocalRS", 1, missExport};
  ComPtr<ID3D12StateObject> deferredCollection;
  HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &missLibrary},
                           {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred},
                           {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &external}},
                          deferredCollection, D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += mismatch(name, "deferred_collection_hr", uint32_t(hr), uint32_t(S_OK));
  // Without the flag the same incomplete collection is rejected.
  ComPtr<ID3D12StateObject> incomplete;
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &missLibrary}}, incomplete,
                  D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += mismatch(name, "incomplete_collection_hr", uint32_t(hr), uint32_t(E_INVALIDARG));
  // An unknown flag bit is rejected.
  D3D12_STATE_OBJECT_CONFIG unknown{static_cast<D3D12_STATE_OBJECT_FLAGS>(0x80)};
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &missLibrary},
                   {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &unknown}},
                  incomplete, D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += mismatch(name, "unknown_flag_hr", uint32_t(hr), uint32_t(E_INVALIDARG));
  if (!deferredCollection)
    return reportCase(name, bad);

  // The pipeline: the rest of the library (whose own configs and root signatures are default
  // associations of the library scope only), plus API-scope configs that reach the collection's
  // export, plus the library's SoLocalRS that the collection's carried association names.
  D3D12_EXPORT_DESC rest[] = {
      {L"SoRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},      {L"SoClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoAnyHit", nullptr, D3D12_EXPORT_FLAG_NONE},      {L"SoProcClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoIntersection", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoHitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoProcGroup", nullptr, D3D12_EXPORT_FLAG_NONE},   {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
  };
  auto restLibrary = run.library(UINT(std::size(rest)), rest);
  D3D12_EXISTING_COLLECTION_DESC collection{deferredCollection.Get(), 0, nullptr};
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{8, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  ID3D12RootSignature *global = run.globalRoot();
  ComPtr<ID3D12StateObject> pipeline;
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &restLibrary},
                   {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &collection},
                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
                   {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global}},
                  pipeline);
  bad += mismatch(name, "pipeline_with_deferred_collection_hr", uint32_t(hr), uint32_t(S_OK));
  if (pipeline) {
    writeTable(run, pipeline.Get());
    std::vector<LaneRay> rays = {ray(-10, 0, -1, 0, 0, 1), atGeometry(0)};
    auto words = run.trace(name, pipeline.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
    bad += expect(name, words, 0, missOf(rays[0], 0x2000));
    bad += expect(name, words, 1, hit(0, 0x1000));
  }
  return reportCase(name, bad);
}

// MaxTraceRecursionDepth 0..31 (0: TraceRay disallowed), 32 rejected.
int caseRecursionDepth(Runner &run, const Scene &scene) {
  const char *name = "recursion-depth";
  int bad = 0;
  D3D12_EXPORT_DESC noTrace[] = {{L"SoRayGenNoTrace", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoAddedAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto library = run.library(UINT(std::size(noTrace)), noTrace);
  for (UINT depth : {0u, 31u, 32u}) {
    D3D12_RAYTRACING_PIPELINE_CONFIG config{depth};
    ComPtr<ID3D12StateObject> object;
    const HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &config}},
                                  object);
    const HRESULT want = depth <= 31 ? S_OK : E_INVALIDARG;
    bad += mismatch(name, depth == 0 ? "depth0_hr" : depth == 31 ? "depth31_hr" : "depth32_hr", uint32_t(hr),
                    uint32_t(want));
    if (depth != 0 || !object)
      continue;
    run.raygen(object.Get(), L"SoRayGenNoTrace");
    std::vector<LaneRay> rays(3, ray(0, 0, 0, 0, 0, 1));
    auto words = run.trace(name, object.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
    for (UINT lane = 0; lane < 3; ++lane) {
      bad += mismatch(name, "no_trace_kind", words[lane * kSlotWords], kKindNoTrace, lane, 0);
      bad += mismatch(name, "dispatch_width", words[lane * kSlotWords + 1], 3, lane, 1);
    }
  }
  // Depth 0 with the tracing raygen: the library minus its own pipeline config, plus an API
  // config of depth 0. The trace is not issued, so neither the miss shader nor any hit shader
  // runs and the payload stays as the raygen initialized it.
  D3D12_EXPORT_DESC tracing[] = {
      {L"SoRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},       {L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE},   {L"SoAnyHit", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoProcClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoIntersection", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoHitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoProcGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},      {L"SoLocalAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
  };
  auto tracingLibrary = run.library(UINT(std::size(tracing)), tracing);
  D3D12_RAYTRACING_PIPELINE_CONFIG zero{0};
  ComPtr<ID3D12StateObject> object;
  HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &tracingLibrary},
                           {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &zero}},
                          object);
  bad += mismatch(name, "depth0_tracing_hr", uint32_t(hr), uint32_t(S_OK));
  if (object) {
    run.raygen(object.Get(), L"SoRayGen");
    run.miss(0, object.Get(), L"SoMiss", 0x2000);
    run.hitGroup(0, object.Get(), L"SoHitGroup", 0x1000);
    std::vector<LaneRay> rays = {atGeometry(0), ray(-10, 0, -1, 0, 0, 1)};
    auto words = run.trace(name, object.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
    for (UINT lane = 0; lane < 2; ++lane) {
      bad += mismatch(name, "depth0_kind_untouched", words[lane * kSlotWords], kFill, lane, 0);
      bad += mismatch(name, "depth0_payload", words[lane * kSlotWords + 15], 0x100u, lane, 15);
    }
  }
  return reportCase(name, bad);
}

// GetShaderStackSize / SetPipelineStackSize.
int caseStackSize(Runner &run) {
  const char *name = "stack-size";
  int bad = 0;
  auto library = run.library();
  ComPtr<ID3D12StateObject> object;
  HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object);
  bad += mismatch(name, "create_hr", uint32_t(hr), uint32_t(S_OK));
  if (!object)
    return reportCase(name, bad);
  ComPtr<ID3D12StateObjectProperties> properties;
  check(object.As(&properties), "stateobj: properties");
  constexpr UINT64 kInvalid = 0xffffffffu;
  struct Query {
    const wchar_t *name;
    UINT64 want;
  } queries[] = {
      {L"SoRayGen", 0},
      {L"SoMiss", 0},
      {L"SoHitGroup::closesthit", 0},
      {L"SoHitGroup::anyhit", 0},
      {L"SoProcGroup::intersection", 0},
      {L"SoProcGroup::closesthit", 0},
      // The hit group itself, a member named directly, an absent member, wrong case, unknown names.
      {L"SoHitGroup", kInvalid},
      {L"SoClosestHit", kInvalid},
      {L"SoHitGroup::intersection", kInvalid},
      {L"SoProcGroup::anyhit", kInvalid},
      {L"SoHitGroup::ClosestHit", kInvalid},
      {L"SoMiss::closesthit", kInvalid},
      {L"NoSuchExport", kInvalid},
      {L"NoSuchGroup::anyhit", kInvalid},
  };
  for (const auto &query : queries) {
    const UINT64 got = properties->GetShaderStackSize(query.name);
    if (got != query.want) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_CASE_MISMATCH case=%s query=%ls got=0x%llx want=0x%llx\n", name,
                   query.name, static_cast<unsigned long long>(got), static_cast<unsigned long long>(query.want));
      ++bad;
    }
  }
  // The default pipeline stack size is the spec formula over these sizes (all 0): 0.
  bad += mismatch(name, "default_pipeline_stack", uint32_t(properties->GetPipelineStackSize()), 0);
  properties->SetPipelineStackSize(4096);
  bad += mismatch(name, "set_pipeline_stack", uint32_t(properties->GetPipelineStackSize()), 4096);
  properties->SetPipelineStackSize(0xffffffffull);
  bad += mismatch(name, "dropped_pipeline_stack", uint32_t(properties->GetPipelineStackSize()), 4096);
  // A collection reports shader stack sizes too, and pipeline stack size 0.
  // This closed collection must include the miss shader's UAV and local-tag bindings.
  D3D12_EXPORT_DESC missOnly[] = {{L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
                                  {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                  {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                  {L"SoLocalAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
                                  {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
                                  {L"SoPipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto missLibrary = run.library(UINT(std::size(missOnly)), missOnly);
  ComPtr<ID3D12StateObject> collection;
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &missLibrary}}, collection,
                  D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += mismatch(name, "collection_hr", uint32_t(hr), uint32_t(S_OK));
  if (collection) {
    ComPtr<ID3D12StateObjectProperties> collectionProperties;
    check(collection.As(&collectionProperties), "stateobj: collection properties");
    bad += mismatch(name, "collection_miss_stack", uint32_t(collectionProperties->GetShaderStackSize(L"SoMiss")), 0);
    collectionProperties->SetPipelineStackSize(512);
    bad += mismatch(name, "collection_pipeline_stack", uint32_t(collectionProperties->GetPipelineStackSize()), 0);
  }
  return reportCase(name, bad);
}

// SetPipelineState1 and DispatchRays recorded in a bundle, executed from a direct list.
int caseBundle(Runner &run, const Scene &scene) {
  const char *name = "bundle";
  int bad = 0;
  auto library = run.library();
  ComPtr<ID3D12StateObject> object;
  check(run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object), "stateobj: bundle pipeline");
  writeTable(run, object.Get());
  ComPtr<ID3D12CommandAllocator> allocator;
  check(run.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&allocator)),
        "stateobj: bundle allocator");
  ComPtr<ID3D12GraphicsCommandList4> bundle;
  {
    ComPtr<ID3D12GraphicsCommandList> base;
    check(run.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&base)),
          "stateobj: bundle list");
    check(base.As(&bundle), "stateobj: bundle list4");
  }
  std::vector<LaneRay> rays = {atGeometry(0), atGeometry(2), ray(-10, 0, -1, 0, 0, 1, 0, 0, 1, 1)};
  const auto dispatch = run.dispatchDesc(UINT(rays.size()));
  ID3D12DescriptorHeap *heaps[] = {run.heap()};
  bundle->SetDescriptorHeaps(1, heaps);
  bundle->SetPipelineState1(object.Get());
  bundle->DispatchRays(&dispatch);
  bad += mismatch(name, "bundle_close_hr", uint32_t(bundle->Close()), uint32_t(S_OK));
  auto words = run.run(name, scene.tlas->GetGPUVirtualAddress(), rays,
                       [&](ID3D12GraphicsCommandList4 *list, const D3D12_DISPATCH_RAYS_DESC &) {
                         list->ExecuteBundle(bundle.Get());
                       });
  bad += expect(name, words, 0, hit(0, 0x1000));
  bad += expect(name, words, 1, hit(2, 0x1002));
  bad += expect(name, words, 2, missOf(rays[2], 0x2001));
  return reportCase(name, bad);
}

// Miss shaders observe the ray they were traced with: WorldRayOrigin / Direction, RayTMin,
// RayTCurrent == TMax, RayFlags.
int caseMissRayValues(Runner &run, const Scene &scene) {
  const char *name = "miss-ray-values";
  int bad = 0;
  auto library = run.library();
  ComPtr<ID3D12StateObject> object;
  check(run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object), "stateobj: miss pipeline");
  writeTable(run, object.Get());
  std::vector<LaneRay> rays = {
      ray(-8.5f, 3.25f, 1.5f, 0.0f, -0.5f, 2.0f, 0, 0, 1, 0, 0.125f, 7.75f),
      ray(-1.0f, -2.0f, -3.0f, -1.0f, 0.0f, 0.0f, kRayFlagCullBack | 0x8 /* SKIP_CLOSEST_HIT */, 0, 1, 1, 0.5f, 1024.0f),
      // Aimed at geometry 0 but stopping short of it (TMax 1.5 < hit t 2).
      ray(0.25f, 0.25f, -1.0f, 0.0f, 0.0f, 1.0f, 0, 0, 1, 0, 0.0f, 1.5f),
      // Starting past it (TMin 2.5 > hit t 2).
      ray(0.25f, 0.25f, -1.0f, 0.0f, 0.0f, 1.0f, 0x4 /* ACCEPT_FIRST_HIT */, 0, 1, 1, 2.5f, 3.0f),
  };
  auto words = run.trace(name, object.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
  bad += expect(name, words, 0, missOf(rays[0], 0x2000));
  bad += expect(name, words, 1, missOf(rays[1], 0x2001));
  bad += expect(name, words, 2, missOf(rays[2], 0x2000));
  bad += expect(name, words, 3, missOf(rays[3], 0x2001));
  return reportCase(name, bad);
}

// GeometryIndex, RAY_FLAG_SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES, and the pipeline config1
// SKIP_* flags, which act on every trace but do not show up in RayFlags().
int caseGeometrySkipFlags(Runner &run, const Scene &scene) {
  const char *name = "geometry-skip-flags";
  int bad = 0;
  auto library = run.library();
  ComPtr<ID3D12StateObject> object;
  check(run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object), "stateobj: skip pipeline");
  writeTable(run, object.Get());
  // A ray along +x at z 0.5 crosses all three geometries? No: each lane aims at one geometry.
  // Multiplier 0 pins every geometry to record 3 so GeometryIndex() is the only thing that
  // distinguishes them.
  std::vector<LaneRay> rays;
  for (UINT g = 0; g < 3; ++g) {
    LaneRay r = atGeometry(g);
    r.multiplier = 0;
    r.contribution = g == 2 ? 0 : 3; // the box instance already contributes 2 (procedural group)
    rays.push_back(r);
  }
  rays.push_back(atGeometry(0, kRayFlagSkipTriangles));
  rays.push_back(atGeometry(2, kRayFlagSkipTriangles));
  rays.push_back(atGeometry(2, kRayFlagSkipProcedural, 1));
  rays.push_back(atGeometry(1, kRayFlagSkipProcedural));
  auto words = run.trace(name, object.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
  bad += expect(name, words, 0, hit(0, 0x1003));
  bad += expect(name, words, 1, hit(1, 0x1003, 0, 1));
  bad += expect(name, words, 2, hit(2, 0x1002));
  bad += expect(name, words, 3, missOf(rays[3], 0x2000));
  bad += expect(name, words, 4, hit(2, 0x1002, kRayFlagSkipTriangles));
  bad += expect(name, words, 5, missOf(rays[5], 0x2001));
  bad += expect(name, words, 6, hit(1, 0x1001, kRayFlagSkipProcedural, 1));

  // Pipeline config1 flags.
  for (const auto flags : {D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES,
                           D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES}) {
    D3D12_RAYTRACING_PIPELINE_CONFIG1 config{1, flags};
    D3D12_EXPORT_DESC exports[] = {
        {L"SoRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},       {L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
        {L"SoClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE},   {L"SoAnyHit", nullptr, D3D12_EXPORT_FLAG_NONE},
        {L"SoProcClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoIntersection", nullptr, D3D12_EXPORT_FLAG_NONE},
        {L"SoHitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoProcGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
        {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},      {L"SoLocalAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
        {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
    };
    auto partial = run.library(UINT(std::size(exports)), exports);
    ComPtr<ID3D12StateObject> skipping;
    const HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &partial},
                                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &config}},
                                  skipping);
    bad += mismatch(name, "config1_hr", uint32_t(hr), uint32_t(S_OK));
    if (!skipping)
      continue;
    writeTable(run, skipping.Get());
    std::vector<LaneRay> pipelineRays = {atGeometry(0), atGeometry(2)};
    auto pipelineWords = run.trace(name, skipping.Get(), scene.tlas->GetGPUVirtualAddress(), pipelineRays);
    const bool skipTriangles = flags == D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES;
    // RayFlags() stays 0: the pipeline flag is not reported.
    bad += expect(name, pipelineWords, 0, skipTriangles ? missOf(pipelineRays[0], 0x2000) : hit(0, 0x1000));
    bad += expect(name, pipelineWords, 1, skipTriangles ? hit(2, 0x1002) : missOf(pipelineRays[1], 0x2000));
  }
  // A config1 flag outside SKIP_TRIANGLES | SKIP_PROCEDURAL_PRIMITIVES is rejected.
  D3D12_RAYTRACING_PIPELINE_CONFIG1 invalid{1, static_cast<D3D12_RAYTRACING_PIPELINE_FLAGS>(0x1)};
  ComPtr<ID3D12StateObject> rejected;
  const HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                                 {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &invalid}},
                                rejected);
  bad += mismatch(name, "config1_invalid_flag_hr", uint32_t(hr), uint32_t(E_INVALIDARG));
  return reportCase(name, bad);
}

// AddToStateObject.
int caseAddToStateObject(Runner &run, const Scene &scene) {
  const char *name = "add-to-state-object";
  int bad = 0;
  // Parent: every export but SoMissAdded (the library's StateObjectConfig sets
  // ALLOW_STATE_OBJECT_ADDITIONS).
  D3D12_EXPORT_DESC parentExports[] = {
      {L"SoRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},       {L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE},   {L"SoAnyHit", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoProcClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoIntersection", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoHitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoProcGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},      {L"SoLocalAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoPipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
  };
  auto parentLibrary = run.library(UINT(std::size(parentExports)), parentExports);
  ComPtr<ID3D12StateObject> parent;
  HRESULT hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &parentLibrary}}, parent);
  bad += mismatch(name, "parent_hr", uint32_t(hr), uint32_t(S_OK));
  if (!parent)
    return reportCase(name, bad);
  uint8_t parentIds[4][kShaderIdentifierSize];
  const wchar_t *kept[] = {L"SoRayGen", L"SoMiss", L"SoHitGroup", L"SoProcGroup"};
  for (UINT i = 0; i < 4; ++i)
    std::memcpy(parentIds[i], Runner::identifier(parent.Get(), kept[i]), kShaderIdentifierSize);
  bad += mismatch(name, "parent_has_no_added_export",
                  uint32_t(Runner::identifier(parent.Get(), L"SoMissAdded") == nullptr), 1);

  // Addition: SoMissAdded with its own (self-contained) configs, root signatures and opt-in.
  D3D12_EXPORT_DESC addedExports[] = {
      {L"SoMissAdded", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoAddedAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},  {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoPipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
  };
  auto addedLibrary = run.library(UINT(std::size(addedExports)), addedExports);
  D3D12_STATE_SUBOBJECT additionSubobjects[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &addedLibrary}};
  D3D12_STATE_OBJECT_DESC addition{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 1, additionSubobjects};
  ComPtr<ID3D12StateObject> child;
  hr = run.device7()->AddToStateObject(&addition, parent.Get(), IID_PPV_ARGS(&child));
  bad += mismatch(name, "add_hr", uint32_t(hr), uint32_t(S_OK));
  if (!child)
    return reportCase(name, bad);
  for (UINT i = 0; i < 4; ++i) {
    const void *id = Runner::identifier(child.Get(), kept[i]);
    bad += mismatch(name, "child_identifier_unchanged",
                    uint32_t(id && std::memcmp(id, parentIds[i], kShaderIdentifierSize) == 0), 1, i);
  }
  bad += mismatch(name, "child_has_added_export",
                  uint32_t(Runner::identifier(child.Get(), L"SoMissAdded") != nullptr), 1);

  // Records written with the PARENT's identifiers dispatch on the child; the added miss runs
  // from miss record 3 written with the child's identifier.
  writeTable(run, parent.Get());
  run.miss(3, child.Get(), L"SoMissAdded", 0x2a03);
  std::vector<LaneRay> rays = {atGeometry(0), atGeometry(2), ray(-10, 0, -1, 0, 0, 1),
                               ray(-10, 0, -1, 0, 0, 1, 0, 0, 1, 3)};
  auto words = run.trace(name, child.Get(), scene.tlas->GetGPUVirtualAddress(), rays);
  bad += expect(name, words, 0, hit(0, 0x1000));
  bad += expect(name, words, 1, hit(2, 0x1002));
  bad += expect(name, words, 2, missOf(rays[2], 0x2000));
  bad += expect(name, words, 3, missOf(rays[3], 0x2a03, kKindAddedMiss));
  // The parent is unchanged and still dispatches.
  std::vector<LaneRay> parentRays = {atGeometry(1)};
  auto parentWords = run.trace(name, parent.Get(), scene.tlas->GetGPUVirtualAddress(), parentRays);
  bad += expect(name, parentWords, 0, hit(1, 0x1001, 0, 1));

  // Rules: re-adding an existing export name, an addition without the opt-in, a different
  // recursion depth, growing from a parent without the opt-in.
  D3D12_EXPORT_DESC duplicate[] = {{L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
                                   {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                   {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                   {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
                                   {L"SoPipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
                                   {L"SoConfig", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto duplicateLibrary = run.library(UINT(std::size(duplicate)), duplicate);
  additionSubobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &duplicateLibrary};
  ComPtr<ID3D12StateObject> rejected;
  hr = run.device7()->AddToStateObject(&addition, parent.Get(), IID_PPV_ARGS(&rejected));
  bad += mismatch(name, "duplicate_name_hr", uint32_t(hr), uint32_t(E_INVALIDARG));

  D3D12_EXPORT_DESC noOptIn[] = {{L"SoMissAdded", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoAddedAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {L"SoPipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto noOptInLibrary = run.library(UINT(std::size(noOptIn)), noOptIn);
  additionSubobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &noOptInLibrary};
  hr = run.device7()->AddToStateObject(&addition, parent.Get(), IID_PPV_ARGS(&rejected));
  bad += mismatch(name, "addition_without_opt_in_hr", uint32_t(hr), uint32_t(E_INVALIDARG));

  D3D12_RAYTRACING_PIPELINE_CONFIG deeper{2};
  D3D12_EXPORT_DESC deeperExports[] = {{L"SoMissAdded", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoAddedAssoc", nullptr, D3D12_EXPORT_FLAG_NONE},
                                       {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                       {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
                                       {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
                                       {L"SoConfig", nullptr, D3D12_EXPORT_FLAG_NONE}};
  auto deeperLibrary = run.library(UINT(std::size(deeperExports)), deeperExports);
  D3D12_STATE_SUBOBJECT deeperSubobjects[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &deeperLibrary},
                                              {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &deeper}};
  D3D12_STATE_OBJECT_DESC deeperAddition{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 2, deeperSubobjects};
  hr = run.device7()->AddToStateObject(&deeperAddition, parent.Get(), IID_PPV_ARGS(&rejected));
  bad += mismatch(name, "different_recursion_depth_hr", uint32_t(hr), uint32_t(E_INVALIDARG));

  auto plainLibrary = run.library();
  D3D12_STATE_OBJECT_CONFIG none{D3D12_STATE_OBJECT_FLAG_NONE};
  const WCHAR *noExports[] = {L"SoRayGen"};
  (void)noExports;
  ComPtr<ID3D12StateObject> closed;
  D3D12_EXPORT_DESC closedExports[] = {
      {L"SoRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},       {L"SoMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},     {L"SoLocalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"SoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"SoPipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
  };
  auto closedLibrary = run.library(UINT(std::size(closedExports)), closedExports);
  (void)plainLibrary;
  hr = run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &closedLibrary},
                   {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &none}},
                  closed);
  bad += mismatch(name, "closed_parent_hr", uint32_t(hr), uint32_t(S_OK));
  if (closed) {
    additionSubobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &addedLibrary};
    hr = run.device7()->AddToStateObject(&addition, closed.Get(), IID_PPV_ARGS(&rejected));
    bad += mismatch(name, "parent_without_opt_in_hr", uint32_t(hr), uint32_t(E_INVALIDARG));
  }
  return reportCase(name, bad);
}

int runAll(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil, contract::Runner &geometry) {
  Runner run(device5, ctx, dxil, geometry);
  const Scene scene = buildScene(run);
  int failures = 0;
  failures += caseDxilSubobjects(run, scene);
  failures += caseConfigFlags(run, scene);
  failures += caseRecursionDepth(run, scene);
  failures += caseStackSize(run);
  failures += caseBundle(run, scene);
  failures += caseMissRayValues(run, scene);
  failures += caseGeometrySkipFlags(run, scene);
  failures += caseAddToStateObject(run, scene);
  std::printf("OWNED_D3D12_RAYTRACING_STATEOBJ failures=%d\n", failures);
  return failures;
}

} // namespace stateobj
