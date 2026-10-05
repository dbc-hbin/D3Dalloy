// Public contracts: VulkanOn12 Triangle fan / D3D9 Triangle Fans and
// D3D12GPUUploadHeaps (tools-only manual tracking). No translator-derived oracle.
constexpr D3D_PRIMITIVE_TOPOLOGY kP4TriangleFan = static_cast<D3D_PRIMITIVE_TOPOLOGY>(6);
const D3D12_HEAP_FLAGS kP4ManualTracking = static_cast<D3D12_HEAP_FLAGS>(0x2000);
constexpr GUID kP4ManualTrackingIID = {0x86ca3b85, 0x49ad, 0x4b6e,
                                     {0xae, 0xd5, 0xed, 0xdb, 0x18, 0x54, 0x0f, 0x41}};

constexpr const char *p4FanShader = R"(
  struct V { float4 position : SV_Position; nointerpolation float tag : TAG;
             nointerpolation float instance : INSTANCE; nointerpolation float3 order : ORDER; };
  V vs(float2 position : POSITION, float instance : INSTANCE, uint id : SV_VertexID) {
    V v; v.position=float4(position,.25,1); v.tag=id+1; v.instance=instance;
    v.order=0; return v;
  }
  [maxvertexcount(3)]
  void gs(triangle V input[3], inout TriangleStream<V> stream) {
    [unroll] for (uint i=0;i<3;++i) {
      V v=input[i]; v.order=float3(input[0].tag,input[1].tag,input[2].tag);
      stream.Append(v);
    }
    stream.RestartStrip();
  }
  float4 ps(V v, bool front : SV_IsFrontFace) : SV_Target {
    return float4(v.tag,v.instance,front ? 1 : 0,1);
  }
  float4 ps_gs(V v) : SV_Target { return float4(v.order,1); }
)";

