// SPDX-License-Identifier: LGPL-2.1-or-later
// Build as ZenlessZoneZero.exe with -Iexternal/nvapi; link -ldxgi -ld3d12. Arguments: vendor device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#define __NVAPI_EMPTY_SAL
#include "nvapi.h"
#undef __NVAPI_EMPTY_SAL

using Microsoft::WRL::ComPtr;

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const unsigned vendor = std::strtoul(argv[1], nullptr, 16);
  const unsigned device_id = std::strtoul(argv[2], nullptr, 16);
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter))) return 1;
  ComPtr<IDXGIAdapter4> latest;
  if (FAILED(adapter.As(&latest))) return 1;
  DXGI_ADAPTER_DESC desc{};
  DXGI_ADAPTER_DESC1 desc1{};
  DXGI_ADAPTER_DESC2 desc2{};
  DXGI_ADAPTER_DESC3 desc3{};
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (!GlobalMemoryStatusEx(&memory) || FAILED(latest->GetDesc(&desc)) || FAILED(latest->GetDesc1(&desc1)) ||
      FAILED(latest->GetDesc2(&desc2)) || FAILED(latest->GetDesc3(&desc3))) return 1;
  std::printf("GPU_PROFILE vendor=%04x device=%04x name=%ls vram=%llu shared=%llu ram=%llu\n",
              desc3.VendorId, desc3.DeviceId, desc3.Description,
              static_cast<unsigned long long>(desc3.DedicatedVideoMemory),
              static_cast<unsigned long long>(desc3.SharedSystemMemory),
              static_cast<unsigned long long>(memory.ullTotalPhys));
  if (desc3.VendorId != vendor || desc3.DeviceId != device_id || !desc3.DedicatedVideoMemory ||
      desc.VendorId != vendor || desc.DeviceId != device_id ||
      desc1.VendorId != vendor || desc1.DeviceId != device_id ||
      desc2.VendorId != vendor || desc2.DeviceId != device_id ||
      desc.DedicatedVideoMemory != desc3.DedicatedVideoMemory ||
      desc1.DedicatedVideoMemory != desc3.DedicatedVideoMemory ||
      desc2.DedicatedVideoMemory != desc3.DedicatedVideoMemory ||
      std::wcscmp(desc.Description, desc3.Description) || std::wcscmp(desc1.Description, desc3.Description) ||
      std::wcscmp(desc2.Description, desc3.Description)) return 1;
  ComPtr<ID3D12Device> device;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  if (FAILED(D3D12CreateDevice(latest.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device))) ||
      FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
      options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) return 1;
  if (vendor == 0x10de) {
    HMODULE nvapi = LoadLibraryW(sizeof(void *) == 8 ? L"nvapi64.dll" : L"nvapi.dll");
    using Query = void *(__cdecl *)(NvU32);
    auto query = reinterpret_cast<Query>(nvapi ? GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr);
    if (!query) return 1;
    auto init = reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
    auto enumerate = reinterpret_cast<decltype(&NvAPI_EnumPhysicalGPUs)>(query(0xe5ac921f));
    auto full_name = reinterpret_cast<decltype(&NvAPI_GPU_GetFullName)>(query(0xceee8e9f));
    auto arch = reinterpret_cast<decltype(&NvAPI_GPU_GetArchInfo)>(query(0xd8265d24));
    auto driver = reinterpret_cast<decltype(&NvAPI_SYS_GetDriverAndBranchVersion)>(query(0x2926aaad));
    auto display_driver = reinterpret_cast<decltype(&NvAPI_GetDisplayDriverVersion)>(query(0xf951a4d1));
    auto version = reinterpret_cast<decltype(&NvAPI_GetInterfaceVersionString)>(query(0x01053fa5));
    if (!init || !enumerate || !full_name || !arch || !driver || !display_driver || !version ||
        init() != NVAPI_OK) return 1;
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS]{};
    NvU32 count = 0;
    if (enumerate(handles, &count) != NVAPI_OK || count == 0) return 1;
    NvAPI_ShortString name;
    std::memset(name, 0xcc, sizeof(name));
    if (full_name(handles[0], name) != NVAPI_OK || !std::memchr(name, 0, sizeof(name))) return 1;
    wchar_t wide[NVAPI_SHORT_STRING_MAX]{};
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, NVAPI_SHORT_STRING_MAX) ||
        std::wcscmp(wide, desc3.Description)) return 1;
    NV_GPU_ARCH_INFO info{};
    info.version = NV_GPU_ARCH_INFO_VER;
    if (arch(handles[0], &info) != NVAPI_OK) return 1;
    if (device_id == 0x2d05 && (info.architecture_id != NV_GPU_ARCHITECTURE_GB200 ||
                               info.implementation_id != 6)) return 1;
    NV_GPU_ARCH_INFO_V1 old_info{};
    old_info.version = NV_GPU_ARCH_INFO_VER_1;
    if (arch(handles[0], reinterpret_cast<NV_GPU_ARCH_INFO *>(&old_info)) != NVAPI_OK ||
        static_cast<NvU32>(old_info.architecture) != info.architecture_id ||
        static_cast<NvU32>(old_info.implementation) != info.implementation_id) return 1;
    NvU32 driver_version = 0;
    std::memset(name, 0xcc, sizeof(name));
    if (driver(&driver_version, name) != NVAPI_OK || !std::memchr(name, 0, sizeof(name))) return 1;
    std::memset(name, 0xcc, sizeof(name));
    if (version(name) != NVAPI_OK || !std::memchr(name, 0, sizeof(name))) return 1;
    NV_DISPLAY_DRIVER_VERSION display_info;
    std::memset(&display_info, 0xcc, sizeof(display_info));
    display_info.version = NV_DISPLAY_DRIVER_VERSION_VER;
    if (display_driver(nullptr, &display_info) != NVAPI_OK ||
        !std::memchr(display_info.szBuildBranchString, 0, sizeof(display_info.szBuildBranchString)) ||
        !std::memchr(display_info.szAdapterString, 0, sizeof(display_info.szAdapterString))) return 1;
    std::printf("GPU_PROFILE_NVAPI_PASS arch=%x implementation=%x\n", info.architecture_id, info.implementation_id);
  }
  std::printf("GPU_PROFILE_PASS dxr=%u\n", static_cast<unsigned>(options.RaytracingTier));
  return 0;
}
