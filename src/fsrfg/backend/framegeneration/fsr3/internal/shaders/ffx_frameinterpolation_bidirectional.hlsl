// MIT; extends the pinned current-depth model with retained real-frame geometry.
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS 1
#define FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_X 1
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_Y 2
#include "frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h"
#include "frameinterpolation/ffx_frameinterpolation_common.h"
Texture2D<float> r_previous_real_depth : register(t2);
Texture2D<uint> r_previous_vector_x : register(t3);
Texture2D<uint> r_previous_vector_y : register(t4);
Texture2D<uint> r_previous_seed : register(t5);
RWTexture2D<uint> rw_previous_vector_x : register(u3);
RWTexture2D<uint> rw_previous_vector_y : register(u4);
RWTexture2D<uint> rw_previous_seed : register(u5);
bool sameSurface(float a,float b) { return abs(a-b)<=0.02f*max(max(abs(a),abs(b)),1e-6f); }
[numthreads(8,8,1)] void CS(uint3 tid:SV_DispatchThreadID) {
  int2 p=int2(tid.xy); if(!IsOnScreen(p,RenderSize()))return;
#if DXMT_BI_PASS == 0
  float depth=LoadDilatedDepth(p);
  float2 motion=LoadDilatedMotionVector(p);
  float2 previousUv=(p+0.5f)/RenderSize()+motion;
  BilinearSamplingData bilinear=GetBilinearSamplingData(previousUv,RenderSize());
  uint2 packed=PackVectorFieldEntries(true,getPriorityFactorFromViewSpaceDepth(ConvertFromDeviceDepthToViewSpace(depth)),1,motion*.5f);
  for(int i=0;i<4;i++) {
    int2 previous=bilinear.iBasePos+bilinear.iOffsets[i];
    if(IsOnScreen(previous,RenderSize())&&bilinear.fWeights[i]>FFX_FRAMEINTERPOLATION_EPSILON&&sameSurface(depth,r_previous_real_depth.Load(int3(previous,0)))) {
      InterlockedMax(rw_previous_vector_x[previous],packed.x);
      InterlockedMax(rw_previous_vector_y[previous],packed.y);
    }
  }
#elif DXMT_BI_PASS == 1
  rw_previous_seed[p]=(r_previous_vector_x.Load(int3(p,0))&MOTION_VECTOR_FIELD_PRIMARY_VECTOR_INDICATION_BIT)?uint(p.y*RenderSize().x+p.x+1):0;
#elif DXMT_BI_PASS == 2
  uint best=r_previous_seed.Load(int3(p,0));
  float distance=3.402823466e38f;
  if(best) {int2 seed=int2((best-1)%RenderSize().x,(best-1)/RenderSize().x);distance=dot(float2(seed-p),float2(seed-p));}
  float depth=r_previous_real_depth.Load(int3(p,0));
  for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++) {
    int2 sample=p+int2(x,y)*NumInstances;
    if(!IsOnScreen(sample,RenderSize()))continue;
    uint candidate=r_previous_seed.Load(int3(sample,0));if(!candidate)continue;
    int2 seed=int2((candidate-1)%RenderSize().x,(candidate-1)/RenderSize().x);
    if(!sameSurface(depth,r_previous_real_depth.Load(int3(seed,0))))continue;
    float d=dot(float2(seed-p),float2(seed-p));
    if(d<distance) {distance=d;best=candidate;}
  }
  rw_previous_seed[p]=best;
#elif DXMT_BI_PASS == 3
  uint seedIndex=r_previous_seed.Load(int3(p,0));if(!seedIndex)return;
  int2 seed=int2((seedIndex-1)%RenderSize().x,(seedIndex-1)/RenderSize().x);
  VectorFieldEntry entry;UnpackVectorFieldEntries(uint2(r_previous_vector_x.Load(int3(seed,0)),r_previous_vector_y.Load(int3(seed,0))),entry);
  float depth=r_previous_real_depth.Load(int3(p,0));
  float2 targetUv=(p+0.5f)/RenderSize()-entry.fMotionVector*(2*InterpolationPhase());
  BilinearSamplingData bilinear=GetBilinearSamplingData(targetUv,RenderSize());
  uint2 packed=PackVectorFieldEntries(true,getPriorityFactorFromViewSpaceDepth(ConvertFromDeviceDepthToViewSpace(depth)),1,entry.fMotionVector);
  for(int i=0;i<4;i++) {
    int2 target=bilinear.iBasePos+bilinear.iOffsets[i];
    if(IsOnScreen(target,RenderSize())&&bilinear.fWeights[i]>FFX_FRAMEINTERPOLATION_EPSILON) {
      UpdateReconstructedDepthInterpolatedFrame(target,depth);
      UpdateGameMotionVectorField(target,packed);
    }
  }
#endif
}
