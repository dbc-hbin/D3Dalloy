#include "owned_native_states.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <atomic>

namespace {
struct Native {unsigned refs;obj_handle_t device;};
std::mutex observedMutex;
std::unordered_map<obj_handle_t,Native> objects;
obj_handle_t nextHandle=1;
unsigned samplerCreates=0,depthCreates=0;
std::atomic<bool> failNext{false};
obj_handle_t create(obj_handle_t device,bool sampler) {
  std::lock_guard lock(observedMutex);if(failNext.exchange(false))return 0;
  sampler?++samplerCreates:++depthCreates;
  auto handle=nextHandle++;objects.emplace(handle,Native{1,device});return handle;
}
}
extern "C" obj_handle_t MTLDevice_newSamplerState(obj_handle_t d,WMTSamplerInfo* s) {
  auto h=create(d,true);s->gpu_resource_id=h+0x10000;return h;
}
extern "C" obj_handle_t MTLDevice_newSamplerStateWithReduction(obj_handle_t d,WMTSamplerInfo* s,uint32_t reduction) {
  assert(reduction==1 || reduction==2);return MTLDevice_newSamplerState(d,s);
}
extern "C" obj_handle_t MTLDevice_newDepthStencilState(obj_handle_t d,const WMTDepthStencilInfo*) {return create(d,false);}
extern "C" void NSObject_retain(obj_handle_t h) {if(!h)return;std::lock_guard lock(observedMutex);assert(objects.contains(h));++objects.at(h).refs;}
extern "C" void NSObject_release(obj_handle_t h) {if(!h)return;std::lock_guard lock(observedMutex);assert(objects.contains(h));if(!--objects.at(h).refs)objects.erase(h);}
int main() {
  using namespace d3dalloy;WMT::Device device{},other{};device.handle=10000;other.handle=20000;
  WMTIndirectCommandBufferInfo indirect{};indirect.type=WMTIndirectCommandTypeConcurrentDispatch;
  indirect.inherit_pso=1;indirect.max_kernel_buffer_binding=31;
  const auto semantic=NativeIndirectCommandKey(indirect);
  indirect.padding_bits=0x1fffff;indirect.padding=0xff;indirect.gpu_resource_id=UINT64_MAX;
  assert(NativeIndirectCommandKey(indirect)==semantic);
  indirect.max_kernel_buffer_binding=30;assert(NativeIndirectCommandKey(indirect)!=semantic);
  {
    OwnedNativeStates cache,second;WMTSamplerInfo sampler{};sampler.min_filter=sampler.mag_filter=WMTSamplerMinMagFilterLinear;
    sampler.s_address_mode=sampler.t_address_mode=sampler.r_address_mode=WMTSamplerAddressModeRepeat;
    sampler.normalized_coords=true;sampler.lod_max_clamp=10;sampler.max_anisotroy=1;
    std::array<Rc<Sampler>,12> results;std::array<std::thread,12> threads;
    for(size_t i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{results[i]=cache.SamplerState(device,sampler,float(i),0);});
    for(auto& thread:threads)thread.join();assert(samplerCreates==2);
    for(size_t i=0;i<results.size();++i) {assert(results[i]->lod_bias==i);assert(results[i]->sampler_state==results[0]->sampler_state);
      assert(results[i]->sampler_state_cube==results[0]->sampler_state_cube);}
    auto reduction=cache.SamplerState(device,sampler,0,1);assert(samplerCreates==4 && reduction->sampler_state!=results[0]->sampler_state);
    auto isolated=second.SamplerState(other,sampler,0,0);assert(isolated->sampler_state!=results[0]->sampler_state);
    sampler.lod_min_clamp=1;failNext=true;assert(!cache.SamplerState(device,sampler,0,0));assert(cache.SamplerState(device,sampler,0,0));
    WMTDepthStencilInfo ds{};ds.depth_compare_function=WMTCompareFunctionLess;ds.depth_write_enabled=true;
    auto a=cache.DepthStencil(device,ds);ds.front_stencil.read_mask=123;ds.back_stencil.write_mask=255;
    auto b=cache.DepthStencil(device,ds);assert(a==b && depthCreates==1);
    ds.front_stencil.enabled=true;auto c=cache.DepthStencil(device,ds);assert(c!=a && depthCreates==2);
    for(unsigned i=0;i<300;++i){ds.front_stencil.read_mask=i;ds.front_stencil.write_mask=i*7;assert(cache.DepthStencil(device,ds));}
    // Eviction drops only cache ownership; the first handle remains alive.
    {std::lock_guard lock(observedMutex);assert(objects.contains(a.handle) && objects.contains(results[0]->sampler_state.handle));}
    auto d=cache.DepthStencil(device,ds);auto e=cache.DepthStencil(device,ds);assert(d==e);
    ds.back_stencil.enabled=true;failNext=true;assert(!cache.DepthStencil(device,ds));assert(cache.DepthStencil(device,ds));
    ds.depth_write_enabled=false;assert(second.DepthStencil(other,ds));
  }
  assert(objects.empty());
  std::puts("NATIVE_STATE_CACHE_PASS concurrent_samplers=12 native_normal_cube_pair=2 caller_bias_isolated=1 reduction_distinct=1 disabled_stencil_padding=ignored device_isolation=1 bounded_eviction_lifetime=1 failure_retry=1 all_references_balanced=1");
}
