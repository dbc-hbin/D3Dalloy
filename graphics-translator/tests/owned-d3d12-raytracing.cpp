// Production-valid D3D12 ray tracing fixture: BLAS + TLAS build, state object,
// shader binding tables, DispatchRays, and GPU readback of what the shaders did.
//
// This is a D3D12-conformance-shaped workload, not a wrapper smoke test. It is
// valid on a real D3D12 driver as written: explicit resource-state transitions and
// UAV barriers around the acceleration-structure builds and the ray dispatch, real
// fence completion, and no reliance on the owned runtime's internal
// same-command-buffer prepass ordering.
//
// It never treats "the call returned success" or "no validation error was printed"
// as a pass. The pass condition is a fence-completed readback of exact expected
// bytes produced by the ray generation / any-hit / closest-hit / miss / callable
// shaders in tests/dxil/raytracing.hlsl.
//
// Failure is loud by construction:
//   * root signature / state object / AS prebuild failures ExitProcess.
//   * command-list recording errors are sticky in D3D12; Close() is asserted S_OK.
//   * every lane asserts exact payload words plus each stage's own log word, so a
//     DispatchRays that never ran leaves the 0xffffffff fill pattern and fails.
//   * the acceleration-structure descriptor is required, not optional: a null AS
//     descriptor is accepted by D3D12 and every ray would silently miss, so the
//     miss/hit split below is what catches a dispatch that traced nothing.
//
// Coverage, one lane per distinct observable DXR contract:
//   * lane 0  closest hit on triangle 0 through hit-group record 0 (accepting any-hit).
//   * lane 1  no geometry at all -> miss on a pristine payload.
//   * lane 2  the only candidate is REJECTED by the any-hit (IgnoreHit): the hit is not
//             committed, traversal continues, the miss shader runs, and the payload the
//             rejected invocation wrote before IgnoreHit survives.
//   * lane 3  a ray meeting TWO candidates where the any-hit rejects triangle 0 and accepts
//             triangle 2, with no early termination: both invocations happen in either
//             enumeration order, and the accepted invocation's payload writes are what reaches
//             the closest hit.
//   * lane 4  RAY_FLAG_SKIP_CLOSEST_HIT_SHADER (0x8): hit committed, any-hit runs, closest
//             hit skipped, miss NOT run, kind stays 0.
//   * lane 5  hit-group SBT record 1 after CallShader, so the local root constant differs.
//   * lane 6  nested TraceRay from inside the closest hit (MaxTraceRecursionDepth 2).
//   * lane 7  RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH (0x4) on the two-candidate ray with
//             no rejection: traversal stops at the first accepted candidate, so only one of
//             the two candidates is ever visited and that one is the committed hit.
//   * lane 8  the same early termination requested from the shader by
//             AcceptHitAndEndSearch() instead of by the ray flag.
//   * lane 9  RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH plus an any-hit that rejects triangle
//             0: IgnoreHit is the documented exception to the flag, so the rejected candidate
//             must never be committed and the accepted one is.
//
// Where DXR leaves something unspecified the fixture does not invent an answer. Candidate
// enumeration order is undefined, so lanes 7, 8 and 9 accept every admissible outcome and
// additionally assert the termination invariant: exactly one of the two candidates was
// visited. Note the flag is only observable when a FARTHER candidate is accepted before a
// NEARER one - accepting a hit shrinks TMax to its t, so a nearer candidate stays inside the
// ray extents and a farther one is culled either way. In the near-first order the flag's
// effect therefore coincides with the ordinary TMax cull and the fixture demands the same
// result; the discriminating variant is the far-first one. Every other lane has a single
// valid outcome. No lane depends on unbound roots, out-of-bounds writes, or any other
// undefined behaviour.
//
// The procedural dispatch's fourth lane is one intersection-shader invocation that reports three
// times, which is where the committed-hit lifetime is pinned down: the first accepted report
// commits the hit and tightens the interval, a later FARTHER report must then be rejected without
// running the any-hit shader, and a later NEARER report is inside the interval so its any-hit runs
// and rejects it - leaving the first report's distance, hit kind, attributes and local root
// constant as what the closest hit observes. That lane is a single invocation on a single
// primitive, so it depends on no enumeration order either.
//
// Raytracing library input is produced only by upstream Microsoft DXC (lib_6_5), never by
// Apple's Metal Shader Converter or the D3DMetal/GPTK compiler. Arena growth uses auxiliary DXBC compute.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kFenceTimeoutMs = 15000;
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";
constexpr UINT kLaneCount = 10;
constexpr UINT kInstanceID = 7;
constexpr UINT kFirstHitGroupTag = 0x11111111u;
constexpr UINT kSecondHitGroupTag = 0x22222222u;
// The procedural hit group's own local root constant. It is distinct from both triangle tags, so
// a runtime that selected the wrong hit-group record for a procedural hit is caught by the
// payload's local_root word rather than by a plausible-looking value.
constexpr UINT kProceduralHitGroupTag = 0x33333333u;
// The pipeline must declare at least 2 so the closest hit may trace again; depth 0 is the
// ray generation trace, depth 1 the closest hit it invokes.
constexpr UINT kMaxTraceRecursionDepth = 2;

// Output buffer layout; must stay in sync with tests/dxil/raytracing.hlsl.
constexpr UINT kPayloadWords = 15;
constexpr UINT kPayloadBaseWords = 0;
constexpr UINT kPayloadStride = kPayloadWords * sizeof(uint32_t);
constexpr UINT kAnyHitSlots = 4;
constexpr UINT kAnyHitLogBase = 160 * sizeof(uint32_t);
constexpr UINT kClosestHitLogBase = 200 * sizeof(uint32_t);
constexpr UINT kMissLogBase = 210 * sizeof(uint32_t);
constexpr UINT kCallableLogBase = 220 * sizeof(uint32_t);
// The procedural dispatch's own regions. They are disjoint from the triangle dispatch's, so the
// ten triangle lanes' readback contract is byte-for-byte unchanged by this coverage.
constexpr UINT kProceduralPayloadBase = 240 * sizeof(uint32_t);
constexpr UINT kProceduralLaneCount = 4;
// How many ReportHit calls one intersection-shader invocation may make. A primitive that reports
// more than once gets one intersection-log word and one any-hit slot per crossing, so a crossing
// whose report was rejected before the any-hit ran leaves its slot at the fill pattern and the
// three decisions of one invocation are all observable at once.
constexpr UINT kIntersectionCrossings = 3;
// One word per (AABB, crossing). Exactly one lane's ray is contained by each AABB, so each word is
// written by exactly one invocation with a value that depends only on that AABB, that crossing
// and that ray.
constexpr UINT kIntersectionLogBase = 300 * sizeof(uint32_t);
constexpr UINT kIntersectionLogWords = kProceduralLaneCount * kIntersectionCrossings;
constexpr UINT kProceduralAnyHitLogBase = 320 * sizeof(uint32_t);
constexpr UINT kProceduralAnyHitCrossingWords = 4;
constexpr UINT kProceduralAnyHitLogWords = kIntersectionCrossings * kProceduralAnyHitCrossingWords;
constexpr UINT kProceduralClosestHitLogBase = 368 * sizeof(uint32_t);
constexpr UINT kProceduralMissLogBase = 372 * sizeof(uint32_t);
constexpr UINT kProceduralHitLogBase = 384 * sizeof(uint32_t);
constexpr UINT kProceduralHitLogWords = 8;
// Word indices inside one procedural hit log, in the order tests/dxil/raytracing.hlsl writes them.
constexpr UINT kProcHitMarkerWord = 0;
constexpr UINT kProcHitDistanceWord = 1;
constexpr UINT kProcHitRayTCurrentWord = 2;
constexpr UINT kProcHitRejectTagWord = 7;
// Large enough for every region above plus the trailing gap the region guard checks.
constexpr UINT kOutputBytes = 2048;
constexpr UINT kOutputWords = kOutputBytes / sizeof(uint32_t);
constexpr uint32_t kFillPattern = 0xffffffffu;
// Any-hit log encoding, mirrored by the shader: bit 31 marks the slot as written, bit 0 is
// IgnoreHit, and the primitive index sits in between so accept and reject of one primitive
// can never encode the same word.
constexpr uint32_t kLogAccepted = 0x80000000u;
// Payload tags written by the any-hit, so accept and reject are distinguishable in the
// payload too (the log alone would not catch a lost payload write).
constexpr uint32_t kAcceptTagBase = 0xA0000000u;
constexpr uint32_t kRejectTagBase = 0xB0000000u;
// The procedural hit group's own payload tags, distinct from the triangle ones so a payload word
// can never be satisfied by the triangle hit group.
constexpr uint32_t kProceduralAcceptTagBase = 0xC0000000u;
constexpr uint32_t kProceduralRejectTagBase = 0xD0000000u;
// Intersection-shader log encoding, mirrored by the shader.
constexpr uint32_t kIsInvoked = 0x80000000u;
constexpr uint32_t kIsReportAccepted = 0x1u;
constexpr uint32_t kIsDistanceInInterval = 0x2u;
constexpr UINT kIsPrimitiveShift = 8;

// ---- Procedural (AABB) geometry. The values the fixture itself needs are here; the shape the
// intersection shader tests inside each AABB, the ray each procedural lane casts and the lane
// whose any-hit rejects live in tests/dxil/raytracing.hlsl, because only the shader uses them.
constexpr UINT kProceduralInstanceID = 8;
// D3D12 selects the hit group by InstanceContributionToHitGroupIndex + RayContributionToHitGroupIndex
// + GeometryIndex * MultiplierForGeometryContributionToHitGroupIndex. The procedural instance's
// contribution is deliberately NONZERO, so the record a procedural hit selects is not the hit
// group table's base: an implementation that dropped this term would run the TRIANGLE hit group of
// record 0 and read kFirstHitGroupTag instead of kProceduralHitGroupTag.
constexpr UINT kProceduralInstanceContribution = 2;
// The x centre of AABB i is kAabbBaseX + i * kAabbSpacingX, which is the same arithmetic
// tests/dxil/raytracing.hlsl derives its shape's x centre from, so the shape the intersection
// shader tests cannot drift out of step with the box the traversal culled against.
constexpr float kAabbBaseX = 100.0f;
constexpr float kAabbSpacingX = 100.0f;
constexpr float kAabbHalfSize = 0.5f;
// The AABB is a deliberately generous conservative bound in z: it reaches behind the ray's own
// origin and far past every shape, so the traversal enumerates the candidate for all three
// procedural lanes and the intersection shader's shape is what decides whether a report happens.
constexpr float kAabbMinZ = -2.0f;
constexpr float kAabbMaxZ = 40.0f;
// The multi-report primitive's own AABB has to contain all three of the shapes its single
// invocation reports, including the one that lies beyond the ray's interval - the AABB is only a
// cull volume, so it is the traversal that must still enumerate this candidate.
constexpr float kMultiReportAabbMaxZ = 20.0f;
constexpr UINT kMultiReportPrimitive = 3;
// The hit kind an intersection shader reports is an arbitrary 0..127 value D3D12 never
// interprets; each box tags its reports with its own primitive index.
constexpr uint32_t kProceduralHitKindBase = 0x70u;
// The marker can only come from the intersection shader, so it is what proves the procedural hit
// shaders read the reported attribute bytes rather than the triangle barycentrics the traversal
// would have produced for a triangle hit.
constexpr uint32_t kProceduralAttributeMarker = 0x5eeda77u;
// The multi-report primitive: one intersection-shader invocation that reports three times along
// the same ray. Crossing 0 (t=1.0) is accepted and committed; crossing 1 (t=5.0) is farther than
// the committed hit, so DXR must reject it against the tightened interval without running the
// any-hit shader; crossing 2 (t=0.5) is nearer and inside the interval, so the any-hit shader runs
// and rejects it with IgnoreHit. The committed hit must stay crossing 0's.
constexpr uint32_t kMultiReportEntry0Bits = 0x3F800000u;  // 1.0f
constexpr uint32_t kMultiReportEntry2Bits = 0x3F000000u;  // 0.5f
constexpr UINT kMultiReportRejectCrossing = 2;
constexpr UINT kMultiReportTightenedCrossing = 1;

// Shader binding table layout. Every table start address is 64-byte aligned and
// every stride is a 32-byte multiple, as D3D12 requires.
constexpr UINT kShaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
constexpr UINT kRayGenRecordOffset = 0;
constexpr UINT kProceduralRayGenRecordOffset = 64;
// The miss table is indexed by the miss index TraceRay passes, so the two dispatches' miss
// records sit at consecutive strides: index 0 is the triangle dispatch's miss shader and index 1
// the procedural dispatch's, and both dispatches declare the whole two-record table.
constexpr UINT kMissRecordOffset = 128;
constexpr UINT kProceduralMissRecordOffset = kMissRecordOffset + kShaderIdentifierSize;
constexpr UINT kMissRecordCount = 2;
// 32-byte identifier + a three-dword local root constant parameter, rounded up to
// the SBT record alignment. Record 0 is the triangle hit group, record 1 the second
// triangle record lane 5 selects, record 2 the procedural hit group the AABB
// instance's contribution selects.
constexpr UINT kHitGroupRecordOffset = 256;
constexpr UINT kHitGroupRecordStride = 64;
constexpr UINT kHitGroupRecordCount = 3;
constexpr UINT kLocalRootDataOffset = 32;
constexpr UINT kCallableRecordOffset = 448;
constexpr UINT kShaderTableBytes = 512;

int fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  return 1;
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) {
    fail(step, hr);
    ExitProcess(1);
  }
}

void require(bool condition, const char *step) {
  if (!condition) {
    fail(step);
    ExitProcess(1);
  }
}

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file =
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    fail("CreateFileW(shader)", HRESULT_FROM_WIN32(GetLastError()));
    ExitProcess(1);
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
    CloseHandle(file);
    fail("GetFileSizeEx(shader)");
    ExitProcess(1);
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    fail("ReadFile(shader)", HRESULT_FROM_WIN32(error));
    ExitProcess(1);
  }
  CloseHandle(file);
  return data;
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = type;
  props.CreationNodeMask = 1;
  props.VisibleNodeMask = 1;
  return props;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  const auto props = heapProperties(heap);
  const auto desc = bufferDesc(size, flags);
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)),
        "CreateCommittedResource(buffer)");
  return resource;
}

// Upload-heap helper: Map declares a write-only range, which is what D3D12 wants
// for a buffer the CPU only fills.
void *mapUpload(ID3D12Resource *resource) {
  void *mapped = nullptr;
  D3D12_RANGE range{0, 0};
  check(resource->Map(0, &range, &mapped), "Map(upload)");
  return mapped;
}

// D3D12 only accepts D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE in a version 1.1
// root signature, so the caller builds a D3D12_ROOT_SIGNATURE_DESC1 and this helper
// serializes it through the versioned entry point. The 1.0 and 1.1 parameter layouts
// differ (the union position and the descriptor-range type), so no reinterpret_cast
// between them is legal.
ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC1 &desc,
                                                const char *step) {
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
  versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  versioned.Desc_1_1 = desc;

  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error;
  const HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &blob, &error);
  if (FAILED(hr)) {
    if (error)
      std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr);
    check(hr, step);
  }
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), step);
  return root;
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}

// D3D12 requires a UAV barrier between an acceleration-structure build and any
// later build or dispatch that reads the same acceleration structure. The owned
// runtime happens to encode its translation prepass inside the same Metal command
// buffer, but that is an implementation detail, not the D3D12 ordering contract,
// so the fixture issues the barrier explicitly.
void uavBarrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  list->ResourceBarrier(1, &barrier);
}

struct Context {
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;

  explicit Context(ID3D12Device *device) {
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> base;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&base)),
          "CreateCommandList");
    // DispatchRays and SetPipelineState1 live on ID3D12GraphicsCommandList4.
    check(base.As(&list), "QueryInterface(ID3D12GraphicsCommandList4)");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
      fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
      ExitProcess(1);
    }
  }

  ~Context() {
    if (event)
      CloseHandle(event);
  }

  void submitAndWait() {
    // Command-list recording errors are sticky in D3D12; Close() must surface them
    // instead of letting a silently rejected DispatchRays look like a pass.
    check(list->Close(), "CommandList::Close");
    executeAndWait();
  }

  void executeAndWait() {
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "Queue::Signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "Fence::SetEventOnCompletion");
      const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
      if (wait != WAIT_OBJECT_0) {
        fail("GPU fence timeout", HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
        ExitProcess(1);
      }
    }
  }
};

struct Resources {
  ComPtr<ID3D12Resource> blas;
  ComPtr<ID3D12Resource> blasScratch;
  ComPtr<ID3D12Resource> aabbs;
  ComPtr<ID3D12Resource> aabbBlas;
  ComPtr<ID3D12Resource> aabbBlasScratch;
  ComPtr<ID3D12Resource> tlas;
  ComPtr<ID3D12Resource> tlasScratch;
  ComPtr<ID3D12Resource> vertices;
  ComPtr<ID3D12Resource> instances;
  ComPtr<ID3D12Resource> shaderTable;
  ComPtr<ID3D12Resource> output;
  ComPtr<ID3D12Resource> fill;
  ComPtr<ID3D12Resource> readback;
};

// Three triangles in ONE geometry, so PrimitiveIndex selects among three distinct
// candidates and the any-hit can accept some and reject others within a single trace:
//   primitive 0: (0,0,0)   (1,0,0)   (0,1,0)     z = 0
//   primitive 1: (2,0,0)   (3,0,0)   (2,1,0)     z = 0
//   primitive 2: (0,0,6)   (0.3,0,6) (0,0.3,6)   z = 6
// Triangle 2 sits behind triangle 0 along +z and its footprint is deliberately small: a ray
// through (0.1,0.1) meets BOTH triangles 0 and 2, while the rays through (0.25,0.25) and
// (0.5,0.25) meet triangle 0 only, so the nested ray lane 6 casts from (0.25,0.25) is
// guaranteed to find nothing and reach the miss shader.
constexpr std::array<float, 27> kTriangleVertices = {
    0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 6.0f, 0.3f, 0.0f, 6.0f, 0.0f, 0.3f, 6.0f,
};

// One admissible outcome of a lane: the committed-hit identity plus the any-hit evidence
// that goes with it. Lanes with a single valid outcome have exactly one variant; lanes whose
// result depends on the unspecified candidate enumeration order list every admissible one.
struct RaytracingVariant {
  uint32_t primitive;
  float hitT;
  float baryU;
  float baryV;
  uint32_t anyHitTag;
  uint32_t rejectTag;
  std::array<uint32_t, kAnyHitSlots> anyHitLog;
};

// One lane's full expected readback. The leading fields hold in every admissible outcome;
// the variant list carries the order-dependent part.
struct RaytracingExpectation {
  uint32_t kind;
  uint32_t instance;
  uint32_t callableValue;
  uint32_t localRoot;
  uint32_t depth;
  uint32_t recurse;
  uint32_t nestedMiss;
  uint32_t closestHitLog;
  uint32_t missLog;
  uint32_t callableLog;
  std::array<RaytracingVariant, 2> variants;
  uint32_t variantCount;
};

// Any-hit evidence shapes, named so the expectation table reads as the shader does. A
// fill-pattern slot is one no invocation ever wrote, which is what makes a missing or an
// extra any-hit invocation visible.
constexpr std::array<uint32_t, kAnyHitSlots> kNoAnyHit = {kFillPattern, kFillPattern, kFillPattern,
                                                          kFillPattern};
constexpr std::array<uint32_t, kAnyHitSlots> kLogsTri0Accepted = {kLogAccepted | (0u << 1u) | 0u, kFillPattern,
                                                                  kFillPattern, kFillPattern};
constexpr std::array<uint32_t, kAnyHitSlots> kLogsTri1Rejected = {kFillPattern, kLogAccepted | (1u << 1u) | 1u,
                                                                  kFillPattern, kFillPattern};
constexpr std::array<uint32_t, kAnyHitSlots> kLogsTri2Accepted = {kFillPattern, kFillPattern,
                                                                  kLogAccepted | (2u << 1u) | 0u, kFillPattern};
constexpr std::array<uint32_t, kAnyHitSlots> kLogsTri0RejectedTri2Accepted = {
    kLogAccepted | (0u << 1u) | 1u, kFillPattern, kLogAccepted | (2u << 1u) | 0u, kFillPattern};

// Triangle 0 accepted: the hit is triangle 0 at t=1, barycentrics taken from the ray.
constexpr RaytracingVariant kVarTri0AcceptedAt025 = {0u, 1.0f, 0.25f, 0.25f, kAcceptTagBase | 0u, 0u,
                                                     kLogsTri0Accepted};
constexpr RaytracingVariant kVarTri0AcceptedAt050 = {0u, 1.0f, 0.5f, 0.25f, kAcceptTagBase | 0u, 0u,
                                                     kLogsTri0Accepted};
// The two-candidate ray's two candidates: triangle 0 at (0.1,0.1) and triangle 2 at t=7.
constexpr RaytracingVariant kVarTri0AcceptedAt010 = {0u, 1.0f, 0.1f, 0.1f, kAcceptTagBase | 0u, 0u,
                                                     kLogsTri0Accepted};
// Triangle 2's barycentrics at (0.1,0.1): a0=(0,0), a1=(0.3,0), a2=(0,0.3) puts 0.1/0.3 = 1/3
// on both weights.
constexpr RaytracingVariant kVarTri2Accepted = {2u, 7.0f, 1.0f / 3.0f, 1.0f / 3.0f, kAcceptTagBase | 2u, 0u,
                                                kLogsTri2Accepted};
// Triangle 2 accepted with triangle 0 rejected in the same trace: both invocations' writes are
// retained, the committed hit is the accepted one.
constexpr RaytracingVariant kVarTri2AcceptedAfterReject = {2u, 7.0f, 1.0f / 3.0f, 1.0f / 3.0f, kAcceptTagBase | 2u,
                                                           kRejectTagBase | 0u, kLogsTri0RejectedTri2Accepted};
// Triangle 0 rejected with nothing else hit: no committed hit, and the rejected invocation's
// writes survive.
constexpr RaytracingVariant kVarMissRejectedTri1 = {0xffffffffu, 0.0f, 0.0f, 0.0f, 0u, kRejectTagBase | 1u,
                                                    kLogsTri1Rejected};
// Nothing hit at all: the payload carries no any-hit evidence and no committed hit.
constexpr RaytracingVariant kVarMissPristine = {0xffffffffu, 0.0f, 0.0f, 0.0f, 0u, 0u, kNoAnyHit};
// A committed hit whose closest hit shader was skipped: the payload keeps the any-hit's
// localRoot write but no hit shader stamped the payload, so primitive stays 0xffffffff and
// the barycentrics stay at their initialized zeroes.
constexpr RaytracingVariant kVarHitCommittedShadersSkipped = {0xffffffffu, 0.0f, 0.0f, 0.0f,
                                                              kAcceptTagBase | 0u, 0u, kLogsTri0Accepted};

// Every value below is what a correct D3D12 implementation must produce.
const std::array<RaytracingExpectation, kLaneCount> kExpectations = {{
    // lane 0: triangle 0, any-hit accepts, hit-group record 0.
    {1u, kInstanceID, 0u, kFirstHitGroupTag, 0u, 0u, 0u, 1u, kFillPattern, kFillPattern,
     {{kVarTri0AcceptedAt025, {}}}, 1u},
    // lane 1: no geometry at all -> miss with a pristine payload. The miss shader's own log
    // word proves the miss ran rather than the lane never executing.
    {2u, 0xffffffffu, 0u, 0u, 0u, 0u, 0u, kFillPattern, 1u, kFillPattern, {{kVarMissPristine, {}}}, 1u},
    // lane 2: the only candidate is rejected. The hit is NOT committed (kind stays miss), and
    // the payload writes the rejected invocation made before IgnoreHit survive: localRoot
    // from its SBT record and rejectTag naming the rejected primitive.
    {2u, 0xffffffffu, 0u, kFirstHitGroupTag, 0u, 0u, 0u, kFillPattern, 1u, kFillPattern,
     {{kVarMissRejectedTri1, {}}}, 1u},
    // lane 3: two candidates, triangle 0 rejected and triangle 2 accepted, no early
    // termination. Both invocations happen in either enumeration order (a rejected candidate
    // does not update RayTCurrent, so triangle 2 stays inside the ray extents), the accepted
    // invocation is what the closest hit reports, and the rejected invocation's writes are
    // retained alongside it.
    {1u, kInstanceID, 0u, kFirstHitGroupTag, 0u, 0u, 0u, 1u, kFillPattern, kFillPattern,
     {{kVarTri2AcceptedAfterReject, {}}}, 1u},
    // lane 4: RAY_FLAG_SKIP_CLOSEST_HIT_SHADER (0x8, not 0x100 = RAY_FLAG_SKIP_TRIANGLES).
    // The hit is committed and the any-hit runs, so its localRoot write survives, but the
    // closest hit is skipped and the miss shader does not run: kind stays 0 and the miss log
    // stays at the fill pattern. localRoot here can only have come from the any-hit.
    {0u, 0xffffffffu, 0u, kFirstHitGroupTag, 0u, 0u, 0u, kFillPattern, kFillPattern, kFillPattern,
     {{kVarHitCommittedShadersSkipped, {}}}, 1u},
    // lane 5: triangle 0 through hit-group record 1, after a callable write.
    {1u, kInstanceID, 0xca11ab1eu ^ 5u, kSecondHitGroupTag, 0u, 0u, 0u, 1u, kFillPattern, 1u,
     {{kVarTri0AcceptedAt050, {}}}, 1u},
    // lane 6: nested trace from inside the closest hit. The nested ray leaves the triangle and
    // reaches the miss shader, which stamps nestedMiss and its own log word, and the closest
    // hit then re-stamps its outer results.
    {1u, kInstanceID, 0u, kFirstHitGroupTag, 1u, 1u, 1u, 1u, 1u, kFillPattern,
     {{kVarTri0AcceptedAt025, {}}}, 1u},
    // lane 7: RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH with no rejection. Traversal stops at
    // the first candidate the any-hit accepts, so only ONE of the two candidates is ever
    // visited - the one that ends up committed - and the other candidate's log slot stays at
    // the fill pattern. Which candidate that is depends on the unspecified enumeration order,
    // so both are admissible. The far-first variant is the discriminating one: without the
    // flag, accepting triangle 2 at t=7 leaves triangle 0 at t=1 inside the ray extents and
    // the second invocation happens, which the fill pattern in slot 0 rejects.
    {1u, kInstanceID, 0u, kFirstHitGroupTag, 0u, 0u, 0u, 1u, kFillPattern, kFillPattern,
     {{kVarTri0AcceptedAt010, kVarTri2Accepted}}, 2u},
    // lane 8: the same early termination requested from inside the shader by
    // AcceptHitAndEndSearch() instead of by the ray flag.
    {1u, kInstanceID, 0u, kFirstHitGroupTag, 0u, 0u, 0u, 1u, kFillPattern, kFillPattern,
     {{kVarTri0AcceptedAt010, kVarTri2Accepted}}, 2u},
    // lane 9: RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH AND a rejection of triangle 0.
    // IgnoreHit is the documented exception to the flag, so triangle 0 must never be the
    // committed hit whatever order it was enumerated in: the accepted candidate (triangle 2)
    // is committed in both admissible outcomes. The two outcomes differ only in whether
    // triangle 0 was visited before the search ended.
    {1u, kInstanceID, 0u, kFirstHitGroupTag, 0u, 0u, 0u, 1u, kFillPattern, kFillPattern,
     {{kVarTri2AcceptedAfterReject, kVarTri2Accepted}}, 2u},
}};

