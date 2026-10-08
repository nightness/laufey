// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <map>
#include <set>

#include "include/cef_app.h"
#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "app.h"
#include "custom_schemes.h"
#include "laufey_backend_common.h"
#include "laufey_cef_sandbox.h"
#include "laufey_launch_args.h"
#include "laufey_launch_config.h"
#include "laufey_auth_session.h"
#include "laufey_notifications.h"
#include "laufey_platform_features.h"
#include "laufey_single_instance.h"
#include "renderer_app.h"
#include "laufey_window.h"
#include "runtime_loader.h"

#include <gio/gio.h>

void LaufeyOpenExternalURL(const std::string& url) {
  g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
}

// Chromium's cookie store encrypts with a key it keeps in the Secret Service
// or KWallet (OSCrypt). When no one here can hand it out (a Secret Service
// that is locked, or not running and may start locked, with no one to
// answer its unlock prompt: a headless, ssh or CI session; a closed KWallet,
// whose key request Chromium never gets answered), Chromium waits for the
// key forever, and every request that carries cookies (navigations,
// fetches, WebSocket handshakes) waits with it. In that case use
// --password-store=basic (a fixed key: the cookies are only obfuscated) and
// say so once. With no Secret Service at all (or no session bus) Chromium
// falls back to basic by itself. An explicit --password-store is kept.
//
// On KDE the OS store is pinned to the kwalletd the probe asked
// (--password-store=kwallet6 / kwallet5 / kwallet). Left to itself
// Chromium picks its daemon from the environment alone: XDG_CURRENT_DESKTOP
// =KDE without KDE_SESSION_VERSION (ssh, a systemd unit, a wrapper that
// copies part of the environment) is KDE 4's org.kde.kwalletd at
// /modules/kwalletd, a name Plasma 6's kwalletd6 owns with no object behind
// it. Chromium's KWallet start then fails, and it falls back to basic,
// dropping every OS-key cookie, while the probe (which asked kwalletd6)
// reported the OS key.
// platform_features() reports the choice as "cookieEncryption". Browser
// process only.
//
// Never basic on a profile that holds cookies encrypted with the OS key
// ("v11" rows in its cookie database, read here before CefInitialize):
// Chromium drops a cookie it can't decrypt and then deletes its whole
// site's cookies from the database, so one basic launch would lose them for
// good. Such a profile keeps the OS key even when no one may be able to
// unlock it: requests that carry cookies wait until the keystore is
// unlocked, stderr says so, and platform_features() reports the reason as
// "cookieEncryptionWait". A database that can't be read counts as holding
// such cookies. An explicit --password-store=basic is honoured, with a
// warning that those cookies will be deleted. Basic (v10) cookies stay
// readable under the OS key, so a profile with none of its own moves
// between the two freely. The profile also records "os" once it has the OS
// key (laufey_platform_features.h, kPasswordStoreMarkerName): a hint; the
// database decides.
static std::string g_root_cache_path;

static void LaufeyApplyPasswordStore(CefRefPtr<CefCommandLine> command_line) {
  laufey_common::RemoveStalePasswordStoreTemps(
      g_root_cache_path, static_cast<int64_t>(time(nullptr)));
  std::string explicit_store;
  bool has_explicit = command_line->HasSwitch("password-store");
  if (has_explicit)
    explicit_store = command_line->GetSwitchValue("password-store");
  std::string detail;
  laufey_common::ProfileCookieKeys cookies =
      laufey_common::ReadProfileCookieKeys(g_root_cache_path, &detail);
  laufey_common::PasswordStoreChoice choice =
      laufey_common::ChoosePasswordStore(
          has_explicit ? &explicit_store : nullptr,
          laufey_common::ReadPasswordStoreMarker(g_root_cache_path), cookies,
          [] {
            laufey_common::PlatformFeatures features;
            laufey_common::ProbeSecretService(&features);
            return features;
          });
  if (choice.append_basic)
    command_line->AppendSwitchWithValue("password-store", "basic");
  else if (!choice.append_store.empty())
    command_line->AppendSwitchWithValue("password-store", choice.append_store);
  if (choice.record)
    laufey_common::WritePasswordStoreMarker(g_root_cache_path, choice.store);
  laufey_common::SetCookieEncryption(
      choice.store.c_str(), choice.wait ? choice.reason.c_str() : nullptr);
  std::string warning =
      laufey_common::PasswordStoreWarning(choice, cookies, detail);
  if (!warning.empty())
    std::cerr << warning << std::endl;
}

// --- Native event monitors (Linux / X11) ---
// Uses XI2 (X Input Extension 2) on a dedicated X11 connection to monitor
// mouse, scroll, cursor enter/leave, and focus events for CEF Views windows.
// Window resize/move events use StructureNotifyMask on the same connection.
//
// A separate X11 connection is used so that event selection doesn't interfere
// with CEF's or GDK's own event handling. The connection's FD is integrated
// into the GLib main loop via g_io_add_watch.

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>

