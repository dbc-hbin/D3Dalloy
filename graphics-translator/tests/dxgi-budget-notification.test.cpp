// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone PE CPU probe; build with -std=c++20 -Isrc/util (x64 and i686).
#include <windows.h>
#include "../../src/dxgi/owned_budget_notification.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr DWORD kTimeout = 10000;
constexpr uint64_t kGiB = uint64_t{1} << 30;
constexpr uint64_t kHigh = 8 * kGiB;
constexpr uint64_t kLow = 4 * kGiB;
constexpr DWORD kEventQueryState = 0x0001;

void require(bool condition, const char *step) {
  if (!condition) {
    std::fprintf(stderr, "DXGI_BUDGET_NOTIFICATION_FAIL: %s (win32=%lu)\n", step,
                 static_cast<unsigned long>(GetLastError()));
    std::exit(1);
  }
}

struct Handle {
  HANDLE value;
  explicit Handle(HANDLE handle) : value(handle) {
    require(value && value != INVALID_HANDLE_VALUE, "create handle");
  }
  ~Handle() { CloseHandle(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};

struct Event : Handle {
  explicit Event(bool manual = true, bool signaled = false)
      : Handle(CreateEventW(nullptr, manual, signaled, nullptr)) {}
  void reset() { require(ResetEvent(value) != FALSE, "reset caller event"); }
  void signal() { require(SetEvent(value) != FALSE, "signal callback barrier"); }
  void wait(const char *step) { require(WaitForSingleObject(value, kTimeout) == WAIT_OBJECT_0, step); }
  void idle(const char *step) { require(WaitForSingleObject(value, 0) == WAIT_TIMEOUT, step); }
};

HANDLE duplicate(HANDLE event, DWORD access, DWORD options = 0) {
  HANDLE result = nullptr;
  require(DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &result,
                          access, FALSE, options) != FALSE, "duplicate caller handle");
  return result;
}

// The documented PUBLIC_OBJECT_BASIC_INFORMATION layout also works with the
// Wine SDK, whose header names the full structure OBJECT_BASIC_INFORMATION.
ULONG handleCount(HANDLE event) {
  struct BasicInfo {
    ULONG attributes;
    ACCESS_MASK access;
    ULONG handles;
    ULONG pointers;
    ULONG reserved[10];
  } info{};
  using QueryObject = LONG(WINAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
  const auto query = reinterpret_cast<QueryObject>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  require(query != nullptr, "resolve NtQueryObject for owned-handle lifetime");
  require(query(event, 0, &info, sizeof(info), nullptr) >= 0, "query event handle count");
  return info.handles;
}

struct Source {
  std::atomic<uint64_t> budget{kHigh};
  std::atomic<unsigned> completed{0};
  std::atomic<bool> armed{false};
  std::atomic<bool> armAfterRead{false};
  std::atomic<DWORD> target{0};
  std::atomic<DWORD> consumedThread{0};
  Event entered;
  Event release;

  void arm(DWORD thread = 0) {
    require(!armed.load(), "only one callback barrier at a time");
    entered.reset();
    release.reset();
    target.store(thread, std::memory_order_relaxed);
    consumedThread.store(0, std::memory_order_relaxed);
    armed.store(true, std::memory_order_release);
  }

  static uint64_t read(void *context) {
    auto &source = *static_cast<Source *>(context);
    // Observe the publication before its target: otherwise a worker can read
    // the old wildcard target, then steal a newly armed thread-specific gate.
    if (source.armed.load(std::memory_order_acquire)) {
      const DWORD target = source.target.load(std::memory_order_relaxed);
      const DWORD thread = GetCurrentThreadId();
      if ((!target || target == thread) && source.armed.exchange(false, std::memory_order_acq_rel)) {
        source.consumedThread.store(thread, std::memory_order_release);
        source.entered.signal();
        source.release.wait("release source callback");
      }
    }
    const uint64_t value = source.budget.load();
    source.completed.fetch_add(1);
    if (source.armAfterRead.exchange(false))
      source.arm();
    return value;
  }
};

DWORD registerEvent(d3dalloy::DxgiBudgetNotifications &observer, HANDLE event) {
  DWORD cookie = 0;
  require(observer.Register(event, &cookie) == S_OK, "register event");
  require(cookie != 0, "successful cookie must not equal failure cookie");
  return cookie;
}

void rejected(d3dalloy::DxgiBudgetNotifications &observer, HANDLE event, const char *step) {
  DWORD cookie = 0xabcdef01;
  require(FAILED(observer.Register(event, &cookie)), step);
  require(cookie == 0, "failed registration clears output cookie");
}

void testTransitionsAndIndependentCookies() {
  Source source;
  Event manual;
  Event automatic(false);
  d3dalloy::DxgiBudgetNotifications observer(Source::read, &source);
  const ULONG manualBaseline = handleCount(manual.value);
  const ULONG autoBaseline = handleCount(automatic.value);
  const DWORD first = registerEvent(observer, manual.value);
  const DWORD second = registerEvent(observer, manual.value);
  const DWORD third = registerEvent(observer, automatic.value);
  require(first != second && first != third && second != third, "distinct simultaneously active cookies");
  require(handleCount(manual.value) == manualBaseline + 2, "each duplicate registration owns its handle");
  require(handleCount(automatic.value) == autoBaseline + 1, "independent event handle ownership");

  require(observer.QueryBudget() == kHigh, "initial 64-bit query");
  source.budget.store(kHigh);
  require(observer.QueryBudget() == kHigh, "same 8 GiB query");
  manual.idle("no initial or stable manual notification");
  automatic.idle("no initial or stable auto notification");

  source.budget.store(kLow);
  require(observer.QueryBudget() == kLow, "query observes 4 GiB without truncation");
  manual.wait("manual receives downward budget transition");
  manual.wait("manual notification remains set until caller reset");
  automatic.wait("independent auto event receives downward transition");
  automatic.idle("auto notification consumes exactly one signal");
  manual.reset();
  source.budget.store(kLow);
  require(observer.QueryBudget() == kLow, "same 4 GiB query");
  manual.idle("unchanged 4 GiB must not signal manual");
  automatic.idle("unchanged 4 GiB must not signal auto");

  source.budget.store(kHigh);
  require(observer.QueryBudget() == kHigh, "query observes upward transition with identical low 32 bits");
  manual.wait("manual receives upward budget transition");
  automatic.wait("auto receives upward budget transition");
  automatic.idle("no fabricated second auto signal");
  manual.reset();

  observer.Unregister(first);
  observer.Unregister(first);
  observer.Unregister(0);
  DWORD unknown = 1;
  while (unknown == first || unknown == second || unknown == third)
    ++unknown;
  observer.Unregister(unknown);
  require(handleCount(manual.value) == manualBaseline + 1, "unregister removes only its owned duplicate");
  source.budget.store(kLow);
  require(observer.QueryBudget() == kLow, "query after unregister");
  manual.wait("duplicate cookie stays active after sibling removal");
  automatic.wait("unrelated cookie stays active after unknown unregister");
  observer.Unregister(second);
  observer.Unregister(third);
  require(handleCount(manual.value) == manualBaseline, "final unregister closes all manual duplicates");
  require(handleCount(automatic.value) == autoBaseline, "final unregister closes auto duplicate");
  manual.reset();
  source.budget.store(kHigh);
  require(observer.QueryBudget() == kHigh, "budget query remains usable without subscribers");
  manual.idle("no signal after final unregister");
  automatic.idle("no auto signal after final unregister");
  std::puts("DXGI_BUDGET_TRANSITIONS_COOKIES_PASS");
}

void testRightsValidationAndCallerLifetime() {
  Source source;
  Event witness;
  Event initiallySignaled(false, true);
  d3dalloy::DxgiBudgetNotifications observer(Source::read, &source);
  const ULONG baseline = handleCount(witness.value);
  Handle queryOnly(duplicate(witness.value, kEventQueryState));
  Handle syncOnly(duplicate(witness.value, SYNCHRONIZE));
  Handle semaphore(CreateSemaphoreW(nullptr, 0, 1, nullptr));
  rejected(observer, nullptr, "reject null event");
  rejected(observer, INVALID_HANDLE_VALUE, "reject invalid/pseudo process event");
  rejected(observer, GetCurrentThread(), "reject non-event thread handle");
  rejected(observer, queryOnly.value, "reject query-only event");
  rejected(observer, syncOnly.value, "reject synchronize-only event");
  rejected(observer, semaphore.value, "reject semaphore despite overlapping modify-right bit");
  HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(closed != nullptr && CloseHandle(closed) != FALSE, "prepare invalid closed event handle");
  rejected(observer, closed, "reject closed handle at duplication failure");
  require(observer.Register(witness.value, nullptr) == E_INVALIDARG, "reject null cookie output");
  require(handleCount(witness.value) == baseline + 2, "failure paths leak no owned handles");
  witness.idle("validation never mutates event state");

  const DWORD initialCookie = registerEvent(observer, initiallySignaled.value);
  initiallySignaled.wait("registration preserves caller's initially signaled auto event");
  initiallySignaled.idle("validation neither consumes nor fabricates auto signal");
  observer.Unregister(initialCookie);

  const HANDLE caller = duplicate(witness.value, EVENT_MODIFY_STATE);
  const DWORD cookie = registerEvent(observer, caller);
  witness.idle("modify-only registration adds no initial signal");
  require(CloseHandle(caller) != FALSE, "close caller's registration handle");
  source.budget.store(kLow);
  require(observer.QueryBudget() == kLow, "change after caller handle closure");
  witness.wait("owned duplicate signals surviving witness");
  observer.Unregister(cookie);
  require(handleCount(witness.value) == baseline + 2, "owned duplicate closes while caller witness survives");
  witness.reset();
  source.budget.store(kHigh);
  require(observer.QueryBudget() == kHigh, "stable caller witness after unregister");
  witness.idle("closed registration cannot receive later notification");
  std::puts("DXGI_BUDGET_HANDLE_RIGHTS_LIFETIME_PASS");
}

struct Operation {
  enum Kind { Register, Unregister, Stop, Destroy } kind;
  d3dalloy::DxgiBudgetNotifications *observer;
  Source *source;
  HANDLE event = nullptr;
  DWORD cookie = 0;
  HRESULT result = E_FAIL;
  std::atomic<DWORD> threadId{0};
  Event started{};
  Event done{};

  static DWORD WINAPI run(void *context) {
    auto &operation = *static_cast<Operation *>(context);
    operation.threadId.store(GetCurrentThreadId());
    if (operation.kind == Register)
      operation.source->arm(GetCurrentThreadId());
    operation.started.signal();
    switch (operation.kind) {
    case Register: operation.result = operation.observer->Register(operation.event, &operation.cookie); break;
    case Unregister: operation.observer->Unregister(operation.cookie); break;
    case Stop: operation.observer->Stop(); break;
    case Destroy: delete operation.observer; break;
    }
    operation.done.signal();
    return 0;
  }
};

void testSecondRegistrationCannotSwallowChange() {
  Source source;
  Event oldEvent;
  Event newEvent;
  d3dalloy::DxgiBudgetNotifications observer(Source::read, &source);
  const DWORD oldCookie = registerEvent(observer, oldEvent.value);
  Operation operation{Operation::Register, &observer, &source};
  operation.event = newEvent.value;
  Handle thread(CreateThread(nullptr, 0, Operation::run, &operation, 0, nullptr));
  source.entered.wait("second registration entered serialized source observation");
  require(operation.threadId.load() != 0 &&
              source.consumedThread.load(std::memory_order_acquire) == operation.threadId.load(),
          "registration thread, not polling worker, consumes targeted callback barrier");
  source.budget.store(kLow);
  source.release.signal();
  operation.done.wait("second registration completed");
  require(WaitForSingleObject(thread.value, kTimeout) == WAIT_OBJECT_0, "join second registration");
  require(operation.result == S_OK && operation.cookie != oldCookie, "second registration publishes distinct cookie");
  oldEvent.wait("second registration dispatches pending change to old subscriber");
  newEvent.idle("new subscriber does not inherit pre-registration change");
  oldEvent.reset();
  source.budget.store(kHigh);
  require(observer.QueryBudget() == kHigh, "change after second registration");
  oldEvent.wait("old subscriber remains active");
  newEvent.wait("new subscriber receives subsequent change");
  std::puts("DXGI_BUDGET_REGISTER_PENDING_CHANGE_PASS");
}

void testWorkerAndUnregisterDispatchOrdering() {
  Source source;
  Event removed;
  Event survivor;
  d3dalloy::DxgiBudgetNotifications observer(Source::read, &source);
  // Arm the next callback from inside Register's baseline read, before the
  // registry lock can be released to the newly started worker.
  source.armAfterRead.store(true);
  const DWORD removedCookie = registerEvent(observer, removed.value);

  // No QueryBudget call participates: the first gated polling observation must
  // compare against Register's baseline, not silently adopt its own first value.
  source.budget.store(kLow);
  source.entered.wait("real worker independently samples while registered");
  source.release.signal();
  removed.wait("worker detects change after registration before gated poll completes");
  removed.reset();
  const DWORD survivorCookie = registerEvent(observer, survivor.value);
  survivor.idle("later worker subscriber gets no unconditional initial signal");

  source.arm();
  source.entered.wait("unchanged worker sample entered");
  source.release.signal();
  require(observer.QueryBudget() == kLow, "serialize after unchanged worker observation");
  removed.idle("unchanged worker observation produces no notification");
  survivor.idle("unchanged worker observation leaves survivor idle");

  source.arm();
  source.entered.wait("in-flight worker source callback holds observation lock");
  source.budget.store(kHigh);
  Operation operation{Operation::Unregister, &observer, &source};
  operation.cookie = removedCookie;
  Handle thread(CreateThread(nullptr, 0, Operation::run, &operation, 0, nullptr));
  operation.started.wait("unregister invoked during held dispatch observation");
  require(WaitForSingleObject(operation.done.value, 250) == WAIT_TIMEOUT,
          "unregister cannot finish through in-flight observation");
  source.release.signal();
  operation.done.wait("unregister finishes after source release");
  require(WaitForSingleObject(thread.value, kTimeout) == WAIT_OBJECT_0, "join unregister");
  survivor.wait("in-flight transition reaches remaining registration");
  // A signal before Unregister returns is allowed; erase it only after return.
  removed.reset();
  survivor.reset();
  source.budget.store(kLow);
  require(observer.QueryBudget() == kLow, "next change after unregister return");
  removed.idle("unregister return fences all future signaling to removed cookie");
  survivor.wait("survivor receives next change after removal");
  observer.Unregister(survivorCookie);
  source.arm();
  require(WaitForSingleObject(source.entered.value, 1200) == WAIT_TIMEOUT,
          "worker performs no source sampling with empty registry");
  source.armed.store(false);
  std::puts("DXGI_BUDGET_REAL_WORKER_UNREGISTER_ORDER_PASS");
}

void testShutdownOrdering(bool destroy) {
  Source source;
  Event witness;
  const ULONG baseline = handleCount(witness.value);
  auto *observer = new d3dalloy::DxgiBudgetNotifications(Source::read, &source);
  registerEvent(*observer, witness.value);
  require(handleCount(witness.value) == baseline + 1, "shutdown fixture owns event duplicate");
  source.arm();
  source.entered.wait("shutdown begins with worker callback in flight");
  source.budget.store(kLow);
  Operation operation{destroy ? Operation::Destroy : Operation::Stop, observer, &source};
  Handle thread(CreateThread(nullptr, 0, Operation::run, &operation, 0, nullptr));
  operation.started.wait("shutdown invoked during blocked source callback");
  require(WaitForSingleObject(operation.done.value, 250) == WAIT_TIMEOUT,
          "shutdown must wait for callback before returning/freeing context");
  source.release.signal();
  operation.done.wait("shutdown completes after callback barrier release");
  require(WaitForSingleObject(thread.value, kTimeout) == WAIT_OBJECT_0, "join shutdown thread");
  require(handleCount(witness.value) == baseline, "shutdown closes final owned duplicate");
  const unsigned completed = source.completed.load();
  source.arm();
  require(WaitForSingleObject(source.entered.value, 1200) == WAIT_TIMEOUT,
          "joined worker cannot access source after shutdown return");
  source.armed.store(false);
  require(source.completed.load() == completed, "source stays quiescent after shutdown");
  witness.reset();
  require(SetEvent(witness.value) != FALSE, "caller witness remains valid after observer teardown");
  witness.wait("caller retains witness lifetime after observer teardown");
  if (!destroy) {
    observer->Stop();
    rejected(*observer, witness.value, "stopped observer rejects new registrations");
    delete observer;
    require(handleCount(witness.value) == baseline, "destruction after Stop is idempotent");
  }
  std::puts(destroy ? "DXGI_BUDGET_DESTRUCTION_ORDER_PASS" : "DXGI_BUDGET_STOP_ORDER_PASS");
}

} // namespace

int main() {
  static_assert(static_cast<uint32_t>(kHigh) == static_cast<uint32_t>(kLow));
  testTransitionsAndIndependentCookies();
  testRightsValidationAndCallerLifetime();
  testSecondRegistrationCannotSwallowChange();
  testWorkerAndUnregisterDispatchOrdering();
  testShutdownOrdering(false);
  testShutdownOrdering(true);
  // Allocation/thread-creation exhaustion has no deterministic available input;
  // this probe does not add production fault-injection seams to manufacture it.
  std::puts("DXGI_BUDGET_NOTIFICATION_PASS");
}
