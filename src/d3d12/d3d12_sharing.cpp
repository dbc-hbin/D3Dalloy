/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#define _CRT_RAND_S
#include <stdlib.h>

#include "d3d12_sharing.hpp"
#include "dxgi.h"
#include "winemetal.h"
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace dxmt::sharing {

namespace {

constexpr uint64_t kGranularity = 0x10000;
constexpr uint64_t kMaxPayload = 1ull << 40;

std::mutex registry_mutex;
std::unordered_map<std::string, std::weak_ptr<SharedObject>> registry;

bool
LinkName(const WCHAR *name, WCHAR out[64]) {
  size_t length = wcslen(name);
  char hash[40];
  if (!length || length > 256 || !WMTShareHashName(reinterpret_cast<const uint16_t *>(name), length, hash))
    return false;
  swprintf(out, 64, L"Local\\d3dalloy.d3d12.%hs", hash);
  return true;
}

void *
MapWhole(HANDLE section, uint64_t &size) {
  void *view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (!view)
    return nullptr;
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(view, &info, sizeof(info))) {
    UnmapViewOfFile(view);
    return nullptr;
  }
  size = info.RegionSize;
  return view;
}

HRESULT
OpenMain(HANDLE section, const uint8_t *expected_token, std::shared_ptr<SharedObject> &out,
         SharedObject *(*make)(), bool (*adopt)(SharedObject *, HANDLE, void *, const WCHAR *)) {
  auto leaf = SectionLeafName(section);
  if (leaf.rfind(L"d3dalloy.share.", 0) != 0 || leaf.size() >= 80)
    return E_INVALIDARG;
  uint64_t size = 0;
  auto *header = static_cast<Header *>(MapWhole(section, size));
  if (!header)
    return E_INVALIDARG;
  const auto hex = TokenHex(header->token);
  const bool valid = size >= kPayloadOffset && header->payload_size <= size - kPayloadOffset &&
                     ValidHeader(*header, leaf, expected_token);
  if (!valid) {
    UnmapViewOfFile(header);
    return E_INVALIDARG;
  }

  std::shared_ptr<SharedObject> existing;
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto it = registry.find(hex.data());
    if (it != registry.end())
      existing = it->second.lock();
  }
  if (existing) {
    UnmapViewOfFile(header);
    out = std::move(existing);
    return S_OK;
  }

  HANDLE own = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    UnmapViewOfFile(header);
    return E_INVALIDARG;
  }
  std::shared_ptr<SharedObject> object(make());
  adopt(object.get(), own, header, leaf.c_str());

  std::lock_guard<std::mutex> lock(registry_mutex);
  auto &slot = registry[hex.data()];
  if (auto raced = slot.lock()) {
    out = std::move(raced);
    return S_OK;
  }
  slot = object;
  out = std::move(object);
  return S_OK;
}

} // namespace

struct SharedObjectAccess {
  static SharedObject *make() { return new SharedObject(); }
  static bool adopt(SharedObject *object, HANDLE section, void *view, const WCHAR *leaf) {
    object->section_ = section;
    object->header_ = static_cast<Header *>(view);
    object->payload_ = static_cast<uint8_t *>(view) + kPayloadOffset;
    swprintf(object->section_name_, 80, L"Local\\%ls", leaf);
    return true;
  }
};

SharedObject::~SharedObject() {
  if (slot_ >= 0) {
    lockSlots();
    auto &slot = header_->slots[slot_];
    if (slot.pid == GetCurrentProcessId())
      memset(&slot, 0, sizeof(slot));
    unlockSlots();
  }
  if (rendezvous_)
    WMTShareRendezvousDestroy(rendezvous_);
  if (header_)
    UnmapViewOfFile(header_);
  if (section_)
    CloseHandle(section_);
}

void
SharedObject::lockSlots() {
  // The lock lives in the section, so it serializes every process. A holder
  // that dies mid-update is survived after about two seconds.
  for (uint32_t spin = 0;; ++spin) {
    if (!InterlockedCompareExchange(&header_->lock, 1, 0))
      return;
    if (spin > 20000) {
      InterlockedExchange(&header_->lock, 1);
      return;
    }
    Sleep(spin < 64 ? 0 : 1);
  }
}

