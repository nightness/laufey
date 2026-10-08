// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// Linux-specific backend implementations: the tray (via appindicator,
// dlopened in backend-common), which needs no GtkWindow parent and so works
// alongside CEF Views' own windows. The application and context menus are
// CEF Views menus (views_menu.cc), for the same reason.

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include <gtk/gtk.h>

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"

#include "runtime_loader.h"
#include "laufey.h"
#include "laufey_backend_common.h"

// ---------------------------------------------------------------------------
// GTK lazy init. CEF's Chromium process initializes GTK internally for its
// own dialogs/theming, but we don't rely on that — call gtk_init_check once
// before any GTK API use.
// ---------------------------------------------------------------------------

static void EnsureGtkInit() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    int argc = 0;
    char** argv = nullptr;
    gtk_init_check(&argc, &argv);
  });
}

void CefEnsureGtkInit() {
  EnsureGtkInit();
}

// Menus are CEF Views menus on Linux (views_menu.cc); tray menus are
// GtkMenus (backend-common).

// ---------------------------------------------------------------------------
// Tray / status-bar icon (appindicator)
// ---------------------------------------------------------------------------
//
// Trampolines over backend-common/src/tray_linux.cc, which handles its
// own GTK-main-thread marshaling via g_idle_add. CreateTrayIcon
// allocates the id atomically and returns immediately.

uint32_t Backend_CreateTrayIcon_Linux(void* /*data*/) {
  return laufey_common::CreateTrayIconLinux();
}
void Backend_DestroyTrayIcon_Linux(void* /*data*/, uint32_t tray_id) {
  laufey_common::DestroyTrayIconLinux(tray_id);
}
void Backend_SetTrayIcon_Linux(void* /*data*/, uint32_t tray_id,
                               const void* png_bytes, size_t len) {
  laufey_common::SetTrayIconLinux(tray_id, png_bytes, len);
}
void Backend_SetTrayTooltip_Linux(void* /*data*/, uint32_t tray_id,
                                  const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipLinux(tray_id, tooltip_or_null);
}
void Backend_SetTrayMenu_Linux(void* data, uint32_t tray_id,
                               laufey_value_t* menu_template,
                               laufey_menu_click_fn on_click,
                               void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  laufey_common::SetTrayMenuLinux(tray_id, menu_template,
                                  &loader->GetBackendApi(), on_click,
                                  on_click_data);
}
void Backend_SetTrayClickHandler_Linux(void* /*data*/, uint32_t tray_id,
                                       laufey_tray_click_fn handler,
                                       void* user_data) {
  laufey_common::SetTrayClickHandlerLinux(tray_id, handler, user_data);
}
void Backend_SetTrayDoubleClickHandler_Linux(void* /*data*/, uint32_t tray_id,
                                             laufey_tray_click_fn handler,
                                             void* user_data) {
  laufey_common::SetTrayDoubleClickHandlerLinux(tray_id, handler, user_data);
}
void Backend_SetTrayIconDark_Linux(void* /*data*/, uint32_t tray_id,
                                   const void* png_bytes, size_t len) {
  laufey_common::SetTrayIconDarkLinux(tray_id, png_bytes, len);
}

// Notifications: runtime_loader.cc over laufey_notifications.h.
