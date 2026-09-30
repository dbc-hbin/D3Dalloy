#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr, const char *where) {
  if (FAILED(hr)) { std::fprintf(stderr,"OWNED_D3D12_REINTERPRET_FAIL %s %08lx\n",where,static_cast<unsigned long>(hr)); ExitProcess(1); }
}
static void require(bool condition, const char *where) {
  if (!condition) { std::fprintf(stderr,"OWNED_D3D12_REINTERPRET_FAIL %s\n",where); ExitProcess(1); }
}
static D3D12_RESOURCE_DESC texture(DXGI_FORMAT format, UINT width, UINT height) {
  D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height;
  d.DepthOrArraySize=d.MipLevels=1; d.Format=format; d.SampleDesc.Count=1; return d;
}
static D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  auto d=texture(DXGI_FORMAT_UNKNOWN,static_cast<UINT>(bytes),1); d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
static D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{}; p.Type=type; p.CreationNodeMask=p.VisibleNodeMask=1; return p;
}
static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
int main() {
  const auto backend=reinterpret_cast<const char*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  require(backend && !std::strcmp(backend(),"owned-graphics-d3d12"),"backend");
  std::printf("OWNED_D3D12_REINTERPRET_BACKEND id=%s\n",backend());
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> dev;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory"); check(factory->EnumAdapters1(0,&adapter),"adapter");
  check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)),"device");
  auto bc=texture(DXGI_FORMAT_BC1_UNORM,8,8), raw=texture(DXGI_FORMAT_R16G16B16A16_UINT,2,2);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT bc_fp{},raw_fp{}; UINT bc_rows=0,raw_rows=0;
  UINT64 bc_bytes=0,raw_bytes=0,bc_total=0,raw_total=0;
  dev->GetCopyableFootprints(&bc,0,1,0,&bc_fp,&bc_rows,&bc_bytes,&bc_total);
  dev->GetCopyableFootprints(&raw,0,1,0,&raw_fp,&raw_rows,&raw_bytes,&raw_total);
  require(bc_rows==2 && raw_rows==2 && bc_bytes==16 && raw_bytes==16,"footprints");
  const auto dh=heap(D3D12_HEAP_TYPE_DEFAULT),uh=heap(D3D12_HEAP_TYPE_UPLOAD),rh=heap(D3D12_HEAP_TYPE_READBACK);
  ComPtr<ID3D12Resource> compressed,uncompressed,recompressed,upload,readback;
  check(dev->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&bc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&compressed)),"BC source");
  check(dev->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&raw,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&uncompressed)),"raw destination");
  check(dev->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&bc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&recompressed)),"BC destination");
  const UINT64 bc_offset=(raw_total+511u)&~UINT64{511};
  auto upload_desc=buffer(bc_total),readback_desc=buffer(bc_offset+bc_total);
  check(dev->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&upload_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"upload");
  check(dev->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&readback_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
  constexpr std::array<BYTE,32> bytes={0x10,0x21,0x32,0x43,0x54,0x65,0x76,0x87,
    0x98,0xa9,0xba,0xcb,0xdc,0xed,0xfe,0x0f,0xf0,0xe1,0xd2,0xc3,0xb4,0xa5,0x96,0x87,
    0x78,0x69,0x5a,0x4b,0x3c,0x2d,0x1e,0x0f};
  BYTE *mapped=nullptr; check(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"map upload");
  std::memset(mapped,0,static_cast<size_t>(bc_total));
  std::memcpy(mapped+bc_fp.Offset,bytes.data(),16);
  std::memcpy(mapped+bc_fp.Offset+bc_fp.Footprint.RowPitch,bytes.data()+16,16);
  upload->Unmap(0,nullptr);
  D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
  check(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
  check(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&allocator)),"allocator");
  check(dev->CreateCommandList(0,qd.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
  check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
  D3D12_TEXTURE_COPY_LOCATION src{},bc_dst{};
  src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=bc_fp;
  bc_dst.pResource=compressed.Get(); bc_dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&bc_dst,0,0,0,&src,nullptr);
  transition(list.Get(),compressed.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(uncompressed.Get(),compressed.Get());
  transition(list.Get(),uncompressed.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION raw_src{},raw_dst{};
  raw_src.pResource=uncompressed.Get(); raw_src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  raw_dst.pResource=readback.Get(); raw_dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; raw_dst.PlacedFootprint=raw_fp;
  list->CopyTextureRegion(&raw_dst,0,0,0,&raw_src,nullptr);
  list->CopyResource(recompressed.Get(),uncompressed.Get());
  transition(list.Get(),recompressed.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION bc_src{},bc_readback{};
  bc_src.pResource=recompressed.Get(); bc_src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  bc_readback.pResource=readback.Get(); bc_readback.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  bc_readback.PlacedFootprint=bc_fp; bc_readback.PlacedFootprint.Offset=bc_offset;
  list->CopyTextureRegion(&bc_readback,0,0,0,&bc_src,nullptr);
  check(list->Close(),"close"); ID3D12CommandList *lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
  check(queue->Signal(fence.Get(),1),"signal"); HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  require(event!=nullptr,"event"); check(fence->SetEventOnCompletion(1,event),"fence event");
  require(WaitForSingleObject(event,15000)==WAIT_OBJECT_0,"wait"); CloseHandle(event);
  check(dev->GetDeviceRemovedReason(),"device removed");
  BYTE *result=nullptr; check(readback->Map(0,nullptr,reinterpret_cast<void**>(&result)),"map readback");
  for(UINT row=0;row<2;row++) for(UINT byte=0;byte<16;byte++) {
    const auto expected=bytes[row*16+byte];
    const auto actual_raw=result[raw_fp.Offset+row*raw_fp.Footprint.RowPitch+byte];
    const auto actual_bc=result[bc_offset+bc_fp.Offset+row*bc_fp.Footprint.RowPitch+byte];
    if(actual_raw!=expected || actual_bc!=expected) {
      std::fprintf(stderr,"OWNED_D3D12_REINTERPRET_FAIL row=%u byte=%u raw=%02x bc=%02x expected=%02x\n",row,byte,actual_raw,actual_bc,expected);
      ExitProcess(1);
    }
  }
  readback->Unmap(0,nullptr);
  std::printf("OWNED_D3D12_REINTERPRET_PASS BC1_8x8_to_RGBA16UINT_2x2_and_back bytes=32 backend=%s\n",backend());
}
