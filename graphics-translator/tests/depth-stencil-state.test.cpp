// CPU-only: python3 graphics-translator/tests/run-depth-stencil-cpu.py
#include "Metal.hpp"
#include "d3d12.h"
#include <array>
#include <cstdio>
#include <optional>
#include <utility>

#include PRODUCTION_MAPS

struct GraphicsPipelineExtras {
  bool depth_bounds = false;
  std::optional<std::pair<UINT8, UINT8>> back_stencil_read_write_mask;
};
struct Device {
  struct StateBuilder {
    WMT::Reference<WMT::DepthStencilState> DepthStencil(WMT::Device device,const WMTDepthStencilInfo& info) {
      return device.newDepthStencilState(info);
    }
  } owned_native_states; // This test isolates descriptor math; cache ownership has its own probe.
  WMT::DepthStencilState default_depth_stencil_state;
  WMT::Device GetMTLDevice() { WMT::Device metal; metal.handle = 100; return metal; }
};
struct PipelineBase {
  virtual WMT::DepthStencilState GetDepthStencilState(UINT, UINT) = 0;
};
struct Pipeline : PipelineBase {
  Device *device_;
  bool emulated_depth_bounds = false;
  WMT::Reference<WMT::DepthStencilState> dsso, dsso_depth_readonly, dsso_stencil_readonly,
      dsso_readonly, dsso_no_stencil, dsso_readonly_no_stencil, depth_bounds_reject_state;
#include PRODUCTION_METHODS
};

static std::array<WMTDepthStencilInfo, 8> captured;
static std::array<unsigned, 8> references;
static unsigned creates, retains, releases;

extern "C" obj_handle_t MTLDevice_newDepthStencilState(obj_handle_t device, const WMTDepthStencilInfo *info) {
  assert(device == 100 && creates + 1 < captured.size());
  const auto handle = ++creates;
  auto &copy = captured[handle];
  copy = {};
  copy.depth_compare_function = info->depth_compare_function;
  copy.depth_write_enabled = info->depth_write_enabled;
  // Native descriptors do not consume the other fields of disabled faces.
  copy.front_stencil.enabled = info->front_stencil.enabled;
  copy.back_stencil.enabled = info->back_stencil.enabled;
  if (info->front_stencil.enabled) copy.front_stencil = info->front_stencil;
  if (info->back_stencil.enabled) copy.back_stencil = info->back_stencil;
  assert(references[handle] == 0);
  references[handle] = 1;
  return handle;
}
extern "C" void NSObject_retain(obj_handle_t handle) {
  if (!handle) return;
  assert(handle <= creates && references[handle] > 0);
  ++references[handle];
  ++retains;
}
extern "C" void NSObject_release(obj_handle_t handle) {
  if (!handle) return;
  assert(handle <= creates && references[handle] > 0);
  --references[handle];
  ++releases;
}

static void checkFace(const WMTStencilInfo &actual, const D3D12_DEPTH_STENCILOP_DESC &face,
                      UINT8 read, UINT8 write, bool readonly) {
  assert(actual.enabled && actual.read_mask == read && actual.write_mask == write);
  assert(actual.stencil_compare_function == static_cast<WMTCompareFunction>(face.StencilFunc - 1));
  assert(actual.stencil_fail_op == (readonly ? WMTStencilOperationKeep :
                                  static_cast<WMTStencilOperation>(face.StencilFailOp - 1)));
  assert(actual.depth_fail_op == (readonly ? WMTStencilOperationKeep :
                                static_cast<WMTStencilOperation>(face.StencilDepthFailOp - 1)));
  assert(actual.depth_stencil_pass_op == (readonly ? WMTStencilOperationKeep :
                                        static_cast<WMTStencilOperation>(face.StencilPassOp - 1)));
}

