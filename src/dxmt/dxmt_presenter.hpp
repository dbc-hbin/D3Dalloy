#pragma once
#include "Metal.hpp"
#include "Metal4.hpp"
#include "Metal4Pipeline.hpp"
#include "dxmt_command.hpp"
#include "rc/util_rc.hpp"
#include "rc/util_rc_ptr.hpp"
#include "util_cpu_fence.hpp"
#include "winemetal.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace dxmt {

enum class DXMTPresentAlpha : uint32_t { Opaque = 0, Premultiplied = 1, Straight = 2 };

struct DXMTPresentGeometry {
  float source_width = 0, source_height = 0;
  float destination_width = 0, destination_height = 0;
  uint32_t rotation = 1;
  uint32_t scaling_none = 0;
  uint32_t right_to_left = 0;
  float background[4] = {0, 0, 0, 1};
  uint32_t restrict_output = 0;
  float output_clip[4] = {0, 0, 0, 0};
};

struct DXMTPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
  DXMTPresentGeometry geometry;
  DXMTPresentAlpha alpha;
};
static_assert(sizeof(DXMTPresentMetadata) == 80);

constexpr uint32_t DXMT_GAMMA_CP_COUNT = 1024;

struct DXMTGammaRamp {
  float red[DXMT_GAMMA_CP_COUNT];
  float green[DXMT_GAMMA_CP_COUNT];
  float blue[DXMT_GAMMA_CP_COUNT];
  uint64_t version;
};

class Presenter : public RcObject {
public:
  Presenter(
      WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count,
      WMT4::Compiler compiler = {}, bool composition = false
  );

  bool changeLayerProperties(
      WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count,
      DXMTPresentAlpha alpha = DXMTPresentAlpha::Opaque
  );

  bool changeLayerColorSpace(WMTColorSpace colorspace);

  void changeHDRMetadata(const WMTHDRMetadata *metadata);

  void changeGammaRamp(const DXMTGammaRamp *gamma_ramp);

  class PresentState {
    void retireEncoding() {
      if (presenter) {
        // A completion count, not the largest encoded ID, means all earlier
        // frames have encoded even when queues finish out of order.
        presenter->frame_presented_.signal(
            presenter->frames_encoded_.fetch_add(1, std::memory_order_acq_rel) + 1
        );
        presenter = nullptr;
      }
    }
  public:
    DXMTPresentMetadata metadata;
    uint64_t frame_id;
    Presenter *presenter;
    PresentState(DXMTPresentMetadata metadata, uint64_t frame_id, Presenter *presenter) :
        metadata(metadata),
        frame_id(frame_id),
        presenter(presenter) {}
    PresentState(const PresentState &copy) = delete;
    PresentState(PresentState &&move)  {
      metadata = move.metadata;
      frame_id = move.frame_id;
      presenter = std::exchange(move.presenter, nullptr);
    };
    PresentState &operator=(PresentState &&move) {
      if (this != &move) {
        retireEncoding();
        metadata = move.metadata;
        frame_id = move.frame_id;
        presenter = std::exchange(move.presenter, nullptr);
      }
      return *this;
    }
    ~PresentState() { retireEncoding(); }
  };

  // D3D12 retires presents explicitly; its layer transitions must wait for GPU
  // completion as well as CPU encoding. Other frontends retain their own drain.
  PresentState synchronizeLayerProperties(bool wait_for_gpu_completion = false);
  // DXGI_PRESENT_DO_NOT_WAIT must not sleep behind earlier GPU presents when
  // a layer/PSO transition needs to drain them. A null result changes no state.
  std::optional<PresentState> trySynchronizeLayerProperties();

  // Blocks until every requested frame has finished encoding.
  void waitPresentsEncoded() {
    frame_presented_.wait(frame_requested_);
  }

  void waitPresentsCompleted() {
    waitPresentsEncoded();
    frame_completed_.wait(frame_requested_);
  }

  void notifyPresentCompleted() {
    // Queue completion threads can retire out of order. A count, rather than
    // the largest frame id, means every accepted present has retired.
    frame_completed_.signal(frames_completed_.fetch_add(1, std::memory_order_acq_rel) + 1);
  }

  // Completion callbacks can arrive out of GPU order on different queues.
  // Never block a callback executor waiting for an earlier callback.
  // Callers must acquire/submit drawables in frame-ID order before reporting
  // ready: filling the native pool must not prevent a missing predecessor
  // from obtaining its drawable. D3D12 waits its predecessor GPU event first.
  template<typename Retirement>
  void retireNativePresent(uint64_t frame_id, Retirement &&retirement) {
    Rc<Presenter> keep_alive(this);
    {
      std::lock_guard lock(native_retire_mutex_);
      if (!native_retirement_aborted_) {
        assert(frame_id >= next_native_retirement_);
        if (native_retirement_draining_ || frame_id != next_native_retirement_) {
          const bool inserted = native_retirements_.emplace(
              frame_id, std::forward<Retirement>(retirement)).second;
          assert(inserted);
          return;
        }
        native_retirement_draining_ = true;
      }
    }
    std::forward<Retirement>(retirement)();
    drainNativeRetirements();
  }
  void abortNativeRetirements();

  // GPU timeline ordering presents in request order when they are encoded on
  // different queues: frame N waits for N-1 and signals N.
  WMT::SharedEvent presentOrderEvent() {
    return present_order_event_;
  }