// ---------------------------------------------------------------------------------------
// Procedural (AABB / intersection shader) expectations
// ---------------------------------------------------------------------------------------
// One procedural lane's full expected readback. Unlike the triangle lanes there is nothing
// order-dependent here: each procedural lane's ray is contained by exactly one AABB, only that
// AABB's intersection shader invocation can report, and every one of the three lanes has exactly
// one admissible outcome. The procedural dispatch therefore asserts exact values, with no
// variant list at all.
struct ProceduralExpectation {
  uint32_t kind;
  uint32_t primitive;
  uint32_t instance;
  uint32_t localRoot;
  // The reported distance and RayTCurrent() as their exact bit patterns, so the assertion is
  // exact instead of tolerance-based: the shader computes the distance from the same constants
  // the fixture filled the AABB buffer with, so no rounding is involved on either side.
  uint32_t hitTBits;
  uint32_t anyHitTag;
  uint32_t rejectTag;
  // One intersection-log word per crossing: the shader's own report evidence, including whether
  // ReportHit answered true and whether the distance was inside the interval the shader tracked.
  std::array<uint32_t, kIntersectionCrossings> intersectionLog;
  // One any-hit slot per crossing: a crossing whose report was rejected before the any-hit ran
  // leaves its slot at the fill pattern, which is how "the any-hit never ran for that report" is
  // observable rather than inferred.
  std::array<uint32_t, kProceduralAnyHitLogWords> anyHitLog;
  uint32_t closestHitLog;
  uint32_t missLog;
  std::array<uint32_t, kProceduralHitLogWords> hitLog;
};

// 0.5f as its exact bit pattern: the distance the intersection shader reports for the two
// in-interval single-report shapes, and the RayTCurrent() the accepted hit group reads back.
// Using the bit pattern keeps the assertion exact instead of tolerance-based.
constexpr uint32_t kHalfBits = 0x3F000000u;
// A procedural any-hit slot no invocation ever wrote, and a hit-log region no closest-hit
// shader ever wrote: the fill pattern is what makes "this stage did not run" observable, which
// is exactly the claim the out-of-interval lane and the tightened crossing rest on.
constexpr std::array<uint32_t, kProceduralAnyHitCrossingWords> kProceduralNoAnyHitSlot = {
    kFillPattern, kFillPattern, kFillPattern, kFillPattern};
constexpr std::array<uint32_t, kProceduralAnyHitLogWords> kProceduralNoAnyHit = {
    kFillPattern, kFillPattern, kFillPattern, kFillPattern, kFillPattern, kFillPattern,
    kFillPattern, kFillPattern, kFillPattern, kFillPattern, kFillPattern, kFillPattern};
constexpr std::array<uint32_t, kProceduralHitLogWords> kProceduralNoHitLog = {
    kFillPattern, kFillPattern, kFillPattern, kFillPattern,
    kFillPattern, kFillPattern, kFillPattern, kFillPattern};

// The intersection shader's own log word for one crossing: it ran, ReportHit's answer, whether the
// distance it reported was inside the interval the shader tracked, and the primitive it reported.
constexpr uint32_t intersectionLog(uint32_t primitive, bool accepted, bool inInterval) {
  return kIsInvoked | (accepted ? kIsReportAccepted : 0u) | (inInterval ? kIsDistanceInInterval : 0u) |
         (primitive << kIsPrimitiveShift);
}

// One procedural any-hit slot: the decision it made (primitive identity and IgnoreHit), the marker
// the reported attributes carried, the hit kind it was given, and the reported distance. The
// distance word ties the attributes the hit group received to the attributes the shader reported.
constexpr std::array<uint32_t, kProceduralAnyHitCrossingWords> proceduralAnyHitSlot(uint32_t primitive,
                                                                                  bool rejected,
                                                                                  uint32_t distanceBits) {
  return {kLogAccepted | (primitive << 1u) | (rejected ? 1u : 0u),
          kProceduralAttributeMarker + primitive,
          kProceduralHitKindBase | primitive,
          distanceBits};
}

// A whole lane's any-hit log: one slot per crossing, in crossing order, with the slots of
// crossings that never ran left at the fill pattern. The slots are passed individually so the
// call sites never have to spell a nested aggregate.
constexpr std::array<uint32_t, kProceduralAnyHitLogWords>
proceduralAnyHitLog(const std::array<uint32_t, kProceduralAnyHitCrossingWords> &crossing0,
                    const std::array<uint32_t, kProceduralAnyHitCrossingWords> &crossing1,
                    const std::array<uint32_t, kProceduralAnyHitCrossingWords> &crossing2) {
  return {crossing0[0], crossing0[1], crossing0[2], crossing0[3],
          crossing1[0], crossing1[1], crossing1[2], crossing1[3],
          crossing2[0], crossing2[1], crossing2[2], crossing2[3]};
}

// The procedural closest-hit shader's log for an accepted report: the reported attributes, the
// system values the reported hit describes, and the payload tags the accepted invocation left.
// The reject tag is carried too, because a later rejected report's payload write survives
// alongside the committed hit's and the closest hit is where that is observable.
constexpr std::array<uint32_t, kProceduralHitLogWords> proceduralHitLog(uint32_t primitive,
                                                                       uint32_t distanceBits,
                                                                       uint32_t anyHitTag,
                                                                       uint32_t rejectTag = 0u) {
  return {kProceduralAttributeMarker + primitive, distanceBits, distanceBits,
          kProceduralHitKindBase | primitive, primitive, kProceduralInstanceID, anyHitTag, rejectTag};
}

// A lane's intersection log: one word per crossing in crossing order, with the crossings the
// invocation never reached left at the fill pattern.
constexpr std::array<uint32_t, kIntersectionCrossings>
proceduralIntersectionLog(uint32_t crossing0, uint32_t crossing1 = kFillPattern,
                          uint32_t crossing2 = kFillPattern) {
  return {crossing0, crossing1, crossing2};
}

// Every value below is what a correct D3D12 implementation must produce for the procedural
// dispatch.
//
// Lane 0's report is inside the ray's interval and accepted, so the hit is committed and the
// closest hit runs. Lane 1's report is inside the interval but its any-hit calls IgnoreHit, so no
// hit is committed, the rejecting invocation's writes survive and the miss shader runs. Lane 2's
// shape lies far outside the ray's interval, so DXR rejects the report before the any-hit shader
// runs at all: its any-hit and hit logs stay at the fill pattern and the payload carries no
// hit-group write, which is why local_root is 0 for that lane and nonzero for the others.
//
// Lane 3 is one intersection-shader invocation making three reports, in this order:
//   crossing 0  t = 1.0, accepted. The hit is committed, so the ray interval tightens to 1.0.
//   crossing 1  t = 5.0, farther than the committed hit. It is outside the tightened interval, so
//               ReportHit must answer false and the any-hit shader must NOT run: its slot stays
//               at the fill pattern. This is the case where a runtime that kept reporting against
//               the untightened TMax would run the any-hit here.
//   crossing 2  t = 0.5, nearer than the committed hit and inside the tightened interval, so the
//               any-hit shader runs and calls IgnoreHit: ReportHit answers false, the committed
//               hit stays crossing 0's distance, kind, attributes and local root, and the
//               rejecting invocation's payload writes survive. This is the case where a runtime
//               that let a later rejected report clobber the committed hit would report crossing
//               2's distance/kind/attributes here.
// The closest-hit log therefore asserts crossing 0's distance and attributes, not crossing 2's.
constexpr std::array<ProceduralExpectation, kProceduralLaneCount> kProceduralExpectations = {{
    // lane 0: accepted report on primitive 0, committed, closest hit runs.
    {1u, 0u, kProceduralInstanceID, kProceduralHitGroupTag, kHalfBits, kProceduralAcceptTagBase | 0u, 0u,
     proceduralIntersectionLog(intersectionLog(0u, true, true)),
     proceduralAnyHitLog(proceduralAnyHitSlot(0u, false, kHalfBits), kProceduralNoAnyHitSlot,
                         kProceduralNoAnyHitSlot),
     1u, kFillPattern, proceduralHitLog(0u, kHalfBits, kProceduralAcceptTagBase | 0u)},
    // lane 1: report inside the interval, any-hit rejects it. The hit is not committed, so no
    // closest hit runs and the miss shader does; the rejected invocation's localRoot and
    // rejectTag survive in the payload.
    {2u, 0xffffffffu, 0xffffffffu, kProceduralHitGroupTag, 0u, 0u, kProceduralRejectTagBase | 1u,
     proceduralIntersectionLog(intersectionLog(1u, false, true)),
     proceduralAnyHitLog(proceduralAnyHitSlot(1u, true, kHalfBits), kProceduralNoAnyHitSlot,
                         kProceduralNoAnyHitSlot),
     kFillPattern, 1u, kProceduralNoHitLog},
    // lane 2: the report's distance is outside the ray's own interval, so ReportHit answers false
    // and the any-hit shader never runs: its whole log stays at the fill pattern, and the payload
    // carries no hit-group write at all.
    {2u, 0xffffffffu, 0xffffffffu, 0u, 0u, 0u, 0u,
     proceduralIntersectionLog(intersectionLog(2u, false, false)), kProceduralNoAnyHit,
     kFillPattern, 1u, kProceduralNoHitLog},
    // lane 3: one invocation, three reports, described above.
    {1u, 3u, kProceduralInstanceID, kProceduralHitGroupTag, kMultiReportEntry0Bits,
     kProceduralAcceptTagBase | 3u, kProceduralRejectTagBase | 3u,
     proceduralIntersectionLog(intersectionLog(3u, true, true), intersectionLog(3u, false, false),
                               intersectionLog(3u, false, true)),
     proceduralAnyHitLog(proceduralAnyHitSlot(3u, false, kMultiReportEntry0Bits),
                         kProceduralNoAnyHitSlot,
                         proceduralAnyHitSlot(3u, true, kMultiReportEntry2Bits)),
     1u, kFillPattern,
     proceduralHitLog(3u, kMultiReportEntry0Bits, kProceduralAcceptTagBase | 3u,
                      kProceduralRejectTagBase | 3u)},
}};

