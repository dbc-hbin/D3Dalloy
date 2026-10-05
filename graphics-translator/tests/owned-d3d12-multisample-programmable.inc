// Included inside the existing fixture namespace; all observations use public D3D12 GPU reads.
#include "owned-d3d12-multisample-position-dxil.inc"
constexpr char programmableShaders[] = R"(
float4 valuePS(float4 p : SV_Position, uint s : SV_SampleIndex) : SV_Target {
  return float4((s+1)/16.0,p.x/8,p.y/8,1);
}
float valueDepthPS(uint s : SV_SampleIndex) : SV_Depth { return (s+1)/16.0; }
Texture2DMS<float4> cm : register(t0);
Texture2DMS<float> dm : register(t0);
Texture2DMS<uint4> sm : register(t0);
Texture2D<float4> c : register(t0);
Texture2D<float> d : register(t0);
Texture2D<uint4> st : register(t0);
RWStructuredBuffer<float4> outData : register(u0);
[numthreads(1,1,1)] void gatherColor(uint3 id : SV_DispatchThreadID) {
  uint w,h,n; cm.GetDimensions(w,h,n);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) for(uint s=0;s<n;s++)
    outData[(y*w+x)*n+s]=cm.Load(int2(x,y),s);
}
[numthreads(1,1,1)] void gatherDepth(uint3 id : SV_DispatchThreadID) {
  uint w,h,n; dm.GetDimensions(w,h,n);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) for(uint s=0;s<n;s++)
    outData[(y*w+x)*n+s]=dm.Load(int2(x,y),s);
}
[numthreads(1,1,1)] void gatherStencil(uint3 id : SV_DispatchThreadID) {
  uint w,h,n; sm.GetDimensions(w,h,n);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) for(uint s=0;s<n;s++)
    outData[(y*w+x)*n+s]=float4(sm.Load(int2(x,y),s).y,0,0,0);
}
[numthreads(1,1,1)] void gatherColor1(uint3 id : SV_DispatchThreadID) {
  uint w,h; c.GetDimensions(w,h);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) outData[y*w+x]=c.Load(int3(x,y,0));
}
[numthreads(1,1,1)] void gatherDepth1(uint3 id : SV_DispatchThreadID) {
  uint w,h; d.GetDimensions(w,h);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) outData[y*w+x]=d.Load(int3(x,y,0));
}
[numthreads(1,1,1)] void gatherStencil1(uint3 id : SV_DispatchThreadID) {
  uint w,h; st.GetDimensions(w,h);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) outData[y*w+x]=float4(st.Load(int3(x,y,0)).y,0,0,0);
}
[numthreads(1,1,1)] void gatherInteger(uint3 id : SV_DispatchThreadID) {
  uint w,h,n; sm.GetDimensions(w,h,n);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) for(uint s=0;s<n;s++)
    outData[(y*w+x)*n+s]=float4(sm.Load(int2(x,y),s).x,0,0,0);
}
[numthreads(1,1,1)] void gatherInteger1(uint3 id : SV_DispatchThreadID) {
  uint w,h; st.GetDimensions(w,h);
  for(uint y=0;y<h;y++) for(uint x=0;x<w;x++) outData[y*w+x]=float4(st.Load(int3(x,y,0)).x,0,0,0);
}
)";

