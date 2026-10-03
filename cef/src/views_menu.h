// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF Views menus for the CEF backend on Windows and Linux (API 41).
//
// A CEF window is a Chromium Views window: a native menu bar (SetMenu on
// Windows, a GtkMenuBar on Linux) can't be attached to it, because Views owns
// the whole client area (and on Linux there is no GtkWindow at all). So the
// application menu is a Views menu bar: a panel of CefMenuButtons above the
// browser view, each dropping a CefMenuModel, the approach of CEF's own
// cefclient. Its accelerators are CefWindow accelerators (high priority:
// they win over the page), and context menus are CefWindow::ShowMenu
// (Linux; Windows keeps the native TrackPopupMenu, see win32_menu.h).
//
// Every function runs on CEF's UI thread (TID_UI).

#ifndef LAUFEY_CEF_VIEWS_MENU_H_
#define LAUFEY_CEF_VIEWS_MENU_H_

#if defined(_WIN32) || defined(__linux__)

#include <cstdint>
#include <vector>

#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "laufey_menu.h"

namespace laufey_cef_menu {

// Shows `entries` as `window`'s menu bar (an empty list removes it) and binds
// the items' accelerators on the window.
void SetApplicationMenu(CefRefPtr<CefWindow> window,
                        CefRefPtr<CefBrowserView> browser_view,
                        uint32_t window_id,
                        std::vector<laufey_common::MenuEntry> entries,
                        laufey_menu_click_fn on_click, void* on_click_data);

// From LaufeyWindowDelegate::OnAccelerator: runs the item bound to
// `command_id` in window `window_id`. Returns false if it isn't one of ours.
bool OnAccelerator(uint32_t window_id, int command_id);

// Test hook: the item bound to `accelerator` in window `window_id`, run
// through OnAccelerator as the window's accelerator would.
bool TestTriggerAccelerator(uint32_t window_id, const char* accelerator);

// A context menu at (x, y) in the browser view's coordinates. Non-blocking;
// `on_closed` fires once it closed (after the chosen item's click).
void ShowContextMenu(CefRefPtr<CefWindow> window,
                     CefRefPtr<CefBrowserView> browser_view, uint32_t window_id,
                     int x, int y,
                     std::vector<laufey_common::MenuEntry> entries,
                     laufey_menu_click_fn on_click, void* on_click_data,
                     laufey_menu_closed_fn on_closed, void* on_closed_data);

// The window closed.
void ForgetWindow(uint32_t window_id);

}  // namespace laufey_cef_menu

#endif  // _WIN32 || __linux__

#endif  // LAUFEY_CEF_VIEWS_MENU_H_