int testRaytracing(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil) {
  // CreateStateObject and GetRaytracingAccelerationStructurePrebuildInfo are
  // ID3D12Device5 entry points; the generic resource and descriptor calls stay on
  // the base device interface.
  ComPtr<ID3D12Device> deviceOwner;
  check(device5->QueryInterface(IID_PPV_ARGS(&deviceOwner)), "QueryInterface(ID3D12Device)");
  ID3D12Device *device = deviceOwner.Get();

  // ---- Global root signature: one descriptor table holding the acceleration
  // structure SRV (t0) and the output UAV (u0), which is the binding shape the
  // shipped ray generation shaders use.
  D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace = 0;
  ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;

  D3D12_ROOT_PARAMETER1 globalParams[1]{};
  globalParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  globalParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  globalParams[0].DescriptorTable.NumDescriptorRanges = 2;
  globalParams[0].DescriptorTable.pDescriptorRanges = ranges;

  D3D12_ROOT_SIGNATURE_DESC1 globalDesc{};
  globalDesc.NumParameters = 1;
  globalDesc.pParameters = globalParams;
  globalDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  const auto globalRoot = createRootSignature(device, globalDesc, "CreateRootSignature(global)");

  // ---- Hit-group local root signature: three root constants at (b0, space16).
  D3D12_ROOT_PARAMETER1 localParams[1]{};
  localParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  localParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  localParams[0].Constants.ShaderRegister = 0;
  localParams[0].Constants.RegisterSpace = 16;
  localParams[0].Constants.Num32BitValues = 3;

  D3D12_ROOT_SIGNATURE_DESC1 localDesc{};
  localDesc.NumParameters = 1;
  localDesc.pParameters = localParams;
  localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
  const auto localRoot = createRootSignature(device, localDesc, "CreateRootSignature(local)");

  // ---- Acceleration-structure inputs. The BLAS is created before the instance
  // descriptor so the instance can reference its address directly.
  Resources resources;
  resources.vertices = createBuffer(device, sizeof(float) * kTriangleVertices.size(), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    void *mapped = mapUpload(resources.vertices.Get());
    std::memcpy(mapped, kTriangleVertices.data(), sizeof(float) * kTriangleVertices.size());
    resources.vertices->Unmap(0, nullptr);
  }

  D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
  geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  // NOT opaque: D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE (and the FORCE_OPAQUE instance flag)
  // suppress any-hit invocation, which is the whole point of this fixture.
  geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
  geometry.Triangles.Transform3x4 = 0;
  geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
  geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  geometry.Triangles.IndexCount = 0;
  geometry.Triangles.VertexCount = static_cast<UINT>(kTriangleVertices.size() / 3);
  geometry.Triangles.IndexBuffer = 0;
  geometry.Triangles.VertexBuffer.StartAddress = resources.vertices->GetGPUVirtualAddress();
  geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
  blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  blasInputs.NumDescs = 1;
  blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  blasInputs.pGeometryDescs = &geometry;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo{};
  device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasInfo);
  std::printf("OWNED_D3D12_RAYTRACING_PREBUILD blas_max=%llu blas_scratch=%llu blas_update_scratch=%llu\n",
              static_cast<unsigned long long>(blasInfo.ResultDataMaxSizeInBytes),
              static_cast<unsigned long long>(blasInfo.ScratchDataSizeInBytes),
              static_cast<unsigned long long>(blasInfo.UpdateScratchDataSizeInBytes));
  // The runtime reports prebuild failure by zeroing the struct, and a real D3D12
  // driver must also produce usable sizes; zero is never a pass.
  require(blasInfo.ResultDataMaxSizeInBytes > 0 && blasInfo.ScratchDataSizeInBytes > 0,
          "GetRaytracingAccelerationStructurePrebuildInfo(BLAS) returned zero sizes");

  resources.blas = createBuffer(device, blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  resources.blasScratch = createBuffer(device, blasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // ---- Procedural (AABB) bottom-level structure. One AABB per procedural primitive, filled with
  // the same arithmetic tests/dxil/raytracing.hlsl derives the shape it intersects from, so the
  // shape and the traversal's cull volume cannot drift apart. The z extents are deliberately
  // generous - the AABB reaches behind the ray's origin and far past every shape - so every
  // procedural lane's candidate is enumerated and the intersection shader's own shape test is
  // what decides whether a report happens.
  resources.aabbs = createBuffer(device, sizeof(D3D12_RAYTRACING_AABB) * kProceduralLaneCount,
                                 D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    void *mapped = mapUpload(resources.aabbs.Get());
    auto *boxes = static_cast<D3D12_RAYTRACING_AABB *>(mapped);
    for (UINT primitive = 0; primitive < kProceduralLaneCount; ++primitive) {
      const float centerX = kAabbBaseX + primitive * kAabbSpacingX;
      // The multi-report primitive's own shapes span z = -1.0 to 4.5, so its AABB has to reach
      // past both ends for the traversal to enumerate the candidate at all.
      const float maxZ = primitive == kMultiReportPrimitive ? kMultiReportAabbMaxZ : kAabbMaxZ;
      boxes[primitive].MinX = centerX - kAabbHalfSize;
      boxes[primitive].MinY = -kAabbHalfSize;
      boxes[primitive].MinZ = kAabbMinZ;
      boxes[primitive].MaxX = centerX + kAabbHalfSize;
      boxes[primitive].MaxY = kAabbHalfSize;
      boxes[primitive].MaxZ = maxZ;
    }
    resources.aabbs->Unmap(0, nullptr);
  }

  D3D12_RAYTRACING_GEOMETRY_DESC aabbGeometry{};
  aabbGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
  // NOT opaque, for the same reason the triangle geometry is not: D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
  // suppresses any-hit invocation, and the procedural coverage exists to run the procedural any-hit.
  aabbGeometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
  aabbGeometry.AABBs.AABBCount = kProceduralLaneCount;
  aabbGeometry.AABBs.AABBs.StartAddress = resources.aabbs->GetGPUVirtualAddress();
  aabbGeometry.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS aabbBlasInputs{};
  aabbBlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  aabbBlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  aabbBlasInputs.NumDescs = 1;
  aabbBlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  aabbBlasInputs.pGeometryDescs = &aabbGeometry;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO aabbBlasInfo{};
  device5->GetRaytracingAccelerationStructurePrebuildInfo(&aabbBlasInputs, &aabbBlasInfo);
  std::printf("OWNED_D3D12_RAYTRACING_PREBUILD_AABB aabb_max=%llu aabb_scratch=%llu\n",
              static_cast<unsigned long long>(aabbBlasInfo.ResultDataMaxSizeInBytes),
              static_cast<unsigned long long>(aabbBlasInfo.ScratchDataSizeInBytes));
  require(aabbBlasInfo.ResultDataMaxSizeInBytes > 0 && aabbBlasInfo.ScratchDataSizeInBytes > 0,
          "GetRaytracingAccelerationStructurePrebuildInfo(AABB BLAS) returned zero sizes");

  resources.aabbBlas = createBuffer(device, aabbBlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  resources.aabbBlasScratch = createBuffer(device, aabbBlasInfo.ScratchDataSizeInBytes,
                                           D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // ---- Two instances in one TLAS: the triangle structure and the procedural one. The procedural
  // instance's InstanceContributionToHitGroupIndex is NONZERO and deliberately so: D3D12 selects
  // the hit group by InstanceContributionToHitGroupIndex + RayContributionToHitGroupIndex +
  // GeometryIndex * MultiplierForGeometryContributionToHitGroupIndex, so this term is what moves
  // a procedural hit off the hit group table's base and onto the procedural record. The two
  // structures' geometry never overlaps (the AABBs are a hundred units away in x), so a ray can
  // only ever meet one of them.
  resources.instances = createBuffer(device, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * 2,
                                     D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = 1.0f;
    instance.Transform[1][1] = 1.0f;
    instance.Transform[2][2] = 1.0f;
    instance.InstanceID = kInstanceID;
    instance.InstanceMask = 0xff;
    instance.InstanceContributionToHitGroupIndex = 0;
    // Not FORCE_OPAQUE, for the same reason the geometry is not opaque.
    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    instance.AccelerationStructure = resources.blas->GetGPUVirtualAddress();

    D3D12_RAYTRACING_INSTANCE_DESC procedural{};
    procedural.Transform[0][0] = 1.0f;
    procedural.Transform[1][1] = 1.0f;
    procedural.Transform[2][2] = 1.0f;
    // A distinct InstanceID from the triangle instance, so the procedural hit's payload word can
    // only have come from the procedural instance.
    procedural.InstanceID = kProceduralInstanceID;
    procedural.InstanceMask = 0xff;
    procedural.InstanceContributionToHitGroupIndex = kProceduralInstanceContribution;
    procedural.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    procedural.AccelerationStructure = resources.aabbBlas->GetGPUVirtualAddress();

    void *mapped = mapUpload(resources.instances.Get());
    auto *descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC *>(mapped);
    descs[0] = instance;
    descs[1] = procedural;
    resources.instances->Unmap(0, nullptr);
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
  tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  tlasInputs.NumDescs = 2;
  tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlasInputs.InstanceDescs = resources.instances->GetGPUVirtualAddress();

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
  device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
  std::printf("OWNED_D3D12_RAYTRACING_PREBUILD_TLAS tlas_max=%llu tlas_scratch=%llu\n",
              static_cast<unsigned long long>(tlasInfo.ResultDataMaxSizeInBytes),
              static_cast<unsigned long long>(tlasInfo.ScratchDataSizeInBytes));
  require(tlasInfo.ResultDataMaxSizeInBytes > 0 && tlasInfo.ScratchDataSizeInBytes > 0,
          "GetRaytracingAccelerationStructurePrebuildInfo(TLAS) returned zero sizes");
  // Prebuild never reads GPU memory, so a null InstanceDescs must report the same sizes (DXR spec).
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS nullTlasInputs = tlasInputs;
  nullTlasInputs.InstanceDescs = 0;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO nullTlasInfo{};
  device5->GetRaytracingAccelerationStructurePrebuildInfo(&nullTlasInputs, &nullTlasInfo);
  require(nullTlasInfo.ResultDataMaxSizeInBytes == tlasInfo.ResultDataMaxSizeInBytes &&
              nullTlasInfo.ScratchDataSizeInBytes == tlasInfo.ScratchDataSizeInBytes,
          "GetRaytracingAccelerationStructurePrebuildInfo(TLAS) rejected a null InstanceDescs");

  resources.tlas = createBuffer(device, tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  resources.tlasScratch = createBuffer(device, tlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // ---- State object: one DXIL library, one triangle hit group that carries BOTH an any-hit
  // and a closest-hit shader and owns the local root signature, plus shader and pipeline
  // configuration.
  D3D12_DXIL_LIBRARY_DESC libraryDesc{};
  libraryDesc.DXILLibrary.pShaderBytecode = dxil.data();
  libraryDesc.DXILLibrary.BytecodeLength = dxil.size();
  // NumExports == 0 publishes every entry point the DXIL library declares.
  libraryDesc.NumExports = 0;
  libraryDesc.pExports = nullptr;

  D3D12_HIT_GROUP_DESC hitGroupDesc{};
  hitGroupDesc.HitGroupExport = L"HitGroup0";
  hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  // The any-hit member is required by this fixture: per-candidate acceptance through
  // IgnoreHit / AcceptHitAndEndSearch is a DXR contract, and a hit group without an any-hit
  // shader cannot exercise it.
  hitGroupDesc.AnyHitShaderImport = L"AnyHit";
  hitGroupDesc.ClosestHitShaderImport = L"ClosestHit";
  hitGroupDesc.IntersectionShaderImport = nullptr;

  // The procedural hit group. D3D12 requires D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE for a hit
  // group that names an intersection shader, and it may only be used with procedural geometry. It
  // carries its own any-hit and closest-hit, which read the attributes ReportHit delivered rather
  // than the triangle barycentrics, and it is selected by the AABB instance's contribution rather
  // than by the table base.
  D3D12_HIT_GROUP_DESC proceduralHitGroupDesc{};
  proceduralHitGroupDesc.HitGroupExport = L"HitGroup1";
  proceduralHitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
  proceduralHitGroupDesc.IntersectionShaderImport = L"ProceduralIntersection";
  proceduralHitGroupDesc.AnyHitShaderImport = L"ProceduralAnyHit";
  proceduralHitGroupDesc.ClosestHitShaderImport = L"ProceduralClosestHit";

  ID3D12RootSignature *globalRootRaw = globalRoot.Get();
  ID3D12RootSignature *localRootRaw = localRoot.Get();

  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
  shaderConfig.MaxPayloadSizeInBytes = kPayloadStride;
  // 8 bytes, which is both the triangle barycentrics and the procedural attribute struct, so the
  // procedural coverage does not widen the pipeline's shader configuration at all.
  shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2;

  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
  pipelineConfig.MaxTraceRecursionDepth = kMaxTraceRecursionDepth;

  // Both hit groups own the same local root signature, so the procedural record's local root
  // constant is read through exactly the same mechanism the triangle records use and the only
  // difference between the three records is which bytes they carry.
  const WCHAR *localRootExports[] = {L"HitGroup0", L"HitGroup1"};

  // The association must point at an element of the array actually passed to
  // CreateStateObject, so the array is fully formed first and the association
  // references its own element 5.
  std::array<D3D12_STATE_SUBOBJECT, 8> subobjects{};
  subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
  subobjects[0].pDesc = &libraryDesc;
  subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
  subobjects[1].pDesc = &hitGroupDesc;
  subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
  subobjects[2].pDesc = &globalRootRaw;
  subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
  subobjects[3].pDesc = &shaderConfig;
  subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
  subobjects[4].pDesc = &pipelineConfig;
  subobjects[5].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
  subobjects[5].pDesc = &localRootRaw;
  subobjects[6].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
  subobjects[6].pDesc = &proceduralHitGroupDesc;

  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{};
  association.pSubobjectToAssociate = &subobjects[5];
  association.NumExports = static_cast<UINT>(std::size(localRootExports));
  association.pExports = localRootExports;
  subobjects[7].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
  subobjects[7].pDesc = &association;

  D3D12_STATE_OBJECT_DESC stateObjectDesc{};
  stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
  stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
  stateObjectDesc.pSubobjects = subobjects.data();

  ComPtr<ID3D12StateObject> stateObject;
  // A state object that cannot be built must fail here; it must never come back as
  // an object that silently no-ops at dispatch time.
  check(device5->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&stateObject)), "CreateStateObject");
  require(stateObject.Get() != nullptr, "CreateStateObject returned a null object");
  ComPtr<ID3D12StateObjectProperties> properties;
  check(stateObject.As(&properties), "QueryInterface(ID3D12StateObjectProperties)");

  // Identifiers are opaque and copied verbatim out of the state object; the fixture
  // never constructs one.
  const void *rayGenIdentifier = properties->GetShaderIdentifier(L"RayGen");
  const void *proceduralRayGenIdentifier = properties->GetShaderIdentifier(L"ProceduralRayGen");
  const void *missIdentifier = properties->GetShaderIdentifier(L"Miss");
  const void *proceduralMissIdentifier = properties->GetShaderIdentifier(L"ProceduralMiss");
  const void *hitGroupIdentifier = properties->GetShaderIdentifier(L"HitGroup0");
  const void *proceduralHitGroupIdentifier = properties->GetShaderIdentifier(L"HitGroup1");
  const void *callableIdentifier = properties->GetShaderIdentifier(L"Callable0");
  require(rayGenIdentifier && proceduralRayGenIdentifier && missIdentifier && proceduralMissIdentifier &&
              hitGroupIdentifier && proceduralHitGroupIdentifier && callableIdentifier,
          "GetShaderIdentifier(export)");

  // ---- Shader binding tables. One table buffer holds both dispatches' records: the two ray
  // generation records, the two miss records (so miss index 0 and miss index 1 select different
  // shaders), the three hit-group records and the callable record. Every record start is 64-byte
  // aligned and every stride is a 32-byte multiple, as D3D12 requires.
  resources.shaderTable = createBuffer(device, kShaderTableBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    void *mapped = mapUpload(resources.shaderTable.Get());
    auto *bytes = static_cast<uint8_t *>(mapped);
    std::memcpy(bytes + kRayGenRecordOffset, rayGenIdentifier, kShaderIdentifierSize);
    std::memcpy(bytes + kProceduralRayGenRecordOffset, proceduralRayGenIdentifier, kShaderIdentifierSize);
    std::memcpy(bytes + kMissRecordOffset, missIdentifier, kShaderIdentifierSize);
    std::memcpy(bytes + kProceduralMissRecordOffset, proceduralMissIdentifier, kShaderIdentifierSize);
    std::memcpy(bytes + kCallableRecordOffset, callableIdentifier, kShaderIdentifierSize);
    // Record 0 and record 1 are the triangle hit group with two different local root constants
    // (lane 5 selects record 1); record 2 is the procedural hit group, which the AABB instance's
    // nonzero contribution is what selects.
    const uint32_t hitGroupTags[kHitGroupRecordCount] = {kFirstHitGroupTag, kSecondHitGroupTag,
                                                         kProceduralHitGroupTag};
    for (UINT record = 0; record < kHitGroupRecordCount; ++record) {
      auto *hitGroupRecord = bytes + kHitGroupRecordOffset + record * kHitGroupRecordStride;
      const void *identifier = record == kHitGroupRecordCount - 1 ? proceduralHitGroupIdentifier
                                                                  : hitGroupIdentifier;
      std::memcpy(hitGroupRecord, identifier, kShaderIdentifierSize);
      const uint32_t localConstants[3] = {hitGroupTags[record], 0u, 0u};
      std::memcpy(hitGroupRecord + kLocalRootDataOffset, localConstants, sizeof(localConstants));
    }
    resources.shaderTable->Unmap(0, nullptr);
  }

  // ---- Output + readback. The fill pattern makes both "DispatchRays never ran" and
  // "this stage never executed" observable.
  resources.output = createBuffer(device, kOutputBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  resources.fill = createBuffer(device, kOutputBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    void *mapped = mapUpload(resources.fill.Get());
    std::memset(mapped, 0xff, kOutputBytes);
    resources.fill->Unmap(0, nullptr);
  }
  resources.readback = createBuffer(device, kOutputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST);

  // ---- Descriptors. The acceleration-structure SRV is REQUIRED, not decorative: the
  // dispatch resolves the native structure through exactly this descriptor, and a
  // missing one is stored as a Null descriptor rather than rejected, so every ray would
  // silently miss.
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 2;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
  const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = srvHandle;
  uavHandle.ptr += descriptorSize;

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = DXGI_FORMAT_UNKNOWN;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.RaytracingAccelerationStructure.Location = resources.tlas->GetGPUVirtualAddress();
  // D3D12 requires pResource == nullptr for an acceleration-structure SRV.
  device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
  uavDesc.Format = DXGI_FORMAT_UNKNOWN;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uavDesc.Buffer.FirstElement = 0;
  uavDesc.Buffer.NumElements = kOutputWords;
  uavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
  uavDesc.Buffer.CounterOffsetInBytes = 0;
  uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
  device->CreateUnorderedAccessView(resources.output.Get(), nullptr, &uavDesc, uavHandle);

  // ---- Record, submit, fence.
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuild{};
  blasBuild.Inputs = blasInputs;
  blasBuild.DestAccelerationStructureData = resources.blas->GetGPUVirtualAddress();
  blasBuild.SourceAccelerationStructureData = 0;
  blasBuild.ScratchAccelerationStructureData = resources.blasScratch->GetGPUVirtualAddress();

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC aabbBlasBuild{};
  aabbBlasBuild.Inputs = aabbBlasInputs;
  aabbBlasBuild.DestAccelerationStructureData = resources.aabbBlas->GetGPUVirtualAddress();
  aabbBlasBuild.SourceAccelerationStructureData = 0;
  aabbBlasBuild.ScratchAccelerationStructureData = resources.aabbBlasScratch->GetGPUVirtualAddress();

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuild{};
  tlasBuild.Inputs = tlasInputs;
  tlasBuild.DestAccelerationStructureData = resources.tlas->GetGPUVirtualAddress();
  tlasBuild.SourceAccelerationStructureData = 0;
  tlasBuild.ScratchAccelerationStructureData = resources.tlasScratch->GetGPUVirtualAddress();

  // The triangle dispatch. Its ray generation and miss records are the first of each table.
  D3D12_DISPATCH_RAYS_DESC dispatch{};
  dispatch.RayGenerationShaderRecord.StartAddress =
      resources.shaderTable->GetGPUVirtualAddress() + kRayGenRecordOffset;
  dispatch.RayGenerationShaderRecord.SizeInBytes = kShaderIdentifierSize;
  dispatch.MissShaderTable.StartAddress = resources.shaderTable->GetGPUVirtualAddress() + kMissRecordOffset;
  dispatch.MissShaderTable.SizeInBytes = kShaderIdentifierSize * kMissRecordCount;
  dispatch.MissShaderTable.StrideInBytes = kShaderIdentifierSize;
  dispatch.HitGroupTable.StartAddress = resources.shaderTable->GetGPUVirtualAddress() + kHitGroupRecordOffset;
  dispatch.HitGroupTable.SizeInBytes = kHitGroupRecordStride * kHitGroupRecordCount;
  dispatch.HitGroupTable.StrideInBytes = kHitGroupRecordStride;
  dispatch.CallableShaderTable.StartAddress = resources.shaderTable->GetGPUVirtualAddress() + kCallableRecordOffset;
  dispatch.CallableShaderTable.SizeInBytes = kShaderIdentifierSize;
  dispatch.CallableShaderTable.StrideInBytes = kShaderIdentifierSize;
  dispatch.Width = kLaneCount;
  dispatch.Height = 1;
  // A single-depth dispatch keeps the lane index one-dimensional, so lane ==
  // DispatchRaysIndex().x. DispatchRays itself accepts any Depth >= 1.
  dispatch.Depth = 1;

  // The procedural dispatch. It is a DispatchRays of its own with its own ray generation record
  // and its own miss record (miss index 1), and it shares the hit-group table with the triangle
  // dispatch - which is the point: the AABB instance's contribution is what selects the
  // procedural record out of that shared table.
  D3D12_DISPATCH_RAYS_DESC proceduralDispatch{};
  proceduralDispatch.RayGenerationShaderRecord.StartAddress =
      resources.shaderTable->GetGPUVirtualAddress() + kProceduralRayGenRecordOffset;
  proceduralDispatch.RayGenerationShaderRecord.SizeInBytes = kShaderIdentifierSize;
  proceduralDispatch.MissShaderTable.StartAddress =
      resources.shaderTable->GetGPUVirtualAddress() + kMissRecordOffset;
  proceduralDispatch.MissShaderTable.SizeInBytes = kShaderIdentifierSize * kMissRecordCount;
  proceduralDispatch.MissShaderTable.StrideInBytes = kShaderIdentifierSize;
  proceduralDispatch.HitGroupTable.StartAddress =
      resources.shaderTable->GetGPUVirtualAddress() + kHitGroupRecordOffset;
  proceduralDispatch.HitGroupTable.SizeInBytes = kHitGroupRecordStride * kHitGroupRecordCount;
  proceduralDispatch.HitGroupTable.StrideInBytes = kHitGroupRecordStride;
  // No callable record: neither procedural shader calls one, and D3D12 accepts a null callable
  // table as long as no shader uses it.
  proceduralDispatch.CallableShaderTable = {};
  proceduralDispatch.Width = kProceduralLaneCount;
  proceduralDispatch.Height = 1;
  proceduralDispatch.Depth = 1;

  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  ctx.list->SetDescriptorHeaps(1, heaps);
  // DispatchRays reads its global root arguments from the compute root arguments.
  ctx.list->SetComputeRootSignature(globalRoot.Get());
  ctx.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());

  ctx.list->CopyBufferRegion(resources.output.Get(), 0, resources.fill.Get(), 0, kOutputBytes);
  transition(ctx.list.Get(), resources.output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ctx.list->BuildRaytracingAccelerationStructure(&blasBuild, 0, nullptr);
  // Triangle BLAS build -> TLAS build, which reads it.
  uavBarrier(ctx.list.Get(), resources.blas.Get());

  // The AABB bottom-level structure is built the same way, and the TLAS must not be built until
  // both bottom-level structures it references are done.
  ctx.list->BuildRaytracingAccelerationStructure(&aabbBlasBuild, 0, nullptr);
  uavBarrier(ctx.list.Get(), resources.aabbBlas.Get());

  ctx.list->BuildRaytracingAccelerationStructure(&tlasBuild, 0, nullptr);
  // TLAS build -> DispatchRays, which reads the TLAS.
  uavBarrier(ctx.list.Get(), resources.tlas.Get());
  // Fill copy -> the UAV writes the ray generation shaders are about to perform.
  uavBarrier(ctx.list.Get(), resources.output.Get());

  ctx.list->SetPipelineState1(stateObject.Get());
  ctx.list->DispatchRays(&dispatch);
  // The two dispatches write disjoint regions of the same UAV, but D3D12 still requires the
  // barrier between them: a UAV barrier orders every write to the resource, and the fixture does
  // not rely on the runtime's internal ordering to keep the procedural dispatch's writes visible.
  uavBarrier(ctx.list.Get(), resources.output.Get());
  ctx.list->DispatchRays(&proceduralDispatch);

  // DispatchRays UAV writes -> the copy that reads them.
  uavBarrier(ctx.list.Get(), resources.output.Get());
  transition(ctx.list.Get(), resources.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(resources.readback.Get(), 0, resources.output.Get(), 0, kOutputBytes);
  ctx.submitAndWait();

  // ---- Assert the GPU results.
  void *mapped = nullptr;
  D3D12_RANGE range{0, kOutputBytes};
  check(resources.readback->Map(0, &range, &mapped), "Map(readback)");
  const auto *words = static_cast<const uint32_t *>(mapped);

  int failures = 0;

  // The gap between the last payload word and the first any-hit log word must still hold the
  // fill pattern: it is the region a payload wider than kPayloadWords would spill into, so a
  // shader/fixture disagreement about the payload size shows up here rather than as a
  // plausible-looking value read out of a neighbouring lane's slot.
  for (UINT word = kLaneCount * kPayloadWords; word < kAnyHitLogBase / sizeof(uint32_t); ++word) {
    if (words[word] != kFillPattern) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_PAYLOAD_OVERRUN word=%u value=0x%08x\n", word, words[word]);
      ++failures;
    }
  }

  // Every word outside the regions both dispatches are supposed to write must still hold the
  // fill pattern. This is the same idea as the payload-gap guard above, applied to the whole
  // buffer: a shader that writes outside its own region - a payload wider than the fixture
  // believes, a log indexed past its lane, an output word past the last region - leaves a
  // non-fill word here and fails, instead of silently overwriting another lane's evidence.
  {
    std::vector<bool> covered(kOutputWords, false);
    const auto cover = [&covered](UINT base, UINT count) {
      for (UINT word = base; word < base + count; ++word)
        covered[word] = true;
    };
    cover(kPayloadBaseWords, kLaneCount * kPayloadWords);
    cover(kAnyHitLogBase / sizeof(uint32_t), kLaneCount * kAnyHitSlots);
    cover(kClosestHitLogBase / sizeof(uint32_t), kLaneCount);
    cover(kMissLogBase / sizeof(uint32_t), kLaneCount);
    cover(kCallableLogBase / sizeof(uint32_t), kLaneCount);
    cover(kProceduralPayloadBase / sizeof(uint32_t), kProceduralLaneCount * kPayloadWords);
    cover(kIntersectionLogBase / sizeof(uint32_t), kIntersectionLogWords);
    cover(kProceduralAnyHitLogBase / sizeof(uint32_t), kProceduralLaneCount * kProceduralAnyHitLogWords);
    cover(kProceduralClosestHitLogBase / sizeof(uint32_t), kProceduralLaneCount);
    cover(kProceduralMissLogBase / sizeof(uint32_t), kProceduralLaneCount);
    cover(kProceduralHitLogBase / sizeof(uint32_t), kProceduralLaneCount * kProceduralHitLogWords);
    for (UINT word = 0; word < kOutputWords; ++word) {
      if (!covered[word] && words[word] != kFillPattern) {
        std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_REGION_OVERRUN word=%u value=0x%08x\n", word, words[word]);
        ++failures;
      }
    }
  }

  // ---- The procedural dispatch's readback. Each procedural lane's ray is contained by exactly
  // one AABB and only that AABB's intersection shader can report, so unlike the triangle lanes
  // there is no order-dependent outcome to enumerate: every word below is asserted exactly. Lane
  // i's ray enters AABB i, so lane i's evidence is indexed by primitive i - including the
  // intersection log, whose words are per (primitive, crossing).
  for (UINT lane = 0; lane < kProceduralLaneCount; ++lane) {
    const auto &expected = kProceduralExpectations[lane];
    const uint32_t *payload = words + kProceduralPayloadBase / sizeof(uint32_t) + lane * kPayloadWords;
    const uint32_t *observedIntersection =
        words + kIntersectionLogBase / sizeof(uint32_t) + lane * kIntersectionCrossings;
    const uint32_t *anyHitLog =
        words + kProceduralAnyHitLogBase / sizeof(uint32_t) + lane * kProceduralAnyHitLogWords;
    const uint32_t closestHitLog = words[kProceduralClosestHitLogBase / sizeof(uint32_t) + lane];
    const uint32_t missLog = words[kProceduralMissLogBase / sizeof(uint32_t) + lane];
    const uint32_t *hitLog = words + kProceduralHitLogBase / sizeof(uint32_t) + lane * kProceduralHitLogWords;

    const float hitT = *reinterpret_cast<const float *>(&payload[3]);
    const float baryU = *reinterpret_cast<const float *>(&payload[7]);
    const float baryV = *reinterpret_cast<const float *>(&payload[8]);
    std::printf("OWNED_D3D12_RAYTRACING_PROCEDURAL lane=%u kind=%u primitive=%u instance=%u hit_t=%.6f "
                "local_root=0x%08x lane_field=%u bary=(%.6f,%.6f) dimensions=%u anyhit_tag=0x%08x "
                "reject_tag=0x%08x\n",
                lane, payload[0], payload[1], payload[2], hitT, payload[5], payload[6], baryU, baryV,
                payload[9], payload[13], payload[14]);
    std::printf("OWNED_D3D12_RAYTRACING_PROCEDURAL_LOG lane=%u "
                "intersection=(0x%08x,0x%08x,0x%08x) "
                "anyhit=(0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x) "
                "closesthit=0x%08x miss=0x%08x "
                "hit=(0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x)\n",
                lane, observedIntersection[0], observedIntersection[1], observedIntersection[2], anyHitLog[0],
                anyHitLog[1], anyHitLog[2], anyHitLog[3], anyHitLog[4], anyHitLog[5], anyHitLog[6],
                anyHitLog[7], anyHitLog[8], anyHitLog[9], anyHitLog[10], anyHitLog[11], closestHitLog,
                missLog, hitLog[0], hitLog[1], hitLog[2], hitLog[3], hitLog[4], hitLog[5], hitLog[6],
                hitLog[7]);

    const bool payloadOk =
        payload[0] == expected.kind && payload[1] == expected.primitive && payload[2] == expected.instance &&
        payload[3] == expected.hitTBits && payload[4] == 0u &&
        payload[5] == expected.localRoot && payload[6] == lane &&
        // A procedural hit has no barycentrics: a runtime that filled these from a triangle path
        // would be reporting triangle attributes for a procedural hit, and this is where it shows.
        payload[7] == 0u && payload[8] == 0u && payload[9] == kProceduralLaneCount && payload[10] == 0u &&
        payload[11] == 0u && payload[12] == 0u && payload[13] == expected.anyHitTag &&
        payload[14] == expected.rejectTag;

    const bool logsOk =
        std::equal(observedIntersection, observedIntersection + kIntersectionCrossings,
                   expected.intersectionLog.begin()) &&
        closestHitLog == expected.closestHitLog && missLog == expected.missLog &&
        std::equal(anyHitLog, anyHitLog + kProceduralAnyHitLogWords, expected.anyHitLog.begin()) &&
        std::equal(hitLog, hitLog + kProceduralHitLogWords, expected.hitLog.begin());

    if (!payloadOk || !logsOk) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_PROCEDURAL_MISMATCH lane=%u payload=%d logs=%d\n", lane,
                   payloadOk ? 1 : 0, logsOk ? 1 : 0);
      ++failures;
    }
  }

  // The multi-report lane's whole point is that one invocation's three reports are all observable
  // at once, so its evidence is asserted a second time against the specific claims the lane exists
  // to pin down. These are the two regressions the lane guards: a runtime that kept testing later
  // reports against the untightened TMax would run the any-hit for crossing 1 (its slot would not
  // be the fill pattern), and a runtime that let a later rejected report overwrite the committed
  // hit would report crossing 2's distance and attributes in the closest-hit log.
  {
    const UINT lane = kProceduralLaneCount - 1;
    const uint32_t *observedIntersection =
        words + kIntersectionLogBase / sizeof(uint32_t) + lane * kIntersectionCrossings;
    const uint32_t *anyHitLog =
        words + kProceduralAnyHitLogBase / sizeof(uint32_t) + lane * kProceduralAnyHitLogWords;
    const uint32_t *hitLog = words + kProceduralHitLogBase / sizeof(uint32_t) + lane * kProceduralHitLogWords;

    if (observedIntersection[kMultiReportTightenedCrossing] !=
        intersectionLog(kMultiReportPrimitive, false, false)) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_PROCEDURAL_MISMATCH lane=%u crossing=%u "
                           "value=0x%08x: a report beyond the tightened interval must be rejected\n",
                   lane, kMultiReportTightenedCrossing, observedIntersection[kMultiReportTightenedCrossing]);
      ++failures;
    }
    if (observedIntersection[kMultiReportRejectCrossing] !=
        intersectionLog(kMultiReportPrimitive, false, true)) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_PROCEDURAL_MISMATCH lane=%u crossing=%u "
                           "value=0x%08x: a nearer report must be inside the interval and rejected by "
                           "the any-hit\n",
                   lane, kMultiReportRejectCrossing, observedIntersection[kMultiReportRejectCrossing]);
      ++failures;
    }
    // Crossing 1's report was rejected before the any-hit shader ran, so every word of its slot
    // must still be the fill pattern; crossing 2's slot is the rejecting invocation's evidence.
    const uint32_t *tightenedSlot = anyHitLog + kMultiReportTightenedCrossing * kProceduralAnyHitCrossingWords;
    for (UINT word = 0; word < kProceduralAnyHitCrossingWords; ++word) {
      if (tightenedSlot[word] != kFillPattern) {
        std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_PROCEDURAL_MISMATCH lane=%u crossing=%u word=%u "
                             "value=0x%08x: the any-hit shader must not run for a report outside the "
                             "tightened interval\n",
                     lane, kMultiReportTightenedCrossing, word, tightenedSlot[word]);
        ++failures;
      }
    }
    // The committed hit is crossing 0's, not the later nearer report's, and the rejecting
    // invocation's payload write survived alongside it: the closest hit reads the reject tag
    // crossing 2 wrote while its own distance, hit kind and attributes stay crossing 0's.
    if (hitLog[kProcHitMarkerWord] != kProceduralAttributeMarker + kMultiReportPrimitive ||
        hitLog[kProcHitDistanceWord] != kMultiReportEntry0Bits ||
        hitLog[kProcHitRayTCurrentWord] != kMultiReportEntry0Bits ||
        hitLog[kProcHitRejectTagWord] != (kProceduralRejectTagBase | kMultiReportPrimitive)) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_PROCEDURAL_MISMATCH lane=%u: the closest hit must "
                           "observe the FIRST committed report with the later rejection's payload "
                           "write retained (marker=0x%08x distance=0x%08x ray_t_current=0x%08x "
                           "reject_tag=0x%08x)\n",
                   lane, hitLog[kProcHitMarkerWord], hitLog[kProcHitDistanceWord],
                   hitLog[kProcHitRayTCurrentWord], hitLog[kProcHitRejectTagWord]);
      ++failures;
    }
  }

  for (UINT lane = 0; lane < kLaneCount; ++lane) {
    const auto &expected = kExpectations[lane];
    const uint32_t *payload = words + lane * kPayloadWords;
    const uint32_t *anyHitLog = words + kAnyHitLogBase / sizeof(uint32_t) + lane * kAnyHitSlots;
    const uint32_t closestHitLog = words[kClosestHitLogBase / sizeof(uint32_t) + lane];
    const uint32_t missLog = words[kMissLogBase / sizeof(uint32_t) + lane];
    const uint32_t callableLog = words[kCallableLogBase / sizeof(uint32_t) + lane];

    const float hitT = *reinterpret_cast<const float *>(&payload[3]);
    const float baryU = *reinterpret_cast<const float *>(&payload[7]);
    const float baryV = *reinterpret_cast<const float *>(&payload[8]);
    std::printf("OWNED_D3D12_RAYTRACING_LANE lane=%u kind=%u primitive=%u instance=%u hit_t=%.6f "
                "callable=0x%08x local_root=0x%08x lane_field=%u bary=(%.6f,%.6f) dimensions=%u depth=%u "
                "recurse=%u nested_miss=%u anyhit_tag=0x%08x reject_tag=0x%08x\n",
                lane, payload[0], payload[1], payload[2], hitT, payload[4], payload[5], payload[6], baryU, baryV,
                payload[9], payload[10], payload[11], payload[12], payload[13], payload[14]);
    std::printf("OWNED_D3D12_RAYTRACING_LOG lane=%u anyhit=(0x%08x,0x%08x,0x%08x,0x%08x) closesthit=0x%08x "
                "miss=0x%08x callable=0x%08x\n",
                lane, anyHitLog[0], anyHitLog[1], anyHitLog[2], anyHitLog[3], closestHitLog, missLog, callableLog);
    for (UINT variant = 0; variant < expected.variantCount; ++variant) {
      const auto &admissible = expected.variants[variant];
      std::printf("OWNED_D3D12_RAYTRACING_VARIANT lane=%u index=%u primitive=%u hit_t=%.6f bary=(%.6f,%.6f) "
                  "anyhit_tag=0x%08x reject_tag=0x%08x anyhit=(0x%08x,0x%08x,0x%08x,0x%08x)\n",
                  lane, variant, admissible.primitive, admissible.hitT, admissible.baryU, admissible.baryV,
                  admissible.anyHitTag, admissible.rejectTag, admissible.anyHitLog[0], admissible.anyHitLog[1],
                  admissible.anyHitLog[2], admissible.anyHitLog[3]);
    }

    // Everything that holds for the lane regardless of which candidate was committed.
    const bool common = payload[0] == expected.kind && payload[2] == expected.instance &&
                        payload[4] == expected.callableValue && payload[5] == expected.localRoot &&
                        payload[6] == lane && payload[9] == kLaneCount && payload[10] == expected.depth &&
                        payload[11] == expected.recurse && payload[12] == expected.nestedMiss &&
                        closestHitLog == expected.closestHitLog && missLog == expected.missLog &&
                        callableLog == expected.callableLog;

    // The committed-hit identity plus the any-hit evidence, which is what the lane's
    // discriminating power rests on. A lane is a pass when ANY admissible variant matches:
    // where DXR defines one outcome there is exactly one variant, and where the candidate
    // enumeration order is unspecified each admissible order is listed, so the fixture never
    // asserts an order DXR does not promise.
    const auto variantMatches = [&](const RaytracingVariant &variant) {
      if (payload[1] != variant.primitive || payload[13] != variant.anyHitTag || payload[14] != variant.rejectTag)
        return false;
      if (std::fabs(hitT - variant.hitT) >= 0.001f)
        return false;
      if (std::fabs(baryU - variant.baryU) >= 0.01f || std::fabs(baryV - variant.baryV) >= 0.01f)
        return false;
      for (UINT slot = 0; slot < kAnyHitSlots; ++slot)
        if (anyHitLog[slot] != variant.anyHitLog[slot])
          return false;
      return true;
    };

    bool ok = false;
    for (UINT variant = 0; variant < expected.variantCount; ++variant)
      ok = ok || variantMatches(expected.variants[variant]);
    ok = ok && common;
    if (!ok) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_LANE_MISMATCH lane=%u\n", lane);
      ++failures;
    }
  }
  resources.readback->Unmap(0, nullptr);

  if (failures)
    return fail("raytracing readback mismatch");
  return 0;
}

} // namespace