static Display* g_monitor_display = nullptr;
static int g_xi2_opcode = 0;
static GIOChannel* g_io_channel = nullptr;
static guint g_io_source_id = 0;
static bool g_monitor_installed = false;

// Cache: WM frame XID -> (laufey_id, CEF content XID).
// With a reparenting WM, the top-level child of root is the WM frame,
// not the CEF window. This cache maps frame XIDs to the laufey IDs and
// the actual content window for coordinate translation.
struct CachedWindow {
  uint32_t laufey_id;
  Window content_xid;
};
static std::map<Window, CachedWindow> g_frame_cache;

static uint32_t XI2ModsToLaufey(XIModifierState* mods) {
  uint32_t laufey = 0;
  if (mods->effective & ShiftMask)
    laufey |= LAUFEY_MOD_SHIFT;
  if (mods->effective & ControlMask)
    laufey |= LAUFEY_MOD_CONTROL;
  if (mods->effective & Mod1Mask)
    laufey |= LAUFEY_MOD_ALT;
  if (mods->effective & Mod4Mask)
    laufey |= LAUFEY_MOD_META;
  return laufey;
}

// Multi-click detection: same button within LAUFEY_MULTI_CLICK_TIME_MS and
// LAUFEY_MULTI_CLICK_DISTANCE_PX of the previous click increments the count.
static constexpr int LAUFEY_MULTI_CLICK_TIME_MS = 500;
static constexpr int LAUFEY_MULTI_CLICK_DISTANCE_PX = 5;

struct ClickTracker {
  Time last_time = 0;
  int last_button = 0;
  double last_x = 0;
  double last_y = 0;
  int count = 0;
};
static ClickTracker g_click_tracker;

// Tracks which laufey window the cursor is currently over and last cursor
// position within it. Populated from per-window Enter/Motion events.
// RawButtonPress/Release events lack window context, so we route them to
// this tracked window with these coordinates.
static uint32_t g_hover_wid = 0;
static double g_hover_x = 0;
static double g_hover_y = 0;
static uint32_t g_hover_modifiers = 0;

static int ComputeClickCount(Time time, int button, double x, double y) {
  double dx = x - g_click_tracker.last_x;
  double dy = y - g_click_tracker.last_y;
  bool close = (dx * dx + dy * dy) <= (LAUFEY_MULTI_CLICK_DISTANCE_PX *
                                       LAUFEY_MULTI_CLICK_DISTANCE_PX);
  if (button == g_click_tracker.last_button &&
      time - g_click_tracker.last_time <= (Time)LAUFEY_MULTI_CLICK_TIME_MS &&
      close) {
    g_click_tracker.count += 1;
  } else {
    g_click_tracker.count = 1;
  }
  g_click_tracker.last_time = time;
  g_click_tracker.last_button = button;
  g_click_tracker.last_x = x;
  g_click_tracker.last_y = y;
  return g_click_tracker.count;
}

static int XI2ButtonToLaufey(int detail) {
  switch (detail) {
    case 1:
      return LAUFEY_MOUSE_BUTTON_LEFT;
    case 2:
      return LAUFEY_MOUSE_BUTTON_MIDDLE;
    case 3:
      return LAUFEY_MOUSE_BUTTON_RIGHT;
    case 8:
      return LAUFEY_MOUSE_BUTTON_BACK;
    case 9:
      return LAUFEY_MOUSE_BUTTON_FORWARD;
    default:
      return LAUFEY_MOUSE_BUTTON_LEFT;
  }
}

// Resolve an XI2 device event to a laufey window ID and content-relative
// coords. With per-window XI2 selection, dev->event is the window we selected
// on (the CEF content window) and dev->event_x/y are already window-relative.
static bool ResolveWindow(XIDeviceEvent* dev, uint32_t* out_wid, double* out_x,
                          double* out_y) {
  if (!dev->event)
    return false;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  uint32_t wid =
      loader->GetLaufeyIdForNativeHandle((void*)(uintptr_t)dev->event);

  if (wid == 0) {
    // Fall back to the frame-cache mapping in case selection landed on a
    // reparented ancestor.
    auto it = g_frame_cache.find(dev->event);
    if (it != g_frame_cache.end())
      wid = it->second.laufey_id;
  }

  if (wid == 0)
    return false;

  *out_wid = wid;
  *out_x = dev->event_x;
  *out_y = dev->event_y;
  return true;
}