void
SharedObject::unlockSlots() {
  InterlockedExchange(&header_->lock, 0);
}

HRESULT
SharedObject::publishPort(uint32_t mach_port) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (rendezvous_)
    return S_OK;
  char name[64];
  uint64_t rendezvous = WMTShareRendezvousCreate(mach_port, name);
  if (!rendezvous)
    return E_FAIL;
  const DWORD pid = GetCurrentProcessId();
  int32_t chosen = -1;
  lockSlots();
  for (uint32_t i = 0; i < kSlotCount && chosen < 0; ++i)
    if (!header_->slots[i].pid)
      chosen = i;
  for (uint32_t i = 0; i < kSlotCount && chosen < 0; ++i) {
    // Reclaim the slot of a process that exited without closing.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, header_->slots[i].pid);
    DWORD code = 0;
    if (!process || (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE))
      chosen = i;
    if (process)
      CloseHandle(process);
  }
  if (chosen >= 0) {
    auto &slot = header_->slots[chosen];
    slot.pid = pid;
    memcpy(slot.name, name, sizeof(slot.name));
  }
  unlockSlots();
  if (chosen < 0) {
    WMTShareRendezvousDestroy(rendezvous);
    return E_OUTOFMEMORY;
  }
  rendezvous_ = rendezvous;
  slot_ = chosen;
  return S_OK;
}

uint32_t
SharedObject::lookUpPort() {
  Slot slots[kSlotCount];
  lockSlots();
  memcpy(slots, header_->slots, sizeof(slots));
  unlockSlots();
  for (auto &slot : slots) {
    if (!slot.pid)
      continue;
    slot.name[sizeof(slot.name) - 1] = '\0';
    mach_port_t port = 0;
    if (WMTShareRendezvousLookUp(slot.name, &port))
      return port;
  }
  return 0;
}

HRESULT
SharedObject::createHandle(const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, LUID creator_luid,
                           HANDLE *handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!header_->adapter.luid.LowPart && !header_->adapter.luid.HighPart) {
    if (!creator_luid.LowPart && !creator_luid.HighPart)
      return E_INVALIDARG;
    AdapterMetadata adapter{};
    adapter.luid = creator_luid;
    if (!AdapterTag(header_->token, header_->kind, creator_luid, section_name_ + 6, {}, adapter.tag))
      return E_FAIL;
    header_->adapter = adapter;
    MemoryBarrier();
  } else if (!ValidAdapter(header_->token, header_->kind, header_->adapter, section_name_ + 6)) {
    return E_INVALIDARG;
  }
  (void)access; // Section rights; D3D12 callers pass GENERIC_ALL.
  if (!name) {
    const BOOL inherit = attributes && attributes->bInheritHandle;
    if (!DuplicateHandle(GetCurrentProcess(), section_, GetCurrentProcess(), handle, 0, inherit, DUPLICATE_SAME_ACCESS))
      return E_OUTOFMEMORY;
    return S_OK;
  }
  WCHAR link_name[64];
  if (!LinkName(name, link_name))
    return E_INVALIDARG;
  HANDLE link = CreateFileMappingW(INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES *>(attributes), PAGE_READWRITE,
                                   0, sizeof(LinkHeader), link_name);
  if (!link)
    return E_OUTOFMEMORY;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(link);
    return DXGI_ERROR_NAME_ALREADY_EXISTS;
  }
  auto *view = static_cast<LinkHeader *>(MapViewOfFile(link, FILE_MAP_WRITE, 0, 0, sizeof(LinkHeader)));
  if (!view) {
    CloseHandle(link);
    return E_OUTOFMEMORY;
  }
  memcpy(view->token, header_->token, sizeof(view->token));
  memcpy(view->section_name, section_name_, sizeof(view->section_name));
  view->version = kVersion;
  view->kind = header_->kind;
  view->adapter.luid = header_->adapter.luid;
  if (!AdapterTag(view->token, view->kind, view->adapter.luid, link_name + 6, section_name_, view->adapter.tag)) {
    UnmapViewOfFile(view);
    CloseHandle(link);
    return E_FAIL;
  }
  MemoryBarrier();
  view->magic = kLinkMagic;
  UnmapViewOfFile(view);
  *handle = link;
  return S_OK;
}

