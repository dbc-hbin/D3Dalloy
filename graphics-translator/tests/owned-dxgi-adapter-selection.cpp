#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void fail(const char *step, HRESULT actual = E_FAIL, HRESULT expected = S_OK) {
  std::fprintf(stderr, "OWNED_DXGI_ADAPTER_SELECTION_FAIL step=%s actual=%08lx expected=%08lx\n", step,
               static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
  throw std::runtime_error(step);
}
void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) fail(step, actual, expected);
}
void require(bool condition, const char *step) { if (!condition) fail(step); }
bool same(LUID a, LUID b) { return a.HighPart == b.HighPart && a.LowPart == b.LowPart; }
void passed(const char *step) {
  std::printf("OWNED_DXGI_ADAPTER_SELECTION_CASE_PASS %s\n", step);
  std::fflush(stdout);
}
}

int main() {
  try {
    ComPtr<IDXGIFactory6> factory;
    expect(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), S_OK, "factory");
    std::vector<LUID> luids;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      HRESULT hr = factory->EnumAdapters1(index, &adapter);
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      expect(hr, S_OK, "enumeration");
      DXGI_ADAPTER_DESC1 desc{};
      expect(adapter->GetDesc1(&desc), S_OK, "enumeration desc");
      luids.push_back(desc.AdapterLuid);
    }
    require(!luids.empty(), "real adapter required");

    for (LUID luid : luids) {
      ComPtr<IDXGIAdapter4> selected;
      expect(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&selected)), S_OK, "select real luid");
      DXGI_ADAPTER_DESC3 desc{};
      expect(selected->GetDesc3(&desc), S_OK, "selected desc");
      require(same(desc.AdapterLuid, luid), "selected exact luid");

      ComPtr<IUnknown> identity;
      ComPtr<IDXGIAdapter1> older;
      ComPtr<IUnknown> older_identity;
      expect(selected.As(&identity), S_OK, "selected identity");
      expect(selected.As(&older), S_OK, "older interface");
      expect(older.As(&older_identity), S_OK, "older identity");
      require(identity.Get() == older_identity.Get(), "canonical IUnknown identity");
      selected.Reset();
      identity.Reset();
      older_identity.Reset();
      DXGI_ADAPTER_DESC1 retained_desc{};
      expect(older->GetDesc1(&retained_desc), S_OK, "retained interface desc");
      require(same(retained_desc.AdapterLuid, luid), "QI reference survives original release");

      ComPtr<ID3D12Device> device;
      expect(D3D12CreateDevice(older.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), S_OK,
             "selected adapter device");
      require(same(device->GetAdapterLuid(), luid), "device uses selected adapter");

      ComPtr<IUnknown> direct_unknown;
      expect(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&direct_unknown)), S_OK, "select IUnknown");
      ComPtr<IDXGIAdapter1> from_unknown;
      expect(direct_unknown.As(&from_unknown), S_OK, "IUnknown to adapter");
      ComPtr<IUnknown> roundtrip;
      expect(from_unknown.As(&roundtrip), S_OK, "IUnknown roundtrip");
      require(roundtrip.Get() == direct_unknown.Get(), "returned IUnknown identity");
      direct_unknown.Reset();
      roundtrip.Reset();
      expect(from_unknown->GetDesc1(&retained_desc), S_OK, "IUnknown retained desc");
      require(same(retained_desc.AdapterLuid, luid), "IUnknown selected exact luid");
    }
    passed("actual-luids-identity-device-retention");

    void *output = reinterpret_cast<void *>(static_cast<ULONG_PTR>(1));
    expect(factory->EnumAdapterByLuid(luids.front(), __uuidof(IDXGIFactory), &output), E_NOINTERFACE,
           "unsupported matched IID");
    require(output == nullptr, "unsupported IID clears output");
    // NULL is forbidden by DXGI; INVALID_CALL follows this runtime's factory contract.
    expect(factory->EnumAdapterByLuid(luids.front(), __uuidof(IDXGIAdapter1), nullptr), DXGI_ERROR_INVALID_CALL,
           "null output");
    passed("unsupported-iid-null-output");

    // Derive absent LUIDs from the observed set, independently changing each half.
    for (bool change_high : {false, true}) {
      LUID missing = luids.front();
      bool exists;
      do {
        if (change_high) missing.HighPart = static_cast<LONG>(static_cast<ULONG>(missing.HighPart) + 1);
        else ++missing.LowPart;
        exists = false;
        for (LUID luid : luids) exists |= same(missing, luid);
      } while (exists);
      output = reinterpret_cast<void *>(static_cast<ULONG_PTR>(1));
      expect(factory->EnumAdapterByLuid(missing, __uuidof(IDXGIAdapter1), &output), DXGI_ERROR_NOT_FOUND,
             "missing luid");
      require(output == nullptr, "missing luid clears output");
      output = reinterpret_cast<void *>(static_cast<ULONG_PTR>(1));
      expect(factory->EnumAdapterByLuid(missing, __uuidof(IDXGIFactory), &output), DXGI_ERROR_NOT_FOUND,
             "missing luid precedes unsupported IID");
      require(output == nullptr, "missing unsupported IID clears output");
      expect(factory->EnumAdapterByLuid(missing, __uuidof(IUnknown), nullptr), DXGI_ERROR_INVALID_CALL,
             "null output precedes lookup");
    }
    passed("missing-luids-both-halves-error-precedence");

    for (UINT index = 0; index < luids.size(); ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      expect(factory->EnumAdapters1(index, &adapter), S_OK, "later enumeration");
      DXGI_ADAPTER_DESC1 desc{};
      expect(adapter->GetDesc1(&desc), S_OK, "later enumeration desc");
      require(same(desc.AdapterLuid, luids[index]), "later enumeration ordering unchanged");
      for (DXGI_GPU_PREFERENCE preference : {DXGI_GPU_PREFERENCE_UNSPECIFIED, DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE}) {
        ComPtr<IDXGIAdapter1> preferred;
        expect(factory->EnumAdapterByGpuPreference(index, preference, IID_PPV_ARGS(&preferred)), S_OK,
               "GPU preference enumeration");
        expect(preferred->GetDesc1(&desc), S_OK, "GPU preference desc");
        require(same(desc.AdapterLuid, luids[index]), "existing GPU preference policy unchanged");
      }
    }
    ComPtr<IDXGIAdapter1> beyond;
    expect(factory->EnumAdapters1(static_cast<UINT>(luids.size()), &beyond), DXGI_ERROR_NOT_FOUND,
           "later enumeration end");
    require(!beyond, "enumeration end clears output");
    passed("later-enumeration-gpu-preference");

    ComPtr<IDXGIAdapter1> survivor;
    expect(factory->EnumAdapterByLuid(luids.front(), IID_PPV_ARGS(&survivor)), S_OK, "lifetime selection");
    IUnknown *factory_identity = nullptr;
    expect(factory->QueryInterface(IID_PPV_ARGS(&factory_identity)), S_OK, "factory identity");
    factory_identity->Release();
    factory.Reset();
    ComPtr<IDXGIFactory6> parent;
    expect(survivor->GetParent(IID_PPV_ARGS(&parent)), S_OK, "factory retained by selected adapter");
    ComPtr<IUnknown> parent_identity;
    expect(parent.As(&parent_identity), S_OK, "retained factory identity");
    require(parent_identity.Get() == factory_identity, "same retained factory");
    survivor.Reset();
    expect(parent->EnumAdapterByLuid(luids.front(), IID_PPV_ARGS(&survivor)), S_OK, "retained factory lookup");
    passed("factory-and-adapter-lifetime");
    std::printf("OWNED_DXGI_ADAPTER_SELECTION_PASS adapters=%zu\n", luids.size());
    return 0;
  } catch (const std::exception &) { return 1; }
}
