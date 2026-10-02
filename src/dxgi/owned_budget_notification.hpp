#pragma once

#include "thread.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>

namespace dxmt {

// Observe the same native budget used by QueryVideoMemoryInfo, not usage or
// reservation changes. The source and its context outlive Stop().
class DxgiBudgetNotifications {
public:
  DxgiBudgetNotifications(uint64_t (*source)(void *), void *context)
      : source_(source), context_(context) {}

  ~DxgiBudgetNotifications() { Stop(); }

  HRESULT Register(HANDLE event, DWORD *cookie) {
    if (!cookie)
      return E_INVALIDARG;
    *cookie = 0;
    if (!event || event == INVALID_HANDLE_VALUE)
      return E_INVALIDARG;

    struct OwnedHandle {
      HANDLE handle = nullptr;
      ~OwnedHandle() {
        if (handle)
          CloseHandle(handle);
      }
    } duplicate;

    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                         &duplicate.handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return HRESULT_FROM_WIN32(GetLastError());

    HRESULT validation = ValidateEvent(duplicate.handle);
    if (FAILED(validation))
      return validation;

    std::unique_lock<dxmt::mutex> lock(mutex_);
    if (stopping_)
      return E_FAIL;
    if (registrations_.size() >= (std::numeric_limits<DWORD>::max)())
      return E_OUTOFMEMORY;

    DWORD candidate = next_cookie_;
    while (!candidate || registrations_.find(candidate) != registrations_.end())
      ++candidate;

    // Dispatch a pending change to old subscribers before the new subscriber
    // joins. With no subscribers this only establishes the baseline.
    ObserveBudget();
    try {
      registrations_.emplace(candidate, duplicate.handle);
      if (!worker_.joinable())
        worker_ = dxmt::thread([this] { Run(); });
    } catch (const std::bad_alloc &) {
      registrations_.erase(candidate);
      return E_OUTOFMEMORY;
    } catch (const MTLD3DError &) {
      registrations_.erase(candidate);
      return E_FAIL;
    }

    duplicate.handle = nullptr;
    next_cookie_ = candidate + 1;
    *cookie = candidate;
    wake_.notify_all();
    return S_OK;
  }

  void Unregister(DWORD cookie) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    auto registration = registrations_.find(cookie);
    if (registration == registrations_.end())
      return;
    CloseHandle(registration->second);
    registrations_.erase(registration);
    wake_.notify_all();
  }

  uint64_t QueryBudget() {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return ObserveBudget();
  }

  void Stop() {
    // Serialize lifecycle callers, but never hold the registry lock across
    // join: the worker needs it to leave its interruptible wait.
    std::lock_guard<dxmt::mutex> lifecycle_lock(stop_mutex_);
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      stopping_ = true;
      wake_.notify_all();
    }
    if (worker_.joinable())
      worker_.join();
    std::lock_guard<dxmt::mutex> lock(mutex_);
    for (const auto &[cookie, event] : registrations_)
      CloseHandle(event);
    registrations_.clear();
  }

private:
  // NtQueryObject's documented public layouts, scoped here so including this
  // helper does not redeclare the global NT types in util_d3dkmt.h.
  // https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntqueryobject
  struct ObjectBasicInformation {
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG Reserved[10];
  };
  struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
  };
  struct ObjectTypeInformation {
    UnicodeString TypeName;
    ULONG Reserved[22];
  };
  enum class ObjectInformationClass : ULONG { Basic = 0, Type = 2 };
  static_assert(sizeof(ObjectBasicInformation) == 56);
  static_assert(offsetof(ObjectBasicInformation, GrantedAccess) == 4);
  static_assert(offsetof(UnicodeString, Buffer) == (sizeof(void *) == 8 ? 8 : 4));
  static_assert(sizeof(UnicodeString) == (sizeof(void *) == 8 ? 16 : 8));
  static_assert(sizeof(ObjectInformationClass) == 4);

  static HRESULT ValidateEvent(HANDLE event) {
    using QueryObjectFn = LONG (NTAPI *)(HANDLE, ObjectInformationClass,
                                             void *, ULONG, ULONG *);
    static const auto query_object = reinterpret_cast<QueryObjectFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtQueryObject")));
    if (!query_object)
      return E_FAIL;

    alignas(8) ObjectBasicInformation basic = {};
    ULONG length = 0;
    if (query_object(event, ObjectInformationClass::Basic, &basic, sizeof(basic), &length) < 0 ||
        length != sizeof(basic))
      return E_INVALIDARG;
    if (!(basic.GrantedAccess & EVENT_MODIFY_STATE))
      return E_INVALIDARG;

    // Object type information starts with UnicodeString. Event type metadata
    // fits here; a failed or oversized query is rejected, never assumed valid.
    alignas(ObjectTypeInformation) unsigned char storage[1024] = {};
    length = 0;
    if (query_object(event, ObjectInformationClass::Type, storage, sizeof(storage), &length) < 0 ||
        length < sizeof(ObjectTypeInformation) || length > sizeof(storage))
      return E_INVALIDARG;
    const auto &type_name = reinterpret_cast<const ObjectTypeInformation *>(storage)->TypeName;
    constexpr WCHAR event_name[] = L"Event";
    constexpr size_t event_name_length = sizeof(event_name) - sizeof(WCHAR);
    const uintptr_t start = reinterpret_cast<uintptr_t>(storage);
    const uintptr_t address = reinterpret_cast<uintptr_t>(type_name.Buffer);
    if (type_name.Length != event_name_length || address < start ||
        address > start + length - event_name_length ||
        std::memcmp(type_name.Buffer, event_name, event_name_length))
      return E_INVALIDARG;
    return S_OK;
  }

  // Caller holds mutex_ through the native observation, comparison and every
  // SetEvent, so queries cannot steal changes or reorder worker observations.
  uint64_t ObserveBudget() {
    const uint64_t budget = source_(context_);
    if (have_budget_ && budget != last_budget_) {
      for (const auto &[cookie, event] : registrations_)
        SetEvent(event);
    }
    last_budget_ = budget;
    have_budget_ = true;
    return budget;
  }

  void Run() {
    std::unique_lock<dxmt::mutex> lock(mutex_);
    while (!stopping_) {
      wake_.wait(lock, [this] { return stopping_ || !registrations_.empty(); });
      if (stopping_)
        break;

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (!stopping_ && !registrations_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          ObserveBudget();
          break;
        }
        wake_.wait_for(lock, deadline - now);
      }
    }
  }

  uint64_t (*source_)(void *);
  void *context_;
  dxmt::mutex mutex_;
  dxmt::mutex stop_mutex_;
  dxmt::condition_variable wake_;
  std::map<DWORD, HANDLE> registrations_;
  dxmt::thread worker_;
  uint64_t last_budget_ = 0;
  DWORD next_cookie_ = 1;
  bool have_budget_ = false;
  bool stopping_ = false;
};

} // namespace dxmt
