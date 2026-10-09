#include "dxmt_sampler.hpp"
#include "winemetal.h"

namespace dxmt {

void
Sampler::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Sampler::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<Sampler>
Sampler::createSampler(WMT::Device device, const WMTSamplerInfo &info, float lod_bias, uint32_t reduction) {
  Rc<Sampler> ret = new Sampler();
  WMTSamplerInfo normal_sampler_info = info;
  auto create = [&](WMTSamplerInfo &sampler_info) {
    return reduction ? device.newSamplerStateWithReduction(sampler_info, reduction)
                     : device.newSamplerState(sampler_info);
  };

  ret->sampler_state = create(normal_sampler_info);
  if (!ret->sampler_state) {
    return nullptr;
  }
  ret->sampler_state_handle = normal_sampler_info.gpu_resource_id;

  WMTSamplerInfo cube_sampler_info = info;
  // it's probably still wrong, but the best result we can get
  if (cube_sampler_info.min_filter == WMTSamplerMinMagFilterLinear &&
      cube_sampler_info.mag_filter == WMTSamplerMinMagFilterLinear) {
    cube_sampler_info.s_address_mode = WMTSamplerAddressModeClampToBorderColor;
    cube_sampler_info.t_address_mode = WMTSamplerAddressModeClampToBorderColor;
    cube_sampler_info.r_address_mode = WMTSamplerAddressModeClampToBorderColor;
  } else {
    cube_sampler_info.s_address_mode = WMTSamplerAddressModeClampToEdge;
    cube_sampler_info.t_address_mode = WMTSamplerAddressModeClampToEdge;
    cube_sampler_info.r_address_mode = WMTSamplerAddressModeClampToEdge;
  }
  ret->sampler_state_cube = create(cube_sampler_info);
  if (!ret->sampler_state_cube) {
    return nullptr;
  }
  ret->sampler_state_cube_handle = cube_sampler_info.gpu_resource_id;
  ret->lod_bias = lod_bias;

  return ret;
};

} // namespace dxmt