static void ProcessXI2Event(XEvent* xev) {
  if (!XGetEventData(xev->xcookie.display, &xev->xcookie))
    return;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  int evtype = xev->xcookie.evtype;

  if (getenv("LAUFEY_CLICK_DEBUG")) {
    fprintf(stderr, "[laufey-click] xi2 evtype=%d\n", evtype);
  }

  if (evtype == XI_Enter || evtype == XI_Leave) {
    // Filter out synthetic Enter/Leave events generated by grab transitions
    // (CEF acquires/releases pointer grabs on button press/release). Only
    // real cursor crossings have mode=XINotifyNormal.
    XIEnterEvent* enter = static_cast<XIEnterEvent*>(xev->xcookie.data);
    if (enter->mode != XINotifyNormal) {
      XFreeEventData(xev->xcookie.display, &xev->xcookie);
      return;
    }
  }

  if (evtype == XI_ButtonPress || evtype == XI_ButtonRelease ||
      evtype == XI_Motion || evtype == XI_Enter || evtype == XI_Leave) {
    XIDeviceEvent* dev = static_cast<XIDeviceEvent*>(xev->xcookie.data);
    uint32_t wid;
    double x, y;

    if (ResolveWindow(dev, &wid, &x, &y)) {
      uint32_t modifiers = XI2ModsToLaufey(&dev->mods);

      switch (evtype) {
        case XI_ButtonPress: {
          int detail = dev->detail;
          if (detail >= 4 && detail <= 7) {
            double dx = 0, dy = 0;
            if (detail == 4)
              dy = -1.0;
            else if (detail == 5)
              dy = 1.0;
            else if (detail == 6)
              dx = -1.0;
            else if (detail == 7)
              dx = 1.0;
            loader->DispatchWheelEvent(wid, dx, dy, x, y, modifiers,
                                       LAUFEY_WHEEL_DELTA_LINE);
          } else {
            int laufey_button = XI2ButtonToLaufey(detail);
            int click_count = ComputeClickCount(dev->time, laufey_button, x, y);
            loader->DispatchMouseClickEvent(wid, LAUFEY_MOUSE_PRESSED,
                                            laufey_button, x, y, modifiers,
                                            click_count);
            // CEF holds an XI2 grab during press/release on its own X
            // connection, so XI_ButtonRelease is never delivered to our
            // monitor (and raw XI2 release events are unreliable under
            // XWayland). Synthesize a release immediately so click/dblclick
            // dispatch in deno still works. CEF's own press+release handling
            // (for its internal rendering / drag) is unaffected because it
            // runs on a separate X connection.
            loader->DispatchMouseClickEvent(wid, LAUFEY_MOUSE_RELEASED,
                                            laufey_button, x, y, modifiers,
                                            click_count);
          }
          break;
        }
        case XI_ButtonRelease: {
          int detail = dev->detail;
          if (detail >= 4 && detail <= 7)
            break;
          int laufey_button = XI2ButtonToLaufey(detail);
          loader->DispatchMouseClickEvent(wid, LAUFEY_MOUSE_RELEASED,
                                          laufey_button, x, y, modifiers,
                                          g_click_tracker.count);
          break;
        }
        case XI_Motion:
          g_hover_wid = wid;
          g_hover_x = x;
          g_hover_y = y;
          g_hover_modifiers = modifiers;
          loader->DispatchMouseMoveEvent(wid, x, y, modifiers);
          break;
        case XI_Enter:
          g_hover_wid = wid;
          g_hover_x = x;
          g_hover_y = y;
          g_hover_modifiers = modifiers;
          loader->DispatchCursorEnterLeaveEvent(wid, 1, x, y, modifiers);
          break;
        case XI_Leave:
          if (g_hover_wid == wid)
            g_hover_wid = 0;
          loader->DispatchCursorEnterLeaveEvent(wid, 0, x, y, modifiers);
          break;
      }
    }
  } else if (evtype == XI_RawButtonRelease) {
    // Raw release bypasses CEF's drag grabs that swallow normal releases.
    // Route to the currently hovered window with last cursor coords.
    XIRawEvent* raw = static_cast<XIRawEvent*>(xev->xcookie.data);
    int detail = raw->detail;
    if (getenv("LAUFEY_CLICK_DEBUG")) {
      fprintf(stderr,
              "[laufey-click] raw-release detail=%d hover_wid=%u count=%d\n",
              detail, g_hover_wid, g_click_tracker.count);
    }
    if (detail < 4 || detail > 7) {
      if (g_hover_wid != 0) {
        int laufey_button = XI2ButtonToLaufey(detail);
        loader->DispatchMouseClickEvent(
            g_hover_wid, LAUFEY_MOUSE_RELEASED, laufey_button, g_hover_x,
            g_hover_y, g_hover_modifiers, g_click_tracker.count);
      }
    }
  }

  XFreeEventData(xev->xcookie.display, &xev->xcookie);
}

static void ProcessStructureEvent(XEvent* xev) {
  if (xev->type != ConfigureNotify)
    return;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  XConfigureEvent* config = &xev->xconfigure;

  uint32_t wid =
      loader->GetLaufeyIdForNativeHandle((void*)(uintptr_t)config->window);
  if (wid > 0) {
    loader->DispatchResizeEvent(wid, config->width, config->height);
    loader->DispatchMoveEvent(wid, config->x, config->y);
  }
}

static gboolean X11IoCallback(GIOChannel* source, GIOCondition condition,
                              gpointer data) {
  if (!g_monitor_display)
    return FALSE;

  while (XPending(g_monitor_display)) {
    XEvent xev;
    XNextEvent(g_monitor_display, &xev);

    if (xev.type == GenericEvent && xev.xcookie.extension == g_xi2_opcode) {
      ProcessXI2Event(&xev);
    } else {
      ProcessStructureEvent(&xev);
    }
  }

  return TRUE;
}

