// Public pinned-preview ABI, GPU-only conversion, replay, lifetime, and CL11 cross-queue fences.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef DXMT_MATRIX_PREVIEW_SDK_HEADER
#include <rpc.h>
#include <rpcndr.h>
#if defined(__clang__) && defined(__MINGW32__)
// The untouched Microsoft SDK supplies UUID attributes, not MinGW template
// specializations. Use Clang's intrinsic to consume those public attributes.
#undef __uuidof
#undef DECLSPEC_UUID
#define DECLSPEC_UUID(value) __declspec(uuid(value))
#undef MIDL_INTERFACE
#define MIDL_INTERFACE(value) struct __declspec(uuid(value))
#endif
#include DXMT_MATRIX_PREVIEW_SDK_HEADER
#else
#include <d3d12.h>
#include "../../src/d3d12/d3d12_interfaces_ext.hpp"
#endif
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
static_assert(sizeof(D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT) == (sizeof(void *) == 8 ? 56 : 48));
static_assert(offsetof(D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT, WaveMatrixMultiply) == (sizeof(void *) == 8 ? 8 : 4));
static_assert(sizeof(D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO) == 56);

static void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) { std::fprintf(stderr, "MATRIX_RUNTIME_FAIL %s hr=%08lx\n", step, (unsigned long)hr); std::exit(1); }
}
static void require(bool value, const char *step) { if (!value) check(E_FAIL, step); }
static void experimental_admission() {
  check(D3D12EnableExperimentalFeatures(0,nullptr,nullptr,nullptr),"initial empty experimental set");
  IID unknown{}; UINT size=4,configuration=0;
  require(D3D12EnableExperimentalFeatures(1,&unknown,nullptr,nullptr)==E_NOINTERFACE,"unknown experimental GUID");
  require(D3D12EnableExperimentalFeatures(1,nullptr,nullptr,nullptr)==E_INVALIDARG,"missing experimental GUID array");
  require(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,&configuration,&size)==E_INVALIDARG,"unexpected experimental configuration");
  require(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,nullptr,&size)==E_INVALIDARG,"unexpected experimental configuration size");
  check(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,nullptr,nullptr),"public experimental shader opt-in");
  ComPtr<ID3D12Device> first,second;
  check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&first)),"experimental first device");
  check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&second)),"experimental second device");
  IID sameSet[]{D3D12ExperimentalShaderModels,D3D12ExperimentalShaderModels};
  check(D3D12EnableExperimentalFeatures(2,sameSet,nullptr,nullptr),"unchanged experimental set");
  size=0;check(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,nullptr,&size),"zero experimental configuration size");
  require(D3D12EnableExperimentalFeatures(1,&unknown,nullptr,nullptr)==E_NOINTERFACE,"unknown leaves experimental set intact");
  require(first->GetDeviceRemovedReason()==S_OK && second->GetDeviceRemovedReason()==S_OK,"unchanged set preserves devices");
  ComPtr<ID3D12Fence> fence;check(first->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"experimental removal fence");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"experimental removal event");
  check(fence->SetEventOnCompletion(42,event),"experimental pending fence completion");
  check(D3D12EnableExperimentalFeatures(0,nullptr,nullptr,nullptr),"changed experimental set");
  require(first->GetDeviceRemovedReason()==DXGI_ERROR_DEVICE_REMOVED && second->GetDeviceRemovedReason()==DXGI_ERROR_DEVICE_REMOVED,"changed set removes all devices");
  require(fence->GetCompletedValue()==UINT64_MAX && WaitForSingleObject(event,0)==WAIT_OBJECT_0,"removal wakes pending fence");CloseHandle(event);
  first.Reset();second.Reset();fence.Reset();
  check(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,nullptr,nullptr),"re-enable after released devices");
  ComPtr<ID3D12Device> released;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&released)),"experimental released device");
  released.Reset();check(D3D12EnableExperimentalFeatures(0,nullptr,nullptr,nullptr),"change after device destruction");
  check(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,nullptr,nullptr),"experimental opt-in before runtime device");
  std::puts("MATRIX_RUNTIME_EXPERIMENTAL_PASS unknown=1 malformed=3 unchanged=1 removal=2 lifetime=1");
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size;
  desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (type == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)), "buffer");
  return result;
}
static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &b);
}
static void wait(ID3D12Fence *fence, UINT64 value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event");
  check(fence->SetEventOnCompletion(value, event), "event completion");
  require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU timeout"); CloseHandle(event);
}
struct Commands {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandListPreview> list;
  Commands(ID3D12Device *device) {
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "preview list");
    ComPtr<ID3D12GraphicsCommandList11> predecessor; check(list.As(&predecessor), "CL11 identity");
  }
  void submit(ID3D12CommandQueue *queue) {
    check(list->Close(), "close"); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
  }
};
static UINT bytes(UINT type) { return type == 4 || type == 5 || type == 8 ? 4 : type == 2 || type == 3 || type == 7 ? 2 : 1; }
static UINT address(UINT row, UINT column, UINT layout, UINT type) {
  const UINT pitch = layout >= 2 ? 5 * bytes(type) : 32;
  return layout == 1 ? column * pitch + row * bytes(type) : row * pitch + column * bytes(type);
}
static void conversions(ID3D12Device *device, ID3D12DevicePreview *preview, ID3D12CommandQueue *queue) {
  constexpr UINT count = 16, size = count * 256;
  const UINT types[]{2,3,4,5,7,8,18,19,20,21};
  for (UINT source_type : types) {
    auto upload = buffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto initial = buffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto source = buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto dest = buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto readback = buffer(device, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<unsigned char> expected(size, 0xa5);
    unsigned char *mapped; D3D12_RANGE empty{0,0};
    check(upload->Map(0, &empty, reinterpret_cast<void **>(&mapped)), "upload map"); std::memset(mapped, 0x5a, size);
    std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO> descriptions;
    UINT index = 0;
    const UINT dest_type = source_type;
    const UINT dest_offset = source_type == 18 ? 32 : source_type == 7 ? 64 : 0;
    for (UINT sl = 0; sl < 4; ++sl) for (UINT dl = 0; dl < 4; ++dl, ++index) {
      D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO d{};
      d.DestInfo = {0, D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT(dl), dl >= 2 ? 7u : 32u, 3, 5, D3D12_LINEAR_ALGEBRA_DATATYPE(dest_type)};
      preview->GetLinearAlgebraMatrixConversionDestinationInfo(&d.DestInfo);
      require(d.DestInfo.DestSize && d.DestInfo.DestSize <= 256 && d.DestInfo.DestSize % 16 == 0, "destination size");
      d.SrcInfo = {256, D3D12_LINEAR_ALGEBRA_DATATYPE(source_type), D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT(sl), sl >= 2 ? 7u : 32u};
      d.DataDesc = {dest->GetGPUVirtualAddress() + index * 256 + dest_offset, source->GetGPUVirtualAddress() + index * 256};
      for (UINT row = 0; row < 3; ++row) for (UINT column = 0; column < 5; ++column) {
        // Raw bits include infinities, NaNs, negative zero, subnormals and
        // large integers. Layout conversion must never interpret them.
        const UINT patterns[]{0,0x80000000,0x7fc00001,0x7f800000,0xff800000,0x00000001,0xffffffff,0x7fffffff,
                              0x80007c00,0x00007e01,0x00008000,0x0000007f,0x000000ff,0x00000080,0x5a123456};
        UINT from = patterns[row*5+column], to = from;
        std::memcpy(mapped + index * 256 + address(row, column, sl, source_type), &from, bytes(source_type));
        std::memcpy(expected.data() + index * 256 + dest_offset + address(row, column, dl, dest_type), &to, bytes(dest_type));
      }
      descriptions.push_back(d);
    }
    upload->Unmap(0, nullptr);
    check(initial->Map(0, &empty, reinterpret_cast<void **>(&mapped)), "sentinel map"); std::memset(mapped, 0xa5, size); initial->Unmap(0, nullptr);
    Commands commands(device);
    commands.list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, size);
    commands.list->CopyBufferRegion(dest.Get(), 0, initial.Get(), 0, size);
    D3D12_BUFFER_BARRIER enhanced[2]{};
    const bool enhanced_case=source_type==8;
    if(enhanced_case) {
      // Frozen enhanced-barrier interop contract: legacy COPY_DEST must
      // return to COMMON before either buffer enters enhanced access tracking.
      transition(commands.list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
      transition(commands.list.Get(),dest.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
      for(UINT i=0;i<2;++i) {
        enhanced[i].SyncBefore=D3D12_BARRIER_SYNC_COPY;
        enhanced[i].SyncAfter=D3D12_BARRIER_SYNC(0x20000000);
        enhanced[i].AccessBefore=D3D12_BARRIER_ACCESS_COPY_DEST;
        enhanced[i].AccessAfter=i?D3D12_BARRIER_ACCESS_UNORDERED_ACCESS:D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        enhanced[i].pResource=i?dest.Get():source.Get(); enhanced[i].Size=UINT64_MAX;
      }
      D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER,2}; group.pBufferBarriers=enhanced;
      commands.list->Barrier(1,&group);
    } else {
      transition(commands.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      transition(commands.list.Get(), dest.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    commands.list->ConvertLinearAlgebraMatrix(descriptions.data(), descriptions.size());
    if(enhanced_case) {
      for(UINT i=0;i<2;++i) {
        enhanced[i].SyncBefore=D3D12_BARRIER_SYNC(0x20000000); enhanced[i].SyncAfter=D3D12_BARRIER_SYNC_COPY;
        enhanced[i].AccessBefore=enhanced[i].AccessAfter;
        enhanced[i].AccessAfter=i?D3D12_BARRIER_ACCESS_COPY_SOURCE:D3D12_BARRIER_ACCESS_COPY_DEST;
      }
      D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER,2}; group.pBufferBarriers=enhanced;
      commands.list->Barrier(1,&group);
    } else {
      transition(commands.list.Get(), source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      transition(commands.list.Get(), dest.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    commands.list->CopyBufferRegion(readback.Get(), 0, dest.Get(), 0, size);
    if(enhanced_case) {
      enhanced[1].SyncBefore=D3D12_BARRIER_SYNC_COPY; enhanced[1].SyncAfter=D3D12_BARRIER_SYNC_COPY;
      enhanced[1].AccessBefore=D3D12_BARRIER_ACCESS_COPY_SOURCE; enhanced[1].AccessAfter=D3D12_BARRIER_ACCESS_COPY_DEST;
      D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER,1}; group.pBufferBarriers=enhanced+1;
      commands.list->Barrier(1,&group);
    } else transition(commands.list.Get(), dest.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Fence> done; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "done");
    commands.submit(queue);
    for (UINT replay = 0; replay < 2; ++replay) {
      if (replay) { ID3D12CommandList *list[]{commands.list.Get()}; queue->ExecuteCommandLists(1, list); }
      check(queue->Signal(done.Get(), replay + 1), "signal"); wait(done.Get(), replay + 1);
      D3D12_RANGE range{0,size}; check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "readback");
      for (UINT i = 0; i < size; ++i) if (mapped[i] != expected[i]) {
        std::fprintf(stderr, "MATRIX_RUNTIME_MISMATCH src=%u byte=%u actual=%02x expected=%02x replay=%u\n", source_type, i, mapped[i], expected[i], replay); std::exit(1);
      }
      readback->Unmap(0, &empty);
    }
    std::printf("MATRIX_RUNTIME_CONVERSIONS_PASS src=%u layouts=16 replay=2 padding=preserved bits=exact\n", source_type);
  }
  std::puts("MATRIX_RUNTIME_ALIGNMENT_PASS sint8_offset=32 float16_offset=64 guards=preserved");
}
static void predication(ID3D12Device *device, ID3D12DevicePreview *preview, ID3D12CommandQueue *queue) {
  auto upload=buffer(device,768,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source=buffer(device,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto dest=buffer(device,512,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicate=buffer(device,16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback=buffer(device,512,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  unsigned char *mapped; D3D12_RANGE empty{0,0};
  check(upload->Map(0,&empty,reinterpret_cast<void **>(&mapped)),"predicate upload");
  std::memset(mapped,0xa5,768);
  for(UINT i=0;i<16;++i) { float value=float(i)-3; std::memcpy(mapped+i*4,&value,4); }
  const UINT64 values[]{0,1}; std::memcpy(mapped+128,values,16);
  std::array<unsigned char,512> expected; std::memset(expected.data(),0xa5,512);
  std::memcpy(expected.data()+256,mapped,64); upload->Unmap(0,nullptr);
  Commands commands(device);
  commands.list->CopyBufferRegion(source.Get(),0,upload.Get(),0,256);
  commands.list->CopyBufferRegion(predicate.Get(),0,upload.Get(),128,16);
  commands.list->CopyBufferRegion(dest.Get(),0,upload.Get(),256,512);
  transition(commands.list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(commands.list.Get(),predicate.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PREDICATION);
  transition(commands.list.Get(),dest.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO conversion{};
  conversion.DestInfo={0,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL,7,1,16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32};
  preview->GetLinearAlgebraMatrixConversionDestinationInfo(&conversion.DestInfo);
  conversion.SrcInfo={256,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,64};
  for(UINT i=0;i<2;++i) {
    conversion.DataDesc={dest->GetGPUVirtualAddress()+i*256,source->GetGPUVirtualAddress()};
    commands.list->SetPredication(predicate.Get(),i*8,D3D12_PREDICATION_OP_EQUAL_ZERO);
    commands.list->ConvertLinearAlgebraMatrix(&conversion,1);
  }
  commands.list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(commands.list.Get(),dest.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(readback.Get(),0,dest.Get(),0,512);
  ComPtr<ID3D12Fence> done; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"predicate done");
  commands.submit(queue); check(queue->Signal(done.Get(),1),"predicate signal"); wait(done.Get(),1);
  D3D12_RANGE range{0,512}; check(readback->Map(0,&range,reinterpret_cast<void **>(&mapped)),"predicate result");
  require(std::memcmp(mapped,expected.data(),512)==0,"predicate suppress/execute and padding"); readback->Unmap(0,&empty);
  std::puts("MATRIX_RUNTIME_PREDICATION_PASS suppressed=1 executed=1 padding=preserved");
}
static void allocation_lifetime(ID3D12Device *device, ID3D12DevicePreview *preview, ID3D12CommandQueue *queue) {
  auto upload=buffer(device,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source=buffer(device,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto dest=buffer(device,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback=buffer(device,256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  unsigned char *mapped; D3D12_RANGE empty{0,0};
  check(upload->Map(0,&empty,reinterpret_cast<void **>(&mapped)),"lifetime upload");
  for(UINT i=0;i<256;++i) mapped[i]=i^0x71; upload->Unmap(0,nullptr);
  ComPtr<ID3D12Fence> gate,done;
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"lifetime gate");
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"lifetime done");
  {
    Commands init(device); init.list->CopyBufferRegion(source.Get(),0,upload.Get(),0,256);
    transition(init.list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    init.submit(queue); check(queue->Signal(done.Get(),1),"lifetime initialized"); wait(done.Get(),1);
  }
  check(queue->Wait(gate.Get(),1),"lifetime queue gate");
  {
    Commands convert(device); D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO desc{};
    desc.DestInfo={0,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL,0,4,16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32};
    preview->GetLinearAlgebraMatrixConversionDestinationInfo(&desc.DestInfo);
    desc.SrcInfo={256,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,64};
    desc.DataDesc={dest->GetGPUVirtualAddress(),source->GetGPUVirtualAddress()};
    convert.list->ConvertLinearAlgebraMatrix(&desc,1);
    transition(convert.list.Get(),dest.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    convert.list->CopyBufferRegion(readback.Get(),0,dest.Get(),0,256); convert.submit(queue);
  }
  check(queue->Signal(done.Get(),2),"lifetime completion");
  source.Reset(); upload.Reset(); dest.Reset();
  check(gate->Signal(1),"lifetime release"); gate.Reset(); wait(done.Get(),2);
  D3D12_RANGE range{0,256}; check(readback->Map(0,&range,reinterpret_cast<void **>(&mapped)),"lifetime result");
  for(UINT i=0;i<256;++i) require(mapped[i]==(i^0x71),"VA retained source allocation"); readback->Unmap(0,&empty);
  std::puts("MATRIX_RUNTIME_ALLOCATION_LIFETIME_PASS released_resources=1 released_recording=1");
}
static void validation(ID3D12Device *device, ID3D12DevicePreview *preview) {
  auto source=buffer(device,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  auto dest=buffer(device,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO valid{};
  valid.DestInfo={0,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,16,1,4,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32};
  preview->GetLinearAlgebraMatrixConversionDestinationInfo(&valid.DestInfo);
  require(valid.DestInfo.DestSize==16,"dense row footprint");
  valid.SrcInfo={16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,16};
  valid.DataDesc={dest->GetGPUVirtualAddress(),source->GetGPUVirtualAddress()};
  for(UINT invalid=0;invalid<10;++invalid) {
    Commands commands(device); auto desc=valid;
    switch(invalid) {
    case 0: desc.DataDesc.SrcVA=0; break;
    case 1: ++desc.DataDesc.DestVA; break;
    case 2: desc.DataDesc.DestVA=desc.DataDesc.SrcVA; break;
    case 3: desc.SrcInfo.SrcDataType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16; break;
    case 4: desc.DestInfo.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT(4); break;
    case 5: desc.SrcInfo.SrcSize=15; break;
    case 6: desc.DestInfo.DestSize=15; break;
    case 7: desc.SrcInfo.SrcStride=17; break;
    case 8: desc.DestInfo.DestStride=17; break;
    default: break;
    }
    commands.list->ConvertLinearAlgebraMatrix(invalid==9 ? nullptr : &desc,1);
    require(commands.list->Close()==E_INVALIDARG,"invalid conversion Close");
  }
  auto huge=valid.DestInfo; huge.NumRows=huge.NumColumns=UINT32_MAX; huge.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL;
  preview->GetLinearAlgebraMatrixConversionDestinationInfo(&huge); require(huge.DestSize==0,"overflow footprint");
  std::puts("MATRIX_RUNTIME_VALIDATION_PASS invalid_recordings=10 overflow=1");
}
static void fence_barriers(ID3D12Device *device, ID3D12CommandQueue *producer, ID3D12CommandQueue *consumer) {
  auto upload = buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto intermediate = buffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  unsigned char *mapped; D3D12_RANGE empty{0,0}; check(upload->Map(0,&empty,reinterpret_cast<void **>(&mapped)), "fence upload");
  for (UINT i=0;i<256;++i) mapped[i]=unsigned(i)^0x59; upload->Unmap(0,nullptr);
  ComPtr<ID3D12Fence> crossing, gate, done;
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&crossing)),"crossing fence");
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate fence");
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"done fence");
  Commands output(device), input(device);
  input.list->CopyBufferRegion(intermediate.Get(),0,upload.Get(),0,256);
  transition(input.list.Get(),intermediate.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
  input.list->SignalBarrier(0,nullptr,crossing.Get(),7);
  ID3D12Fence *fences[]{crossing.Get(),gate.Get()}; UINT64 values[]{7,1};
  output.list->WaitBarrier(0,nullptr,2,fences,values);
  transition(output.list.Get(),intermediate.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  output.list->CopyBufferRegion(readback.Get(),0,intermediate.Get(),0,256);
  output.submit(consumer); input.submit(producer);
  check(consumer->Signal(done.Get(),1),"consumer done");
  wait(crossing.Get(),7); require(done->GetCompletedValue()==0,"consumer must wait on second fence");
  check(crossing->Signal(0),"logical rewind"); require(crossing->GetCompletedValue()==0,"rewind visible");
  // Release all public recording/data owners while the consumer worker is blocked.
  input.list.Reset(); input.allocator.Reset(); output.list.Reset(); output.allocator.Reset(); upload.Reset(); intermediate.Reset();
  check(gate->Signal(1),"release gate"); gate.Reset(); crossing.Reset();
  wait(done.Get(),1);
  D3D12_RANGE range{0,256}; check(readback->Map(0,&range,reinterpret_cast<void **>(&mapped)),"fence result");
  for(UINT i=0;i<256;++i) require(mapped[i]==(unsigned(i)^0x59),"crossqueue result"); readback->Unmap(0,&empty);
  std::puts("MATRIX_RUNTIME_FENCE_BARRIERS_PASS crossqueue=1 rewind=1 async=1 lifetime=1 state_handoff=1");
}
#ifndef DXMT_MATRIX_RUNTIME_NO_MAIN
int main() {
  experimental_admission();
  ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  using BackendId=const char *(WINAPI *)();
  const auto identity=reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  require(identity && std::strcmp(identity(),"owned-graphics-d3d12")==0,"backend identity");
  std::printf("MATRIX_RUNTIME_BACKEND id=%s\n",identity());
  ComPtr<ID3D12DevicePreview> preview; check(device.As(&preview),"DevicePreview");
  require(preview->ConfigureDumpFile(D3D12_DUMP_FILE_DRIVER_OPTION_NO_OVERHEAD)==DXGI_ERROR_UNSUPPORTED,"truthful dump cap");
  require(preview->AddBlobToDumpFile(nullptr,0,0)==DXGI_ERROR_UNSUPPORTED,"truthful dump blob");
  require(preview->GetDeviceErrorCode()==D3D12_DEVICE_ERROR_NONE,"device error code");
  D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT tier{}; check(device->CheckFeatureSupport(D3D12_FEATURE(77),&tier,sizeof(tier)),"tier ABI");
  require(device->CheckFeatureSupport(D3D12_FEATURE(77),&tier,sizeof(tier)-1)==E_INVALIDARG,"tier wrongsize");
  for(UINT operation=0;operation<6;++operation) {
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT query{}; query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE(operation);
    check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"operation ABI");
  }
  D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT query{};
  query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
  query.WaveMatrixMultiply.Inputs={32,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32};
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"wave count");
  const bool advertised=tier.LinearAlgebraTier!=D3D12_LINEAR_ALGEBRA_TIER_NOT_SUPPORTED;
  require(query.WaveMatrixMultiply.NumShapes==(advertised?1u:0u),"wave count value");
  D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE shapes[2]{{99,99,99},{77,77,77}};
  query.WaveMatrixMultiply.NumShapes=2; query.WaveMatrixMultiply.Shapes=shapes;
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"wave fill");
  require(shapes[1].M==77 && shapes[1].K==77 && shapes[1].N==77,"count capacity tail");
  if(advertised) require(shapes[0].M==16 && shapes[0].K==16 && shapes[0].N==16,"native tile");
  query.WaveMatrixMultiply.NumShapes=1; query.WaveMatrixMultiply.Shapes=nullptr;
  require(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query))==E_INVALIDARG,"nonzero null shapes");
  query.WaveMatrixMultiply.NumShapes=0; query.WaveMatrixMultiply.Inputs.WaveSize=31;
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"unsupported wave");
  require(!query.WaveMatrixMultiply.NumShapes && !query.WaveMatrixMultiply.SupportFlags,"unsupported wave output");
  query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_MATRIX_CONSTRUCTION;
  query.MatrixConstruction={D3D12_LINEAR_ALGEBRA_DATATYPE(6),32,99,99,99};
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"unsupported component");
  require(!query.MatrixConstruction.MinM && !query.MatrixConstruction.MinK && !query.MatrixConstruction.MinN,"unsupported dimensions");
  query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE(6);
  require(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query))==E_INVALIDARG,"unknown operation");
  require(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)-1)==E_INVALIDARG,"query wrongsize");
  struct LegacyQuery {UINT input,m,n; BOOL supported; UINT k,accum,min,max;} legacy{2,1,1};
  check(device->CheckFeatureSupport(D3D12_FEATURE(38),&legacy,sizeof(legacy)),"legacy ABI");
#ifdef DXMT_MATRIX_PREVIEW_SDK_HEADER
  constexpr UINT pinned_sdk=1;
#else
  constexpr UINT pinned_sdk=0;
#endif
  std::printf("MATRIX_RUNTIME_ABI_PASS pointer=%zu union=%zu tier=%u legacy=%u pinned_sdk=%u\n",sizeof(void *),sizeof(D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT),UINT(tier.LinearAlgebraTier),legacy.supported,pinned_sdk);
  D3D12_COMMAND_QUEUE_DESC desc{}; ComPtr<ID3D12CommandQueue> producer,consumer;
  check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&producer)),"producer queue");
  check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&consumer)),"consumer queue");
  validation(device.Get(),preview.Get()); conversions(device.Get(),preview.Get(),producer.Get());
  allocation_lifetime(device.Get(),preview.Get(),producer.Get());
  predication(device.Get(),preview.Get(),producer.Get()); fence_barriers(device.Get(),producer.Get(),consumer.Get());
  require(device->GetDeviceRemovedReason()==S_OK,"device survived"); std::puts("MATRIX_RUNTIME_PASS");
}
#endif
