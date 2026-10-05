// Final positive-capability smoke: build separately against each untouched public SDK.
#ifdef DXMT_MATRIX_RETIRED_SDK_HEADER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <rpc.h>
#include <rpcndr.h>
#if defined(__clang__) && defined(__MINGW32__)
#undef __uuidof
#undef DECLSPEC_UUID
#define DECLSPEC_UUID(value) __declspec(uuid(value))
#undef MIDL_INTERFACE
#define MIDL_INTERFACE(value) struct __declspec(uuid(value))
#endif
#include DXMT_MATRIX_RETIRED_SDK_HEADER
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr,const char *step) {
  if(FAILED(hr)){std::fprintf(stderr,"MATRIX_CAPABILITIES_FAIL %s hr=%08lx\n",step,(unsigned long)hr);std::exit(1);}
}
static void require(bool value,const char *step){if(!value)check(E_FAIL,step);}
static_assert(sizeof(D3D12_FEATURE_DATA_WAVE_MMA)==32);
#else
#ifndef DXMT_MATRIX_PREVIEW_SDK_HEADER
#error Build with an untouched pinned public SDK header.
#endif
// Reuse the established single-conversion exact GPU readback, not the full runtime suite.
#define DXMT_MATRIX_RUNTIME_NO_MAIN
#include "owned-d3d12-matrix-runtime.cpp"
#endif

int main() {
  check(D3D12EnableExperimentalFeatures(1,&D3D12ExperimentalShaderModels,nullptr,nullptr),"public experimental opt-in");
  ComPtr<ID3D12Device> device;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  using BackendId=const char *(WINAPI *)();
  const auto identity=reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  require(identity && std::strcmp(identity(),"owned-graphics-d3d12")==0,"backend identity");
  std::printf("MATRIX_CAPABILITIES_BACKEND id=%s\n",identity());
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9,&options,sizeof(options)),"OPTIONS9");
  require(options.WaveMMATier==D3D12_WAVE_MMA_TIER_1_0,"positive WaveMMATier");
#ifdef DXMT_MATRIX_RETIRED_SDK_HEADER
  for(UINT input=1;input<=3;++input)for(UINT m=1;m<=2;++m)for(UINT n=1;n<=2;++n) {
    D3D12_FEATURE_DATA_WAVE_MMA query{};
    query.InputDataType=D3D12_WAVE_MMA_INPUT_DATATYPE(input);
    query.M=D3D12_WAVE_MMA_DIMENSION(m);query.N=D3D12_WAVE_MMA_DIMENSION(n);
    check(device->CheckFeatureSupport(D3D12_FEATURE_WAVE_MMA,&query,sizeof(query)),"retired public query");
    require(query.Supported && query.K==16 && UINT(query.AccumDataTypes)==(input==1?1u:input==2?6u:4u) &&
            query.RequiredWaveLaneCountMin==32 && query.RequiredWaveLaneCountMax==32,"retired advertised shape");
  }
  std::printf("MATRIX_CAPABILITIES_PASS sdk=retired pointer=%zu query=32 wave_mma_tier=10 shapes=12\n",sizeof(void *));
#else
  D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT tier{};
  check(device->CheckFeatureSupport(D3D12_FEATURE(77),&tier,sizeof(tier)),"linear algebra tier");
  require(tier.LinearAlgebraTier==D3D12_LINEAR_ALGEBRA_TIER_1_0,"positive linear algebra tier");
  D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT query{};
  for(UINT component:{2u,3u,4u,5u,7u,8u,18u,19u,20u,21u}) {
    query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_MATRIX_CONSTRUCTION;
    query.MatrixConstruction={D3D12_LINEAR_ALGEBRA_DATATYPE(component),32,0,0,0};
    check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"construction support");
    require(query.MatrixConstruction.MinM==16 && query.MatrixConstruction.MinK==16 && query.MatrixConstruction.MinN==16,"construction dimensions");
  }
  for(UINT component:{18u,19u,7u}) {
    const auto input=D3D12_LINEAR_ALGEBRA_DATATYPE(component);
    const auto accumulator=component==7?D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16:D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32;
    query={};query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
    query.WaveMatrixMultiply.Inputs={32,input,input,accumulator};
    check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"wave count");
    const UINT flags=component==7?5:1;
    require(query.WaveMatrixMultiply.NumShapes==1 && UINT(query.WaveMatrixMultiply.SupportFlags)==flags,"positive wave count flags");
    D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE shapes[2]{{0,0,0},{77,77,77}};
    query.WaveMatrixMultiply.NumShapes=2;query.WaveMatrixMultiply.Shapes=shapes;
    check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"wave fill");
    require(query.WaveMatrixMultiply.NumShapes==1 && shapes[0].M==16 && shapes[0].K==16 && shapes[0].N==16 &&
            shapes[1].M==77 && shapes[1].K==77 && shapes[1].N==77,"native tile and untouched capacity tail");
    query={};query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREADGROUP_MATRIX_MULTIPLY;
    query.ThreadGroupMatrixMultiply.WaveInputs={32,input,input,accumulator};query.ThreadGroupMatrixMultiply.Shape={16,16,16};
    check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"threadgroup support");
    require(UINT(query.ThreadGroupMatrixMultiply.SupportFlags)==flags && query.ThreadGroupMatrixMultiply.MinThreadGroupSize==32 &&
            query.ThreadGroupMatrixMultiply.MaxThreadGroupSize==1024 && query.ThreadGroupMatrixMultiply.PreferredThreadGroupSize==32,"threadgroup dimensions flags");
  }
  query={};query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
  query.ThreadVectorMatrixMultiply.VectorInputType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
  query.ThreadVectorMatrixMultiply.MatrixInputType=D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8;
  query.ThreadVectorMatrixMultiply.VectorResultType=D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32;
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"thread vector support");
  require(UINT(query.ThreadVectorMatrixMultiply.SupportFlags)==9,"vector supported transpose");
  query={};query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_OUTER_PRODUCT;
  query.ThreadOuterProductSupport.InputComponentType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
  query.ThreadOuterProductSupport.ResultComponentType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"outer product support");
  require(query.ThreadOuterProductSupport.Supported,"outer product positive");
  query={};query.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_ATOMIC_ACCUMULATE_STORE;
  query.AccumulateStore.ComponentType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
  check(device->CheckFeatureSupport(D3D12_FEATURE(78),&query,sizeof(query)),"atomic accumulate support");
  require(query.AccumulateStore.RWByteAddressBufferSupported && query.AccumulateStore.GroupSharedSupported,"atomic buffer and group positive");
  ComPtr<ID3D12DevicePreview> preview;check(device.As(&preview),"DevicePreview");
  D3D12_COMMAND_QUEUE_DESC queue_desc{};ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)),"queue");
  allocation_lifetime(device.Get(),preview.Get(),queue.Get());
  std::printf("MATRIX_CAPABILITIES_PASS sdk=successor pointer=%zu query=%zu wave_mma_tier=10 linear_algebra_tier=16 operations=6 readback=exact\n",sizeof(void *),sizeof(query));
#endif
  require(device->GetDeviceRemovedReason()==S_OK,"device survived positive smoke");
}
