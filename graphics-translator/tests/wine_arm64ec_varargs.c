#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#define VALUE 0x123456789abcdef0ull
#define V2 VALUE, VALUE
#define V4 V2, V2
#define V8 V4, V4
#define V16 V8, V8
#define V32 V16, V16
#define V64 V32, V32
#define V128 V64, V64
#define V256 V128, V128
#define V512 V256, V256
#define V1024 V512, V512

#ifdef __arm64ec__
typedef uint64_t (__cdecl *VarargsCallback)(unsigned count, ...);

__declspec(dllexport) unsigned WINAPI RunVarargs(VarargsCallback callback) {
  if (callback(0) != 0) return 1;
  if (callback(3, VALUE, VALUE, VALUE) != 3 * VALUE) return 2;
  if (callback(4, V4) != 4 * VALUE) return 3;
  if (callback(5, V4, VALUE) != 5 * VALUE) return 4;
  if (callback(1024, V1024) != 1024 * VALUE) return 5;
  __try {
    callback(~0u, V1024);
    return 6;
  } __except (GetExceptionCode() == 0xe123abcd ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
  }
  return 0;
}
#else
static uint64_t __cdecl check(unsigned count, ...) {
  if (count == ~0u) RaiseException(0xe123abcd, 0, 0, NULL);
  va_list args;
  uint64_t result = 0;
  va_start(args, count);
  for (unsigned i = 0; i < count; ++i) result += va_arg(args, uint64_t);
  va_end(args);
  return result;
}

int main(void) {
  HMODULE module = LoadLibraryW(L"wine-arm64ec-varargs.dll");
  if (!module) return 10;
  typedef unsigned (WINAPI *Run)(uint64_t (__cdecl *)(unsigned, ...));
  Run run = (Run)GetProcAddress(module, "RunVarargs");
  if (!run) return 11;
  unsigned result = run(check);
  printf("ARM64EC_VARARGS result=%u stack_cases=0,1,2,1021 unwind=large-frame\n", result);
  FreeLibrary(module);
  return result;
}
#endif