#endif  // GDK_WINDOWING_X11

void InstallNativeMouseMonitor() {
#ifdef GDK_WINDOWING_X11
  if (g_monitor_installed)
    return;

  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;

  // Open a dedicated X11 connection for event monitoring.
  g_monitor_display = XOpenDisplay(nullptr);
  if (!g_monitor_display)
    return;

  // Check for XI2 support.
  int xi2_event, xi2_error;
  if (!XQueryExtension(g_monitor_display, "XInputExtension", &g_xi2_opcode,
                       &xi2_event, &xi2_error)) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
    return;
  }

  int major = 2, minor = 0;
  if (XIQueryVersion(g_monitor_display, &major, &minor) != Success) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
    return;
  }

  // Select XI2 RawButtonRelease on the root. Raw events bypass grabs, so
  // releases still reach us even while CEF holds a drag grab on the
  // window. Press/motion are delivered fine via per-window selection.
  Window root = DefaultRootWindow(g_monitor_display);
  unsigned char raw_mask_bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(raw_mask_bits, XI_RawButtonRelease);

  XIEventMask raw_mask;
  raw_mask.deviceid = XIAllMasterDevices;
  raw_mask.mask_len = sizeof(raw_mask_bits);
  raw_mask.mask = raw_mask_bits;
  XISelectEvents(g_monitor_display, root, &raw_mask, 1);
  XFlush(g_monitor_display);

  // Integrate the monitoring connection into the GLib main loop.
  int fd = ConnectionNumber(g_monitor_display);
  g_io_channel = g_io_channel_unix_new(fd);
  g_io_source_id = g_io_add_watch(
      g_io_channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      X11IoCallback, nullptr);

  g_monitor_installed = true;
#endif
}

void RemoveNativeMouseMonitor() {
#ifdef GDK_WINDOWING_X11
  if (!g_monitor_installed)
    return;

  if (g_io_source_id) {
    g_source_remove(g_io_source_id);
    g_io_source_id = 0;
  }
  if (g_io_channel) {
    g_io_channel_unref(g_io_channel);
    g_io_channel = nullptr;
  }
  if (g_monitor_display) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
  }

  g_frame_cache.clear();
  g_monitor_installed = false;
#endif
}

void SetLinuxWindowResizable(unsigned long xid, bool resizable) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  XSizeHints hints;
  long supplied;
  XGetWMNormalHints(dpy, xid, &hints, &supplied);

  if (resizable) {
    hints.flags &= ~(PMinSize | PMaxSize);
    hints.min_width = 0;
    hints.min_height = 0;
    hints.max_width = 0;
    hints.max_height = 0;
  } else {
    XWindowAttributes attrs;
    XGetWindowAttributes(dpy, xid, &attrs);
    hints.flags |= PMinSize | PMaxSize;
    hints.min_width = attrs.width;
    hints.min_height = attrs.height;
    hints.max_width = attrs.width;
    hints.max_height = attrs.height;
  }
  XSetWMNormalHints(dpy, xid, &hints);
  XFlush(dpy);
#endif
}

void ConfigureLinuxWindowAsPanel(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  // Mark the window as a utility/panel type so the WM treats it as an
  // auxiliary tool window — floated, not part of normal focus/taskbar
  // handling — which is the closest X11 equivalent of a non-activating
  // macOS panel.
  Atom net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(dpy, xid, net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&utility), 1);

  // Keep it out of the taskbar and pager, like a tray popover.
  Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
  Atom skip_pager = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
  Atom states[] = {skip_taskbar, skip_pager};
  XChangeProperty(dpy, xid, net_wm_state, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(states), 2);

  XFlush(dpy);
#endif
}

bool IsLinuxWindowResizable(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return true;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  XSizeHints hints;
  long supplied;
  XGetWMNormalHints(dpy, xid, &hints, &supplied);

  if ((hints.flags & PMinSize) && (hints.flags & PMaxSize)) {
    return hints.min_width != hints.max_width ||
           hints.min_height != hints.max_height;
  }
  return true;
#else
  return true;
#endif
}

void SetLinuxWindowOpacity(unsigned long xid, double opacity) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  // _NET_WM_WINDOW_OPACITY is a CARDINAL where 0xffffffff is fully opaque and
  // 0 fully transparent; compositing WMs scale the whole window by it.
  Atom net_wm_opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
  if (opacity >= 1.0) {
    // Fully opaque: drop the hint so the WM treats the window normally.
    XDeleteProperty(dpy, xid, net_wm_opacity);
  } else {
    unsigned long value = (unsigned long)(opacity * 0xffffffffUL);
    XChangeProperty(dpy, xid, net_wm_opacity, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&value), 1);
  }
  XFlush(dpy);
#endif
}

