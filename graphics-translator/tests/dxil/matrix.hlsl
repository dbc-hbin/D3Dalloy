#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
RWByteAddressBuffer outputBuffer : register(u0);
RWByteAddressBuffer inputBuffer : register(u1);
ByteAddressBuffer threadInput : register(t0);
groupshared float sharedData[1024];
groupshared int sharedSigned[256];
groupshared uint sharedUnsigned[256];

[WaveSize(32)] [numthreads(32,1,1)]
void cs_matrix_wave(uint lane : SV_GroupIndex) {
  using A = Matrix<ComponentType::F32,16,16,MatrixUse::A,MatrixScope::Wave>;
  using B = Matrix<ComponentType::F32,16,16,MatrixUse::B,MatrixScope::Wave>;
  auto a=A::Load(inputBuffer,32,80,MatrixLayout::RowMajor,4);
  auto b=B::Load(inputBuffer,1536,80,MatrixLayout::ColMajor,4);
  a.Store(sharedData,0,16,MatrixLayout::RowMajor);
  b.Store(sharedData,256,16,MatrixLayout::ColMajor);
  GroupMemoryBarrierWithGroupSync();
  a=A::Load(sharedData,0,16,MatrixLayout::RowMajor);
  b=B::Load(sharedData,256,16,MatrixLayout::ColMajor);
  auto c=Multiply(a,b);
  c.MultiplyAccumulate(a,b);
  c.Accumulate(a);
  c.Store(outputBuffer,32,80,MatrixLayout::RowMajor,4);
  auto trans=a.Cast<ComponentType::F32,MatrixUse::B,true>();
  trans.Store(outputBuffer,1536,80,MatrixLayout::ColMajor,4);
  uint2 xy=a.GetCoordinate(lane%a.Length());
  outputBuffer.Store4(3008+lane*16,uint4(a.Length(),xy,asuint(a.Get(lane%a.Length()))));
  a.Set(0,float(lane));
  a.Store(outputBuffer,3584,64,MatrixLayout::RowMajor,4);
  auto h=a.Cast<ComponentType::F16,MatrixUse::A>();
  h.Store(outputBuffer,4736,32,MatrixLayout::RowMajor,2);
  auto i=a.Cast<ComponentType::I8,MatrixUse::A>();
  i.Store(outputBuffer,5376,16,MatrixLayout::RowMajor,4);
  auto f8=a.Cast<ComponentType::F8_E4M3FN,MatrixUse::A>();
  auto f8back=f8.Cast<ComponentType::F32,MatrixUse::A>();
  f8back.Store(outputBuffer,5760,64,MatrixLayout::RowMajor,4);
  auto f5=a.Cast<ComponentType::F8_E5M2,MatrixUse::A>();
  f5.Cast<ComponentType::F32,MatrixUse::A>().Store(outputBuffer,6912,64,MatrixLayout::RowMajor,4);
  if(lane==0)outputBuffer.Store(8064,(uint)AccumulatorLayout());
}

[WaveSize(32)] [numthreads(32,1,1)]
void cs_matrix_required(uint lane : SV_GroupIndex) {
  using SA = Matrix<ComponentType::I8,16,16,MatrixUse::A,MatrixScope::Wave>;
  using SB = Matrix<ComponentType::I8,16,16,MatrixUse::B,MatrixScope::Wave>;
  using UA = Matrix<ComponentType::U8,16,16,MatrixUse::A,MatrixScope::Wave>;
  using UB = Matrix<ComponentType::U8,16,16,MatrixUse::B,MatrixScope::Wave>;
  auto sa=SA::Load(inputBuffer,0,16,MatrixLayout::RowMajor,4);
  auto sb=SB::Load(inputBuffer,256,16,MatrixLayout::ColMajor,4);
  auto ua=UA::Load(inputBuffer,512,16,MatrixLayout::RowMajor,4);
  auto ub=UB::Load(inputBuffer,768,16,MatrixLayout::ColMajor,4);
  Multiply<ComponentType::I32>(sa,sb).Store(outputBuffer,32,64,MatrixLayout::RowMajor,4);
  Multiply<ComponentType::I32>(ua,ub).Store(outputBuffer,1152,64,MatrixLayout::RowMajor,4);
}

