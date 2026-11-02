// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12_antilag_interfaces.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include <algorithm>
#include <cstddef>
#include <new>

// AMD Anti-Lag 2 SDK driver ABI. The SDK calls UpdateAntiLagState(nullptr)
// immediately before input; non-null packets configure it or identify FG work.
DEFINE_COM_INTERFACE("44085fbe-e839-40c5-bf38-0ebc5ab4d0a6", IAmdExtAntiLagApi) : public IUnknown {
  virtual HRESULT UpdateAntiLagState(void *data) = 0;
};

namespace d3dalloy {
namespace {
struct AntiLagOptions {
  UINT size, version, mode;
  const char *control_string;
  UINT control_length, max_fps;
};
struct alignas(8) AntiLagFrame {
  UINT size, version, flags;
  UINT64 index, reserved[19];
};
static_assert(sizeof(AntiLagOptions) == (sizeof(void *) == 8 ? 32 : 24));
static_assert(sizeof(AntiLagFrame) == 176 && offsetof(AntiLagFrame, index) == 16);

class AntiLag final : public ComObject<IAmdExtAntiLagApi> {
  Com<IMTLD3D12AntiLag> device_;

public:
  explicit AntiLag(Com<IMTLD3D12AntiLag> device) : device_(std::move(device)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IAmdExtAntiLagApi)) return E_NOINTERFACE;
    *out = static_cast<IAmdExtAntiLagApi *>(this);
    AddRef();
    return S_OK;
  }

  HRESULT UpdateAntiLagState(void *data) override {
    if (!data) return device_->AntiLagSleep();
    const auto *header = static_cast<const UINT *>(data);
    if (header[0] < 2 * sizeof(UINT)) return E_INVALIDARG;
    switch (header[1]) {
    case 1: {
      if (header[0] != sizeof(AntiLagOptions)) return E_INVALIDARG;
      const auto &options = *static_cast<const AntiLagOptions *>(data);
      if (options.mode != 1 && options.mode != 2) return E_INVALIDARG;
      if (options.control_string || options.control_length) return E_NOTIMPL;
      // Round up so the optional cap cannot admit faster than requested.
      const UINT interval = options.max_fps ? UINT((1000000ull + options.max_fps - 1) / options.max_fps) : 0;
      return device_->SetAntiLagMode(options.mode == 1, interval);
    }
    case 2: {
      if (header[0] != sizeof(AntiLagFrame)) return E_INVALIDARG;
      const auto &frame = *static_cast<const AntiLagFrame *>(data);
      constexpr UINT frame_type = 1u << 2, interpolated = 1u << 3, end_frame = 1u << 5;
      if (frame.flags & ~(frame_type | interpolated | end_frame)) return E_NOTIMPL;
      if ((frame.flags & interpolated) && !(frame.flags & frame_type)) return E_INVALIDARG;
      if (frame.index || std::any_of(std::begin(frame.reserved), std::end(frame.reserved),
                                    [](UINT64 value) { return value != 0; })) return E_INVALIDARG;
      if (!(frame.flags & (frame_type | end_frame))) return E_INVALIDARG;
      if (frame.flags & end_frame) {
        const auto hr = device_->AntiLagEndFrame();
        if (FAILED(hr)) return hr;
      }
      return frame.flags & frame_type ? device_->AntiLagFrameType((frame.flags & interpolated) != 0) : S_OK;
    }
    default:
      return E_NOTIMPL;
    }
  }
};
} // namespace

extern "C" HRESULT __cdecl
AmdExtD3DCreateInterface(IUnknown *outer, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!outer) return E_INVALIDARG;
  if (iid != __uuidof(IAmdExtAntiLagApi)) return E_NOINTERFACE;
  Com<IMTLD3D12AntiLag> device;
  const auto hr = outer->QueryInterface(IID_PPV_ARGS(&device));
  if (FAILED(hr)) return hr;
  auto *object = new (std::nothrow) AntiLag(std::move(device));
  if (!object) return E_OUTOFMEMORY;
  object->AddRef();
  *out = static_cast<IAmdExtAntiLagApi *>(object);
  return S_OK;
}

extern "C" const char *WINAPI OwnedAntiLagBackendId() {
  return "d3dalloy-owned-antilag2";
}
} // namespace d3dalloy