struct GPUList {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  GPUList(Fixture &f, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
    check(f.device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "programmable allocator");
    check(f.device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "programmable list");
  }
};
struct GPUSurface {
  ComPtr<ID3D12Resource> image;
  ComPtr<ID3D12DescriptorHeap> targets;
  D3D12_CPU_DESCRIPTOR_HANDLE target{};
  D3D12_RESOURCE_STATES state;
  DXGI_FORMAT view;
  UINT count;
  bool depth;
  std::array<D3D12_SAMPLE_POSITION,8> pattern{};
  UINT patternCount=0;
  GPUSurface(Fixture &f, DXGI_FORMAT format, DXGI_FORMAT typed, UINT samples, bool ds,
             bool writable = true, UINT quality = 0) : view(typed), count(samples), depth(ds) {
    auto flags = writable ? ds ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL :
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    state = writable ? ds ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET :
                           D3D12_RESOURCE_STATE_RESOLVE_DEST;
    image = validTexture(f.device.Get(), texture(format, samples, quality, flags), state);
    if (!writable) return;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = ds ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&targets)), "programmable target heap");
    target = targets->GetCPUDescriptorHandleForHeapStart();
    if (ds) {
      D3D12_DEPTH_STENCIL_VIEW_DESC vd{}; vd.Format=typed;
      vd.ViewDimension=samples>1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
      f.device->CreateDepthStencilView(image.Get(), &vd, target);
    } else {
      D3D12_RENDER_TARGET_VIEW_DESC vd{}; vd.Format=typed;
      vd.ViewDimension=samples>1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
      f.device->CreateRenderTargetView(image.Get(), &vd, target);
    }
  }
  void move(ID3D12GraphicsCommandList *list, D3D12_RESOURCE_STATES next) {
    if (state != next) transition(list, image.Get(), state, next);
    state = next;
  }
};
using PixelSamples = std::vector<std::array<float,4>>;
PixelSamples observe(Fixture &f, GPUSurface &surface, bool stencil = false) {
  GPUList commands(f);
  auto original = surface.state;
  if(surface.depth) commands.list->SetSamplePositions(surface.patternCount,surface.patternCount ? 1 : 0,
                                                       surface.patternCount ? surface.pattern.data() : nullptr);
  surface.move(commands.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=1;
  hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> views;
  check(f.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)), "programmable read heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format=surface.depth ? surface.view == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ?
      stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS :
      DXGI_FORMAT_R32_FLOAT : surface.view;
  sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension=surface.count>1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
  if(surface.count==1) { sd.Texture2D.MipLevels=1; sd.Texture2D.PlaneSlice=stencil ? 1 : 0; }
  f.device->CreateShaderResourceView(surface.image.Get(),&sd,views->GetCPUDescriptorHandleForHeapStart());
  const char *entry = surface.depth ? stencil ? surface.count>1 ? "gatherStencil" : "gatherStencil1" :
      surface.count>1 ? "gatherDepth" : "gatherDepth1" : surface.view==DXGI_FORMAT_R32_UINT ?
      surface.count>1 ? "gatherInteger" : "gatherInteger1" : surface.count>1 ? "gatherColor" : "gatherColor1";
  auto code=compile(programmableShaders,entry,"cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=f.readRoot.Get();
  pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(f.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)), "programmable gather PSO");
  const UINT bytes=16*surface.count*sizeof(std::array<float,4>);
  auto bd=buffer(bytes); bd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto dh=heap(D3D12_HEAP_TYPE_DEFAULT), rh=heap(D3D12_HEAP_TYPE_READBACK);
  ComPtr<ID3D12Resource> output, readback;
  check(f.device->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&bd,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)), "programmable output");
  bd.Flags=D3D12_RESOURCE_FLAG_NONE;
  check(f.device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,
      D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)), "programmable readback");
  commands.list->SetPipelineState(pso.Get());
  commands.list->SetComputeRootSignature(f.readRoot.Get());
  ID3D12DescriptorHeap *heaps[]{views.Get()}; commands.list->SetDescriptorHeaps(1,heaps);
  commands.list->SetComputeRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
  commands.list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
  commands.list->Dispatch(1,1,1);
  transition(commands.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyResource(readback.Get(),output.Get());
  surface.move(commands.list.Get(),original);
  f.submit(commands.list.Get());
  PixelSamples values(16*surface.count);
  void *mapped=nullptr; D3D12_RANGE range{0,bytes};
  check(readback->Map(0,&range,&mapped), "programmable map");
  std::memcpy(values.data(),mapped,bytes); D3D12_RANGE empty{0,0}; readback->Unmap(0,&empty);
  return values;
}
ComPtr<ID3D12PipelineState> samplePSO(Fixture &f, GPUSurface &surface, UINT quality, bool positions) {
  auto pd=graphicsDesc(f,surface.view,surface.count,quality,surface.depth);
  ComPtr<ID3DBlob> ps;
  if(positions) {
    pd.VS={sampleEdgeDXIL,sizeof(sampleEdgeDXIL)};
    pd.PS=surface.depth ? D3D12_SHADER_BYTECODE{sampleDepthDXIL,sizeof(sampleDepthDXIL)} :
                         D3D12_SHADER_BYTECODE{sampleColorDXIL,sizeof(sampleColorDXIL)};
  } else {
    ps=compile(programmableShaders,surface.depth ? "valueDepthPS" : "valuePS","ps_5_0");
    pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
  }
  ComPtr<ID3D12PipelineState> result;
  check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&result)), "programmable graphics PSO");
  return result;
}
void bindDraw(Fixture &f, ID3D12GraphicsCommandList1 *list, GPUSurface &surface,
              ID3D12PipelineState *pso, UINT row = 4) {
  list->SetGraphicsRootSignature(f.graphicsRoot.Get()); list->SetPipelineState(pso);
  if(surface.depth) list->OMSetRenderTargets(0,nullptr,FALSE,&surface.target);
  else list->OMSetRenderTargets(1,&surface.target,FALSE,nullptr);
  D3D12_VIEWPORT vp{0,0,4,4,0,1}; D3D12_RECT rect{0,LONG(row==4 ? 0 : row),4,LONG(row==4 ? 4 : row+1)};
  list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&rect);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void clearSurface(ID3D12GraphicsCommandList1 *list, GPUSurface &surface, UINT row=4,
                  float value=0, UINT8 stencil=0) {
  D3D12_RECT rect{0,LONG(row==4 ? 0 : row),4,LONG(row==4 ? 4 : row+1)};
  if(surface.depth) list->ClearDepthStencilView(surface.target,
      surface.view==DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL :
      D3D12_CLEAR_FLAG_DEPTH,value,stencil,1,&rect);
  else { const FLOAT values[4]{value,value,value,value}; list->ClearRenderTargetView(surface.target,values,1,&rect); }
}
void nearValue(float actual,float expected,const char *step) {
  if(!std::isfinite(actual) || std::fabs(actual-expected) > .00005f) {
    std::fprintf(stderr,"%s_OBSERVATION step=%s actual=%.9g expected=%.9g\n",marker,step,actual,expected);
    fail(step);
  }
}
std::array<D3D12_SAMPLE_POSITION,8> customPattern(UINT count, bool reverse) {
  std::array<D3D12_SAMPLE_POSITION,8> p{};
  for(UINT i=0;i<count;i++) p[i]={INT8((i%2==0 ? -6 : 5)*(reverse ? -1 : 1)),INT8(-7+int(i)*2)};
  return p;
}
std::array<D3D12_SAMPLE_POSITION,8> defaultPattern(UINT count) {
  if(count==2) return {{{4,4},{-4,-4}}};
  if(count==4) return {{{-2,-6},{6,-2},{-6,2},{2,6}}};
  return {{{1,-3},{-1,3},{5,1},{-3,-5},{-5,5},{-7,-1},{3,7},{7,-7}}};
}
void verifyPattern(const PixelSamples &values, UINT count, bool depth, UINT row,
                   const std::array<D3D12_SAMPLE_POSITION,8> &p) {
  for(UINT x=0;x<4;x++) for(UINT s=0;s<count;s++) {
    const bool covered=x==0 || (x==1 && p[s].X<0);
    const float sx=x+.5f+p[s].X/16.f, sy=row+.5f+p[s].Y/16.f;
    const auto &v=values[(row*4+x)*count+s];
    if(depth) nearValue(v[0],covered ? .125f+sx/8+sy/32 : 0,"custom depth coverage/interpolation");
    else for(UINT c=0;c<4;c++) {
      const float expected=covered ? c%2==0 ? sx : sy : 0;
      if(!std::isfinite(v[c]) || std::fabs(v[c]-expected)>.00005f)
        std::fprintf(stderr,"sample mismatch count=%u row=%u x=%u sample=%u component=%u position=%d,%d rgba=%.9g,%.9g,%.9g,%.9g\n",
                     count,row,x,s,c,p[s].X,p[s].Y,v[0],v[1],v[2],v[3]);
      nearValue(v[c],expected,"custom color coverage/SV_Position/interpolation");
    }
  }
}
void programmableCases(Fixture &f) {
  {
    GPUList probe(f); auto pattern=customPattern(2,false);
    probe.list->SetSamplePositions(2,1,pattern.data());
    check(probe.list->Close(),"SetSamplePositions public valid recording");
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
  check(f.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,&options,sizeof(options)), "OPTIONS2");
  require(options.ProgrammableSamplePositionsTier==D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1,
          "programmable sample Tier1");
  for(UINT count : {2u,4u,8u}) for(bool depth : {false,true}) {
    GPUSurface surface(f,depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G32B32A32_FLOAT,
      depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT,count,depth,true,standardPattern);
    auto pso=samplePSO(f,surface,standardPattern,true);
    auto a=customPattern(count,false), b=customPattern(count,true), def=defaultPattern(count);
    // Different-pattern partial depth clears make untouched contents undefined.
    // Separate depth attachments retain each observation while still switching within one list.
    std::vector<GPUSurface> depthTargets;
    if(depth) for(UINT i=0;i<3;i++) depthTargets.emplace_back(f,DXGI_FORMAT_R32_TYPELESS,
        DXGI_FORMAT_D32_FLOAT,count,true,true,standardPattern);
    auto targetFor=[&](UINT row) -> GPUSurface & { return depth && row ? depthTargets[row-1] : surface; };
    GPUList bundle(f,D3D12_COMMAND_LIST_TYPE_BUNDLE);
    bundle.list->SetPipelineState(pso.Get()); // Bundles inherit sample positions, not the PSO.
    bundle.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    bundle.list->DrawInstanced(6,1,0,0); check(bundle.list->Close(),"inheriting bundle close");
    GPUList commands(f);
    for(UINT row=0;row<4;row++) {
      if(row==2) commands.list->ClearState(nullptr);
      else {
        auto positions=row==1 ? b : a;
        commands.list->SetSamplePositions(count,1,positions.data());
        positions=row==1 ? a : b; // Caller storage may change immediately after the public call.
      }
      auto &target=targetFor(row);
      target.patternCount=row==2 ? 0 : count; target.pattern=row==1 ? b : a;
      bindDraw(f,commands.list.Get(),target,pso.Get(),row);
      clearSurface(commands.list.Get(),target,depth ? 4 : row);
      if(row==3) {
        commands.list->ExecuteBundle(bundle.list.Get());
        commands.list->DrawInstanced(6,1,0,0); // Bundle return must retain the parent's pattern.
      }
      else commands.list->DrawInstanced(6,1,0,0);
    }
    f.submit(commands.list.Get());
    auto validate=[&] {
      for(UINT row=0;row<4;row++) {
        auto values=observe(f,targetFor(row));
        verifyPattern(values,count,depth,row,row==2 ? def : row==1 ? b : a);
      }
    };
    validate();
    if(depth) {
      GPUSurface resolved(f,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT,1,false,false);
      GPUList resolve(f);
      for(UINT row=0;row<4;row++) {
        if(row==2) resolve.list->SetSamplePositions(0,0,nullptr);
        else resolve.list->SetSamplePositions(count,1,row==1 ? b.data() : a.data());
        targetFor(row).move(resolve.list.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        D3D12_RECT rect{0,LONG(row),4,LONG(row+1)};
        resolve.list->ResolveSubresourceRegion(resolved.image.Get(),0,0,row,targetFor(row).image.Get(),0,&rect,
            DXGI_FORMAT_R32_FLOAT,D3D12_RESOLVE_MODE_MAX);
        targetFor(row).move(resolve.list.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE);
      }
      f.submit(resolve.list.Get()); auto values=observe(f,resolved);
      for(UINT row=0;row<4;row++) {
        auto samples=observe(f,targetFor(row));
        for(UINT x=0;x<4;x++) {
          const UINT pixel=row*4+x; float maximum=0;
          for(UINT s=0;s<count;s++) maximum=std::fmax(maximum,samples[pixel*count+s][0]);
          nearValue(values[pixel][0],maximum,"custom depth pattern resolve/history");
        }
      }
    }
    // Poison the attachments on another list before replay: unchanged data cannot pass.
    GPUList poison(f);
    for(UINT row=0;row<4;row++) {
      if(row!=2) poison.list->SetSamplePositions(count,1,row==1 ? b.data() : a.data());
      else poison.list->SetSamplePositions(0,0,nullptr);
      clearSurface(poison.list.Get(),targetFor(row),depth ? 4 : row,.9375f);
    }
    f.submit(poison.list.Get());
    ID3D12CommandList *replay[]{commands.list.Get()}; f.queue->ExecuteCommandLists(1,replay);
    GPUList wait(f); f.submit(wait.list.Get()); validate();
    check(commands.allocator->Reset(),"programmable allocator reset");
    check(commands.list->Reset(commands.allocator.Get(),nullptr),"programmable list reset");
    surface.patternCount=0;
    bindDraw(f,commands.list.Get(),surface,pso.Get()); clearSurface(commands.list.Get(),surface);
    commands.list->DrawInstanced(6,1,0,0); f.submit(commands.list.Get());
    auto defaults=observe(f,surface);
    for(UINT row=0;row<4;row++) verifyPattern(defaults,count,depth,row,def);
    GPUList explicitDefault(f);
    explicitDefault.list->SetSamplePositions(count,1,a.data());
    explicitDefault.list->SetSamplePositions(0,0,nullptr);
    bindDraw(f,explicitDefault.list.Get(),surface,pso.Get()); clearSurface(explicitDefault.list.Get(),surface);
    explicitDefault.list->DrawInstanced(6,1,0,0); f.submit(explicitDefault.list.Get());
    defaults=observe(f,surface);
    for(UINT row=0;row<4;row++) verifyPattern(defaults,count,depth,row,def);
    // A fresh list never inherits the preceding list's custom state.
    check(commands.allocator->Reset(),"cross-list allocator reset");
    check(commands.list->Reset(commands.allocator.Get(),nullptr),"cross-list reset");
    commands.list->SetSamplePositions(count,1,b.data());
    surface.patternCount=count; surface.pattern=b;
    bindDraw(f,commands.list.Get(),surface,pso.Get()); clearSurface(commands.list.Get(),surface);
    commands.list->DrawInstanced(6,1,0,0); f.submit(commands.list.Get());
    surface.patternCount=0;
    GPUList fresh(f); bindDraw(f,fresh.list.Get(),surface,pso.Get()); clearSurface(fresh.list.Get(),surface);
    fresh.list->DrawInstanced(6,1,0,0); f.submit(fresh.list.Get());
    defaults=observe(f,surface);
    for(UINT row=0;row<4;row++) verifyPattern(defaults,count,depth,row,def);
    std::printf("%s_PROGRAMMABLE_GPU_PASS count=%u depth=%u\n",marker,count,depth);
  }
  std::printf("%s_PROGRAMMABLE_LIFECYCLE_PASS switches=2 replay=6 reset=6 clearstate=6 crosslist=6 bundles=6\n",marker);
  UINT rejected=0;
  auto error=[&](UINT count,UINT pixels,D3D12_SAMPLE_POSITION *p) {
    std::array<D3D12_SAMPLE_POSITION,16> positions{};
    if(p) for(UINT i=0;i<4;i++) positions[i]=p[i];
    GPUList c(f); c.list->SetSamplePositions(count,pixels,p ? positions.data() : nullptr);
    ComPtr<ID3D12GraphicsCommandList4> meta; check(c.list.As(&meta),"recording-error List4");
    meta->ExecuteMetaCommand(nullptr,nullptr,0); // Unsupported: E_NOTIMPL cannot replace E_INVALIDARG.
    require(c.list->Close()==E_INVALIDARG,"SetSamplePositions first recording error"); ++rejected;
  };
  auto p=customPattern(4,false);
  error(1,1,p.data()); error(3,1,p.data()); error(16,1,p.data()); error(4,4,p.data());
  error(4,1,nullptr); error(0,1,nullptr); error(4,0,p.data()); error(0,0,p.data());
  p[0].X=-9; error(4,1,p.data()); p[0].X=8; error(4,1,p.data());
  p=customPattern(4,false);
  GPUList forbidden(f,D3D12_COMMAND_LIST_TYPE_BUNDLE);
  forbidden.list->SetSamplePositions(4,1,p.data());
  require(forbidden.list->Close()==E_INVALIDARG,"bundle cannot set sample positions"); ++rejected;
  GPUSurface surface(f,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,4,false);
  auto pso=samplePSO(f,surface,0,true); GPUList mismatch(f); auto a=customPattern(2,false);
  mismatch.list->SetSamplePositions(2,1,a.data()); bindDraw(f,mismatch.list.Get(),surface,pso.Get());
  mismatch.list->DrawInstanced(6,1,0,0);
  require(mismatch.list->Close()==E_INVALIDARG,"custom pattern/PSO count mismatch"); ++rejected;
  GPUList reverse(f); ComPtr<ID3D12GraphicsCommandList4> meta;
  check(reverse.list.As(&meta),"reverse recording-error List4");
  meta->ExecuteMetaCommand(nullptr,nullptr,0); reverse.list->SetSamplePositions(3,1,p.data());
  require(reverse.list->Close()==E_NOTIMPL,"first E_NOTIMPL survives invalid sample pattern"); ++rejected;
  std::printf("%s_PROGRAMMABLE_ERRORS_PASS rejected=%u first_error=E_INVALIDARG\n",marker,rejected);
}

void regionResolveCases(Fixture &f) {
  for(UINT count : {2u,4u,8u}) for(bool stencil : {false,true}) {
    GPUSurface src(f,stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_UINT,
        stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_R32_UINT,count,stencil);
    std::vector<ComPtr<ID3D12PipelineState>> psos;
    GPUList draw(f); clearSurface(draw.list.Get(),src);
    for(UINT s=0;s<(stencil ? count : 1u);s++) {
      auto pd=graphicsDesc(f,src.view,count,0,stencil);
      if(stencil) {
        pd.SampleMask=1u<<s; pd.DepthStencilState.DepthEnable=FALSE;
        pd.DepthStencilState.StencilEnable=TRUE; pd.DepthStencilState.StencilReadMask=255;
        pd.DepthStencilState.StencilWriteMask=255;
        pd.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_REPLACE,D3D12_COMPARISON_FUNC_ALWAYS};
        pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
      }
      ComPtr<ID3D12PipelineState> pso;
      check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"integer/stencil sample PSO");
      bindDraw(f,draw.list.Get(),src,pso.Get()); draw.list->OMSetStencilRef(33+s);
      draw.list->DrawInstanced(3,1,0,0); psos.push_back(pso);
    }
    src.move(draw.list.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE); f.submit(draw.list.Get());
    for(auto mode : {D3D12_RESOLVE_MODE_MIN,D3D12_RESOLVE_MODE_MAX}) {
      GPUSurface dst(f,stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_UINT,
          stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_R32_UINT,1,stencil,false);
      GPUList resolve(f); resolve.list->ResolveSubresourceRegion(dst.image.Get(),stencil ? 1 : 0,0,0,
          src.image.Get(),stencil ? 1 : 0,nullptr,
          stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_UINT,mode);
      f.submit(resolve.list.Get()); auto values=observe(f,dst,stencil);
      for(const auto &v : values) nearValue(v[0],stencil ? mode==D3D12_RESOLVE_MODE_MIN ? 33 : 32+count :
          mode==D3D12_RESOLVE_MODE_MIN ? 1 : count,"integer/stencil MIN/MAX resolve");
      std::printf("%s_RESOLVE_INTEGER_GPU_PASS count=%u stencil=%u mode=%u no_target_flag=1\n",
                  marker,count,stencil,UINT(mode));
    }
  }
  for(UINT count : {2u,4u,8u}) for(bool depth : {false,true}) {
    GPUSurface src(f,depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G32B32A32_FLOAT,
      depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT,count,depth);
    auto pso=samplePSO(f,src,0,false);
    GPUList draw(f); bindDraw(f,draw.list.Get(),src,pso.Get()); clearSurface(draw.list.Get(),src);
    draw.list->DrawInstanced(3,1,0,0); f.submit(draw.list.Get());
    for(auto mode : {D3D12_RESOLVE_MODE_MIN,D3D12_RESOLVE_MODE_MAX,D3D12_RESOLVE_MODE_AVERAGE}) {
      // Depth-to-color uses matching typeless resource groups and a typed R32_FLOAT resolve.
      GPUSurface dst(f,depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G32B32A32_FLOAT,
        depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT,1,false,false);
      GPUList resolve(f); src.move(resolve.list.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      resolve.list->ResolveSubresourceRegion(dst.image.Get(),0,0,0,src.image.Get(),0,nullptr,
        depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT,mode);
      f.submit(resolve.list.Get()); auto values=observe(f,dst);
      const float expected=mode==D3D12_RESOLVE_MODE_MIN ? 1/16.f :
        mode==D3D12_RESOLVE_MODE_MAX ? count/16.f : (count+1)/32.f;
      for(const auto &v : values) nearValue(v[0],expected,"full resolve min/max/average");
      std::printf("%s_RESOLVE_GPU_PASS count=%u depth=%u mode=%u no_target_flag=1\n",marker,count,depth,UINT(mode));
    }
  }
  // Separate-resource DECOMPRESS is sample-preserving, not an averaged resolve or a no-op.
  for(UINT count : {1u,2u,4u,8u}) for(bool depth : {false,true}) {
    const auto resourceFormat=depth ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32G32B32A32_FLOAT;
    const auto typed=depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    GPUSurface src(f,resourceFormat,typed,count,depth), seed(f,resourceFormat,typed,count,depth);
    GPUSurface dst(f,resourceFormat,typed,count,depth,false);
    auto pso=samplePSO(f,src,0,false); GPUList draw(f);
    bindDraw(f,draw.list.Get(),src,pso.Get()); clearSurface(draw.list.Get(),src,4,0,37);
    draw.list->DrawInstanced(3,1,0,0);
    clearSurface(draw.list.Get(),seed,4,.875f,91);
    seed.move(draw.list.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.move(draw.list.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
    draw.list->CopyResource(dst.image.Get(),seed.image.Get());
    dst.move(draw.list.Get(),D3D12_RESOURCE_STATE_RESOLVE_DEST);
    src.move(draw.list.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE); f.submit(draw.list.Get());
    const auto sourceValues=observe(f,src); const auto untouched=observe(f,dst);
    for(UINT plane=0;plane<(depth ? 2u : 1u);plane++) {
      GPUList resolve(f); D3D12_RECT rect{1,0,3,2};
      resolve.list->ResolveSubresourceRegion(dst.image.Get(),plane,0,2,src.image.Get(),plane,&rect,
        depth ? plane ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : typed,
        D3D12_RESOLVE_MODE_DECOMPRESS);
      f.submit(resolve.list.Get());
      auto values=observe(f,dst); auto stencilValues=depth ? observe(f,dst,true) : PixelSamples{};
      for(UINT y=0;y<4;y++) for(UINT x=0;x<4;x++) for(UINT s=0;s<count;s++) {
        const UINT index=(y*4+x)*count+s; const bool inside=x<2 && y>=2;
        const auto &expected=inside ? sourceValues[((y-2)*4+x+1)*count+s] : untouched[index];
        for(UINT c=0;c<(depth ? 1u : 4u);c++) nearValue(values[index][c],expected[c],"DECOMPRESS samples/rect/outside");
        if(depth) nearValue(stencilValues[index][0],plane==1 && inside ? 37.f : 91.f,
                           "DECOMPRESS other plane preserved");
      }
      std::printf("%s_DECOMPRESS_GPU_PASS count=%u depth=%u plane=%u partial=1 no_target_flag=1\n",
                  marker,count,depth,plane);
    }
    GPUList inplace(f); D3D12_RECT rect{0,0,2,2};
    inplace.list->ResolveSubresourceRegion(src.image.Get(),0,0,0,src.image.Get(),0,&rect,
      depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : typed,D3D12_RESOLVE_MODE_DECOMPRESS);
    f.submit(inplace.list.Get()); auto after=observe(f,src);
    for(UINT i=0;i<after.size();i++) for(UINT c=0;c<(depth ? 1u : 4u);c++)
      nearValue(after[i][c],sourceValues[i][c],"in-place DECOMPRESS retains samples");
  }
  {
    GPUSurface src(f,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,4,true,true,standardPattern);
    GPUSurface dst(f,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,4,true,true,standardPattern);
    auto pattern=customPattern(4,false); src.pattern=dst.pattern=pattern; src.patternCount=dst.patternCount=4;
    auto pso=samplePSO(f,src,standardPattern,true); GPUList draw(f);
    draw.list->SetSamplePositions(4,1,pattern.data());
    clearSurface(draw.list.Get(),src,4,0,37); clearSurface(draw.list.Get(),dst,4,.875f,91);
    bindDraw(f,draw.list.Get(),src,pso.Get()); draw.list->DrawInstanced(6,1,0,0);
    src.move(draw.list.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.move(draw.list.Get(),D3D12_RESOURCE_STATE_COPY_DEST); f.submit(draw.list.Get());
    auto expected=observe(f,src);
    for(UINT plane=0;plane<2;plane++) {
      GPUList copy(f); copy.list->SetSamplePositions(4,1,pattern.data());
      D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
      source.pResource=src.image.Get(); destination.pResource=dst.image.Get();
      source.Type=destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source.SubresourceIndex=destination.SubresourceIndex=plane;
      copy.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr); f.submit(copy.list.Get());
      auto values=observe(f,dst); auto stencilValues=observe(f,dst,true);
      for(UINT i=0;i<values.size();i++) {
        nearValue(values[i][0],expected[i][0],"custom depth CopyTextureRegion samples");
        nearValue(stencilValues[i][0],plane==0 ? 91.f : 37.f,"custom depth copy other plane");
      }
    }
    std::printf("%s_CUSTOM_DEPTH_COPY_GPU_PASS count=4 planes=2 other_plane=1\n",marker);
  }
  std::printf("%s_RESOLVE_LIFECYCLE_PASS inplace=8 separate=8 partial=12 planes=4\n",marker);
}