[numthreads(1,1,1)]
void cs_matrix_vector(uint lane : SV_DispatchThreadID) {
  using S = Matrix<ComponentType::I8,4,8,MatrixUse::A,MatrixScope::Thread>;
  using U = Matrix<ComponentType::U8,4,8,MatrixUse::A,MatrixScope::Thread>;
  using H = Matrix<ComponentType::F16,4,8,MatrixUse::A,MatrixScope::Thread>;
  auto s=S::Load<MatrixLayout::MulOptimal>(threadInput,0,8,4);
  auto u=U::Load<MatrixLayout::MulOptimal>(threadInput,32,8,4);
  auto h=H::Load<MatrixLayout::MulOptimal>(threadInput,64,16,2);
  vector<float,8> fv=threadInput.Load<vector<float,8> >(128);
  vector<float16_t,8> hv=threadInput.Load<vector<float16_t,8> >(160);
  auto sv=Convert<ComponentType::I8,ComponentType::F32>(fv);
  auto uv=Convert<ComponentType::U8,ComponentType::F32>(fv);
  int4 bias=int4(-100,3,50,1000);
  int4 sp,up;
  __builtin_LinAlg_MatrixVectorMultiply(sp,s.__handle,true,sv.Data,sv.Interpretation);
  __builtin_LinAlg_MatrixVectorMultiply(up,u.__handle,true,uv.Data,uv.Interpretation);
  outputBuffer.Store<int4>(32,sp);
  outputBuffer.Store<int4>(64,MultiplyAdd<int>(s,sv,bias));
  outputBuffer.Store<int4>(96,up);
  outputBuffer.Store<int4>(128,MultiplyAdd<int>(u,uv,bias));
  outputBuffer.Store<int4>(160,Multiply<int>(s,fv));
  outputBuffer.Store<int4>(192,MultiplyAdd<int>(s,fv,bias));
  vector<float16_t,4> hb=vector<float16_t,4>(1,2,3,4);
  outputBuffer.Store<vector<float16_t,4> >(224,Multiply<float16_t>(h,hv));
  outputBuffer.Store<vector<float16_t,4> >(256,MultiplyAdd<float16_t>(h,hv,hb));
}

[WaveSize(32)] [numthreads(64,1,1)]
void cs_matrix_group(uint lane : SV_GroupIndex) {
  using A = Matrix<ComponentType::F32,16,16,MatrixUse::A,MatrixScope::ThreadGroup>;
  using B = Matrix<ComponentType::F32,16,16,MatrixUse::B,MatrixScope::ThreadGroup>;
  auto a=A::Load(inputBuffer,32,80,MatrixLayout::RowMajor,4);
  auto b=B::Load(inputBuffer,1536,80,MatrixLayout::ColMajor,4);
  auto c=Multiply(a,b);
  c.MultiplyAccumulate(a,b);
  c.Store(outputBuffer,32,80,MatrixLayout::RowMajor,4);
}

