// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Keeps a native modal loop's input (a context or tray menu's keys and
// clicks) away from Chromium's message pump on Windows. See
// LaufeyGuardModalLoopInput in app.h.
//
// Inside a menu's modal loop (TrackPopupMenu), the loop dispatches the
// kMsgHaveWork message Chromium posts to its message window to get a time
// slice. MessagePumpForUI::HandleWorkMessage first calls
// ProcessPumpReplacementMessage, which takes the next message of the
// thread's queue, of any kind, with PeekMessage(PM_REMOVE) and dispatches it
// itself (base/message_loop/message_pump_win.cc). A key press or a click
// already waiting in the queue is taken from the menu that way: it goes to
// the focused page instead, and the menu never sees it. With the backend's
// tasks allowed inside the loop (CefSetNestableTasksAllowed), a busy page
// keeps kMsgHaveWork coming, so a person's Escape, arrow key or Return was
// lost now and then.
//
// The menu loop shows every message it takes to the thread's WH_MSGFILTER
// hook (MSGF_MENU) before handling it. While input is waiting, the hook
// eats a kMsgHaveWork and posts it again from a timer: a WM_TIMER comes
// after input, so the menu loop takes the input first and Chromium gets its
// time slice once the queue holds no input. Chromium posts one kMsgHaveWork
// at a time (until it handled the last one it posts no other), so each one
// eaten is posted again exactly once.

#include "app.h"

#include <windows.h>

#include <cwchar>
#include <iterator>
#include <vector>

namespace {

// base/message_loop/message_pump_win.cc: kMsgHaveWork, posted to the pump's
// message window (base::win::MessageWindow, class "Chrome_MessageWindow").
constexpr UINT kMsgHaveWork = WM_USER + 1;

// UI thread only, like the hook.
HHOOK g_filter = nullptr;
UINT_PTR g_timer = 0;
std::vector<HWND> g_deferred;

bool IsChromiumMessageWindow(HWND hwnd) {
  wchar_t name[32] = {};
  return hwnd &&
         GetClassNameW(hwnd, name, static_cast<int>(std::size(name))) > 0 &&
         std::wcscmp(name, L"Chrome_MessageWindow") == 0;
}

void CALLBACK PostDeferred(HWND, UINT, UINT_PTR id, DWORD) {
  KillTimer(nullptr, id);
  g_timer = 0;
  std::vector<HWND> deferred;
  deferred.swap(g_deferred);
  for (HWND hwnd : deferred) {
    if (IsWindow(hwnd))
      PostMessageW(hwnd, kMsgHaveWork, 0, 0);
  }
}

LRESULT CALLBACK FilterMessage(int code, WPARAM wparam, LPARAM lparam) {
  const MSG* msg = reinterpret_cast<const MSG*>(lparam);
  if (code >= 0 && msg && msg->message == kMsgHaveWork &&
      HIWORD(GetQueueStatus(QS_INPUT)) != 0 &&
      IsChromiumMessageWindow(msg->hwnd)) {
    if (!g_timer)
      g_timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, PostDeferred);
    if (g_timer) {
      g_deferred.push_back(msg->hwnd);
      return TRUE;  // Eaten: the menu loop takes the input next.
    }
  }
  return CallNextHookEx(g_filter, code, wparam, lparam);
}

}  // namespace

void LaufeyGuardModalLoopInput(bool entering) {
  if (entering) {
    if (!g_filter) {
      g_filter = SetWindowsHookExW(WH_MSGFILTER, FilterMessage, nullptr,
                                   GetCurrentThreadId());
    }
  } else if (g_filter) {
    UnhookWindowsHookEx(g_filter);
    g_filter = nullptr;
    // A kMsgHaveWork still deferred is posted by its timer, which the outer
    // loop dispatches.
  }
}