double GetLinuxWindowOpacity(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return 1.0;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  Atom net_wm_opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* prop = nullptr;
  double result = 1.0;
  if (XGetWindowProperty(dpy, xid, net_wm_opacity, 0, 1, False, XA_CARDINAL,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &prop) == Success) {
    if (prop && nitems >= 1 && actual_format == 32) {
      unsigned long value = *reinterpret_cast<unsigned long*>(prop);
      result = (double)value / (double)0xffffffffUL;
    }
    if (prop)
      XFree(prop);
  }
  return result;
#else
  return 1.0;
#endif
}

void LaufeySetDialogTransientFor(void* dialog, unsigned long parent_xid) {
#ifdef GDK_WINDOWING_X11
  GtkWidget* dlg = static_cast<GtkWidget*>(dialog);
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!dlg || !parent_xid || !gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  GdkWindow* parent =
      gdk_x11_window_foreign_new_for_display(gdk_display, parent_xid);
  if (!parent)
    return;
  gtk_widget_realize(dlg);
  gdk_window_set_transient_for(gtk_widget_get_window(dlg), parent);
  // The foreign wrapper lives as long as the dialog.
  g_object_set_data_full(G_OBJECT(dlg), "laufey-transient-parent", parent,
                         g_object_unref);
#else
  (void)dialog;
  (void)parent_xid;
#endif
}

// X11 has no cheap query for "is the input shape empty", so remember which
// windows we made click-passthrough. Only touched on the CEF UI thread.
static std::set<unsigned long> g_click_passthrough_xids;

void SetLinuxWindowClickPassthrough(unsigned long xid, bool enabled) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  // Wrap the foreign XID in a GdkWindow so GDK's own XShape linkage sets the
  // input region — no direct libXext dependency needed.
  GdkWindow* gdk_win = gdk_x11_window_foreign_new_for_display(gdk_display, xid);
  if (!gdk_win)
    return;
  if (enabled) {
    // An empty input shape makes the whole window transparent to pointer
    // events; they fall through to whatever is beneath. Best-effort under a
    // reparenting window manager (the WM frame may still catch clicks), so
    // pair it with a frameless window.
    cairo_region_t* empty = cairo_region_create();
    gdk_window_input_shape_combine_region(gdk_win, empty, 0, 0);
    cairo_region_destroy(empty);
    g_click_passthrough_xids.insert(xid);
  } else {
    // NULL restores the default input shape (the full window).
    gdk_window_input_shape_combine_region(gdk_win, nullptr, 0, 0);
    g_click_passthrough_xids.erase(xid);
  }
  g_object_unref(gdk_win);
#endif
}

bool IsLinuxWindowClickPassthrough(unsigned long xid) {
  return g_click_passthrough_xids.count(xid) != 0;
}

void MonitorLinuxWindowEvents(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  if (!g_monitor_display)
    return;

  // Select StructureNotifyMask on the CEF window to get ConfigureNotify
  // (resize/move) events on our monitoring connection.
  XSelectInput(g_monitor_display, xid, StructureNotifyMask);

  // Select XI2 events on this CEF window for all master devices. Per-window
  // selection works reliably under both native X11 and XWayland, unlike
  // root-window passive selection. Release is handled via RawButtonRelease
  // on root so it bypasses CEF's drag grabs (see InstallNativeMouseMonitor).
  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(mask_bits, XI_ButtonPress);
  XISetMask(mask_bits, XI_ButtonRelease);
  XISetMask(mask_bits, XI_Motion);
  XISetMask(mask_bits, XI_Enter);
  XISetMask(mask_bits, XI_Leave);

  XIEventMask xi_mask;
  xi_mask.deviceid = XIAllMasterDevices;
  xi_mask.mask_len = sizeof(mask_bits);
  xi_mask.mask = mask_bits;
  XISelectEvents(g_monitor_display, xid, &xi_mask, 1);

  // Walk up the window tree to find the WM frame (direct child of root).
  // With a reparenting WM, the CEF window is reparented inside the frame.
  // XI2 events on root report the frame as `child`, so we cache the
  // frame → (laufey_id, content_xid) mapping for fast lookup.
  Window root_ret, parent;
  Window* children;
  unsigned int nchildren;
  Window current = xid;

  while (XQueryTree(g_monitor_display, current, &root_ret, &parent, &children,
                    &nchildren)) {
    if (children)
      XFree(children);
    if (parent == root_ret || parent == 0) {
      // current is the top-level frame (or the window itself if no WM).
      if (current != xid) {
        uint32_t wid = RuntimeLoader::GetInstance()->GetLaufeyIdForNativeHandle(
            (void*)(uintptr_t)xid);
        if (wid > 0) {
          g_frame_cache[current] = {wid, xid};
        }
      }
      break;
    }
    current = parent;
  }

  XFlush(g_monitor_display);
#else
  (void)xid;
#endif
}

// --- Headless / forked worker support ---

static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  // No UI loop in a headless worker: UI tasks are answered "not run" at
  // once instead of waiting for a loop that never runs.
  laufey_common::UiLoopEnded();
  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  // It ends when the runtime returns, however long that takes.
  loader->WaitForRuntime();
  loader->Shutdown();
  return 0;
}

