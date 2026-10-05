#include "windef.h"
#include "winbase.h"
#include "wineunixlib.h"
extern NTSTATUS WMTInitializeGuestWindow(void);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return !__wine_init_unix_call() && !WMTInitializeGuestWindow();
}

extern BOOL WINAPI DllMainCRTStartup(HANDLE hDllHandle, DWORD dwReason,
                                       LPVOID lpreserved);
