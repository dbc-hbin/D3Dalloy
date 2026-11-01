#pragma once

#ifndef _WIN32

#include <windows.h>

#include "log/log.hpp"

#define THREAD_PRIORITY_TIME_CRITICAL 15

inline HANDLE GetCurrentProcess() {
  d3dalloy::Logger::warn("GetCurrentProcess not implemented.");
  return nullptr;
}

inline HANDLE GetCurrentThread() {
  d3dalloy::Logger::warn("GetCurrentThread not implemented.");
  return nullptr;
}

inline DWORD GetCurrentProcessId() {
  d3dalloy::Logger::warn("GetCurrentProcessId not implemented.");
  return 0;
}

inline BOOL ProcessIdToSessionId(DWORD pid, DWORD *id) {
  d3dalloy::Logger::warn("ProcessIdToSessionId not implemented.");
  *id = 0;
  return FALSE;
}

inline BOOL SetThreadPriority(HANDLE hThread, int nPriority) {
  d3dalloy::Logger::warn("SetThreadPriority not implemented.");
  return FALSE;
}

inline HANDLE GetModuleHandle(LPCSTR lpModuleName) {
  d3dalloy::Logger::warn("GetModuleHandle not implemented.");
  return nullptr;
}

inline void ExitThread(DWORD dwExitCode) {
  d3dalloy::Logger::warn("ExitThread not implemented.");
}

#define WM_SIZE 0

inline LONG SendMessage(HWND hWnd, UINT Msg, UINT_PTR wParam, LONG_PTR lParam) {
  d3dalloy::Logger::warn("SendMessage not implemented.");
  return 0;
}

inline DWORD GetProcessId(HANDLE Process) {
  d3dalloy::Logger::warn("GetProcessId not implemented.");
  return 0;
}

inline DWORD GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId) {
  d3dalloy::Logger::warn("GetWindowThreadProcessId not implemented.");
  *lpdwProcessId = 0;
  return 0;
}

inline DWORD MAKELONG(WORD a, WORD b) {
  return (((DWORD)a) << 16) | ((DWORD)b);
}

typedef SECURITY_ATTRIBUTES *LPSECURITY_ATTRIBUTES;

inline HANDLE CreateSemaphore(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes,
                              LONG                  lInitialCount,
                              LONG                  lMaximumCount,
                              LPCSTR                lpName) {
  d3dalloy::Logger::warn("CreateSemaphore not implemented.");
  return nullptr;
}

inline BOOL ReleaseSemaphore(HANDLE hSemaphore,
                             LONG   lReleaseCount,
                             LPLONG lpPreviousCount) {
  d3dalloy::Logger::warn("ReleaseSemaphore not implemented.");
  return FALSE;
}

inline BOOL CloseHandle(HANDLE hObject) {
  d3dalloy::Logger::warn("CloseHandle not implemented.");
  return FALSE;
}

inline BOOL DuplicateHandle(HANDLE   hSourceProcessHandle,
                            HANDLE   hSourceHandle,
                            HANDLE   hTargetProcessHandle,
                            LPHANDLE lpTargetHandle,
                            DWORD    dwDesiredAccess,
                            BOOL     bInheritHandle,
                            DWORD    dwOptions)
{
  d3dalloy::Logger::warn("DuplicateHandle not implemented.");
  return FALSE;
}


#define ARRAYSIZE(a) (sizeof(a)/sizeof(*(a)))

#endif