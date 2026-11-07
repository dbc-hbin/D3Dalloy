#include "owned-fg-contract.h"

namespace {
uint32_t fgCaptureSerial;
const bool fgCaptureEnabled = std::getenv("OWNED_FG_CONTRACT_PROBE") != nullptr;
enum class FgProbeCase { cameraMotion, cameraInputs, motionInputs };

void armFgCapture() {
  if (!fgCaptureEnabled) return;
  ++fgCaptureSerial;
  FILE* f=std::fopen("fg-contract.arm","wb");
  if (!f || std::fwrite(&fgCaptureSerial,sizeof(fgCaptureSerial),1,f)!=1 || std::fclose(f))
    fail("arm FG contract observer");
}

uint8_t fgByte(float linear, bool alpha) {
  if (!std::isfinite(linear)) fail("FG nonfinite native color");
  float value=std::clamp(linear,0.f,1.f);
  if (!alpha) value=value<=.0031308f ? value*12.92f : 1.055f*std::pow(value,1.f/2.4f)-.055f;
  return uint8_t(std::lround(value*255.f));
}

template<size_t N>
void checkFgObservation(FgProbeCase test, bool variant, UINT frame, UINT mode,
                        const std::vector<uint8_t>& scene, const std::vector<uint8_t>& image,
                        uint64_t (&effects)[N]) {
  if (!fgCaptureEnabled) return;
  char path[64]; std::snprintf(path,sizeof(path),"fg-contract-%u.bin",fgCaptureSerial);
  FILE* f=std::fopen(path,"rb");
  std::vector<uint8_t> bytes(fgProbeBytes);
  if (!f || std::fread(bytes.data(),1,bytes.size(),f)!=bytes.size() || std::fgetc(f)!=EOF || std::fclose(f))
    fail("FG missing same-evaluation capture");
  FgProbeFrame h; std::memcpy(&h,bytes.data(),sizeof(h));
  const bool guides=test==FgProbeCase::motionInputs, camera=test==FgProbeCase::cameraInputs;
  const bool renderGrid=guides && !variant && mode!=1 && mode!=2;
  const unsigned content=renderGrid ? fgProbeExtent/2 : fgProbeExtent;
  if (h.serial!=fgCaptureSerial || h.width!=fgProbeExtent || h.height!=fgProbeExtent || h.contentWidth!=content ||
      h.contentHeight!=content || h.reset!=UINT(!frame) || h.reversed!=1 || h.hasUI || h.hasDistortion ||
      h.motionScale[0]!=1 || h.motionScale[1]!=1 || !h.effect || (frame && h.effect!=effects[mode]))
    fail("FG native descriptor, history identity or reset");
  for (unsigned i=0;i<mode;++i)
    if (effects[i]==h.effect) fail("FG independent feature histories share an interpolator");
  effects[mode]=h.effect;
  const float jitter=guides ? (!variant ? float(frame%2)*.5f : 0.f) : camera ? 0.f : (frame%2 ? .25f : -.25f);
  if (h.jitter[0]!=jitter || h.jitter[1]!=(guides || camera ? jitter : -jitter)) fail("FG native jitter");
  auto close=[](float a,float b){ return a==b || (std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=1e-6f*std::max(1.f,std::abs(b))); };
  const float farPlane=camera && variant ? (mode==1?std::numeric_limits<float>::infinity():1e8f) : 1000.f;
  if (!close(h.nearPlane,.1f) || !close(h.farPlane,farPlane) || !close(h.aspect,camera?2.f:1.f) ||
      !close(h.fov,57.2957795f) || !close(h.delta,1.f/30)) fail("FG native camera parameters");
  float view[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float projection[16]; std::memcpy(projection,view,sizeof(view));
  if (camera) {
    const float cot=1.f/std::tan(.5f);
    projection[0]=cot/2; projection[5]=cot; projection[10]=variant?0.f:-.1f/999.9f;
    projection[11]=1; projection[14]=variant?.1f:.1f*1000.f/999.9f; projection[15]=0;
  } else if (!guides && variant) {
    projection[0]=projection[5]=.25f; projection[10]=-1.f/999.9f; projection[14]=1000.f/999.9f;
  }
  for (unsigned i=0;i<16;i++)
    if (!close(h.view[i],view[i]) || !close(h.projection[i],projection[i])) fail("FG native view/projection");
  const bool zeroMotion=mode==(guides?3u:2u);
  float mx=zeroMotion?0.f:float(content)/32, my=zeroMotion?0.f:float(content)/64;
  if (guides && !variant && frame) {
    const float cancel=(frame%2?.5f:-.5f)*content/(fgProbeExtent/2);
    mx+=cancel; my+=cancel;
  }
  for (unsigned y=0;y<fgProbeExtent;++y) {
    const uint8_t* row=bytes.data()+fgProbeHeader+y*fgProbeRow;
    for (unsigned x=0;x<fgProbeExtent;++x) {
      const bool active=x<content && y<content;
      const float expectedDepth=active ? (camera || (!guides && variant) ? .2f+.6f*float(x+y)/254.f:.75f):0.f;
      float depth; std::memcpy(&depth,row+fgProbePlane+x*4,4);
      _Float16 motion[2]; std::memcpy(motion,row+2*fgProbePlane+x*4,4);
      if (!close(depth,expectedDepth) || float(motion[0])!=(active?mx:0.f) || float(motion[1])!=(active?my:0.f))
        fail("FG native normalized depth, motion or inactive padding");
      for (unsigned ch=0;ch<4;ch++) {
        unsigned i=(y*fgProbeExtent+x)*4+ch;
        _Float16 color, generated; std::memcpy(&color,row+(x*4+ch)*2,2);
        std::memcpy(&generated,row+3*fgProbePlane+(x*4+ch)*2,2);
        if (std::abs(int(fgByte(float(color),ch==3))-scene[i])>1 ||
            std::abs(int(fgByte(float(generated),ch==3))-image[i])>1) fail("FG decoded scene or same-encode scatter");
      }
    }
  }
  std::remove(path);
}
}
