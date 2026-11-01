// The production owned D3D12 path, not a native feasibility shader.
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
#include <windows.h>
#include <rpc.h>
#include <rpcndr.h>
#if defined(__clang__) && defined(__MINGW32__)
#undef __uuidof
#undef MIDL_INTERFACE
#define MIDL_INTERFACE(value) struct __declspec(uuid(value))
#endif
#include D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
#endif
#define wmain sm6_suite_main
#include "owned-d3d12-sm6.cpp"
#undef wmain
#ifndef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
#include "../../src/d3d12/d3d12_interfaces_ext.hpp"
#endif

static float matrixA(unsigned r,unsigned c) { return float(int((r*3+c*5)%17)-8)*.25f; }
static float matrixB(unsigned r,unsigned c) { return float(int((r*7+c*2)%13)-6)*.5f; }
static float fp8Value(unsigned code,bool e4) {
  unsigned fraction=e4?3:2,bias=e4?7:15,exponent=(code>>fraction)&(e4?15:31),mantissa=code&((1u<<fraction)-1);
  float value=exponent?std::ldexp(1.0f+float(mantissa)/(1u<<fraction),int(exponent)-int(bias)):std::ldexp(float(mantissa),1-int(bias)-int(fraction));
  if(e4 && exponent==15 && mantissa==7)value=NAN;
  if(!e4 && exponent==31)value=mantissa?NAN:INFINITY;
  return code&128?-value:value;
}
static float fp8Round(float value,bool e4) {
  float best=0,error=INFINITY;unsigned bestCode=0;
  for(unsigned code=0;code<256;++code) {
    float candidate=fp8Value(code,e4),distance=std::fabs(candidate-value);
    if(distance<error || (distance==error && !(code&1) && (bestCode&1))) {best=candidate;error=distance;bestCode=code;}
  }
  return best;
}
static void exactBytes(const char *name,const std::vector<std::vector<uint8_t>> &result,const std::vector<uint8_t> &expected) {
  if(result.size()!=3 || result[0].size()!=expected.size())die("matrix result buffer contract");
  for(unsigned i=0;i<expected.size();++i) EXPECT_EQ(name,"result.and.guards",i,result[0][i],expected[i]);
}
int wmain() {
  using BackendIdFn=const char *(WINAPI *)();
  auto backend=reinterpret_cast<BackendIdFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  if(!backend || !backend() || std::strcmp(backend(),kExpectedBackendId))die("matrix backend identity");
  std::printf("OWNED_D3D12_MATRICES_BACKEND id=%s\n",backend());
  using EnableExperimentalFeaturesFn=HRESULT (WINAPI *)(UINT,const IID *,void *,UINT *);
  auto enableExperimental=reinterpret_cast<EnableExperimentalFeaturesFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"D3D12EnableExperimentalFeatures"));
  if(!enableExperimental)die("matrix experimental shader export");
  check(enableExperimental(1,&D3D12ExperimentalShaderModels,nullptr,nullptr),"matrix experimental shader admission");
  std::puts("MATRIX_PUBLIC_SHADER_ADMISSION_PASS");
  ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"matrix factory");
  ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapters1(0,&adapter),"matrix adapter");
  ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"matrix device");
  Gpu gpu(device.Get());Compute compute{gpu,computeRoot(device.Get()),nullptr};
  for(const char *entry:{"cs_wavematrix","cs_matrix_wave","cs_matrix_group"}) {
    begin();auto input=filled(3072),expected=filled(!std::strcmp(entry,"cs_matrix_wave")?8192:!std::strcmp(entry,"cs_wavematrix")?9472:1536);
    bool retired=!std::strcmp(entry,"cs_wavematrix"),group=!std::strcmp(entry,"cs_matrix_group");
    for(unsigned r=0;r<16;++r)for(unsigned c=0;c<16;++c) {put(input,(retired?0:32)+r*(retired?64:80)+c*4,matrixA(r,c));put(input,(retired?1024:1536)+c*(retired?64:80)+r*4,matrixB(r,c));}
    for(unsigned r=0;r<16;++r)for(unsigned c=0;c<16;++c) {
      float product=0,left=0,right=0;
      for(unsigned k=0;k<16;++k) {product=std::fma(matrixA(r,k),matrixB(k,c),product);left+=matrixA(r,k);right+=matrixB(k,c);}
      float final=group?2*product:retired?2*product+2+left+right:2*product+matrixA(r,c);
      put(expected,(retired?0:32)+r*(retired?64:80)+c*4,final);
      if(retired) {if(c==0){put(expected,1024+r*4,left);put(expected,7552+r*4,int32_t(-16));}if(r==0){put(expected,1088+c*4,right);put(expected,7616+c*4,int32_t(1548));}put(expected,1280+(r*16+c)*4,96.0f);put(expected,2304+(r*16+c)*4,int32_t(-32));put(expected,3456+(r*16+c)*4,int32_t(-128024));put(expected,4480+(r*16+c)*4,int32_t(-129556));put(expected,5504+(r*16+c)*4,int32_t(130072));put(expected,6528+(r*16+c)*4,int32_t(325676));}
      if(!retired && !group) {
        put(expected,1536+r*80+c*4,matrixA(r,c));
        float changed=c<2?float(r*2+c):matrixA(r,c);
        put(expected,3584+(r*16+c)*4,changed);put(expected,4736+(r*16+c)*2,floatToHalf(changed));
        expected[5376+r*16+c]=uint8_t(int8_t(std::nearbyint(changed)));
        put(expected,5760+(r*16+c)*4,fp8Round(changed,true));put(expected,6912+(r*16+c)*4,fp8Round(changed,false));
      }
      if(retired){put(expected,7808+(r*16+c)*2,floatToHalf(0.00006103515625f));put(expected,8384+(r*16+c)*4,0.00006103515625f);}
    }
    if(retired)put(expected,1152,uint32_t(16));
    if(!retired && !group) {
      for(unsigned lane=0;lane<32;++lane) {unsigned local=lane%8,row=lane/2,col=lane%2+local*2;put(expected,3008+lane*16,uint32_t(8));put(expected,3012+lane*16,col);put(expected,3016+lane*16,row);put(expected,3020+lane*16,matrixA(row,col));}
      put(expected,8064,uint32_t(0));
    }
    exactBytes(entry,compute.run(entry,filled(expected.size()),input,filled(64),{0,0,0,0},1),expected);end(entry);
  }
  begin();auto input=filled(1024,0),expected=filled(2240);
  for(unsigned r=0;r<16;++r)for(unsigned c=0;c<16;++c) {input[r*16+c]=uint8_t(int8_t(int((r*3+c*7)%255)-128));input[256+c*16+r]=uint8_t(int8_t(int((r*11+c*13)%255)-128));input[512+r*16+c]=uint8_t((r*17+c*19)%256);input[768+c*16+r]=uint8_t((r*23+c*29)%256);}
  for(unsigned r=0;r<16;++r)for(unsigned c=0;c<16;++c) {int32_t s=0,u=0;for(unsigned k=0;k<16;++k){s+=int8_t(input[r*16+k])*int8_t(input[256+c*16+k]);u+=input[512+r*16+k]*input[768+c*16+k];}put(expected,32+(r*16+c)*4,s);put(expected,1152+(r*16+c)*4,u);}
  exactBytes("cs_matrix_required",compute.run("cs_matrix_required",filled(expected.size()),input,filled(64),{0,0,0,0},1),expected);end("cs_matrix_required");

  begin();input=filled(192,0);expected=filled(288);
  const float values[8]={-200,-2.5f,-1.5f,.5f,1.5f,2.5f,127.75f,300};
  const float halfValues[8]={.5f,1,-.5f,2,3,-2,.25f,4};
  const int32_t bias[4]={-100,3,50,1000};
  for(unsigned r=0;r<4;++r)for(unsigned k=0;k<8;++k) {input[r*8+k]=uint8_t(int8_t(int((r*11+k*17)%255)-128));input[32+r*8+k]=uint8_t((r*23+k*31)%256);put(input,64+(r*8+k)*2,floatToHalf(float(int((r*3+k*5)%9)-4)*.5f));}
  for(unsigned k=0;k<8;++k){put(input,128+k*4,values[k]);put(input,160+k*2,floatToHalf(halfValues[k]));}
  for(unsigned r=0;r<4;++r){int32_t s=0,u=0;float h=0,hb=float(r+1);for(unsigned k=0;k<8;++k){int32_t sv=std::clamp(int32_t(std::nearbyint(values[k])),-128,127),uv=std::clamp(int32_t(std::nearbyint(values[k])),0,255);s+=int8_t(input[r*8+k])*sv;u+=input[32+r*8+k]*uv;float a=float(int((r*3+k*5)%9)-4)*.5f;h=std::fma(a,halfValues[k],h);hb=std::fma(a,halfValues[k],hb);}for(unsigned offset:{32u,160u})put(expected,offset+r*4,s);for(unsigned offset:{64u,192u})put(expected,offset+r*4,s+bias[r]);put(expected,96+r*4,u);put(expected,128+r*4,u+bias[r]);put(expected,224+r*2,floatToHalf(h));put(expected,256+r*2,floatToHalf(hb));}
  // Only the converter can populate the shader's three weight matrices.
  // Source columns have stride 16; optimal destination rows are dense.
  auto columnMajor=filled(768,0xcacacaca);
  const UINT weightOffsets[]{0,32,64},componentBytes[]{1,1,2};
  const D3D12_LINEAR_ALGEBRA_DATATYPE componentTypes[]{D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16};
  for(UINT format=0;format<3;++format)for(UINT r=0;r<4;++r)for(UINT c=0;c<8;++c)
    std::memcpy(columnMajor.data()+format*256+c*16+r*componentBytes[format],input.data()+weightOffsets[format]+(r*8+c)*componentBytes[format],componentBytes[format]);
  std::fill(input.begin(),input.begin()+128,0xcd);
  auto source=gpu.uavBuffer(columnMajor),threadInput=gpu.uavBuffer(input);
  auto optimal=gpu.buffer(768,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  gpu.transition(source.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  gpu.transition(threadInput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DevicePreview> previewDevice;check(device.As(&previewDevice),"converted vector DevicePreview");
  ComPtr<ID3D12GraphicsCommandListPreview> previewList;check(gpu.list.As(&previewList),"converted vector CLPreview");
  D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO conversions[3]{};
  for(UINT format=0;format<3;++format) {
    auto &conversion=conversions[format];
    conversion.DestInfo={0,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL,0,4,8,componentTypes[format]};
    previewDevice->GetLinearAlgebraMatrixConversionDestinationInfo(&conversion.DestInfo);
    if(conversion.DestInfo.DestSize!=32*componentBytes[format])die("converted vector destination size");
    conversion.SrcInfo={128,componentTypes[format],D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_COLUMN_MAJOR,16};
    conversion.DataDesc={optimal->GetGPUVirtualAddress()+format*256,source->GetGPUVirtualAddress()+format*256};
  }
  previewList->ConvertLinearAlgebraMatrix(conversions,3);
  gpu.transition(optimal.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  for(UINT format=0;format<3;++format)gpu.list->CopyBufferRegion(threadInput.Get(),weightOffsets[format],optimal.Get(),format*256,32*componentBytes[format]);
  gpu.transition(threadInput.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=UINT(input.size()/4);srv.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;device->CreateShaderResourceView(threadInput.Get(),&srv,gpu.cpu(3));
  exactBytes("cs_matrix_vector",compute.run("cs_matrix_vector",filled(expected.size()),filled(64),filled(64),{0,0,0,0},1),expected);end("cs_matrix_vector");
  if(!g_failures)std::puts("MATRIX_CONVERTED_VECTOR_CONSUMER_PASS");

  begin();expected=filled(6912,0);for(unsigned i=0;i<256;++i){put(expected,32+i*4,2.0f);put(expected,1152+i*4,2.0f);for(unsigned offset:{2560u,3648u,4736u,5824u})put(expected,offset+i*4,uint32_t(0xfffffffd));}for(unsigned i=0;i<4;++i)put(expected,2304+i*4,float(i+1));const float outer[4]={-1,2,0,4};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)put(expected,2368+(r*4+c)*4,float(r+1)*outer[c]);
  exactBytes("cs_matrix_atomic",compute.run("cs_matrix_atomic",filled(expected.size(),0),filled(64),filled(64),{0,0,0,0},1),expected);end("cs_matrix_atomic");
  begin();input=filled(1536);expected=filled(40992);
  for(unsigned r=0;r<16;++r)for(unsigned c=0;c<16;++c)put(input,32+r*80+c*4,matrixA(r,c));
  const unsigned sizes[10]={2,2,4,4,2,4,1,1,1,1};
  for(unsigned format=0;format<10;++format)for(unsigned r=0;r<16;++r)for(unsigned c=0;c<16;++c) {
    float value=matrixA(r,c),converted=value;unsigned bytes=sizes[format],raw=0,one=1;
    if(format==4) {raw=floatToHalf(value);one=floatToHalf(1);}
    else if(format==5) {raw=floatBits(value);one=floatBits(1);}
    else if(format>=8) {converted=fp8Round(value,format==8);one=format==8?56:60;for(unsigned code=0;code<256;++code)if(floatBits(fp8Value(code,format==8))==floatBits(converted)){raw=code;break;}}
    else {int32_t rounded=int32_t(std::nearbyint(value));if(format==1 || format==3 || format==7)rounded=std::max(rounded,0);raw=uint32_t(rounded);converted=float(rounded);}
    unsigned flat=r*16+c,base=format*4096;
    std::memcpy(expected.data()+32+base+flat*bytes,&raw,bytes);
    put(expected,1056+base+flat*4,converted);
    std::memcpy(expected.data()+2080+base+flat*bytes,&one,bytes);
  }
  exactBytes("cs_matrix_formats",compute.run("cs_matrix_formats",filled(expected.size()),input,filled(64),{0,0,0,0},1),expected);end("cs_matrix_formats");
  begin();exactBytes("cs_matrix_groupformats",compute.run("cs_matrix_groupformats",filled(expected.size()),input,filled(64),{0,0,0,0},1),expected);end("cs_matrix_groupformats");
  const struct {const char *entry;unsigned m,n;} largeCases[]={{"cs_wavematrix16x64",16,64},{"cs_wavematrix64x16",64,16},{"cs_wavematrix64x64",64,64}};
  for(const auto &test:largeCases) {
    begin();input=filled(14336);unsigned stride=test.n*4+16,leftOffset=64+test.m*stride,rightOffset=leftOffset+test.m*8+32;expected=filled(rightOffset+test.n*8+64);
    for(unsigned r=0;r<test.m;++r)for(unsigned k=0;k<16;++k)put(input,32+r*80+k*4,matrixA(r,k));
    for(unsigned c=0;c<test.n;++c)for(unsigned k=0;k<16;++k)put(input,8192+c*80+k*4,matrixB(k,c));
    for(unsigned r=0;r<test.m;++r)for(unsigned c=0;c<test.n;++c) {float product=0,left=0,right=0;for(unsigned k=0;k<16;++k){product=std::fma(matrixA(r,k),matrixB(k,c),product);left+=matrixA(r,k);right+=matrixB(k,c);}put(expected,32+r*stride+c*4,2*product+left+right);if(c==0)put(expected,leftOffset+r*8,left);if(r==0)put(expected,rightOffset+c*8,right);}
    exactBytes(test.entry,compute.run(test.entry,filled(expected.size()),input,filled(64),{0,0,0,0},1),expected);end(test.entry);
  }
  std::printf("OWNED_D3D12_MATRICES_%s failures=%u\n",g_failures?"FAIL":"PASS",g_failures);
  return g_failures?1:0;
}
