/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include "util_d3dkmt.h"
#include "wsi_window.hpp"
#include "wsi_monitor.hpp"

#include "util_string.hpp"
#include "log/log.hpp"

namespace d3dalloy::wsi {

static bool getWindowLong(HWND hWindow, int index, LONG *value) {
  ::SetLastError(0);
  LONG result = ::GetWindowLongW(hWindow, index);
  if (result == 0 && ::GetLastError() != 0)
    return false;
  *value = result;
  return true;
}

static bool setWindowLong(HWND hWindow, int index, LONG value) {
  ::SetLastError(0);
  return ::SetWindowLongW(hWindow, index, value) != 0 || ::GetLastError() == 0;
}

struct NativeWindowState {
  RECT rect;
  LONG style;
  LONG exstyle;
};

static bool getNativeWindowState(HWND hWindow, NativeWindowState *state) {
  return ::GetWindowRect(hWindow, &state->rect) &&
         getWindowLong(hWindow, GWL_STYLE, &state->style) &&
         getWindowLong(hWindow, GWL_EXSTYLE, &state->exstyle);
}

static void restoreNativeWindowState(HWND hWindow,
                                     const NativeWindowState &state) {
  if (!::IsWindow(hWindow))
    return;

  setWindowLong(hWindow, GWL_STYLE, state.style);
  setWindowLong(hWindow, GWL_EXSTYLE, state.exstyle);
  ::SetWindowPos(hWindow, (state.exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST
                                                       : HWND_NOTOPMOST,
                 state.rect.left, state.rect.top,
                 state.rect.right - state.rect.left,
                 state.rect.bottom - state.rect.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE |
                     ((state.style & WS_VISIBLE) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

static bool getMonitorDisplayMode(HMONITOR hMonitor, DWORD modeNum,
                                  DEVMODEW *pMode) {
  ::MONITORINFOEXW monInfo;
  monInfo.cbSize = sizeof(monInfo);

  if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO *>(&monInfo))) {
    Logger::err("Failed to query monitor info");
    return false;
  }

  return ::EnumDisplaySettingsW(monInfo.szDevice, modeNum, pMode);
}

static bool setMonitorDisplayMode(HMONITOR hMonitor, DEVMODEW *pMode) {
  ::MONITORINFOEXW monInfo;
  monInfo.cbSize = sizeof(monInfo);

  if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO *>(&monInfo))) {
    Logger::err("Failed to query monitor info");
    return false;
  }

  Logger::info(str::format("Setting display mode: ", pMode->dmPelsWidth, "x",
                           pMode->dmPelsHeight, "@",
                           pMode->dmDisplayFrequency));

  DEVMODEW curMode = {};
  curMode.dmSize = sizeof(curMode);

  if (getMonitorDisplayMode(hMonitor, ENUM_CURRENT_SETTINGS, &curMode)) {
    bool eq = curMode.dmPelsWidth == pMode->dmPelsWidth &&
              curMode.dmPelsHeight == pMode->dmPelsHeight &&
              curMode.dmBitsPerPel == pMode->dmBitsPerPel;

    if (pMode->dmFields & DM_DISPLAYFREQUENCY)
      eq &= curMode.dmDisplayFrequency == pMode->dmDisplayFrequency;

    if (eq)
      return true;
  }

  LONG status = ::ChangeDisplaySettingsExW(monInfo.szDevice, pMode, nullptr,
                                           CDS_FULLSCREEN, nullptr);

  if (status != DISP_CHANGE_SUCCESSFUL) {
    pMode->dmFields &= ~DM_DISPLAYFREQUENCY;

    status = ::ChangeDisplaySettingsExW(monInfo.szDevice, pMode, nullptr,
                                        CDS_FULLSCREEN, nullptr);
  }

  return status == DISP_CHANGE_SUCCESSFUL;
}

void getWindowSize(HWND hWindow, uint32_t *pWidth, uint32_t *pHeight) {
  RECT rect = {};
  ::GetClientRect(hWindow, &rect);

  if (pWidth)
    *pWidth = rect.right - rect.left;

  if (pHeight)
    *pHeight = rect.bottom - rect.top;
}

bool resizeWindow(HWND hWindow, D3DalloyWindowState *pState, uint32_t width,
                  uint32_t height) {
  // Adjust window position and size
  RECT newRect = {0, 0, 0, 0};
  NativeWindowState oldState = {};

  if (!getNativeWindowState(hWindow, &oldState))
    return false;

  ::SetRect(&newRect, 0, 0, width, height);
  if (!::AdjustWindowRectEx(&newRect, oldState.style, FALSE, oldState.exstyle))
    return false;
  ::SetRect(&newRect, 0, 0, newRect.right - newRect.left,
            newRect.bottom - newRect.top);
  ::OffsetRect(&newRect, oldState.rect.left, oldState.rect.top);
  if (!::MoveWindow(hWindow, newRect.left, newRect.top,
                    newRect.right - newRect.left, newRect.bottom - newRect.top,
                    TRUE)) {
    restoreNativeWindowState(hWindow, oldState);
    return false;
  }
  return true;
}

bool setWindowMode(HMONITOR hMonitor, HWND hWindow, const WsiMode &mode) {
  ::MONITORINFOEXW monInfo;
  monInfo.cbSize = sizeof(monInfo);

  if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO *>(&monInfo))) {
    Logger::err("Win32 WSI: setWindowMode: Failed to query monitor info");
    return false;
  }

  DEVMODEW devMode = {};
  devMode.dmSize = sizeof(devMode);
  devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
  devMode.dmPelsWidth = mode.width;
  devMode.dmPelsHeight = mode.height;
  devMode.dmBitsPerPel = mode.bitsPerPixel;

  if (mode.refreshRate.numerator != 0) {
    devMode.dmFields |= DM_DISPLAYFREQUENCY;
    devMode.dmDisplayFrequency =
        mode.refreshRate.numerator / mode.refreshRate.denominator;
  }

  return setMonitorDisplayMode(hMonitor, &devMode);
}