#include "owned-d3d12-raytracing-contract.inc"
#include "owned-d3d12-raytracing-stateobj.inc"
#include "owned-d3d12-raytracing-asserialize.inc"
#include "owned-d3d12-raytracing-rayquery.inc"
#include "owned-d3d12-raytracing-rootargs.inc"
#include "owned-d3d12-raytracing-transitions.inc"
#include "owned-d3d12-raytracing-coverage.inc"
#include "owned-d3d12-raytracing-rayquery-stages.inc"
#include "owned-d3d12-raytracing-linkage.inc"
#include "owned-d3d12-raytracing-hitobject.inc"
#include "owned-d3d12-raytracing-paq.inc"
#include "owned-d3d12-raytracing-refit-chain.inc"

namespace {

int testCollectionStaticSamplers(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil) {
  const char *name = "collection-local-static-sampler";
  ComPtr<ID3D12Device> device;
  check(device5->QueryInterface(IID_PPV_ARGS(&device)), "collection sampler: device");
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                               D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, 0};
  D3D12_ROOT_PARAMETER1 params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  D3D12_ROOT_SIGNATURE_DESC1 globalDesc{};
  globalDesc.NumParameters = 2;
  globalDesc.pParameters = params;
  auto globalRoot = createRootSignature(device.Get(), globalDesc, "collection sampler: global root");
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC1 localDesc{};
  localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
  localDesc.NumStaticSamplers = 1;
  localDesc.pStaticSamplers = &sampler;
  auto localRoot = createRootSignature(device.Get(), localDesc, "collection sampler: local root");
  localDesc.NumStaticSamplers = 0;
  auto emptyLocal = createRootSignature(device.Get(), localDesc, "collection sampler: missing sampler root");
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  globalDesc.NumStaticSamplers = 1;
  globalDesc.pStaticSamplers = &sampler;
  auto conflictRoot = createRootSignature(device.Get(), globalDesc, "collection sampler: conflicting root");
  globalDesc.NumStaticSamplers = 0;
  range.BaseShaderRegister = 1;
  auto missingSrvRoot = createRootSignature(device.Get(), globalDesc, "collection sampler: missing SRV root");
  range.BaseShaderRegister = 0;
  globalDesc.NumParameters = 1;
  auto missingUavRoot = createRootSignature(device.Get(), globalDesc, "collection sampler: missing UAV root");