void testTriangleFan(Context &c) {
  c.setQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto root = c.root();
  auto vs = shader(p4FanShader, "vs", "vs_5_0");
  auto ps = shader(p4FanShader, "ps", "ps_5_0");
  auto gs = shader(p4FanShader, "gs", "gs_5_0");
  auto ps_gs = shader(p4FanShader, "ps_gs", "ps_5_0");
  struct Point { float x, y; };
  const Point shape[] = {{4,4},{1,1},{7,1},{7,7},{1,7},{1,1}};
  std::vector<Point> points(65536, {-16,-16});
  for (UINT i = 0; i < 6; ++i) points[3+i] = shape[i];
  // With strip cut disabled, 0xffff is an ordinary, addressable 16-bit index.
  points[0] = shape[0]; points[1] = shape[1]; points[65535] = shape[2];
  std::vector<uint8_t> vertex_bytes(points.size() * sizeof(Point));
  for (size_t i = 0; i < points.size(); ++i) {
    const Point ndc{points[i].x / 4 - 1, 1 - points[i].y / 4};
    std::memcpy(vertex_bytes.data() + i * sizeof(Point), &ndc, sizeof(ndc));
  }
  auto vertices = c.buffer(vertex_bytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(vertices.Get(), vertex_bytes);
  const float instance_values[] = {11,22,33,44,55,66};
  std::vector<uint8_t> instance_bytes(sizeof(instance_values));
  std::memcpy(instance_bytes.data(), instance_values, sizeof(instance_values));
  auto instances = c.buffer(instance_bytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(instances.Get(), instance_bytes);
  const D3D12_VERTEX_BUFFER_VIEW views[] = {
    {vertices->GetGPUVirtualAddress(), UINT(vertex_bytes.size()), sizeof(Point)},
    {instances->GetGPUVirtualAddress(), UINT(instance_bytes.size()), sizeof(float)},
  };
  const D3D12_INPUT_ELEMENT_DESC layout[] = {
    {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    {"INSTANCE",0,DXGI_FORMAT_R32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},
  };
  enum class Path { Direct, Indexed16, Indexed32, Indirect, IndirectIndexed, Bundle, BundleIndexed };
  struct Row {
    const char *name;
    Path path;
    std::vector<UINT> indices;
    UINT count, start, instances, first_instance;
    INT base;
    bool restart, geometry, reverse_front;
    UINT draws;
  };
  const UINT cut = UINT_MAX;
  const std::vector<UINT> fan{0,1,2,3,4,5};
  // Each row adds a different public boundary or execution path, not a Cartesian product.
  const Row rows[] = {
    {"direct_empty",Path::Direct,{},0,3,1,0,0,false,false,false,1},
    {"direct_one",Path::Direct,{},1,3,1,0,0,false,false,false,1},
    {"direct_two",Path::Direct,{},2,3,1,0,0,false,false,false,1},
    {"direct_triangle",Path::Direct,{},3,3,1,0,0,false,false,false,1},
    {"direct_start_instances",Path::Direct,{},6,3,2,2,0,false,false,false,1},
    {"direct_zero_instances",Path::Direct,{},6,3,0,0,0,false,false,false,1},
    {"indexed16_offset_base",Path::Indexed16,fan,6,2,1,1,3,false,false,false,1},
    {"indexed32_negative_base",Path::Indexed32,{5,6,7,8,9,10},6,2,1,0,-2,false,false,false,1},
    {"indexed16_restart",Path::Indexed16,{cut,0,1,cut,cut,0,1,2,3,cut,0,3,4,5,cut},15,2,1,0,3,true,false,false,1},
    {"indexed32_restart_gs",Path::Indexed32,{0,1,2,cut,0,2,3,4,5,cut,1},11,2,1,0,3,true,true,false,1},
    {"indexed16_cut_disabled",Path::Indexed16,{0,1,65535},3,2,1,0,0,false,false,false,1},
    {"indirect_draw",Path::Indirect,{},6,3,2,1,0,false,false,false,1},
    {"indirect_indexed_count",Path::IndirectIndexed,{0,1,2,cut,0,2,3,4,5},9,2,1,0,3,true,false,false,2},
    {"bundle_direct_twice",Path::Bundle,{},6,3,1,0,0,false,false,false,2},
    {"bundle_indexed16",Path::BundleIndexed,fan,6,2,1,0,3,false,false,false,1},
    {"gs_input_order",Path::Direct,{},6,3,1,0,0,false,true,false,1},
    {"backface_culled",Path::Indexed32,fan,6,2,1,0,3,false,false,true,1},
    {"degenerate_count",Path::Indexed32,{0,1,1,2,2,3},6,2,1,0,3,false,false,false,1},
  };
  UINT total_pixels = 0, total_primitives = 0;
  for (const auto &row : rows) {
    const bool indexed = row.path == Path::Indexed16 || row.path == Path::Indexed32 ||
        row.path == Path::IndirectIndexed || row.path == Path::BundleIndexed;
    const bool index16 = row.path == Path::Indexed16 || row.path == Path::BundleIndexed;
    auto desc = pipelineDesc(root.Get(), vs.Get(), row.geometry ? ps_gs.Get() : ps.Get());
    desc.InputLayout = {layout, UINT(std::size(layout))};
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = row.reverse_front;
    desc.IBStripCutValue = !row.restart ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED :
        index16 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    if (row.geometry) desc.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    check(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "triangle fan PSO");
    ComPtr<ID3D12Resource> indices;
    D3D12_INDEX_BUFFER_VIEW ib{};
    if (indexed) {
      const UINT stride = index16 ? 2 : 4;
      // Both a view byte offset and StartIndexLocation must be honored.
      std::vector<uint8_t> bytes(8 + (row.start + row.count) * stride, 0);
      for (UINT i = 0; i < row.count; ++i) {
        const UINT value = row.indices[i];
        if (index16) { const UINT16 small = UINT16(value); std::memcpy(bytes.data()+8+(row.start+i)*stride,&small,2); }
        else std::memcpy(bytes.data()+8+(row.start+i)*stride,&value,4);
      }
      indices = c.buffer(bytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      upload(indices.Get(), bytes);
      ib = {indices->GetGPUVirtualAddress()+8, UINT(bytes.size()-8), index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    }
    ComPtr<ID3D12CommandSignature> signature;
    ComPtr<ID3D12Resource> arguments, count_buffer;
    const bool indirect = row.path == Path::Indirect || row.path == Path::IndirectIndexed;
    if (indirect) {
      D3D12_INDIRECT_ARGUMENT_DESC arg{};
      arg.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
      const UINT stride = indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
      D3D12_COMMAND_SIGNATURE_DESC sd{stride,1,&arg,0};
      check(c.device->CreateCommandSignature(&sd,nullptr,IID_PPV_ARGS(&signature)), "fan indirect signature");
      std::vector<uint8_t> bytes(16 + stride * 3, 0);
      for (UINT i = 0; i < 3; ++i) {
        if (indexed) {
          const D3D12_DRAW_INDEXED_ARGUMENTS a{row.count,row.instances,row.start,row.base,row.first_instance};
          std::memcpy(bytes.data()+16+i*stride,&a,sizeof(a));
        } else {
          const D3D12_DRAW_ARGUMENTS a{row.count,row.instances,row.start,row.first_instance};
          std::memcpy(bytes.data()+16+i*stride,&a,sizeof(a));
        }
      }
      arguments = c.buffer(bytes.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
      upload(arguments.Get(),bytes);
      std::vector<uint8_t> counts(8,0); std::memcpy(counts.data()+4,&row.draws,4);
      count_buffer = c.buffer(8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
      upload(count_buffer.Get(),counts);
    }
    auto bind = [&](ID3D12GraphicsCommandList *list) {
      list->SetGraphicsRootSignature(root.Get());
      list->IASetPrimitiveTopology(kP4TriangleFan);
      list->IASetVertexBuffers(0,2,views);
      if (indexed) list->IASetIndexBuffer(&ib);
    };
    auto draw = [&](ID3D12GraphicsCommandList *list) {
      if (indexed) list->DrawIndexedInstanced(row.count,row.instances,row.start,row.base,row.first_instance);
      else list->DrawInstanced(row.count,row.instances,row.start,row.first_instance);
    };
    Context::Recording bundle;
    const bool bundled = row.path == Path::Bundle || row.path == Path::BundleIndexed;
    if (bundled) {
      check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE,IID_PPV_ARGS(&bundle.allocator)), "fan bundle allocator");
      check(c.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_BUNDLE,bundle.allocator.Get(),pso.Get(),
                                       IID_PPV_ARGS(&bundle.list)), "fan bundle list");
      bind(bundle.list.Get()); draw(bundle.list.Get()); check(bundle.list->Close(), "fan bundle Close");
    }
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,1,0};
    ComPtr<ID3D12QueryHeap> query;
    check(c.device->CreateQueryHeap(&qd,IID_PPV_ARGS(&query)), "fan statistics heap");
    auto statistics = c.buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),D3D12_HEAP_TYPE_READBACK);
    const D3D12_QUERY_HEAP_DESC od{D3D12_QUERY_HEAP_TYPE_OCCLUSION,1,0};
    ComPtr<ID3D12QueryHeap> occlusion;
    check(c.device->CreateQueryHeap(&od,IID_PPV_ARGS(&occlusion)), "fan occlusion heap");
    auto samples = c.buffer(sizeof(UINT64),D3D12_HEAP_TYPE_READBACK);
    const auto pixels = render(c,pso.Get(),root.Get(),{0,0,8,8,0,1},{0,0,0,0},
        [&](ID3D12GraphicsCommandList *list) {
          bind(list);
          list->BeginQuery(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
          list->BeginQuery(occlusion.Get(),D3D12_QUERY_TYPE_OCCLUSION,0);
          if (indirect) list->ExecuteIndirect(signature.Get(),3,arguments.Get(),16,count_buffer.Get(),4);
          else for (UINT i = 0; i < row.draws; ++i) {
            if (bundled) list->ExecuteBundle(bundle.list.Get()); else draw(list);
          }
          list->EndQuery(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
          list->EndQuery(occlusion.Get(),D3D12_QUERY_TYPE_OCCLUSION,0);
          list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,statistics.Get(),0);
          list->ResolveQueryData(occlusion.Get(),D3D12_QUERY_TYPE_OCCLUSION,0,1,samples.Get(),0);
        },true);
    // D3D9's normative fan order is (rim[i], rim[i+1], center), not center-first.
    // Assemble only from our public draw arguments and CPU input, never GPU results.
    // D3D11.3 FunctionalSpec 8.16: SV_VertexID starts at zero for non-indexed
    // draws and is the raw index for indexed draws. Neither includes the fetch base.
    // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#VertexID
    struct FanVertex { UINT fetch, shader_id; };
    std::vector<std::array<FanVertex,3>> triangles;
    std::vector<FanVertex> segment;
    for (UINT i = 0; i <= row.count; ++i) {
      const bool end = i == row.count || (indexed && row.restart && row.indices[i] == cut);
      if (end) {
        for (size_t j = 1; j + 1 < segment.size(); ++j)
          triangles.push_back({segment[j],segment[j+1],segment[0]});
        segment.clear();
      } else {
        const INT64 vertex = indexed ? INT64(row.indices[i])+row.base : INT64(row.start)+i;
        require(vertex >= 0 && UINT64(vertex) < points.size(), "CPU fan input bounds");
        segment.push_back({UINT(vertex), indexed ? row.indices[i] : i});
      }
    }
    const UINT64 primitive_count = triangles.size() * row.instances * row.draws;
    const auto query_bytes = readback(statistics.Get(),sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    D3D12_QUERY_DATA_PIPELINE_STATISTICS stats{}; std::memcpy(&stats,query_bytes.data(),sizeof(stats));
    require(stats.IAPrimitives == primitive_count, "fan IA primitive count including degenerates");
    require(stats.GSInvocations == (row.geometry ? primitive_count : 0), "fan GS invocation count");
    if (row.geometry) require(stats.GSPrimitives == primitive_count, "fan GS output primitive count");
    UINT checked = 0, colored = 0;
    UINT64 expected_samples = 0;
    for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
      const Point sample{float(x)+.5f,float(y)+.5f};
      std::array<float,4> wanted{};
      for (const auto &tri : triangles) {
        const Point a = points[tri[0].fetch], b = points[tri[1].fetch], d = points[tri[2].fetch];
        const float area = (b.x-a.x)*(d.y-a.y)-(b.y-a.y)*(d.x-a.x);
        if (area == 0 || !row.instances || (area > 0) == row.reverse_front) continue;
        const float e[] = {(b.x-a.x)*(sample.y-a.y)-(b.y-a.y)*(sample.x-a.x),
                           (d.x-b.x)*(sample.y-b.y)-(d.y-b.y)*(sample.x-b.x),
                           (a.x-d.x)*(sample.y-d.y)-(a.y-d.y)*(sample.x-d.x)};
        // D3D single-sample top-left rule, in screen coordinates (Y down).
        // Integer vertices and half-integer sample centers need no tolerance.
        const Point starts[] = {a,b,d}, ends[] = {b,d,a};
        bool inside = true;
        for (UINT j = 0; j < 3; ++j) {
          const float sign = area > 0 ? 1.0f : -1.0f;
          const float dx = (ends[j].x-starts[j].x)*sign;
          const float dy = (ends[j].y-starts[j].y)*sign;
          const bool top_left = dy < 0 || (dy == 0 && dx > 0);
          inside = inside && (e[j]*sign > 0 || (e[j] == 0 && top_left));
        }
        if (inside) {
          expected_samples += row.instances * row.draws;
          if (row.geometry) wanted = {float(tri[0].shader_id+1),float(tri[1].shader_id+1),float(tri[2].shader_id+1),1};
          else wanted = {float(tri[0].shader_id+1),instance_values[row.first_instance+row.instances-1],1,1};
        }
      }
      for (UINT k = 0; k < 4; ++k) expectFloat(pixels[(y*8+x)*4+k],wanted[k],row.name);
      ++checked; if (wanted[3]) ++colored;
    }
    require(checked == 64, "fan complete independent pixel coverage");
    UINT64 actual_samples = 0;
    const auto sample_bytes = readback(samples.Get(),sizeof(UINT64));
    std::memcpy(&actual_samples,sample_bytes.data(),sizeof(actual_samples));
    require(actual_samples == expected_samples, "fan exact occlusion samples with top-left rule");
    total_pixels += checked; total_primitives += UINT(primitive_count);
    std::printf("OWNED_D3D12_OPTIONS13_FAN row=%s pixels=%u colored=%u ia=%llu gs=%llu samples=%llu replay=1\n",
                row.name,checked,colored,(unsigned long long)primitive_count,
                (unsigned long long)(row.geometry ? primitive_count : 0),(unsigned long long)expected_samples);
  }
  std::printf("OWNED_D3D12_OPTIONS13_FAN_ORACLE rows=%zu pixels=%u primitives=%u cpu=1 winding=1 provoking=1 restart=1 indirect=1 bundles=1 gs=1 queries=1 occlusion=1 replay=1\n",
              std::size(rows),total_pixels,total_primitives);
}

void testManualWriteTracking(Context &c) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS17 options{};
  check(c.device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(46),&options,sizeof(options)), "OPTIONS17 query");
  require(options.ManualWriteTrackingResourceSupported == FALSE, "manual tracking is a tools override, not runtime capability");
  const D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_UPLOAD,D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                        D3D12_MEMORY_POOL_UNKNOWN,1,1};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 65536;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> rejected;
  const HRESULT committed = c.device->CreateCommittedResource(&properties,kP4ManualTracking,&desc,
      D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&rejected));
  require(FAILED(committed) && !rejected, "tools-only committed heap flag rejected when public support is false");
  const D3D12_HEAP_DESC heap_desc{65536,properties,65536,
      static_cast<D3D12_HEAP_FLAGS>(UINT(kP4ManualTracking)|UINT(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS))};
  ComPtr<ID3D12Heap> rejected_heap;
  const HRESULT heap_hr = c.device->CreateHeap(&heap_desc,IID_PPV_ARGS(&rejected_heap));
  require(FAILED(heap_hr) && !rejected_heap, "tools-only heap flag rejected when public support is false");
  auto upload_data = c.buffer(64,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  ComPtr<IUnknown> tools;
  require(upload_data->QueryInterface(kP4ManualTrackingIID,reinterpret_cast<void **>(tools.GetAddressOf())) == E_NOINTERFACE && !tools,
          "manual tracking QI absent without an external tool");
  check(c.device->GetDeviceRemovedReason(), "tools flag rejection preserves device");
  std::printf("OWNED_D3D12_OPTIONS13_MANUAL_TRACKING public=0 committed_rejected=1 heap_rejected=1 tools_qi=absent\n");

  // WriteBufferImmediate is a separate GPU command, not a substitute for TrackWrite.
  // Snapshot before/after two writes and an intervening ordinary copy; check
  // guarded untouched bytes and repeat the closed list with changed CPU input.
  const D3D12_COMMAND_LIST_TYPE queues[] = {D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_COMMAND_LIST_TYPE_COPY};
  UINT queue_count = 0;
  for (const auto type : queues) {
    c.setQueue(type);
    // COPY ownership starts in COMMON; the first copy promotes to COPY_DEST.
    // Buffers decay at ExecuteCommandLists boundaries, so keep the same
    // COMMON entry/exit state when replaying on all three queue types.
    auto target = c.buffer(64,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    auto result = c.buffer(64*3,D3D12_HEAP_TYPE_READBACK);
    auto recording = c.begin();
    ComPtr<ID3D12GraphicsCommandList2> list2;
    check(recording.list.As(&list2), "WriteBufferImmediate public list2 QI");
    recording.list->CopyBufferRegion(target.Get(),0,upload_data.Get(),0,64);
    barrier(recording.list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    recording.list->CopyBufferRegion(result.Get(),0,target.Get(),0,64);
    barrier(recording.list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER first[] = {
      {target->GetGPUVirtualAddress()+4,0x10203040},
      {target->GetGPUVirtualAddress()+60,0x50607080},
    };
    list2->WriteBufferImmediate(2,first,nullptr);
    barrier(recording.list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    recording.list->CopyBufferRegion(result.Get(),64,target.Get(),0,64);
    barrier(recording.list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    recording.list->CopyBufferRegion(target.Get(),4,upload_data.Get(),12,4);
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER last[] = {
      {target->GetGPUVirtualAddress()+4,0xa1b2c3d4},
      {target->GetGPUVirtualAddress()+32,0xdeadbeef},
      {target->GetGPUVirtualAddress()+36,0x76543210},
    };
    // MARKER_OUT orders the overwrite after the previous copy. MARKER_IN
    // follows that write; its own completion is observed after the fence.
    const D3D12_WRITEBUFFERIMMEDIATE_MODE modes[] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT};
    list2->WriteBufferImmediate(3,last,modes);
    barrier(recording.list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    recording.list->CopyBufferRegion(result.Get(),128,target.Get(),0,64);
    barrier(recording.list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    for (UINT replay = 0; replay < 2; ++replay) {
      std::vector<uint8_t> input(64);
      for (UINT i = 0; i < 64; ++i) input[i] = uint8_t(i*3+7+replay*53);
      upload(upload_data.Get(),input);
      std::vector<uint8_t> expected(192);
      std::copy(input.begin(),input.end(),expected.begin());
      std::copy(input.begin(),input.end(),expected.begin()+64);
      for (const auto &write : first) std::memcpy(expected.data()+64+(write.Dest-target->GetGPUVirtualAddress()),&write.Value,4);
      std::copy_n(expected.data()+64,64,expected.data()+128);
      for (const auto &write : last) std::memcpy(expected.data()+128+(write.Dest-target->GetGPUVirtualAddress()),&write.Value,4);
      c.submit(recording,replay == 0);
      equalBytes(readback(result.Get(),192),expected,"immediate write ordering/coherence/sentinels/replay");
    }
    ++queue_count;
    std::printf("OWNED_D3D12_OPTIONS13_IMMEDIATE queue=%u snapshots=3 bytes=192 modes=3 replay=1 changed_cpu_input=1 guards=1\n",UINT(type));
  }
  c.setQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
  std::printf("OWNED_D3D12_OPTIONS13_MANUAL_ORACLE public=0 rejected=2 tools_qi=absent immediate_queues=%u snapshots=9 replay=3 cpu=1\n",queue_count);
}