int main() {
  Device device;
  device.default_depth_stencil_state.handle = 99;
  unsigned cases = 0;
  for (unsigned depth = 0; depth != 2; ++depth)
    for (unsigned write = 0; write != 2; ++write)
      for (unsigned stencil = 0; stencil != 2; ++stencil)
        for (unsigned bounds = 0; bounds != 3; ++bounds)
          for (unsigned masks = 0; masks != 2; ++masks)
            for (unsigned compare = 1; compare != 9; ++compare)
              for (unsigned operation = 1; operation != 9; ++operation) {
                creates = retains = releases = 0;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
                auto &ds = desc.DepthStencilState;
                ds.DepthEnable = depth;
                ds.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                ds.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(compare);
                ds.StencilEnable = stencil;
                ds.StencilReadMask = 0xa5;
                ds.StencilWriteMask = 0x5a;
                ds.FrontFace = {static_cast<D3D12_STENCIL_OP>(operation),
                                static_cast<D3D12_STENCIL_OP>(operation % 8 + 1),
                                static_cast<D3D12_STENCIL_OP>((operation + 1) % 8 + 1),
                                static_cast<D3D12_COMPARISON_FUNC>(compare)};
                ds.BackFace = {static_cast<D3D12_STENCIL_OP>((operation + 2) % 8 + 1),
                               static_cast<D3D12_STENCIL_OP>((operation + 3) % 8 + 1),
                               static_cast<D3D12_STENCIL_OP>((operation + 4) % 8 + 1),
                               static_cast<D3D12_COMPARISON_FUNC>(compare % 8 + 1)};
                GraphicsPipelineExtras extras;
                extras.depth_bounds = bounds != 0;
                if (masks) extras.back_stencil_read_write_mask = {{0x3c, 0xc3}};
                const unsigned variants = (depth && write ? 2 : 1) * (stencil ? 3 : 1);
                const unsigned reject = bounds == 1;
                {
                  Pipeline pipeline;
                  pipeline.device_ = &device;
                  pipeline.emulated_depth_bounds = bounds == 2;
                  pipeline.InitializeDSSO(&desc, extras);
                  assert(creates == variants + reject);
                  assert(retains == 6 - variants && releases == 0);
                  assert(bool(pipeline.depth_bounds_reject_state) == bool(reject));
                  if (reject) {
                    const auto &info = captured[pipeline.depth_bounds_reject_state.handle];
                    assert(info.depth_compare_function == WMTCompareFunctionNever && !info.depth_write_enabled);
                    assert(!info.front_stencil.enabled && !info.back_stencil.enabled);
                  }
                  if (!(depth && write)) {
                    assert(pipeline.dsso == pipeline.dsso_depth_readonly);
                    assert(pipeline.dsso_stencil_readonly == pipeline.dsso_readonly);
                    assert(pipeline.dsso_no_stencil == pipeline.dsso_readonly_no_stencil);
                  }
                  if (!stencil) {
                    assert(pipeline.dsso == pipeline.dsso_stencil_readonly && pipeline.dsso == pipeline.dsso_no_stencil);
                    assert(pipeline.dsso_depth_readonly == pipeline.dsso_readonly &&
                           pipeline.dsso_depth_readonly == pipeline.dsso_readonly_no_stencil);
                  }
                  for (UINT planar : {0u, 1u, 3u})
                    for (UINT readonly = 0; readonly != 4; ++readonly) {
                      const auto selected = pipeline.GetDepthStencilState(planar, readonly);
                      const auto expected = planar == 0 ? device.default_depth_stencil_state.handle :
                          planar == 1 ? (readonly & 1 ? pipeline.dsso_readonly_no_stencil.handle : pipeline.dsso_no_stencil.handle) :
                          readonly == 3 ? pipeline.dsso_readonly.handle : readonly == 2 ? pipeline.dsso_stencil_readonly.handle :
                          readonly == 1 ? pipeline.dsso_depth_readonly.handle : pipeline.dsso.handle;
                      assert(selected.handle == expected);
                      if (!planar) continue;
                      const auto &info = captured[selected.handle];
                      assert(info.depth_compare_function == (depth ? static_cast<WMTCompareFunction>(compare - 1) : WMTCompareFunctionAlways));
                      assert(info.depth_write_enabled == bool(depth && write && !(readonly & 1)));
                      const bool enabled = stencil && planar == 3;
                      assert(info.front_stencil.enabled == enabled && info.back_stencil.enabled == enabled);
                      if (enabled) {
                        checkFace(info.front_stencil, ds.FrontFace, ds.StencilReadMask, ds.StencilWriteMask, readonly & 2);
                        checkFace(info.back_stencil, ds.BackFace, masks ? 0x3c : ds.StencilReadMask,
                                  masks ? 0xc3 : ds.StencilWriteMask, readonly & 2);
                      }
                    }
                  assert(creates == variants + reject && retains == 6 - variants && releases == 0);
                }
                assert(releases == creates + retains);
                for (unsigned count : references) assert(count == 0);
                ++cases;
              }
  std::printf("DEPTH_STENCIL_CPU_PASS cases=%u selections=%u create_counts=1/2/3/6+reject ownership=balanced\n", cases, cases * 12);
}