bool enterFullscreenMode(HMONITOR hMonitor, HWND hWindow,
                         D3DalloyWindowState *pState,
                         [[maybe_unused]] bool modeSwitch) {
  RECT rect = {};
  NativeWindowState oldState = {};
  if (!getDesktopCoordinates(hMonitor, &rect) ||
      !getNativeWindowState(hWindow, &oldState))
    return false;

  // Publish the saved window state only after the native transition succeeds.
  if (!setWindowLong(hWindow, GWL_STYLE,
                     oldState.style & ~WS_OVERLAPPEDWINDOW) ||
      !setWindowLong(hWindow, GWL_EXSTYLE,
                     oldState.exstyle & ~WS_EX_OVERLAPPEDWINDOW) ||
      !::SetWindowPos(hWindow, HWND_TOPMOST, rect.left, rect.top,
                      rect.right - rect.left, rect.bottom - rect.top,
                      SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
    restoreNativeWindowState(hWindow, oldState);
    return false;
  }

  pState->rect = oldState.rect;
  pState->style = oldState.style;
  pState->exstyle = oldState.exstyle;

  D3DKMT_ESCAPE escape = {};
  escape.Type = D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE;
  escape.pPrivateDriverData = &rect;
  escape.PrivateDriverDataSize = sizeof(rect);
  escape.hContext = HandleToUlong(hWindow);
  D3DKMTEscape(&escape);

  return true;
}

bool leaveFullscreenMode(HWND hWindow, D3DalloyWindowState *pState,
                         bool restoreCoordinates) {
  NativeWindowState oldState = {};
  if (!getNativeWindowState(hWindow, &oldState))
    return false;

  // Only restore the window style if the application hasn't
  // changed them. This is in line with what native DXGI does.
  LONG curStyle = oldState.style & ~WS_VISIBLE;
  LONG curExstyle = oldState.exstyle & ~WS_EX_TOPMOST;

  if (curStyle == (pState->style & ~(WS_VISIBLE | WS_OVERLAPPEDWINDOW)) &&
      curExstyle ==
          (pState->exstyle & ~(WS_EX_TOPMOST | WS_EX_OVERLAPPEDWINDOW))) {
    if (!setWindowLong(hWindow, GWL_STYLE, pState->style) ||
        !setWindowLong(hWindow, GWL_EXSTYLE, pState->exstyle)) {
      restoreNativeWindowState(hWindow, oldState);
      return false;
    }
  }

  // Restore window position and apply the style
  UINT flags = SWP_FRAMECHANGED | SWP_NOACTIVATE;
  const RECT rect = pState->rect;

  if (!restoreCoordinates)
    flags |= SWP_NOSIZE | SWP_NOMOVE;

  if (!::SetWindowPos(hWindow,
                      (pState->exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST
                                                        : HWND_NOTOPMOST,
                      rect.left, rect.top, rect.right - rect.left,
                      rect.bottom - rect.top, flags)) {
    restoreNativeWindowState(hWindow, oldState);
    return false;
  }

  RECT empty = {};
  D3DKMT_ESCAPE escape = {};
  escape.Type = D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE;
  escape.pPrivateDriverData = &empty;
  escape.PrivateDriverDataSize = sizeof(empty);
  escape.hContext = HandleToUlong(hWindow);
  D3DKMTEscape(&escape);

  return true;
}

bool restoreDisplayMode(HMONITOR hMonitor) {
  WCHAR device_name[32] = {};
  DEVMODEW current_mode = {}, registry_mode = {};
  current_mode.dmSize = sizeof(current_mode);
  registry_mode.dmSize = sizeof(registry_mode);
  if (!getDisplayName(hMonitor, device_name))
    return false;
  if (!EnumDisplaySettingsExW(device_name, ENUM_CURRENT_SETTINGS, &current_mode, 0))
    return false;
  if (!EnumDisplaySettingsExW(device_name, ENUM_REGISTRY_SETTINGS, &registry_mode, 0))
    return false;
  do {
    if (current_mode.dmPelsWidth != registry_mode.dmPelsWidth)
      break;
    if (current_mode.dmPelsHeight != registry_mode.dmPelsHeight)
      break;
    if (current_mode.dmBitsPerPel != registry_mode.dmBitsPerPel)
      break;
    if ((current_mode.dmFields & registry_mode.dmFields & DM_DISPLAYFREQUENCY) &&
        current_mode.dmDisplayFrequency != registry_mode.dmDisplayFrequency)
      break;
    return true;
  } while (0);
  LONG ret = ChangeDisplaySettingsExW(device_name, NULL, NULL, 0, NULL);
  return ret == DISP_CHANGE_SUCCESSFUL;
}

HMONITOR getWindowMonitor(HWND hWindow) {
  RECT windowRect = {0, 0, 0, 0};
  ::GetWindowRect(hWindow, &windowRect);

  HMONITOR monitor =
      ::MonitorFromPoint({(windowRect.left + windowRect.right) / 2,
                          (windowRect.top + windowRect.bottom) / 2},
                         MONITOR_DEFAULTTOPRIMARY);

  return monitor;
}

bool isWindow(HWND hWindow) { return ::IsWindow(hWindow); }

bool updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow,
                            bool forceTopmost) {
  RECT bounds = {};
  NativeWindowState oldState = {};
  if (!wsi::getDesktopCoordinates(hMonitor, &bounds) ||
      !getNativeWindowState(hWindow, &oldState))
    return false;

  // In D3D9, changing display modes re-forces the window
  // to become top most, whereas in DXGI, it does not.
  if (forceTopmost) {
    if (!::SetWindowPos(hWindow, HWND_TOPMOST, bounds.left, bounds.top,
                        bounds.right - bounds.left, bounds.bottom - bounds.top,
                        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
      restoreNativeWindowState(hWindow, oldState);
      return false;
    }
  } else {
    if (!::MoveWindow(hWindow, bounds.left, bounds.top,
                      bounds.right - bounds.left, bounds.bottom - bounds.top,
                      TRUE)) {
      restoreNativeWindowState(hWindow, oldState);
      return false;
    }
  }

  D3DKMT_ESCAPE escape = {};
  escape.Type = D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE;
  escape.pPrivateDriverData = &bounds;
  escape.PrivateDriverDataSize = sizeof(bounds);
  escape.hContext = HandleToUlong(hWindow);
  D3DKMTEscape(&escape);
  return true;
}

bool isForeground(HWND hWindow) {
  return ::GetForegroundWindow() == hWindow;
}

bool isMinimized(HWND hWindow) {
  return ::IsIconic(hWindow);
}

} // namespace d3dalloy::wsi