// Combined app that handles both browser and renderer processes (single-exe
// model)
class LaufeyCombinedApp : public CefApp, public CefBrowserProcessHandler {
 public:
  LaufeyCombinedApp() : renderer_app_(new LaufeyRendererApp()) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return renderer_app_->GetRenderProcessHandler();
  }

  // "app" plus the schemes declared with --laufey-custom-schemes /
  // LAUFEY_CUSTOM_SCHEMES become standard, secure, fetch/CORS-enabled schemes
  // in every process (single-exe model: this runs in the browser and in each
  // subprocess). See custom_schemes.h.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override {
    laufey_schemes::RegisterAll(registrar);
  }

  void OnBeforeChildProcessLaunch(
      CefRefPtr<CefCommandLine> command_line) override {
    laufey_schemes::ForwardToChild(command_line);
  }

  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override {
    return LaufeyHandleAlreadyRunningAppRelaunch();
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    // A deep-link launch keeps none of its own command line's Chromium
    // switches. First, so the defaults below see what is left.
    if (process_type.empty()) {
      LaufeyStripDeepLinkSwitches(command_line);
      LaufeyApplyPasswordStore(command_line);
    }

    // The Ozone platform: the display that is actually there
    // (laufey_common::DisplayBackend: a Wayland socket that exists, else
    // $DISPLAY), always as an explicit --ozone-platform. Chromium's own
    // --ozone-platform-hint=auto keys off XDG_SESSION_TYPE, which is only a
    // hint and often wrong: GDM's autologin into an Xorg session (XFCE, i3)
    // leaves it "wayland" with only $DISPLAY set, and Ozone/Wayland then
    // finds no compositor and opens no window. The hint is left only when
    // there is no display at all. Only the browser process (empty
    // process_type) needs the switch; CEF propagates the resolved platform to
    // its subprocesses. An app's own --ozone-platform or
    // --ozone-platform-hint wins.
    if (process_type.empty() &&
        !command_line->HasSwitch("ozone-platform-hint") &&
        !command_line->HasSwitch("ozone-platform")) {
      const std::string backend = laufey_common::DisplayBackend();
      if (backend.empty()) {
        command_line->AppendSwitchWithValue("ozone-platform-hint", "auto");
      } else {
        command_line->AppendSwitchWithValue("ozone-platform", backend);
      }
    }

    // Thin, auto-hiding overlay scrollbars so the webview matches the
    // GNOME/Adwaita look instead of Chromium's chunky classic Linux scrollbars
    // with stepper arrows (issue #21). Only the browser process needs the
    // switch; CEF propagates resolved features to its subprocesses. Skip if the
    // embedder already set enable-features so we don't clobber their list.
    if (process_type.empty() && !command_line->HasSwitch("enable-features")) {
      command_line->AppendSwitchWithValue("enable-features",
                                          "OverlayScrollbar");
    }

    // Silence Chromium's background networking. The GCM (Google Cloud
    // Messaging) client tries to register on startup and logs noisy
    // `registration_request.cc ... PHONE_REGISTRATION_ERROR` /
    // `DEPRECATED_ENDPOINT` errors that have nothing to do with the app. A
    // webview-embedding desktop app doesn't use GCM, the component updater,
    // safebrowsing auto-update, etc., so disable the lot (matches what
    // Electron/Puppeteer do). Only the browser process needs the switch; CEF
    // propagates it to subprocesses.
    if (process_type.empty()) {
      command_line->AppendSwitch("disable-background-networking");
      // What --disable-background-networking leaves: the Chrome services
      // that still contact Google at startup (laufey_cef_network_quiet.h).
      LaufeyApplyNetworkQuietDefaults(command_line);
      LaufeyApplyInspectableToCommandLine(command_line);
    }
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    // Before any browser starts the spellchecker, which would download a
    // missing Hunspell dictionary from Google.
    LaufeyKeepLocalSpellcheckDictionaries(g_root_cache_path);

    // Keep the handler alive for the lifetime of the app.
    // Backend_CreateWindow uses LaufeyHandler::GetInstance() from the runtime
    // thread, so the handler must outlive this function scope.
    static CefRefPtr<LaufeyHandler> handler(new LaufeyHandler());

    if (!g_runtime_path.empty()) {
      if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
        std::cerr << "Failed to load runtime, exiting" << std::endl;
        CefQuitMessageLoop();
        return;
      }
      // Defer Start() to the next message loop iteration.
      // OnContextInitialized runs during CefInitialize(), before
      // CefRunMessageLoop() has started. The runtime thread's
      // Backend_CreateWindow posts CefPostTasks to the UI thread and blocks
      // until they complete — this deadlocks if the loop isn't running yet.
      CefPostTask(TID_UI, base::BindOnce(
                              []() { RuntimeLoader::GetInstance()->Start(); }));
    } else {
      // No runtime: create a default window for demo
      uint32_t laufey_id = RuntimeLoader::GetInstance()->AllocateWindowId();
      g_pending_laufey_ids.push(laufey_id);
      CefBrowserSettings browser_settings;
      CefRefPtr<CefBrowserView> browser_view =
          CefBrowserView::CreateBrowserView(handler, "https://example.com",
                                            browser_settings, nullptr, nullptr,
                                            nullptr);
      CefWindow::CreateTopLevelWindow(
          new LaufeyWindowDelegate(browser_view, laufey_id));
    }
  }

 private:
  CefRefPtr<LaufeyRendererApp> renderer_app_;
  IMPLEMENT_REFCOUNTING(LaufeyCombinedApp);
};