  D3D12_EXPORT_DESC exportDesc{L"CollectionSamplerRayGen", nullptr, D3D12_EXPORT_FLAG_NONE};
  D3D12_DXIL_LIBRARY_DESC library{{dxil.data(), dxil.size()}, 1, &exportDesc};
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
  ID3D12RootSignature *global = globalRoot.Get(), *local = localRoot.Get();
  D3D12_STATE_SUBOBJECT subobjects[] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
  };
  D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(std::size(subobjects)), subobjects};
  ComPtr<ID3D12StateObject> collection;
  const HRESULT collectionHr = device5->CreateStateObject(&desc, IID_PPV_ARGS(&collection));
  int bad = stateobj::mismatch(name, "local_only_collection_hr", uint32_t(collectionHr), uint32_t(S_OK));
  std::printf("OWNED_D3D12_RAYTRACING_COLLECTION_SAMPLER field=local_only_collection_hr hr=0x%08x expected=0x%08x\n",
              uint32_t(collectionHr), uint32_t(S_OK));
  struct Negative {
    const char *field;
    ID3D12RootSignature *global;
    ID3D12RootSignature *local;
  };
  const Negative negatives[] = {
      {"missing_sampler_hr", globalRoot.Get(), emptyLocal.Get()},
      {"conflicting_sampler_hr", conflictRoot.Get(), localRoot.Get()},
      {"missing_srv_hr", missingSrvRoot.Get(), localRoot.Get()},
      {"missing_uav_hr", missingUavRoot.Get(), localRoot.Get()},
  };
  for (const auto &negative : negatives) {
    global = negative.global;
    local = negative.local;
    ComPtr<ID3D12StateObject> rejected;
    const HRESULT hr = device5->CreateStateObject(&desc, IID_PPV_ARGS(&rejected));
    bad += stateobj::mismatch(name, negative.field, uint32_t(hr), uint32_t(E_INVALIDARG));
    std::printf("OWNED_D3D12_RAYTRACING_COLLECTION_SAMPLER field=%s hr=0x%08x expected=0x%08x\n",
                negative.field, uint32_t(hr), uint32_t(E_INVALIDARG));
  }
  // Each export keeps its own global parameter indices, even within one collection.
  D3D12_ROOT_PARAMETER1 reversedParams[] = {params[1], params[0]};
  globalDesc.NumParameters = 2;
  globalDesc.pParameters = reversedParams;
  auto reversedRoot = createRootSignature(device.Get(), globalDesc, "collection sampler: reversed global root");
  ID3D12RootSignature *firstGlobal = globalRoot.Get(), *secondGlobal = reversedRoot.Get();
  D3D12_EXPORT_DESC distinctExports[] = {
      {L"CollectionSamplerRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"CollectionSamplerRenamed", L"CollectionSamplerRayGen", D3D12_EXPORT_FLAG_NONE},
  };
  D3D12_DXIL_LIBRARY_DESC distinctLibrary{{dxil.data(), dxil.size()}, 2, distinctExports};
  const WCHAR *firstName[] = {L"CollectionSamplerRayGen"}, *secondName[] = {L"CollectionSamplerRenamed"};
  local = localRoot.Get();
  D3D12_STATE_SUBOBJECT distinctSubobjects[] = {
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &distinctLibrary},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &firstGlobal},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &secondGlobal},
      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, nullptr},
      {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, nullptr},
  };
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION firstAssociation{&distinctSubobjects[1], 1, firstName};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION secondAssociation{&distinctSubobjects[2], 1, secondName};
  distinctSubobjects[6].pDesc = &firstAssociation;
  distinctSubobjects[7].pDesc = &secondAssociation;
  D3D12_STATE_OBJECT_DESC distinctDesc{D3D12_STATE_OBJECT_TYPE_COLLECTION,
                                      UINT(std::size(distinctSubobjects)), distinctSubobjects};
  ComPtr<ID3D12StateObject> distinctCollection;
  const HRESULT distinctHr = device5->CreateStateObject(&distinctDesc, IID_PPV_ARGS(&distinctCollection));
  bad += stateobj::mismatch(name, "distinct_global_roots_hr", uint32_t(distinctHr), uint32_t(S_OK));
  std::printf("OWNED_D3D12_RAYTRACING_COLLECTION_SAMPLER field=distinct_global_roots_hr hr=0x%08x expected=0x%08x\n",
              uint32_t(distinctHr), uint32_t(S_OK));
  if (!collection) {
    std::printf("OWNED_D3D12_RAYTRACING_COLLECTION_SAMPLER_CASE %s status=FAIL\n", name);
    return 1;
  }
  D3D12_EXISTING_COLLECTION_DESC existing{collection.Get(), 0, nullptr};
  D3D12_STATE_SUBOBJECT import{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing};
  D3D12_STATE_OBJECT_DESC pipelineDesc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 1, &import};
  ComPtr<ID3D12StateObject> pipeline;
  check(device5->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&pipeline)), "collection sampler: import pipeline");
  ComPtr<ID3D12StateObjectProperties> properties;
  check(pipeline.As(&properties), "collection sampler: properties");
  const void *identifier = properties->GetShaderIdentifier(L"CollectionSamplerRayGen");
  require(identifier != nullptr, "collection sampler: identifier");
  auto table = createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  std::memcpy(mapUpload(table.Get()), identifier, kShaderIdentifierSize);
  table->Unmap(0, nullptr);
  D3D12_RESOURCE_DESC textureDesc{};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = 2;
  textureDesc.Height = textureDesc.DepthOrArraySize = textureDesc.MipLevels = 1;
  textureDesc.Format = DXGI_FORMAT_R32_FLOAT;
  textureDesc.SampleDesc.Count = 1;
  const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
        "collection sampler: texture");
  auto upload = createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  const uint32_t uploadWords[] = {0x3e800000u, 0x3f400000u, 0xffffffffu, 0xffffffffu};
  std::memcpy(mapUpload(upload.Get()), uploadWords, sizeof(uploadWords));
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = upload.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 2, 1, 1, 256};
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = texture.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ctx.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  transition(ctx.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "collection sampler: heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(texture.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());
  auto output = createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  // Initialize to values different from either sample, so a dropped dispatch fails.
  ctx.list->CopyBufferRegion(output.Get(), 0, upload.Get(), 8, 8);
  transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  ctx.list->SetDescriptorHeaps(1, heaps);
  ctx.list->SetComputeRootSignature(globalRoot.Get());
  ctx.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  ctx.list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  ctx.list->SetPipelineState1(pipeline.Get());
  D3D12_DISPATCH_RAYS_DESC dispatch{};
  dispatch.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), kShaderIdentifierSize};
  dispatch.Width = 2;
  dispatch.Height = dispatch.Depth = 1;
  ctx.list->DispatchRays(&dispatch);
  transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 8);
  ctx.submitAndWait();
  check(ctx.allocator->Reset(), "collection sampler: allocator reset");
  check(ctx.list->Reset(ctx.allocator.Get(), nullptr), "collection sampler: list reset");
  void *data = nullptr;
  check(readback->Map(0, nullptr, &data), "collection sampler: readback");
  const auto *words = static_cast<const uint32_t *>(data);
  bad += stateobj::mismatch(name, "sample_inside", words[0], 0x3e800000u);
  bad += stateobj::mismatch(name, "sample_clamp", words[1], 0x3f400000u);
  std::printf("OWNED_D3D12_RAYTRACING_COLLECTION_SAMPLER samples=0x%08x,0x%08x\n", words[0], words[1]);
  readback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_RAYTRACING_COLLECTION_SAMPLER_CASE %s status=%s\n", name, bad ? "FAIL" : "PASS");
  std::fflush(stdout);
  return bad ? 1 : 0;
}

