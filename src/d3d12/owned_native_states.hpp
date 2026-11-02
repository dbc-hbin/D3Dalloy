#pragma once
#include "d3dalloy_sampler.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <mutex>

namespace d3dalloy {
inline std::array<uint32_t,19> NativeIndirectCommandKey(const WMTIndirectCommandBufferInfo& i) {
  return {uint32_t(i.type),i.inherit_buffers,i.inherit_pso,i.inherit_cull_mode,i.inherit_depth_bias,
      i.inherit_depth_clip_mode,i.inherit_depth_stencil_state,i.inherit_front_facing,i.inherit_fill_mode,
      i.support_ray_tracing,i.support_dynamic_attribute_stride,i.support_color_attachment_mapping,
      i.max_vertex_buffer_binding,i.max_fragment_buffer_binding,i.max_kernel_buffer_binding,
      i.max_mesh_buffer_binding,i.max_object_buffer_binding,
      i.max_object_threadgroup_memory_binding,i.max_kernel_threadgroup_memory_binding};
}
// D3D12 device-owned immutable native state. Keys contain semantic fields, not
// padding/output IDs. Shader-side bias/border metadata stays with the caller.
inline std::array<uint32_t,15> NativeSamplerKey(const WMTSamplerInfo& s,uint32_t reduction) {
  return {uint32_t(s.min_filter),uint32_t(s.mag_filter),uint32_t(s.mip_filter),
      uint32_t(s.r_address_mode),uint32_t(s.s_address_mode),uint32_t(s.t_address_mode),
      uint32_t(s.border_color),uint32_t(s.compare_function),std::bit_cast<uint32_t>(s.lod_min_clamp),
      std::bit_cast<uint32_t>(s.lod_max_clamp),s.max_anisotroy,uint32_t(s.normalized_coords),
      uint32_t(s.lod_average),uint32_t(s.support_argument_buffers),reduction};
}
inline std::array<uint32_t,16> NativeDepthStencilKey(const WMTDepthStencilInfo& s) {
  std::array<uint32_t,16> key{};key[0]=s.depth_compare_function;key[1]=s.depth_write_enabled;
  size_t offset=2;
  for(const auto* stencil:{&s.front_stencil,&s.back_stencil}) {
    key[offset]=stencil->enabled;
    if(stencil->enabled) {
      key[offset+1]=stencil->depth_stencil_pass_op;key[offset+2]=stencil->stencil_fail_op;
      key[offset+3]=stencil->depth_fail_op;key[offset+4]=stencil->stencil_compare_function;
      key[offset+5]=stencil->write_mask;key[offset+6]=stencil->read_mask;
    }
    offset+=7;
  }
  return key;
}
class OwnedNativeStates {
  template<class Value> struct Entry {Value value;uint64_t last_use;};
  std::mutex mutex_;
  std::map<std::array<uint32_t,15>,Entry<Rc<Sampler>>> samplers_;
  std::map<std::array<uint32_t,16>,Entry<WMT::Reference<WMT::DepthStencilState>>> depth_;
  uint64_t epoch_=0;
  template<class Map,class Key,class Value> void remember(Map& map,const Key& key,const Value& value) {
    // Optional admission never invalidates a successfully created object.
    try {
      if(map.size()==256) {
        auto old=std::min_element(map.begin(),map.end(),[](const auto& a,const auto& b){return a.second.last_use<b.second.last_use;});
        map.erase(old);
      }
      map.emplace(key,typename Map::mapped_type{value,++epoch_});
    } catch(const std::bad_alloc&) {}
  }
public:
  Rc<Sampler> SamplerState(WMT::Device device,const WMTSamplerInfo& info,float bias,uint32_t reduction) {
    Rc<Sampler> native;
    {
      std::lock_guard lock(mutex_);const auto key=NativeSamplerKey(info,reduction);
      auto found=samplers_.find(key);
      if(found!=samplers_.end()) {found->second.last_use=++epoch_;native=found->second.value;}
      else {native=Sampler::createSampler(device,info,0,reduction);if(native)remember(samplers_,key,native);}
    }
    if(!native)return nullptr;
    Rc<Sampler> caller=new Sampler();
    caller->lod_bias=bias;caller->sampler_state=native->sampler_state;caller->sampler_state_cube=native->sampler_state_cube;
    caller->sampler_state_handle=native->sampler_state_handle;caller->sampler_state_cube_handle=native->sampler_state_cube_handle;
    return caller;
  }
  WMT::Reference<WMT::DepthStencilState> DepthStencil(WMT::Device device,const WMTDepthStencilInfo& info) {
    std::lock_guard lock(mutex_);const auto key=NativeDepthStencilKey(info);
    auto found=depth_.find(key);
    if(found!=depth_.end()){found->second.last_use=++epoch_;return found->second.value;}
    auto state=device.newDepthStencilState(info);if(state)remember(depth_,key,state);return state;
  }
};
} // namespace d3dalloy