  // A D3D12 queue can discard an accepted present after device removal while another queue
  // already waits for its frame ID. Release all outstanding GPU order waits terminally.
  void cancelPresentOrder() {
    present_order_event_.signalValue(UINT64_MAX);
    frame_completed_.signal(UINT64_MAX);
  }

  // Credits restart returns immediately: only frames still owned by a queue worker. Frames
  // already encoded return theirs on GPU completion, discarded or not.
  uint32_t restartableQueuedCredits() const {
    std::lock_guard lock(restart_mutex_);
    uint32_t result = 0;
    for (const auto &entry : queued_presents_)
      result += entry.credit->load(std::memory_order_relaxed);
    return result;
  }

  // Discards every present not yet handed to Core Animation. Queue-owned frames are skipped by
  // their worker; encoded frames observe the advanced token in their completion handler and
  // release their drawable unpresented. Returns the queue-owned credits taken over.
  uint32_t restartPresentQueue() {
    std::lock_guard lock(restart_mutex_);
    ++restart_epoch_;
    restart_token_.signalValue(restart_epoch_);
    uint32_t canceled = 0;
    for (const auto &entry : queued_presents_)
      canceled += entry.credit->exchange(false, std::memory_order_acq_rel);
    return canceled;
  }

  // Signaled value always equals the current restart epoch.
  WMT::SharedEvent restartToken() {
    return restart_token_;
  }

  uint64_t presentRestartEpoch() const {
    std::lock_guard lock(restart_mutex_);
    return restart_epoch_;
  }

  void prepareQueuedPresent() {
    std::lock_guard lock(restart_mutex_);
    queued_presents_.reserve(queued_presents_.size() + 1);
  }

  void registerQueuedPresent(uint64_t frame_id, const std::shared_ptr<std::atomic<bool>> &credit) {
    std::lock_guard lock(restart_mutex_);
    queued_presents_.push_back({frame_id, credit});
  }

  void unregisterQueuedPresent(uint64_t frame_id) {
    std::lock_guard lock(restart_mutex_);
    for (auto it = queued_presents_.begin(); it != queued_presents_.end(); ++it)
      if (it->frame_id == frame_id) {
        queued_presents_.erase(it);
        return;
      }
  }

  bool beginNativePresent(uint64_t epoch, uint64_t frame_id) {
    std::lock_guard lock(restart_mutex_);
    for (auto it = queued_presents_.begin(); it != queued_presents_.end(); ++it)
      if (it->frame_id == frame_id) {
        queued_presents_.erase(it);
        break;
      }
    return epoch == restart_epoch_;
  }

  void skipQueuedPresent(uint64_t frame_id) {
    present_order_event_.signalValue(frame_id);
    notifyPresentCompleted();
  }

  WMT::MetalDrawable encodeCommands(
      WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::RenderCommandEncoder)> &&update_fences
  );
  WMT::MetalDrawable encodeCommandsMetal4(
      WMT::MTL4CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::MTL4RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::MTL4RenderCommandEncoder)> &&update_fences
  );

private:
  void drainNativeRetirements();
  std::optional<PresentState> synchronizeLayerPropertiesImpl(bool wait_for_gpu_completion, bool do_not_wait);
  void buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable,
                                bool composition_residual, bool linear_composition_transport);

  WMT::Device device_;
  bool composition_ = false;
  WMT::Reference<WMT4::Compiler> compiler_;
  WMT::MetalLayer layer_;
  WMT::Reference<WMT::Library> lib_;
  WMTLayerProps layer_props_;
  uint32_t sample_count_;
  WMTColorSpace colorspace_ = WMTColorSpaceSRGB;
  DXMTPresentAlpha alpha_ = DXMTPresentAlpha::Opaque;
  WMTHDRMetadata hdr_metadata_;
  bool has_hdr_metadata_ = false;
  WMTPixelFormat source_format_ = WMTPixelFormatInvalid;
  uint64_t display_setting_version_ = 0;
  WMTColorSpace display_colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata display_hdr_metadata_;
  WMTEDRValue display_edr_value_{0.0, 1.0};
  uint64_t gamma_version_ = 0;
  std::array<float, DXMT_GAMMA_CP_COUNT * 4> gamma_lut_rgba_;
  WMT::Reference<WMT::Texture> gamma_lut_texture_;
  WMT::Reference<WMT::RenderPipelineState> present_blit_;
  WMT::Reference<WMT::RenderPipelineState> present_scale_;
  std::atomic_flag pso_valid = 0;
  uint64_t frame_requested_ = 0;
  std::atomic<uint64_t> frames_encoded_ = 0;
  CpuFence frame_presented_ = 0;
  std::atomic<uint64_t> frames_completed_ = 0;
  CpuFence frame_completed_ = 0;
  WMT::Reference<WMT::SharedEvent> present_order_event_;
  std::mutex native_retire_mutex_;
  uint64_t next_native_retirement_ = 1;
  bool native_retirement_draining_ = false;
  bool native_retirement_aborted_ = false;
  std::map<uint64_t, std::function<void()>> native_retirements_;
  mutable std::mutex restart_mutex_;
  uint64_t restart_epoch_ = 0;
  WMT::Reference<WMT::SharedEvent> restart_token_;
  struct QueuedPresent {
    uint64_t frame_id;
    std::shared_ptr<std::atomic<bool>> credit;
  };
  std::vector<QueuedPresent> queued_presents_;
};
} // namespace dxmt