// Two 36 KiB AS table snapshots exceed one 64 KiB upload block. The following TLAS
// uses a nonzero slice in the second block. Read the public visualization payload
// too: native traversal alone does not prove the geometry/job shadow copies survived.
int testMetadataArena(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil) {
  constexpr UINT geometryCount = 256;
  constexpr UINT triangleWords = 9;
  contract::Runner run(device5, ctx, dxil);
  std::array<std::vector<float>, 2> vertices;
  std::array<ComPtr<ID3D12Resource>, 2> bottom;
  std::array<D3D12_RAYTRACING_INSTANCE_DESC, 2> instances{};
  std::vector<contract::LaneRay> rays;
  std::vector<contract::Expect> expects;
  const auto identity = contract::translateScale(1, 0, 0, 0);
  for (UINT b = 0; b < bottom.size(); ++b) {
    const float y = 4.0f * b, z = 2.0f * b;
    vertices[b].resize(geometryCount * triangleWords);
    for (UINT g = 0; g < geometryCount; ++g) {
      const float x = 4.0f * g;
      const float triangle[] = {x, y, z, x + 1, y, z, x, y + 1, z};
      std::memcpy(vertices[b].data() + g * triangleWords, triangle, sizeof(triangle));
    }
    auto input = run.upload(vertices[b].data(), vertices[b].size() * sizeof(float));
    std::array<D3D12_RAYTRACING_GEOMETRY_DESC, geometryCount> geometries{};
    for (UINT g = 0; g < geometryCount; ++g)
      geometries[g] = contract::triangles(input->GetGPUVirtualAddress() + g * triangleWords * sizeof(float),
          12, 3, DXGI_FORMAT_R32G32B32_FLOAT, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
    bottom[b] = run.build(contract::blasInputs(geometries.data(), geometryCount), "metadata arena: BLAS");
    instances[b] = contract::instanceDesc(identity.m, 0x70 + b, 0xff, 0, 0, bottom[b]->GetGPUVirtualAddress());
    for (const UINT g : {0u, geometryCount - 1}) {
      rays.push_back(contract::ray(4.0f * g + 0.25f, y + 0.25f, -1, 0, 0, 1, 0, 0xff, 0, 0));
      expects.push_back(contract::triangleHit(z + 1, 0.25f, 0.25f, contract::kHitGroupTagBase, 0x70 + b, b, g));
    }
  }
  auto instanceBuffer = run.upload(instances.data(), sizeof(instances));
  auto top = run.build(contract::tlasInputs(instanceBuffer->GetGPUVirtualAddress(), UINT(instances.size())),
      "metadata arena: TLAS");
  const auto words = run.trace("metadata-arena", top->GetGPUVirtualAddress(), rays);
  int bad = 0;
  constexpr UINT observedFields[] = {0, 1, 2, 3, 4, 5, 6, 7, 45, 46, 47, 48, 62};
  for (UINT lane = 0; lane < rays.size(); ++lane) {
    bad += run.expect("metadata-arena", words, lane, expects[lane]);
    std::printf("OWNED_D3D12_RAYTRACING_METADATA_ARENA_RAY lane=%u words=", lane);
    for (UINT i = 0; i < std::size(observedFields); ++i)
      std::printf("%s%08x", i ? "," : "", words[lane * contract::kSlotWords + observedFields[i]]);
    std::putchar('\n');
  }
  for (UINT b = 0; b < bottom.size(); ++b) {
    auto output = run.uavBuffer(asserialize::query(run, bottom[b].Get(), 1));
    ctx.list->CopyRaytracingAccelerationStructure(output->GetGPUVirtualAddress(), bottom[b]->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
    uavBarrier(ctx.list.Get(), output.Get());
    const auto bytes = asserialize::read(run, output.Get(), output->GetDesc().Width);
    require(bytes.size() >= 8 + geometryCount * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC),
        "metadata arena: truncated visualization");
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    int descriptorsBad = header.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL ||
        header.NumDescs != geometryCount;
    std::vector<uint32_t> observed(geometryCount * triangleWords, contract::kFill);
    const auto base = output->GetGPUVirtualAddress();
    for (UINT g = 0; g < geometryCount; ++g) {
      D3D12_RAYTRACING_GEOMETRY_DESC desc{};
      std::memcpy(&desc, bytes.data() + 8 + g * sizeof(desc), sizeof(desc));
      const auto &tri = desc.Triangles;
      descriptorsBad += desc.Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES ||
          desc.Flags != D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE || tri.Transform3x4 != 0 ||
          tri.IndexFormat != DXGI_FORMAT_UNKNOWN || tri.VertexFormat != DXGI_FORMAT_R32G32B32_FLOAT ||
          tri.IndexCount != 0 || tri.VertexCount != 3 || tri.IndexBuffer != 0 || tri.VertexBuffer.StrideInBytes != 12;
      const auto address = tri.VertexBuffer.StartAddress;
      const UINT size = triangleWords * sizeof(float);
      if (address < base || address - base > bytes.size() || size > bytes.size() - (address - base)) {
        ++descriptorsBad;
        continue;
      }
      std::memcpy(observed.data() + g * triangleWords, bytes.data() + (address - base), size);
    }
    bad += descriptorsBad;
    bad += std::memcmp(observed.data(), vertices[b].data(), vertices[b].size() * sizeof(float)) != 0;
    std::printf("OWNED_D3D12_RAYTRACING_METADATA_ARENA_SHADOW blas=%u descriptors=%u descriptor_mismatches=%d words=",
        b, geometryCount, descriptorsBad);
    for (UINT i = 0; i < observed.size(); ++i)
      std::printf("%s%08x", i ? "," : "", observed[i]);
    std::putchar('\n');
  }
  // TLAS native traversal reads sourceInstances separately. Its public shadow
  // copy is the consumer that proves the nonzero-offset job table was bound.
  auto topOutput = run.uavBuffer(asserialize::query(run, top.Get(), 1));
  ctx.list->CopyRaytracingAccelerationStructure(topOutput->GetGPUVirtualAddress(), top->GetGPUVirtualAddress(),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
  uavBarrier(ctx.list.Get(), topOutput.Get());
  const auto topBytes = asserialize::read(run, topOutput.Get(), topOutput->GetDesc().Width);
  require(topBytes.size() >= 8 + sizeof(instances), "metadata arena: truncated TLAS visualization");
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER topHeader{};
  std::memcpy(&topHeader, topBytes.data(), sizeof(topHeader));
  bad += topHeader.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL ||
      topHeader.NumDescs != instances.size();
  bad += std::memcmp(topBytes.data() + 8, instances.data(), sizeof(instances)) != 0;
  std::printf("OWNED_D3D12_RAYTRACING_METADATA_ARENA_TOP type=%u descriptors=%u\n",
      UINT(topHeader.Type), topHeader.NumDescs);
  for (UINT b = 0; b < instances.size(); ++b) {
    std::array<uint32_t, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) / sizeof(uint32_t)> observed{};
    std::memcpy(observed.data(), topBytes.data() + 8 + b * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), sizeof(observed));
    // Visualization preserves the source BLAS addresses; they are not rebased
    // into topOutput like the BLAS visualization's vertex payload addresses.
    std::printf("OWNED_D3D12_RAYTRACING_METADATA_ARENA_INSTANCE index=%u blas_address=%016llx words=",
        b, static_cast<unsigned long long>(bottom[b]->GetGPUVirtualAddress()));
    for (UINT i = 0; i < observed.size(); ++i)
      std::printf("%s%08x", i ? "," : "", observed[i]);
    std::putchar('\n');
  }
  std::printf("OWNED_D3D12_RAYTRACING_METADATA_ARENA_RESULT rays=4 blas=2 geometries=512 vertex_bytes=18432 "
      "instances=2 instance_bytes=128 failures=%d\n", bad);
  if (!bad) std::puts("OWNED_D3D12_RAYTRACING_METADATA_ARENA_PASS");
  return bad ? 1 : 0;
}

