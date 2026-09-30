#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr, const char *where) {
  if (FAILED(hr)) { std::fprintf(stderr, "STENCIL_OWNED_FAIL %s hr=%08lx\n", where, (unsigned long)hr); ExitProcess(1); }
}
static std::vector<unsigned char> bytes(const wchar_t *path) {
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr,"STENCIL_OWNED_FAIL open %lu\n",GetLastError()); ExitProcess(1); }
  DWORD n = GetFileSize(h, nullptr); std::vector<unsigned char> v(n); DWORD read = 0;
  if (!ReadFile(h, v.data(), n, &read, nullptr) || read != n) ExitProcess(2);
  CloseHandle(h); return v;
}
static D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{}; h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1; return h;
}
static int run(const wchar_t *vsPath, const wchar_t *psPath) {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(),"owned-graphics-d3d12")) return 3;
  std::printf("OWNED_D3D12_STENCIL_REFERENCE_BACKEND id=%s\n",backend());
  auto vs = bytes(vsPath), ps = bytes(psPath);
  ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  D3D12_ROOT_SIGNATURE_DESC rsDesc{}; rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, errs; check(D3D12SerializeRootSignature(&rsDesc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errs),"rootSerialize");
  ComPtr<ID3D12RootSignature> root; check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature=root.Get(); pd.VS={vs.data(),vs.size()}; pd.PS={ps.data(),ps.size()};
  pd.SampleMask=UINT_MAX; pd.SampleDesc.Count=1; pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; pd.NumRenderTargets=1; pd.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.DepthStencilState.StencilEnable=TRUE; pd.DepthStencilState.StencilReadMask=0xf0; pd.DepthStencilState.StencilWriteMask=0x0f;
  pd.DepthStencilState.FrontFace.StencilFunc=D3D12_COMPARISON_FUNC_EQUAL;
  pd.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_REPLACE;
  pd.DepthStencilState.FrontFace.StencilFailOp=D3D12_STENCIL_OP_KEEP;
  pd.DepthStencilState.FrontFace.StencilDepthFailOp=D3D12_STENCIL_OP_KEEP;
  pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
  ComPtr<ID3D12PipelineState> pso; check(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"pso");
  auto hp=heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC dd{}; dd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; dd.Width=2; dd.Height=1;
  dd.DepthOrArraySize=1; dd.MipLevels=1; dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.SampleDesc.Count=1;
  dd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear{}; clear.Format=dd.Format; clear.DepthStencil.Depth=1;
  ComPtr<ID3D12Resource> ds; check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&ds)),"ds");
  D3D12_RESOURCE_DESC cd=dd; cd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; cd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> color; check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&cd,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&color)),"color");
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors=1; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  ComPtr<ID3D12DescriptorHeap> dsv; check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)),"dsvHeap");
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; ComPtr<ID3D12DescriptorHeap> rtv;
  check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"rtvHeap");
  D3D12_DEPTH_STENCIL_VIEW_DESC vd{}; vd.Format=dd.Format; vd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
  auto dh=dsv->GetCPUDescriptorHandleForHeapStart(); device->CreateDepthStencilView(ds.Get(),&vd,dh);
  auto rh=rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(color.Get(),nullptr,rh);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows=0; UINT64 rowBytes=0,total=0;
  device->GetCopyableFootprints(&dd,1,1,0,&fp,&rows,&rowBytes,&total);
  D3D12_RESOURCE_DESC bd{}; bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=total;
  bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  auto hr=heap(D3D12_HEAP_TYPE_READBACK); ComPtr<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&hr,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
  D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
  ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pso.Get(),IID_PPV_ARGS(&list)),"list");
  list->SetGraphicsRootSignature(root.Get()); list->OMSetRenderTargets(1,&rh,FALSE,&dh);
  list->ClearDepthStencilView(dh,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,1,0x30,0,nullptr);
  D3D12_VIEWPORT vp{0,0,2,1,0,1}; D3D12_RECT sc{0,0,2,1};
  list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&sc);
  list->OMSetStencilRef(0x11); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3,1,0,0);
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource=ds.Get(); barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1,&barrier);
  D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=ds.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=1;
  D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp;
  list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); check(list->Close(),"close");
  ID3D12CommandList *lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
  ComPtr<ID3D12Fence> fence; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); check(queue->Signal(fence.Get(),1),"signal");
  check(fence->SetEventOnCompletion(1,event),"fenceEvent");
  DWORD wait=WaitForSingleObject(event,15000); CloseHandle(event);
  if (wait!=WAIT_OBJECT_0) return 4;
  void *mapped=nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(total)}; check(readback->Map(0,&range,&mapped),"map");
  auto data=static_cast<const unsigned char *>(mapped)+fp.Offset;
  unsigned left=data[0],right=data[1];
  std::printf("STENCIL_OWNED_READBACK left=%02x right=%02x row=%u bytes=%llu\n",left,right,fp.Footprint.RowPitch,(unsigned long long)rowBytes);
  readback->Unmap(0,nullptr); return left==0x30 && right==0x35?0:5;
}

int wmain(int argc, wchar_t **) {
  if (argc != 1) return 2;
  wchar_t executable[MAX_PATH];
  const DWORD length=GetModuleFileNameW(nullptr,executable,MAX_PATH);
  if (!length || length>=MAX_PATH) return 2;
  std::wstring directory(executable,executable+length);
  const auto separator=directory.find_last_of(L"\\/");
  if (separator==std::wstring::npos) return 2;
  directory.resize(separator+1);
  const auto vertex=directory+L"raster-order-vertex.dxil";
  for (const auto &variant : {L"dxbc",L"dxil"}) {
    const auto pixel=directory+L"raster-order-stencil."+variant;
    const int result=run(vertex.c_str(),pixel.c_str());
    std::printf("STENCIL_REFERENCE_CASE %ls status=%s\n",variant,result?"FAIL":"PASS");
    if (result) return result;
  }
  std::puts("OWNED_D3D12_STENCIL_REFERENCE_PASS");
  return 0;
}