// The Chromium sandbox (laufey_cef_sandbox.h): on when this machine has a
// layer-1 sandbox Chromium can use, else off. One line on stderr says which
// and why. Chromium looks for chrome-sandbox next to the real executable
// (/proc/self/exe). CHROME_DEVEL_SANDBOX (Chromium's override of the helper's
// path) is not consulted: it can only make Chromium pick a helper and abort
// when that one is unusable, never turn the sandbox off. The choice is a
// platform fact ("sandbox", "sandboxReason"). An app that requires the
// sandbox (LaunchRequireSandbox) refuses to start without one: false with
// `*refuse` set.
static bool LaufeyChooseSandbox(bool* refuse) {
  std::string exe_dir;
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    exe[len] = '\0';
    exe_dir = exe;
    size_t slash = exe_dir.find_last_of('/');
    exe_dir =
        slash == std::string::npos ? std::string() : exe_dir.substr(0, slash);
  }
  laufey_common::LinuxSandboxDecision decision =
      laufey_common::DecideLinuxSandbox(
          laufey_common::ProbeLinuxSandbox(exe_dir));
  std::cerr << "laufey: sandbox: "
            << laufey_common::LinuxSandboxModeName(decision.mode) << " ("
            << decision.reason << ")" << std::endl;
  laufey_common::SetSandboxMode(
      laufey_common::LinuxSandboxModeName(decision.mode),
      decision.reason.c_str());
  std::string message;
  *refuse = laufey_common::RefuseUnsandboxedStart(
      decision, laufey_common::LaunchRequireSandbox(), &message);
  if (*refuse)
    std::cerr << message << std::endl;
  return decision.enabled();
}

// SIGTERM, SIGINT and SIGHUP end the app through its own quit, the path
// quit() and closing the last window take: the windows close, the runtime is
// shut down, and CefShutdown writes the profile and ends the child processes
// (laufey_common::InstallTerminationSignalHandlers). They replace the
// handlers Chromium installs during CefInitialize
// (chrome/browser/shutdown_signal_handlers_posix.cc), which for SIGTERM call
// chrome::SessionEnding(): that ends the browser process at once with
// _exit(0), without the runtime's shutdown or CefShutdown, and leaves the
// GPU, renderer and zygote processes to find their browser gone.