[WaveSize(32)] [numthreads(32,1,1)]
void cs_matrix_atomic(uint lane : SV_GroupIndex) {
  using C = Matrix<ComponentType::F32,16,16,MatrixUse::Accumulator,MatrixScope::Wave>;
  for(uint i=lane;i<256;i+=32)sharedData[i]=0;
  GroupMemoryBarrierWithGroupSync();
  auto c=C::Splat(2.0f);
  c.InterlockedAccumulate(sharedData,0,16,MatrixLayout::RowMajor);
  c.InterlockedAccumulate(outputBuffer,32,64,MatrixLayout::RowMajor,4);
  GroupMemoryBarrierWithGroupSync();
  C::Load(sharedData,0,16,MatrixLayout::RowMajor).Store(outputBuffer,1152,64,MatrixLayout::RowMajor,4);
  if(lane==0)InterlockedAccumulate(float4(1,2,3,4),outputBuffer,2304,4);
  if(lane==0)OuterProduct<ComponentType::F32>(float4(1,2,3,4),float4(-1,2,0,4)).InterlockedAccumulate(outputBuffer,2368);
  using S=Matrix<ComponentType::I32,16,16,MatrixUse::Accumulator,MatrixScope::Wave>;
  using U=Matrix<ComponentType::U32,16,16,MatrixUse::Accumulator,MatrixScope::Wave>;
  for(uint i=lane;i<256;i+=32){sharedSigned[i]=0;sharedUnsigned[i]=0;}
  GroupMemoryBarrierWithGroupSync();
  auto s=S::Splat(-3);auto u=U::Splat(0xfffffffdU);
  s.InterlockedAccumulate(sharedSigned,0,16,MatrixLayout::RowMajor);
  u.InterlockedAccumulate(sharedUnsigned,0,16,MatrixLayout::RowMajor);
  s.InterlockedAccumulate(outputBuffer,2560,64,MatrixLayout::RowMajor,4);
  u.InterlockedAccumulate(outputBuffer,3648,64,MatrixLayout::RowMajor,4);
  GroupMemoryBarrierWithGroupSync();
  S::Load(sharedSigned,0,16,MatrixLayout::RowMajor).Store(outputBuffer,4736,64,MatrixLayout::RowMajor,4);
  U::Load(sharedUnsigned,0,16,MatrixLayout::RowMajor).Store(outputBuffer,5824,64,MatrixLayout::RowMajor,4);
}

template<ComponentEnum Component,uint Bytes,uint Slot,MatrixScopeEnum Scope=MatrixScope::Wave>
void formatCase(Matrix<ComponentType::F32,16,16,MatrixUse::A,Scope> a) {
  using T=Matrix<Component,16,16,MatrixUse::A,Scope>;
  auto converted=a.template Cast<Component,MatrixUse::A>();
  converted.Store(outputBuffer,32+Slot*4096,16*Bytes,MatrixLayout::RowMajor,Bytes);
  DeviceMemoryBarrierWithGroupSync();
  auto restored=T::Load(outputBuffer,32+Slot*4096,16*Bytes,MatrixLayout::RowMajor,Bytes);
  auto back=restored.template Cast<ComponentType::F32,MatrixUse::A>();
  back.Store(outputBuffer,1056+Slot*4096,64,MatrixLayout::RowMajor,4);
  T::Splat(1.0f).Store(outputBuffer,2080+Slot*4096,16*Bytes,MatrixLayout::RowMajor,Bytes);
}
[WaveSize(32)] [numthreads(32,1,1)]
void cs_matrix_formats(uint lane : SV_GroupIndex) {
  using A=Matrix<ComponentType::F32,16,16,MatrixUse::A,MatrixScope::Wave>;
  auto a=A::Load(inputBuffer,32,80,MatrixLayout::RowMajor,4);
  formatCase<ComponentType::I16,2,0>(a);
  formatCase<ComponentType::U16,2,1>(a);
  formatCase<ComponentType::I32,4,2>(a);
  formatCase<ComponentType::U32,4,3>(a);
  formatCase<ComponentType::F16,2,4>(a);
  formatCase<ComponentType::F32,4,5>(a);
  formatCase<ComponentType::I8,1,6>(a);
  formatCase<ComponentType::U8,1,7>(a);
  formatCase<ComponentType::F8_E4M3FN,1,8>(a);
  formatCase<ComponentType::F8_E5M2,1,9>(a);
}

[WaveSize(32)] [numthreads(64,1,1)]
void cs_matrix_groupformats(uint lane : SV_GroupIndex) {
  using A=Matrix<ComponentType::F32,16,16,MatrixUse::A,MatrixScope::ThreadGroup>;
  auto a=A::Load(inputBuffer,32,80,MatrixLayout::RowMajor,4);
  formatCase<ComponentType::I16,2,0,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::U16,2,1,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::I32,4,2,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::U32,4,3,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::F16,2,4,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::F32,4,5,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::I8,1,6,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::U8,1,7,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::F8_E4M3FN,1,8,MatrixScope::ThreadGroup>(a);
  formatCase<ComponentType::F8_E5M2,1,9,MatrixScope::ThreadGroup>(a);
}