int run(int argc, wchar_t **argv) {
  if (argc < 4 && !(argc == 3 && (std::wcscmp(argv[1], L"--collection-samplers") == 0 ||
                                std::wcscmp(argv[1], L"--refit-chain") == 0 ||
                                std::wcscmp(argv[1], L"--metadata-arena") == 0))) {
    std::fprintf(stderr, "usage: owned-d3d12-raytracing.exe <raytracing.dxil> <raytracing-contract.dxil> "
                         "<raytracing-stateobj.dxil> [further suite DXIL]\n"
                         "       owned-d3d12-raytracing.exe --collection-samplers <raytracing-rootargs.dxil>\n"
                         "       owned-d3d12-raytracing.exe --transitions <raytracing-rootargs.dxil> "
                         "<raytracing-transition-cs.dxil>\n"
                         "       owned-d3d12-raytracing.exe --metadata-arena <raytracing-contract.dxil>\n"
                         "       owned-d3d12-raytracing.exe --refit-chain <raytracing-contract.dxil>\n");
    return 2;
  }

  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  if (!d3d12)
    return fail("GetModuleHandleW(d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));
  using BackendIdFn = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"));
  if (!backendId || !backendId() || std::strcmp(backendId(), kExpectedBackendId) != 0)
    return fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_RAYTRACING_BACKEND id=%s\n", backendId());
  std::fflush(stdout);

  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

  // Reported for evidence only. The tier is deliberately not asserted: it is a
  // capability advertisement, not the ray tracing contract this fixture proves, and
  // gating on it would turn an unimplemented dispatch path into a silent skip.
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  const HRESULT optionsHr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
  std::printf("OWNED_D3D12_RAYTRACING_OPTIONS5 hr=0x%08lx raytracing_tier=%u\n",
              static_cast<unsigned long>(optionsHr), static_cast<unsigned>(options5.RaytracingTier));

  // The ray tracing entry point is ID3D12Device5; a device that cannot expose it
  // cannot run this workload at all.
  ComPtr<ID3D12Device5> device5;
  check(device.As(&device5), "QueryInterface(ID3D12Device5)");

  Context context(device.Get());
  if (argc == 4 && std::wcscmp(argv[1], L"--transitions") == 0)
    return rttransition::run(device5.Get(), context, readFile(argv[2]), readFile(argv[3]));
  if (argc == 3 && std::wcscmp(argv[1], L"--refit-chain") == 0)
    return refitchain::runAll(device5.Get(), context, readFile(argv[2]));
  if (argc == 3 && std::wcscmp(argv[1], L"--metadata-arena") == 0)
    return testMetadataArena(device5.Get(), context, readFile(argv[2]));
  if (argc == 3 && std::wcscmp(argv[1], L"--collection-samplers") == 0)
    return testCollectionStaticSamplers(device5.Get(), context, readFile(argv[2]));
  if (argc == 4 && std::wcscmp(argv[1], L"--hitobject") == 0) {
    contract::Runner geometry(device5.Get(), context, readFile(argv[2]));
    return hitobject::run(device5.Get(), context, readFile(argv[3]), geometry);
  }
  if (argc == 4 && std::wcscmp(argv[1], L"--atomic64") == 0) {
    contract::Runner geometry(device5.Get(), context, readFile(argv[2]));
    return hitobject::runAtomic(device5.Get(), context, readFile(argv[3]), geometry);
  }
  if (argc == 4 && std::wcscmp(argv[1], L"--paq") == 0) {
    contract::Runner geometry(device5.Get(), context, readFile(argv[2]));
    return paq::run(device5.Get(), context, readFile(argv[3]), geometry);
  }

  const auto dxil = readFile(argv[1]);

  const auto contractDxil = readFile(argv[2]);
  const auto stateobjDxil = readFile(argv[3]);

  int failures = testRaytracing(device5.Get(), context, dxil);
  // The base workload leaves its list closed; the contract cases record on a fresh one.
  check(context.allocator->Reset(), "CommandAllocator::Reset(contract)");
  check(context.list->Reset(context.allocator.Get(), nullptr), "CommandList::Reset(contract)");
  failures += contract::runAll(device5.Get(), context, contractDxil);
  {
    // The state-object cases build their scenes through a contract runner's geometry helpers.
    contract::Runner geometry(device5.Get(), context, contractDxil);
    failures += stateobj::runAll(device5.Get(), context, stateobjDxil, geometry);
  }
  failures += asserialize::runAll(device5.Get(), context, contractDxil);
  if (argc < 10) {
    std::fprintf(stderr,
                 "OWNED_D3D12_RAYTRACING_FAIL step=rayquery: missing raytracing-rayquery-{cs,vs,fs,ps,lib,args}.dxil\n");
    ++failures;
  } else {
    failures += rayquery::runAll(device5.Get(), context, readFile(argv[4]), readFile(argv[5]), readFile(argv[6]),
                                 readFile(argv[7]), readFile(argv[8]), readFile(argv[9]));
  }
  if (argc < 12) {
    std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=rootargs: missing raytracing-rootargs.dxil\n");
    ++failures;
  } else {
    const auto rootargsDxil = readFile(argv[11]);
    failures += rootargs::run(device5.Get(), context, rootargsDxil);
    if (argc < 27) {
      std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=transitions: missing raytracing-transition-cs.dxil\n");
      ++failures;
    } else {
      failures += rttransition::run(device5.Get(), context, rootargsDxil, readFile(argv[26]));
    }
    failures += testCollectionStaticSamplers(device5.Get(), context, rootargsDxil);
  }
  if (argc < 11) {
    std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=coverage: missing raytracing-coverage.dxil\n");
    ++failures;
  } else {
    contract::Runner geometry(device5.Get(), context, contractDxil);
    failures += coverage::runAll(device5.Get(), context, readFile(argv[10]), geometry);
  }
  if (argc < 22) {
    std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=rayquery-stages: missing frozen stage DXIL\n");
    ++failures;
  } else {
    failures += rayquery_stages::runAll(device5.Get(), context, argv);
  }
  if (argc < 25) {
    std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=linkage: missing frozen function libraries\n");
    ++failures;
  } else {
    contract::Runner geometry(device5.Get(), context, contractDxil);
    failures += linkage::run(device5.Get(), context, stateobjDxil, geometry, readFile(argv[22]),
                             readFile(argv[23]), readFile(argv[24]));
  }
  if (argc < 26) {
    std::fprintf(stderr, "OWNED_D3D12_RAYTRACING_FAIL step=hitobject: missing raytracing-hitobject.dxil\n");
    ++failures;
  } else {
    contract::Runner geometry(device5.Get(), context, contractDxil);
    failures += hitobject::run(device5.Get(), context, readFile(argv[25]), geometry);
  }
  if (!failures)
    std::puts("OWNED_D3D12_RAYTRACING_PASS");
  return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  return run(argc, argv);
}