int main(int argc, char* argv[]) {
  // D-Bus activation for a notification click (`<app id>.service` passes
  // --laufey-dbus-activated): noted, and left out of the copy of argv that
  // Chromium and the single-instance forwarding get. (A CEF subprocess never
  // has it.) The process's argv is never changed: the runtime library's
  // .init_array functions still get its original argc and argv. The runtime
  // leaves the argument out itself (laufey::args_os).
  // Deliberately leaked: whatever keeps a pointer into the copy (the
  // toolkit, at-exit handlers) can still read it during exit, whatever the
  // order static destructors run in.
  static auto* host_argv = new std::vector<char*>;
  laufey_common::SetDBusActivationLaunch(
      laufey_common::CopyArgvWithoutDBusActivationArg(argc, argv, host_argv));
  argc = static_cast<int>(host_argv->size()) - 1;
  argv = host_argv->data();

  // LAUFEY_CWD is only for the Windows CEF host behind CEF's bootstrap
  // (cef/src/main_windows.cc); never pass it on to what the app starts.
  unsetenv("LAUFEY_CWD");

  // CEF gets its own copy of argv. Chromium sets the process title by
  // rewriting the argv strings in place (setproctitle), which garbles the
  // arguments the runtime later reads with std::env::args() or its
  // equivalent (they point at the original argv); see docs/deep-links.md.
  std::vector<std::string> cef_arg_storage(argv, argv + argc);
  std::vector<char*> cef_argv;
  for (std::string& arg : cef_arg_storage)
    cef_argv.push_back(&arg[0]);
  cef_argv.push_back(nullptr);
  CefMainArgs main_args(argc, cef_argv.data());

  // Single-exe model: check if we are a subprocess first
  CefRefPtr<LaufeyCombinedApp> app(new LaufeyCombinedApp());
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  // A scheduled notification's systemd timer (`<exe> --laufey-notify <id>`,
  // docs/notifications.md): post it and exit before CEF or the runtime
  // starts.
  {
    std::string notify_id;
    if (laufey_common::ParseNotifyLaunch(
            std::vector<std::string>(argv + (argc > 0 ? 1 : 0), argv + argc),
            &notify_id))
      return laufey_common::RunNotifyLaunch(notify_id);
  }

  // The browser process's command line hook drops every Chromium switch of a
  // deep-link launch (LaufeyStripDeepLinkSwitches): a link handed to the app
  // by its .desktop entry's %u can't pass switches through, as on Windows.
  // See laufey_launch_args.h.
  laufey_common::SetProcessArgs(
      std::vector<std::string>(argv + (argc > 0 ? 1 : 0), argv + argc));

  // The runtime library: a packaged app (a launch file, or a runtime next to
  // the executable) loads only the one next to its executable; a development
  // host takes --runtime (before "--"), then LAUFEY_RUNTIME_PATH. See
  // laufey_launch_args.h.
  {
    laufey_common::RuntimeChoice choice = laufey_common::ResolveRuntimePath(
        std::vector<std::string>(argv + 1, argv + argc),
        {LaufeyFindColocatedRuntime()}, {});
    // A packaged app without its runtime exits at once, before CEF starts.
    if (laufey_common::IsMissingPackagedRuntime(choice)) {
      laufey_common::ReportMissingPackagedRuntime();
      return laufey_common::kMissingRuntimeExitCode;
    }
    g_runtime_path = choice.path;
  }

  // Wayland app_id / X11 WM_CLASS for our windows (see LaufeyWindowDelegate::
  // GetLinuxWindowProperties). Prefer the reverse-DNS identifier the embedder
  // also uses for the `.desktop` file (LAUFEY_APP_ID, or "appId" in the
  // launch file); fall back to the display name.
  g_app_id = laufey_common::LaunchAppId();
  if (g_app_id.empty()) {
    if (const char* app_name = getenv("LAUFEY_APP_NAME")) {
      if (*app_name) {
        g_app_id = app_name;
      }
    }
  }

  // Check for headless / forked worker mode (skip CEF entirely)
  if (laufey_common::IsHeadlessWorkerLaunch(
          std::vector<std::string>(argv + 1, argv + argc))) {
    return run_headless(g_runtime_path);
  }

  // Single-instance mode (docs/deep-links.md): a second launch forwards its
  // arguments to the running instance and exits here, before CefInitialize
  // (so CEF's own profile singleton is never reached) and before the runtime
  // loads.
  int single_instance_exit = 0;
  if (!laufey_common::SingleInstanceStartup(argc, argv,
                                            &single_instance_exit)) {
    return single_instance_exit;
  }
  // Notifications (API 41): the Windows toast activator / the Linux
  // scheduler start before the runtime, so a click on a toast that launched
  // the app, or a notification scheduled for while it wasn't running, is
  // delivered.
  laufey_common::InitNotificationsAtLaunch();

  CefSettings settings;
  bool refuse_unsandboxed = false;
  settings.no_sandbox = !LaufeyChooseSandbox(&refuse_unsandboxed);
  if (refuse_unsandboxed)
    return laufey_common::kSandboxRequiredExitCode;
  settings.log_severity = LaufeyCefLogSeverity();

  // Set cache path. With a per-app data dir (LAUFEY_DATA_DIR / LAUFEY_APP_ID)
  // the profile persists there; cache_path must be set too (equal to the root)
  // or CEF runs the browser "incognito" and keeps localStorage/cookies in
  // memory. Without one, a throwaway per-process root: a fresh 0700
  // directory with a random name under $TMPDIR or /tmp (never a fixed name
  // another user of /tmp could create first). If even that fails the root
  // stays unset and CEF keeps the profile in memory.
  std::string cache_path = laufey_common::AppDataSubdir("CEF");
  if (!cache_path.empty()) {
    CefString(&settings.root_cache_path) = cache_path;
    CefString(&settings.cache_path) = cache_path;
  } else {
    cache_path = laufey_common::MakePrivateTempDir("", "laufey_cef_");
    if (!cache_path.empty())
      CefString(&settings.root_cache_path) = cache_path;
  }
  // Where the profile records its password store (LaufeyApplyPasswordStore,
  // called from CefInitialize).
  g_root_cache_path = cache_path;

  // No remote debugging while DevTools are off (API 40, inspectable).
  const char* port_env = laufey_common::LaunchInspectable()
                             ? getenv("LAUFEY_REMOTE_DEBUGGING_PORT")
                             : nullptr;
  if (port_env) {
    int port = atoi(port_env);
    if (port > 0 && port < 65536) {
      settings.remote_debugging_port = port;
    }
  }

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    LaufeyReportCefInitializeFailure(cache_path);
    return 1;
  }
  LaufeyInstallSecondInstanceHooks();
  laufey_common::InstallTerminationSignalHandlers(LaufeyRequestQuit);

  CefRunMessageLoop();

  // A signal from here on takes its default action.
  laufey_common::RemoveTerminationSignalHandlers();

  // The loop is over: UI tasks still queued are answered "not run" and an
  // auth session in progress ends cancelled, so a runtime thread waiting on
  // either is released before Shutdown waits for it.
  laufey_common::UiLoopEnded();

  LaufeyClearSecondInstanceHooks();
  RuntimeLoader::GetInstance()->Shutdown();

  CefShutdown();

  // exit_app's code (API 46), else 0.
  return laufey_common::RequestedExitCode();
}