HRESULT
Create(Kind kind, uint64_t payload_size, std::shared_ptr<SharedObject> &out) {
  if (payload_size > kMaxPayload)
    return E_OUTOFMEMORY;
  uint8_t token[16];
  for (unsigned i = 0; i < 4; ++i) {
    unsigned int value = 0;
    if (rand_s(&value))
      return E_FAIL;
    memcpy(token + i * 4, &value, 4);
  }
  const auto hex = TokenHex(token);
  WCHAR name[80];
  swprintf(name, 80, L"Local\\d3dalloy.share.%u.%hs", unsigned(GetCurrentProcessId()), hex.data());
  const uint64_t payload = (payload_size + kGranularity - 1) & ~(kGranularity - 1);
  const uint64_t total = kPayloadOffset + payload;
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(total >> 32), DWORD(total),
                                      name);
  if (!section)
    return E_OUTOFMEMORY;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return E_FAIL;
  }
  uint64_t size = 0;
  auto *header = static_cast<Header *>(MapWhole(section, size));
  if (!header || size < total) {
    if (header)
      UnmapViewOfFile(header);
    CloseHandle(section);
    return E_OUTOFMEMORY;
  }
  header->version = kVersion;
  header->kind = kind;
  memcpy(header->token, token, sizeof(token));
  header->payload_offset = kPayloadOffset;
  header->payload_size = payload_size;
  MemoryBarrier();
  header->magic = kMagic;

  std::shared_ptr<SharedObject> object(SharedObjectAccess::make());
  SharedObjectAccess::adopt(object.get(), section, header, name + 6);
  std::lock_guard<std::mutex> lock(registry_mutex);
  for (auto it = registry.begin(); it != registry.end();)
    it = it->second.expired() ? registry.erase(it) : std::next(it);
  registry[hex.data()] = object;
  out = std::move(object);
  return S_OK;
}

HRESULT
Open(HANDLE handle, std::shared_ptr<SharedObject> &out) {
  if (!handle || handle == INVALID_HANDLE_VALUE)
    return E_INVALIDARG;
  auto leaf = SectionLeafName(handle);
  if (leaf.rfind(L"d3dalloy.share.", 0) == 0)
    return OpenMain(handle, nullptr, out, SharedObjectAccess::make, SharedObjectAccess::adopt);
  if (leaf.rfind(L"d3dalloy.d3d12.", 0) != 0)
    return E_INVALIDARG;

  uint64_t size = 0;
  auto *link = static_cast<LinkHeader *>(MapWhole(handle, size));
  if (!link)
    return E_INVALIDARG;
  LinkHeader copy{};
  if (size >= sizeof(LinkHeader))
    memcpy(&copy, link, sizeof(copy));
  UnmapViewOfFile(link);
  if (!ValidLink(copy, leaf))
    return E_INVALIDARG;
  HANDLE section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, copy.section_name);
  if (!section)
    return E_INVALIDARG;
  HRESULT hr = OpenMain(section, copy.token, out, SharedObjectAccess::make, SharedObjectAccess::adopt);
  CloseHandle(section);
  return hr;
}

HRESULT
OpenByName(const WCHAR *name, DWORD access, HANDLE *handle) {
  (void)access;
  if (!handle)
    return E_INVALIDARG;
  *handle = nullptr;
  WCHAR link_name[64];
  if (!name || !LinkName(name, link_name))
    return E_INVALIDARG;
  HANDLE link = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, link_name);
  if (!link)
    return E_INVALIDARG;
  *handle = link;
  return S_OK;
}

} // namespace dxmt::sharing
