// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "runtime_loader.h"
#include "laufey_window.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_launch_config.h"
#include "laufey_menu.h"
#include "laufey_notifications.h"
#include "laufey_system.h"
#include "laufey_single_instance.h"
#include "laufey_json.h"
#include "laufey_scheme_body_stream.h"
#include "laufey_scheme_cancel.h"
#include "laufey_scheme_registry.h"
#include "laufey_ui_tasks.h"
#include "init_script.h"
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <iostream>
#include <map>
#include <mutex>
#include <set>

// Runs `fn` synchronously on the GTK main thread (inline when already
// there). Through the UI task dispatcher (laufey_ui_tasks.h), so a call never
// outlives the loop: false when `fn` did not run because the loop had ended,
// and the caller answers with its defaults.
template <typename F>
static bool gtk_invoke_sync(F&& fn) {
  if (g_main_context_is_owner(g_main_context_default())) {
    fn();
    return true;
  }
  return laufey_common::RunOnUiThreadAndWait(fn);
}

namespace keyboard {

// GDK → W3C key/code lives in backend-common
// (laufey_common::GdkKeyvalToKey / GdkKeycodeToCode).
inline std::string GdkKeyvalToKey(guint keyval) {
  return laufey_common::GdkKeyvalToKey(keyval);
}
inline std::string GdkKeycodeToCode(guint16 hardware_keycode) {
  return laufey_common::GdkKeycodeToCode(hardware_keycode);
}

uint32_t GdkModifiersToLaufey(guint state) {
  uint32_t modifiers = 0;
  if (state & GDK_SHIFT_MASK)
    modifiers |= LAUFEY_MOD_SHIFT;
  if (state & GDK_CONTROL_MASK)
    modifiers |= LAUFEY_MOD_CONTROL;
  if (state & GDK_MOD1_MASK)
    modifiers |= LAUFEY_MOD_ALT;
  if (state & GDK_MOD4_MASK)
    modifiers |= LAUFEY_MOD_META;
  return modifiers;
}

}  // namespace keyboard

// GtkWidget → laufey_id mapping for event routing
static std::map<GtkWidget*, uint32_t> g_widget_to_laufey_id;
static std::mutex g_widget_mutex;

static uint32_t LaufeyIdForWidget(GtkWidget* widget) {
  if (!widget)
    return 0;
  // Walk up to find the toplevel window
  GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  auto it = g_widget_to_laufey_id.find(toplevel);
  return it != g_widget_to_laufey_id.end() ? it->second : 0;
}

static void RegisterWidget(GtkWidget* widget, uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_laufey_id[widget] = window_id;
}

static void UnregisterWidget(GtkWidget* widget) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_laufey_id.erase(widget);
}

// Per-window state
struct LinuxWindowState {
  uint32_t window_id;
  GtkWidget* window;
  GtkWidget* vbox;      // container for menu bar + webview
  GtkWidget* menu_bar;  // per-window menu bar (nullptr = none)
  // The menu bar's accelerators, bound on the window (nullptr = none).
  GtkAccelGroup* accel_group = nullptr;
  WebKitWebView* webview;
  WebKitUserContentManager* content_manager;
  // GTK has no getter for the input shape region, so remember what we set.
  bool click_passthrough = false;
};

// Track the click_count from press events for use in the corresponding release.
static int32_t g_last_click_count = 1;

static gboolean on_button_event(GtkWidget* widget, GdkEventButton* event,
                                gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  int state;
  int32_t click_count;

  switch (event->type) {
    case GDK_BUTTON_PRESS:
      state = LAUFEY_MOUSE_PRESSED;
      click_count = 1;
      break;
    case GDK_2BUTTON_PRESS:
      state = LAUFEY_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_3BUTTON_PRESS:
      state = LAUFEY_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_BUTTON_RELEASE:
      state = LAUFEY_MOUSE_RELEASED;
      click_count = g_last_click_count;
      break;
    default:
      return FALSE;
  }

  if (state == LAUFEY_MOUSE_PRESSED) {
    g_last_click_count = click_count;
  }

  int button;
  switch (event->button) {
    case 1:
      button = LAUFEY_MOUSE_BUTTON_LEFT;
      break;
    case 2:
      button = LAUFEY_MOUSE_BUTTON_MIDDLE;
      break;
    case 3:
      button = LAUFEY_MOUSE_BUTTON_RIGHT;
      break;
    case 8:
      button = LAUFEY_MOUSE_BUTTON_BACK;
      break;
    case 9:
      button = LAUFEY_MOUSE_BUTTON_FORWARD;
      break;
    default:
      button = static_cast<int>(event->button);
      break;
  }
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);

  RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
      wid, state, button, event->x, event->y, modifiers, click_count);

  return FALSE;
}

static gboolean on_motion_event(GtkWidget* widget, GdkEventMotion* event,
                                gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchMouseMoveEvent(wid, event->x, event->y,
                                                       modifiers);
  return FALSE;
}

static gboolean on_scroll_event(GtkWidget* widget, GdkEventScroll* event,
                                gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  double delta_x = 0, delta_y = 0;
  int32_t delta_mode = LAUFEY_WHEEL_DELTA_LINE;

  switch (event->direction) {
    case GDK_SCROLL_UP:
      delta_y = -1.0;
      break;
    case GDK_SCROLL_DOWN:
      delta_y = 1.0;
      break;
    case GDK_SCROLL_LEFT:
      delta_x = -1.0;
      break;
    case GDK_SCROLL_RIGHT:
      delta_x = 1.0;
      break;
    case GDK_SCROLL_SMOOTH:
      gdk_event_get_scroll_deltas((GdkEvent*)event, &delta_x, &delta_y);
      delta_mode = LAUFEY_WHEEL_DELTA_PIXEL;
      break;
  }

  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchWheelEvent(
      wid, delta_x, delta_y, event->x, event->y, modifiers, delta_mode);
  return FALSE;
}

static gboolean on_enter_notify_event(GtkWidget* widget,
                                      GdkEventCrossing* event,
                                      gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(
      wid, 1, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_leave_notify_event(GtkWidget* widget,
                                      GdkEventCrossing* event,
                                      gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(
      wid, 0, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_focus_in_event(GtkWidget* widget, GdkEventFocus* event,
                                  gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
  return FALSE;
}

static gboolean on_focus_out_event(GtkWidget* widget, GdkEventFocus* event,
                                   gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
  return FALSE;
}

// --- File drags over a web view (API >= 39) --------------------------------
//
// The handlers are connected to the WebKitWebView with g_signal_connect, so
// they run before WebKitWebViewBase's own (RUN_LAST class handlers) and only
// observe: each returns FALSE / doesn't stop the emission, and WebKit handles
// the drag for the page exactly as before. The paths come from the
// text/uri-list data WebKit itself requests when a drag enters (requesting it
// again here would hand WebKit a reply it didn't ask for), so ENTER waits for
// that data and carries the paths. GTK emits drag-leave right before
// drag-drop, so LEAVE is deferred to an idle callback that the drop cancels.
// WebKit's answer decides whether the drop happens at all: a page that
// refuses file drops (dropEffect "none") hides them from the runtime too.

namespace {

struct GtkFileDrag {
  bool active = false;   // a uri-list drag is over the view
  bool entered = false;  // ENTER was dispatched (the data had arrived)
  std::vector<std::string> paths;
  double x = 0, y = 0;
  guint leave_idle = 0;
};

// Keyed by window id; UI thread only.
std::map<uint32_t, GtkFileDrag> g_file_drags;

bool DragHasUris(GdkDragContext* ctx) {
  GdkAtom uri_list = gdk_atom_intern_static_string("text/uri-list");
  for (GList* l = gdk_drag_context_list_targets(ctx); l; l = l->next) {
    if (GDK_POINTER_TO_ATOM(l->data) == uri_list)
      return true;
  }
  return false;
}

void EndFileDrag(uint32_t wid, bool send_leave) {
  auto it = g_file_drags.find(wid);
  if (it == g_file_drags.end())
    return;
  GtkFileDrag d = std::move(it->second);
  g_file_drags.erase(it);
  if (d.leave_idle)
    g_source_remove(d.leave_idle);
  if (send_leave && d.entered)
    laufey_common::DispatchFileDrop(wid, LAUFEY_DRAG_LEAVE, d.x, d.y, {}, 0);
}

gboolean on_file_drag_motion(GtkWidget*, GdkDragContext* ctx, gint x, gint y,
                             guint, gpointer user_data) {
  if (!DragHasUris(ctx))
    return FALSE;
  uint32_t wid = GPOINTER_TO_UINT(user_data);
  GtkFileDrag& d = g_file_drags[wid];
  if (d.leave_idle) {
    g_source_remove(d.leave_idle);
    d.leave_idle = 0;
  }
  bool moved = !d.active || d.x != x || d.y != y;
  d.active = true;
  d.x = x;
  d.y = y;
  if (d.entered && moved)
    laufey_common::DispatchFileDrop(wid, LAUFEY_DRAG_OVER, x, y, d.paths,
                                    d.paths.size());
  return FALSE;
}

void on_file_drag_data_received(GtkWidget*, GdkDragContext* ctx, gint, gint,
                                GtkSelectionData* data, guint, guint,
                                gpointer user_data) {
  uint32_t wid = GPOINTER_TO_UINT(user_data);
  auto it = g_file_drags.find(wid);
  if (it == g_file_drags.end() || !it->second.active || !data)
    return;
  if (gtk_selection_data_get_target(data) !=
      gdk_atom_intern_static_string("text/uri-list"))
    return;
  GtkFileDrag& d = it->second;
  d.paths.clear();
  if (gchar** uris = gtk_selection_data_get_uris(data)) {
    for (gchar** u = uris; *u; u++) {
      if (gchar* path = g_filename_from_uri(*u, nullptr, nullptr)) {
        if (d.paths.size() < LAUFEY_MAX_DROP_PATHS &&
            g_utf8_validate(path, -1, nullptr))
          d.paths.emplace_back(path);
        g_free(path);
      }
    }
    g_strfreev(uris);
  }
  if (!d.entered && !d.paths.empty()) {
    d.entered = true;
    laufey_common::DispatchFileDrop(wid, LAUFEY_DRAG_ENTER, d.x, d.y, d.paths,
                                    d.paths.size());
  }
  (void)ctx;
}

void on_file_drag_leave(GtkWidget*, GdkDragContext*, guint,
                        gpointer user_data) {
  uint32_t wid = GPOINTER_TO_UINT(user_data);
  auto it = g_file_drags.find(wid);
  if (it == g_file_drags.end() || it->second.leave_idle)
    return;
  it->second.leave_idle = g_idle_add(
      [](gpointer data) -> gboolean {
        uint32_t id = GPOINTER_TO_UINT(data);
        auto found = g_file_drags.find(id);
        if (found != g_file_drags.end())
          found->second.leave_idle = 0;
        EndFileDrag(id, true);
        return G_SOURCE_REMOVE;
      },
      GUINT_TO_POINTER(wid));
}

gboolean on_file_drag_drop(GtkWidget*, GdkDragContext* ctx, gint x, gint y,
                           guint, gpointer user_data) {
  if (!DragHasUris(ctx))
    return FALSE;
  uint32_t wid = GPOINTER_TO_UINT(user_data);
  std::vector<std::string> paths;
  auto it = g_file_drags.find(wid);
  if (it != g_file_drags.end())
    paths = it->second.paths;
  EndFileDrag(wid, false);
  laufey_common::DispatchFileDrop(wid, LAUFEY_DRAG_DROP, x, y, paths,
                                  paths.size());
  return FALSE;
}

}  // namespace

static gboolean on_configure_event(GtkWidget* widget, GdkEventConfigure* event,
                                   gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchResizeEvent(wid, event->width,
                                                    event->height);
  RuntimeLoader::GetInstance()->DispatchMoveEvent(wid, event->x, event->y);
  // Normal-bounds tracker (API 38), in the get_window_position /
  // get_window_size convention.
  laufey_common::Bounds b;
  gtk_window_get_position(GTK_WINDOW(widget), &b.x, &b.y);
  gtk_window_get_size(GTK_WINDOW(widget), &b.width, &b.height);
  laufey_common::NoteWindowGeometry(
      wid, b, laufey_common::LastReportedWindowState(wid) == 0,
      laufey_common::MonotonicMs());
  return FALSE;
}

// GDK window state bits to LAUFEY_WINDOW_STATE_*.
static uint32_t LaufeyStateFromGdk(GdkWindowState s) {
  uint32_t state = 0;
  if (s & GDK_WINDOW_STATE_FULLSCREEN)
    state |= LAUFEY_WINDOW_STATE_FULLSCREEN;
  else if (s & GDK_WINDOW_STATE_MAXIMIZED)
    state |= LAUFEY_WINDOW_STATE_MAXIMIZED;
  if (s & GDK_WINDOW_STATE_ICONIFIED)
    state |= LAUFEY_WINDOW_STATE_MINIMIZED;
  return state;
}

static gboolean on_window_state_event(GtkWidget* widget,
                                      GdkEventWindowState* event,
                                      gpointer /*user_data*/) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t state = LaufeyStateFromGdk(event->new_window_state);
  if (state != 0 && laufey_common::LastReportedWindowState(wid) == 0)
    laufey_common::NoteWindowLeftNormal(wid, laufey_common::MonotonicMs());
  laufey_common::ReportWindowState(wid, state);
  return FALSE;
}

// Display ids: GDK has no stable monitor id, so hash what identifies the
// panel (manufacturer, model) plus its index among identical ones.
static int64_t LaufeyMonitorId(GdkDisplay* display, GdkMonitor* monitor) {
  const char* make = gdk_monitor_get_manufacturer(monitor);
  const char* model = gdk_monitor_get_model(monitor);
  std::string key = std::string(make ? make : "") + "/" + (model ? model : "");
  int same = 0;
  int n = gdk_display_get_n_monitors(display);
  for (int i = 0; i < n; ++i) {
    GdkMonitor* other = gdk_display_get_monitor(display, i);
    if (other == monitor)
      break;
    const char* omake = gdk_monitor_get_manufacturer(other);
    const char* omodel = gdk_monitor_get_model(other);
    if (std::string(omake ? omake : "") + "/" + (omodel ? omodel : "") == key)
      ++same;
  }
  key += "#" + std::to_string(same);
  return laufey_common::HashDisplayName(key.data(), key.size());
}

static bool LaufeyIsWaylandDisplay(GdkDisplay* display) {
  return display &&
         g_strcmp0(G_OBJECT_TYPE_NAME(display), "GdkWaylandDisplay") == 0;
}

static void on_display_monitors_changed() {
  laufey_common::NotifyDisplayChanged();
}

static void LaufeyWatchMonitor(GdkMonitor* monitor) {
  g_signal_connect(monitor, "notify::workarea",
                   G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {
                     on_display_monitors_changed();
                   }),
                   nullptr);
  g_signal_connect(monitor, "notify::scale-factor",
                   G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {
                     on_display_monitors_changed();
                   }),
                   nullptr);
  g_signal_connect(monitor, "notify::geometry",
                   G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {
                     on_display_monitors_changed();
                   }),
                   nullptr);
}

// The web context every webview (and every URI scheme registration) uses.
// With a per-app data dir (LAUFEY_DATA_DIR / LAUFEY_APP_ID) this is an owned
// context whose website data (localStorage, IndexedDB, caches) lives under
// <dir>/WebKitGTK/{data,cache} and whose cookies persist to
// <dir>/WebKitGTK/data/cookies.sqlite. Otherwise it is the default context, as
// before (data under the prgname in the XDG dirs, cookies not persisted). GTK
// main thread only.
static WebKitWebContext* LaufeyWebContext() {
  static WebKitWebContext* ctx = [] {
    std::string root = laufey_common::AppDataSubdir("WebKitGTK");
    if (root.empty()) {
      return webkit_web_context_get_default();
    }
    std::string data_dir = laufey_common::JoinPath(root, "data");
    std::string cache_dir = laufey_common::JoinPath(root, "cache");
    if (!laufey_common::EnsureDirectory(data_dir) ||
        !laufey_common::EnsureDirectory(cache_dir)) {
      std::cerr << "laufey: could not create web data directory \"" << root
                << "\"; web data will not be persisted per app" << std::endl;
      return webkit_web_context_get_default();
    }
    WebKitWebsiteDataManager* manager = webkit_website_data_manager_new(
        "base-data-directory", data_dir.c_str(), "base-cache-directory",
        cache_dir.c_str(), nullptr);
    WebKitWebContext* context =
        webkit_web_context_new_with_website_data_manager(manager);
    std::string cookies = laufey_common::JoinPath(data_dir, "cookies.sqlite");
    webkit_cookie_manager_set_persistent_storage(
        webkit_website_data_manager_get_cookie_manager(manager),
        cookies.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    g_object_unref(manager);  // the context keeps its own reference
    return context;           // owned for the life of the process
  }();
  return ctx;
}

// Fired as a navigation progresses through its load states. We only care about
// WEBKIT_LOAD_FINISHED, which signals the document and subresources have loaded
// (and the web process has content to composite). The window_id is passed
// directly as user_data because this signal is on the WebKitWebView, not the
// toplevel registered with LaufeyIdForWidget.
static void on_load_changed(WebKitWebView* /*webview*/,
                            WebKitLoadEvent load_event, gpointer user_data) {
  if (load_event != WEBKIT_LOAD_FINISHED) {
    return;
  }
  uint32_t wid = static_cast<uint32_t>(GPOINTER_TO_UINT(user_data));
  if (wid == 0) {
    return;
  }
  RuntimeLoader::GetInstance()->DispatchPageLoadEvent(wid);
}

// Fired when the page requests a new webview (`target="_blank"` or
// `window.open()`). These never reach the Navigation API interceptor, so route
// http(s) destinations to the OS browser and create no new webview.
static WebKitWebView* on_create(WebKitWebView* /*webview*/,
                                WebKitNavigationAction* navigation_action,
                                gpointer /*user_data*/) {
  WebKitURIRequest* req =
      webkit_navigation_action_get_request(navigation_action);
  const char* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
  if (uri &&
      (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
    g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
  }
  return nullptr;
}

static gboolean on_key_event(GtkWidget* widget, GdkEventKey* event,
                             gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  int state =
      (event->type == GDK_KEY_PRESS) ? LAUFEY_KEY_PRESSED : LAUFEY_KEY_RELEASED;
  std::string key = keyboard::GdkKeyvalToKey(event->keyval);
  std::string code = keyboard::GdkKeycodeToCode(event->hardware_keycode);
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);

  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return FALSE;
}

// ============================================================================
// WebKitGTK Backend
// ============================================================================

class WebKitGTKBackend : public LaufeyBackend {
 public:
  WebKitGTKBackend();
  ~WebKitGTKBackend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void OpenExternalURL(const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 laufey_js_result_fn callback, void* callback_data) override;
  void Quit() override;
  void SetWindowSize(uint32_t window_id, int width, int height) override;
  void GetWindowSize(uint32_t window_id, int* width, int* height) override;
  void GetWindowOuterSize(uint32_t window_id, int* width, int* height) override;
  double GetWindowScaleFactor(uint32_t window_id) override;
  void SetWindowPosition(uint32_t window_id, int x, int y) override;
  // Drag and drop, file dialogs, rich clipboard (API >= 39).
  void SetFileDropHandler(laufey_file_drop_fn handler,
                          void* user_data) override {
    laufey_common::SetFileDropHandler(handler, user_data);
  }
  bool TestTriggerFileDrop(uint32_t window_id, int phase, double x, double y,
                           const char* const* paths, size_t count) override {
    // The OS path dispatches on the GTK thread; so does the hook.
    bool delivered = false;
    gtk_invoke_sync([&] {
      delivered = laufey_common::TestTriggerFileDrop(window_id, phase, x, y,
                                                     paths, count);
    });
    return delivered;
  }
  void StartFileDrag(uint32_t window_id, const char* const* paths, size_t count,
                     const uint8_t* icon_png, size_t icon_len,
                     laufey_drag_result_fn callback, void* user_data) override;
  uint32_t ShowFileDialog(uint32_t window_id,
                          const laufey_file_dialog_options_t* options,
                          laufey_file_dialog_result_fn callback,
                          void* user_data) override;
  bool CancelFileDialog(uint32_t dialog_id) override {
    return laufey_common::CancelFileDialogLinux(dialog_id);
  }
  bool TestFileDialogRespond(int action, const char* path) override {
    return laufey_common::TestFileDialogRespondLinux(action, path);
  }
  uint32_t ClipboardCapabilities() override {
    return laufey_common::ClipboardCapabilitiesLinux();
  }
  char* ReadClipboardHtml() override {
    return laufey_common::ClipboardReadHtmlLinux();
  }
  bool WriteClipboardHtml(const std::string& html,
                          const char* text_or_null) override {
    return laufey_common::ClipboardWriteHtmlLinux(html, text_or_null);
  }
  uint8_t* ReadClipboardImage(size_t* len_out) override {
    return laufey_common::ClipboardReadImageLinux(len_out);
  }
  bool WriteClipboardImage(const uint8_t* png, size_t len) override {
    return laufey_common::ClipboardWriteImageLinux(png, len);
  }
  char* ReadClipboardFormats() override {
    return laufey_common::ClipboardReadFormatsLinux();
  }
  void SetClipboardChangeHandler(laufey_clipboard_change_fn handler,
                                 void* user_data) override {
    laufey_common::SetClipboardChangeHandler(handler, user_data);
  }

  // Global shortcuts, launch at login, DevTools (API >= 40). The X11 /
  // portal shortcut platform is installed on first use.
  static void EnsureShortcuts() {
    static std::once_flag once;
    std::call_once(once, [] {
      laufey_common::InstallShortcutPlatform(
          laufey_common::CreateShortcutPlatformLinux());
    });
  }
  uint32_t SystemCapabilities() override {
    EnsureShortcuts();
    uint32_t caps =
        laufey_common::ShortcutCapabilities() | LAUFEY_SYSTEM_CAP_DEVTOOLS;
    if (laufey_common::GetLaunchAtLogin() != LAUFEY_LOGIN_ITEM_NOT_SUPPORTED)
      caps |= LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN;
    return caps;
  }
  void SetShortcutHandler(laufey_shortcut_fn handler,
                          void* user_data) override {
    laufey_common::SetShortcutHandler(handler, user_data);
  }
  void RegisterShortcut(const char* accelerator,
                        laufey_shortcut_result_fn callback,
                        void* user_data) override {
    EnsureShortcuts();
    laufey_common::RegisterShortcut(accelerator, callback, user_data);
  }
  bool UnregisterShortcut(const char* accelerator) override {
    return laufey_common::UnregisterShortcut(accelerator);
  }
  void UnregisterAllShortcuts() override {
    laufey_common::UnregisterAllShortcuts();
  }
  char* ListShortcuts() override {
    return laufey_common::ListShortcuts();
  }
  char* CanonicalizeAccelerator(const char* accelerator) override {
    return laufey_common::CanonicalizeAccelerator(accelerator);
  }
  bool TestTriggerShortcut(const char* accelerator) override {
    return laufey_common::TestTriggerShortcut(accelerator);
  }
  int GetLaunchAtLogin() override {
    return laufey_common::GetLaunchAtLogin();
  }
  int SetLaunchAtLogin(bool enabled, std::string* error) override {
    return laufey_common::SetLaunchAtLogin(enabled, error);
  }
  void CloseDevTools(uint32_t window_id) override;
  bool IsDevToolsOpen(uint32_t window_id) override;
  bool IsDevToolsEnabled(uint32_t window_id) override;

  // Window state, constraints and screens (API >= 38). No title bar styles
  // or backdrops: GTK has no API for either (see
  // docs/window-management.md), so those keep the base class's "false".
  uint32_t WindowCapabilities() override;
  void SetWindowState(uint32_t window_id, int action) override;
  uint32_t GetWindowState(uint32_t window_id) override;
  void SetWindowStateHandler(laufey_window_state_fn handler,
                             void* user_data) override {
    laufey_common::SetWindowStateHandler(handler, user_data);
  }
  void SetWindowSizeConstraints(uint32_t window_id, int min_width,
                                int min_height, int max_width,
                                int max_height) override;
  void GetWindowSizeConstraints(uint32_t window_id, int* min_width,
                                int* min_height, int* max_width,
                                int* max_height) override;
  size_t GetScreens(laufey_screen_t* out, size_t capacity) override;
  int64_t GetWindowScreen(uint32_t window_id) override;
  void SetDisplayChangedHandler(laufey_display_changed_fn handler,
                                void* user_data) override;
  bool GetWindowNormalBounds(uint32_t window_id, int* x, int* y, int* width,
                             int* height) override;
  void SetQuitOnLastWindowClosed(bool quit) override {
    laufey_common::SetQuitOnLastWindowClosed(quit);
  }
  void GetWindowPosition(uint32_t window_id, int* x, int* y) override;
  void GetWindowInnerPosition(uint32_t window_id, int* x, int* y) override;
  void SetResizable(uint32_t window_id, bool resizable) override;
  bool IsResizable(uint32_t window_id) override;
  void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) override;
  bool IsAlwaysOnTop(uint32_t window_id) override;
  void SetWindowOpacity(uint32_t window_id, double opacity) override;
  double GetWindowOpacity(uint32_t window_id) override;
  void SetClickPassthrough(uint32_t window_id, bool enabled) override;
  bool IsClickPassthrough(uint32_t window_id) override;
  bool IsVisible(uint32_t window_id) override;
  void Show(uint32_t window_id) override;
  void Hide(uint32_t window_id) override;
  void Focus(uint32_t window_id) override;
  bool PostUiTask(void (*task)(void*), void* data) override;
  void SetSecondInstanceHandler(laufey_second_instance_fn handler,
                                void* user_data) override {
    laufey_common::SetSecondInstanceHandler(handler, user_data);
  }

  void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                        laufey::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       laufey::ValuePtr result,
                       laufey::ValuePtr error) override;

  void Run() override;

  void RegisterSchemeHandler(const std::string& scheme) override;

  void SetApplicationMenu(uint32_t window_id, laufey_value_t* menu_template,
                          const laufey_backend_api_t* api,
                          laufey_menu_click_fn on_click,
                          void* on_click_data) override;

  void ShowContextMenu(uint32_t window_id, int x, int y,
                       laufey_value_t* menu_template,
                       const laufey_backend_api_t* api,
                       laufey_menu_click_fn on_click,
                       void* on_click_data) override;

  void OpenDevTools(uint32_t window_id) override;

  void PrintToPdf(uint32_t window_id, laufey_pdf_result_fn callback,
                  void* callback_data) override;

  int ShowDialog(uint32_t window_id, int dialog_type, const std::string& title,
                 const std::string& message, const std::string& default_value,
                 char** out_input_value) override;

  char* ReadClipboardText() override {
    return laufey_common::ClipboardReadTextLinux();
  }
  void WriteClipboardText(const std::string& text) override {
    laufey_common::ClipboardWriteTextLinux(text);
  }

  void BounceDock(int type) override;
  void SetDockBadge(const char* badge_or_null) override;

  uint32_t CreateTrayIcon() override;
  void DestroyTrayIcon(uint32_t tray_id) override;
  void SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                   size_t len) override;
  void SetTrayTooltip(uint32_t tray_id, const char* tooltip_or_null) override;
  void SetTrayMenu(uint32_t tray_id, laufey_value_t* menu_template,
                   const laufey_backend_api_t* api,
                   laufey_menu_click_fn on_click, void* on_click_data) override;
  void SetTrayClickHandler(uint32_t tray_id, laufey_tray_click_fn handler,
                           void* user_data) override;

  uint32_t ShowNotification(laufey_value_t* options,
                            const laufey_backend_api_t* api,
                            laufey_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  // Granted when a notification server is on the session bus; no prompt.
  void QueryPermission(int kind, laufey_permission_callback_fn cb,
                       void* user_data) override {
    laufey_common::QueryNotificationPermission(kind, cb, user_data);
  }
  void RequestPermission(int kind, laufey_permission_callback_fn cb,
                         void* user_data) override {
    laufey_common::RequestNotificationPermission(kind, cb, user_data);
  }

  // Notifications and menus (API >= 41): backend-common.
  uint32_t NotificationCapabilities() override {
    return laufey_common::NotificationCapabilities();
  }
  void SetNotificationResponseHandler(laufey_notification_response_fn handler,
                                      void* user_data) override {
    laufey_common::SetNotificationResponseHandler(handler, user_data);
  }
  void ListScheduledNotifications(laufey_notification_list_fn cb,
                                  void* user_data) override {
    laufey_common::ListScheduledNotifications(cb, user_data);
  }
  void CancelNotification(const char* tag) override {
    laufey_common::CancelNotification(tag);
  }
  bool TestNotificationRespond(const char* tag,
                               const char* action_id) override {
    return laufey_common::TestNotificationRespond(tag, action_id);
  }
  uint32_t MenuCapabilities() override {
    return LAUFEY_MENU_CAP_APP_MENU | LAUFEY_MENU_CAP_ACCELERATORS |
           LAUFEY_MENU_CAP_CONTEXT_MENU | LAUFEY_MENU_CAP_CONTEXT_CLOSED |
           LAUFEY_MENU_CAP_ICONS | LAUFEY_MENU_CAP_TOOLTIPS;
  }
  void ShowContextMenuEx(uint32_t window_id, int x, int y,
                         laufey_value_t* menu_template,
                         const laufey_backend_api_t* api,
                         laufey_menu_click_fn on_click, void* on_click_data,
                         laufey_menu_closed_fn on_closed,
                         void* on_closed_data) override;
  bool TestDismissContextMenu() override {
    return laufey_common::DismissOpenContextMenu();
  }
  bool TestTriggerMenuAccelerator(uint32_t window_id,
                                  const char* accelerator) override;

  void HandleJsMessage(uint32_t window_id, const char* json);

  // Drops the state of a window GTK destroyed on its own (the user closed it:
  // delete-event fell through to GTK's default handler). CloseWindow() drops
  // the state itself and never reaches this. GTK thread.
  void ForgetDestroyedWindow(uint32_t window_id);

 private:
  LinuxWindowState* GetWindow(uint32_t window_id);

  std::map<uint32_t, LinuxWindowState> windows_;
  std::mutex windows_mutex_;

  // Set once the first web view exists; a RegisterSchemeHandler after that
  // point breaks the (cross-backend) ordering contract and warns.
  std::atomic<bool> any_web_view_created_{false};
};

// Static instance pointer for GTK callbacks
static WebKitGTKBackend* g_gtk_backend = nullptr;

// GtkWidget → window_id mapping for script message routing
static std::map<WebKitUserContentManager*, uint32_t>
    g_content_manager_to_laufey_id;

static void on_script_message(WebKitUserContentManager* manager,
                              WebKitJavascriptResult* js_result,
                              gpointer user_data) {
  auto it = g_content_manager_to_laufey_id.find(manager);
  uint32_t wid = (it != g_content_manager_to_laufey_id.end()) ? it->second : 0;

  JSCValue* value = webkit_javascript_result_get_js_value(js_result);
  if (jsc_value_is_string(value)) {
    gchar* str = jsc_value_to_string(value);
    if (g_gtk_backend) {
      g_gtk_backend->HandleJsMessage(wid, str);
    }
    g_free(str);
  }
}

// "destroy" fires only after the widget is already being torn down -- too
// late to veto anything, so it's just final cleanup. The close-requested
// dispatch (and any veto) happens earlier, from "delete-event" below.
//
// A window CloseWindow() closes is unregistered (and its state dropped) before
// gtk_widget_destroy, so `wid` is 0 here for it -- which also keeps this
// handler from taking windows_mutex_, which CloseWindow holds while it
// destroys. A window the user closed still has its id: its state goes here,
// or every later call naming it would reach the destroyed widgets.
static void on_window_destroy(GtkWidget* widget, gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid > 0) {
    // Lock order windows_mutex_ -> g_widget_mutex: the two are taken one
    // after the other here, never nested.
    if (g_gtk_backend)
      g_gtk_backend->ForgetDestroyedWindow(wid);
    UnregisterWidget(widget);
    laufey_common::ForgetWindow(wid);
  }
  // If no more windows, quit -- unless the app keeps running without one
  // (set_quit_on_last_window_closed(false)); quit() ends it anyway.
  {
    std::lock_guard<std::mutex> lock(g_widget_mutex);
    if (g_widget_to_laufey_id.empty() &&
        laufey_common::ShouldEndLoopAfterLastWindow()) {
      gtk_main_quit();
    }
  }
}

// "delete-event" fires when the user requests a close (e.g. clicks the
// titlebar close button) and, unlike "destroy", can veto it: returning TRUE
// blocks GTK's default handler from calling gtk_widget_destroy. A registered
// handler always defers (returns TRUE here); no handler falls through to
// the unchanged default flow (proceeds to destroy, which fires
// on_window_destroy above).
static gboolean on_window_delete_event(GtkWidget* widget, GdkEvent* event,
                                       gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0) {
    return FALSE;
  }
  bool proceed = RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
  return proceed ? FALSE : TRUE;
}

WebKitGTKBackend::WebKitGTKBackend() {
  g_gtk_backend = this;
}

WebKitGTKBackend::~WebKitGTKBackend() {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    webkit_user_content_manager_unregister_script_message_handler(
        state.content_manager, "laufey");
    g_content_manager_to_laufey_id.erase(state.content_manager);
    UnregisterWidget(state.window);
  }
  windows_.clear();
  g_gtk_backend = nullptr;
}

void WebKitGTKBackend::ForgetDestroyedWindow(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  webkit_user_content_manager_unregister_script_message_handler(
      state->content_manager, "laufey");
  g_content_manager_to_laufey_id.erase(state->content_manager);
  windows_.erase(window_id);
}

LinuxWindowState* WebKitGTKBackend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

static gboolean on_script_dialog(WebKitWebView* webview,
                                 WebKitScriptDialog* dialog,
                                 gpointer user_data) {
  WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(dialog);
  const gchar* message = webkit_script_dialog_get_message(dialog);

  GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(webview));
  GtkWindow* parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr;

  if (type == WEBKIT_SCRIPT_DIALOG_ALERT) {
    GtkWidget* dlg =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    webkit_script_dialog_confirm_set_confirmed(dialog, TRUE);
    return TRUE;
  }

  if (type == WEBKIT_SCRIPT_DIALOG_CONFIRM) {
    GtkWidget* dlg =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", message);
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    webkit_script_dialog_confirm_set_confirmed(dialog,
                                               result == GTK_RESPONSE_OK);
    return TRUE;
  }

  if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
    GtkWidget* dlg =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", message);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* entry = gtk_entry_new();
    const gchar* default_text =
        webkit_script_dialog_prompt_get_default_text(dialog);
    if (default_text) {
      gtk_entry_set_text(GTK_ENTRY(entry), default_text);
    }
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show(entry);
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    if (result == GTK_RESPONSE_OK) {
      webkit_script_dialog_prompt_set_text(
          dialog, gtk_entry_get_text(GTK_ENTRY(entry)));
    }
    webkit_script_dialog_confirm_set_confirmed(dialog,
                                               result == GTK_RESPONSE_OK);
    gtk_widget_destroy(dlg);
    return TRUE;
  }

  return FALSE;
}

// --- System dark-mode (prefers-color-scheme) sync ---------------------------
// WebKitGTK derives the CSS `prefers-color-scheme` media feature from the GTK
// `gtk-application-prefer-dark-theme` setting. Plain GTK apps default to light,
// so pages never observe the user's system preference (issue #21). Mirror the
// xdg-desktop-portal `org.freedesktop.appearance` color-scheme into that GTK
// setting and keep it live via the portal's SettingChanged signal.
static void apply_color_scheme(guint32 scheme) {
  // 0 = no preference, 1 = prefer dark, 2 = prefer light.
  GtkSettings* settings = gtk_settings_get_default();
  if (settings) {
    g_object_set(settings, "gtk-application-prefer-dark-theme",
                 scheme == 1 ? TRUE : FALSE, nullptr);
  }
}

static void on_portal_setting_changed(GDBusConnection* /*conn*/,
                                      const gchar* /*sender*/,
                                      const gchar* /*object_path*/,
                                      const gchar* /*interface*/,
                                      const gchar* /*signal*/,
                                      GVariant* parameters, gpointer /*data*/) {
  const gchar* ns = nullptr;
  const gchar* key = nullptr;
  GVariant* value = nullptr;
  g_variant_get(parameters, "(&s&sv)", &ns, &key, &value);
  if (ns && key && g_strcmp0(ns, "org.freedesktop.appearance") == 0 &&
      g_strcmp0(key, "color-scheme") == 0 && value &&
      g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
    apply_color_scheme(g_variant_get_uint32(value));
  }
  if (value) {
    g_variant_unref(value);
  }
}

static void init_dark_theme_sync() {
  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) {
    if (error) {
      g_error_free(error);
    }
    return;
  }

  // Initial read of the current color-scheme. Returns (v); the value may be
  // wrapped one or more times in a variant depending on the portal backend.
  GVariant* reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Settings", "Read",
      g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (reply) {
    GVariant* cur = nullptr;
    g_variant_get(reply, "(v)", &cur);
    while (cur && g_variant_is_of_type(cur, G_VARIANT_TYPE_VARIANT)) {
      GVariant* next = g_variant_get_variant(cur);
      g_variant_unref(cur);
      cur = next;
    }
    if (cur && g_variant_is_of_type(cur, G_VARIANT_TYPE_UINT32)) {
      apply_color_scheme(g_variant_get_uint32(cur));
    }
    if (cur) {
      g_variant_unref(cur);
    }
    g_variant_unref(reply);
  } else if (error) {
    // Portal unavailable (e.g. no xdg-desktop-portal); leave GTK default.
    g_error_free(error);
  }

  // Live updates. `bus` is intentionally kept alive for the process lifetime so
  // the subscription stays active.
  g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
      "SettingChanged", "/org/freedesktop/portal/desktop", nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, on_portal_setting_changed, nullptr, nullptr);
}

void WebKitGTKBackend::CreateWindow(uint32_t window_id, int width, int height) {
  CreateWindowEx(window_id, width, height, 0);
}

void WebKitGTKBackend::CreateWindowEx(uint32_t window_id, int width, int height,
                                      uint32_t flags) {
  gtk_invoke_sync([&] {
    static std::once_flag dark_theme_once;
    std::call_once(dark_theme_once, init_dark_theme_sync);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (flags & LAUFEY_WINDOW_FLAG_FRAMELESS) {
      gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    }
    bool transparent = (flags & LAUFEY_WINDOW_FLAG_TRANSPARENT) != 0;
    if (transparent) {
      // Give the toplevel an RGBA visual (must be set before realization) so
      // the compositor blends the window's alpha. Requires a compositing WM.
      GdkScreen* screen = gtk_widget_get_screen(window);
      GdkVisual* rgba = gdk_screen_get_rgba_visual(screen);
      if (rgba) {
        gtk_widget_set_visual(window, rgba);
      }
      gtk_widget_set_app_paintable(window, TRUE);
    }
    if (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) {
      // Treat as a utility/panel window: out of taskbar & pager, and don't
      // grab focus when shown (the GTK equivalent of a non-activating panel).
      gtk_window_set_type_hint(GTK_WINDOW(window),
                               GDK_WINDOW_TYPE_HINT_UTILITY);
      gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
      gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
      gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
    }
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete_event),
                     nullptr);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_event),
                     nullptr);
    g_signal_connect(window, "key-release-event", G_CALLBACK(on_key_event),
                     nullptr);
    g_signal_connect(window, "button-press-event", G_CALLBACK(on_button_event),
                     nullptr);
    g_signal_connect(window, "button-release-event",
                     G_CALLBACK(on_button_event), nullptr);
    gtk_widget_add_events(window, GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                                      GDK_SMOOTH_SCROLL_MASK |
                                      GDK_ENTER_NOTIFY_MASK |
                                      GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_motion_event),
                     nullptr);
    g_signal_connect(window, "scroll-event", G_CALLBACK(on_scroll_event),
                     nullptr);
    g_signal_connect(window, "enter-notify-event",
                     G_CALLBACK(on_enter_notify_event), nullptr);
    g_signal_connect(window, "leave-notify-event",
                     G_CALLBACK(on_leave_notify_event), nullptr);
    g_signal_connect(window, "focus-in-event", G_CALLBACK(on_focus_in_event),
                     nullptr);
    g_signal_connect(window, "focus-out-event", G_CALLBACK(on_focus_out_event),
                     nullptr);
    g_signal_connect(window, "window-state-event",
                     G_CALLBACK(on_window_state_event), nullptr);
    g_signal_connect(window, "configure-event", G_CALLBACK(on_configure_event),
                     nullptr);

    RegisterWidget(window, window_id);

    WebKitUserContentManager* content_manager =
        webkit_user_content_manager_new();
    g_signal_connect(content_manager, "script-message-received::laufey",
                     G_CALLBACK(on_script_message), nullptr);
    webkit_user_content_manager_register_script_message_handler(content_manager,
                                                                "laufey");
    g_content_manager_to_laufey_id[content_manager] = window_id;

    WebKitWebView* webview = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", LaufeyWebContext(),
                     "user-content-manager", content_manager, nullptr));
    any_web_view_created_.store(true);

    g_signal_connect(webview, "script-dialog", G_CALLBACK(on_script_dialog),
                     nullptr);
    g_signal_connect(webview, "load-changed", G_CALLBACK(on_load_changed),
                     GUINT_TO_POINTER(window_id));
    g_signal_connect(webview, "create", G_CALLBACK(on_create), nullptr);
    g_signal_connect(webview, "drag-motion", G_CALLBACK(on_file_drag_motion),
                     GUINT_TO_POINTER(window_id));
    g_signal_connect(webview, "drag-data-received",
                     G_CALLBACK(on_file_drag_data_received),
                     GUINT_TO_POINTER(window_id));
    g_signal_connect(webview, "drag-leave", G_CALLBACK(on_file_drag_leave),
                     GUINT_TO_POINTER(window_id));
    g_signal_connect(webview, "drag-drop", G_CALLBACK(on_file_drag_drop),
                     GUINT_TO_POINTER(window_id));

    WebKitSettings* wk_settings = webkit_web_view_get_settings(webview);
    // DevTools (API 40): the inspector, its context-menu item and its
    // shortcut exist only with developer extras on, which follows
    // LAUFEY_INSPECTABLE / "inspectable" (default on).
    webkit_settings_set_enable_developer_extras(
        wk_settings, laufey_common::LaunchInspectable() ? TRUE : FALSE);

    if (transparent) {
      // Let the page's own alpha show through the webview (any region the
      // document leaves transparent composites against the desktop).
      GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
      webkit_web_view_set_background_color(webview, &clear);
    }

    std::string initScript = BuildInitScript(
        RuntimeLoader::GetInstance()->GetJsNamespace(),
        "window.webkit.messageHandlers.laufey.postMessage(JSON.stringify({\n"
        "            callId: callId,\n"
        "            method: path.join('.'),\n"
        "            args: processedArgs\n"
        "          }));");
    // Inject the bridge into the top frame only. Cross-origin/sub frames must
    // not inherit it; otherwise embedded content could invoke bindings running
    // with the host process's permissions. Matches the macOS/iOS
    // forMainFrameOnly:YES behavior.
    WebKitUserScript* script = webkit_user_script_new(
        initScript.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(content_manager, script);
    webkit_user_script_unref(script);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(webview), TRUE, TRUE, 0);

    LinuxWindowState state;
    state.window_id = window_id;
    state.window = window;
    state.vbox = vbox;
    state.menu_bar = nullptr;
    state.webview = webview;
    state.content_manager = content_manager;

    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      windows_[window_id] = state;
    }

    // A hidden window is created but not mapped; the embedder reveals it later
    // (typically from a page-load handler) so the empty initial frame is never
    // shown. The load state machine still advances while unmapped, so the
    // page-load event fires regardless.
    if (!(flags & LAUFEY_WINDOW_FLAG_HIDDEN)) {
      gtk_widget_show_all(window);
    }
  });
}

void WebKitGTKBackend::CloseWindow(uint32_t window_id) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_user_content_manager_unregister_script_message_handler(
          state->content_manager, "laufey");
      g_content_manager_to_laufey_id.erase(state->content_manager);
      UnregisterWidget(state->window);
      laufey_common::ForgetWindow(window_id);
      gtk_widget_destroy(state->window);
      windows_.erase(window_id);
    }
  });
}

void WebKitGTKBackend::Navigate(uint32_t window_id, const std::string& url) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_web_view_load_uri(state->webview, url.c_str());
    }
  });
}

void WebKitGTKBackend::OpenExternalURL(const std::string& url) {
  gtk_invoke_sync([&] {
    g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
  });
}

void WebKitGTKBackend::SetTitle(uint32_t window_id, const std::string& title) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_set_title(GTK_WINDOW(state->window), title.c_str());
    }
  });
}

struct ExecuteJsCallbackData {
  laufey_js_result_fn callback;
  void* user_data;
};

static void on_execute_js_finished(GObject* source, GAsyncResult* result,
                                   gpointer user_data) {
  auto* cb_data = static_cast<ExecuteJsCallbackData*>(user_data);

  GError* error = nullptr;
  WebKitJavascriptResult* js_result = webkit_web_view_run_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);

  if (error) {
    auto errVal = laufey::Value::String(error->message);
    laufey_value errLaufey(errVal);
    cb_data->callback(nullptr, &errLaufey, cb_data->user_data);
    g_error_free(error);
  } else if (js_result) {
    JSCValue* value = webkit_javascript_result_get_js_value(js_result);
    if (jsc_value_is_null(value) || jsc_value_is_undefined(value)) {
      cb_data->callback(nullptr, nullptr, cb_data->user_data);
    } else if (jsc_value_is_boolean(value)) {
      auto val = laufey::Value::Bool(jsc_value_to_boolean(value));
      laufey_value laufey(val);
      cb_data->callback(&laufey, nullptr, cb_data->user_data);
    } else if (jsc_value_is_number(value)) {
      double d = jsc_value_to_double(value);
      if (d == (int)d && d >= INT_MIN && d <= INT_MAX) {
        auto val = laufey::Value::Int((int)d);
        laufey_value laufey(val);
        cb_data->callback(&laufey, nullptr, cb_data->user_data);
      } else {
        auto val = laufey::Value::Double(d);
        laufey_value laufey(val);
        cb_data->callback(&laufey, nullptr, cb_data->user_data);
      }
    } else if (jsc_value_is_string(value)) {
      gchar* str = jsc_value_to_string(value);
      auto val = laufey::Value::String(str);
      laufey_value laufey(val);
      cb_data->callback(&laufey, nullptr, cb_data->user_data);
      g_free(str);
    } else {
      // For objects/arrays, serialize to JSON and parse
      gchar* json = jsc_value_to_json(value, 0);
      if (json) {
        auto val = json::ParseJson(json);
        laufey_value laufey(val);
        cb_data->callback(&laufey, nullptr, cb_data->user_data);
        g_free(json);
      } else {
        cb_data->callback(nullptr, nullptr, cb_data->user_data);
      }
    }
    webkit_javascript_result_unref(js_result);
  } else {
    cb_data->callback(nullptr, nullptr, cb_data->user_data);
  }

  delete cb_data;
}

void WebKitGTKBackend::ExecuteJs(uint32_t window_id, const std::string& script,
                                 laufey_js_result_fn callback,
                                 void* callback_data) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state) {
      if (callback)
        callback(nullptr, nullptr, callback_data);
      return;
    }
    if (!callback) {
      webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                     nullptr, nullptr);
    } else {
      auto* cb_data = new ExecuteJsCallbackData{callback, callback_data};
      webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                     on_execute_js_finished, cb_data);
    }
  });
}

void WebKitGTKBackend::Quit() {
  laufey_common::MarkQuitting();
  g_idle_add(
      [](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
      },
      nullptr);
}

void WebKitGTKBackend::SetWindowSize(uint32_t window_id, int width,
                                     int height) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      int w = width, h = height;
      laufey_common::ClampSizeForWindow(window_id, &w, &h);
      gtk_window_resize(GTK_WINDOW(state->window), w, h);
    }
  });
}

double WebKitGTKBackend::GetWindowScaleFactor(uint32_t window_id) {
  int scale = 1;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      scale = gtk_widget_get_scale_factor(GTK_WIDGET(state->window));
    }
  });
  return scale > 0 ? (double)scale : 1.0;
}

void WebKitGTKBackend::GetWindowSize(uint32_t window_id, int* width,
                                     int* height) {
  int w = 0, h = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_get_size(GTK_WINDOW(state->window), &w, &h);
    }
  });
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void WebKitGTKBackend::GetWindowOuterSize(uint32_t window_id, int* width,
                                          int* height) {
  int w = 0, h = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(state->window));
      if (gw) {
        GdkRectangle ext = {0, 0, 0, 0};
        gdk_window_get_frame_extents(gw, &ext);
        w = ext.width;
        h = ext.height;
      } else {
        gtk_window_get_size(GTK_WINDOW(state->window), &w, &h);
      }
    }
  });
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void WebKitGTKBackend::SetWindowPosition(uint32_t window_id, int x, int y) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_move(GTK_WINDOW(state->window), x, y);
    }
  });
}

void WebKitGTKBackend::GetWindowInnerPosition(uint32_t window_id, int* x,
                                              int* y) {
  int wx = 0, wy = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state && state->webview) {
      GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(state->webview));
      if (gw)
        gdk_window_get_origin(gw, &wx, &wy);
    }
  });
  if (x)
    *x = wx;
  if (y)
    *y = wy;
}

void WebKitGTKBackend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  int wx = 0, wy = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_get_position(GTK_WINDOW(state->window), &wx, &wy);
    }
  });
  if (x)
    *x = wx;
  if (y)
    *y = wy;
}

// --- Window state, constraints and screens (API >= 38) ---

uint32_t WebKitGTKBackend::WindowCapabilities() {
  uint32_t caps = LAUFEY_WINDOW_CAP_STATE | LAUFEY_WINDOW_CAP_STATE_EVENTS |
                  LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS |
                  LAUFEY_WINDOW_CAP_SCREENS | LAUFEY_WINDOW_CAP_DISPLAY_EVENTS |
                  LAUFEY_WINDOW_CAP_NORMAL_BOUNDS |
                  LAUFEY_WINDOW_CAP_KEEP_ALIVE;
  bool wayland = false;
  gtk_invoke_sync(
      [&] { wayland = LaufeyIsWaylandDisplay(gdk_display_get_default()); });
  // Wayland clients can't place their windows.
  if (!wayland)
    caps |= LAUFEY_WINDOW_CAP_SET_POSITION;
  // Drag and drop and file dialogs (API >= 39). Drag-out uses the window as
  // the drag source, so it works on X11 and Wayland alike.
  caps |= LAUFEY_WINDOW_CAP_FILE_DROP |
          LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS |
          LAUFEY_WINDOW_CAP_FILE_DRAG_OUT | LAUFEY_WINDOW_CAP_FILE_DIALOGS |
          LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL;
  return caps;
}

void WebKitGTKBackend::StartFileDrag(uint32_t window_id,
                                     const char* const* paths, size_t count,
                                     const uint8_t* icon_png, size_t icon_len,
                                     laufey_drag_result_fn callback,
                                     void* user_data) {
  auto* req = new laufey_common::DragOutRequest();
  req->callback = callback;
  req->user_data = user_data;
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    known = GetWindow(window_id) != nullptr;
  }
  if (!known || !laufey_common::ValidateDragPaths(paths, count, &req->paths)) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    delete req;
    return;
  }
  if (icon_png && icon_len > 0)
    req->icon_png.assign(icon_png, icon_png + icon_len);
  // The window is looked up again on the GTK thread; if it closed in between
  // there is no source and the drag fails there.
  laufey_common::StartFileDragLinux(
      [this, window_id]() -> void* {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        return state ? state->window : nullptr;
      },
      req);
}

uint32_t WebKitGTKBackend::ShowFileDialog(
    uint32_t window_id, const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data) {
  laufey_common::ParentResolver parent;
  if (window_id != 0) {
    parent = [this, window_id]() -> void* {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      return state ? state->window : nullptr;
    };
  }
  return laufey_common::ShowFileDialogLinux(std::move(parent), options,
                                            callback, user_data);
}

void WebKitGTKBackend::SetWindowState(uint32_t window_id, int action) {
  gtk_invoke_sync([&] {
    GtkWindow* window = nullptr;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id))
        window = GTK_WINDOW(state->window);
    }
    if (!window)
      return;
    switch (action) {
      case LAUFEY_WINDOW_ACTION_MAXIMIZE:
        gtk_window_maximize(window);
        break;
      case LAUFEY_WINDOW_ACTION_UNMAXIMIZE:
        gtk_window_unmaximize(window);
        break;
      case LAUFEY_WINDOW_ACTION_MINIMIZE:
        gtk_window_iconify(window);
        break;
      case LAUFEY_WINDOW_ACTION_RESTORE:
        gtk_window_deiconify(window);
        break;
      case LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN:
        gtk_window_fullscreen(window);
        break;
      case LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN:
        gtk_window_unfullscreen(window);
        break;
      default:
        break;
    }
  });
}

uint32_t WebKitGTKBackend::GetWindowState(uint32_t window_id) {
  uint32_t result = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state)
      return;
    GdkWindow* gw = gtk_widget_get_window(state->window);
    if (gw)
      result = LaufeyStateFromGdk(gdk_window_get_state(gw));
  });
  return result;
}

void WebKitGTKBackend::SetWindowSizeConstraints(uint32_t window_id,
                                                int min_width, int min_height,
                                                int max_width, int max_height) {
  laufey_common::SizeConstraints c = laufey_common::SetSizeConstraints(
      window_id, min_width, min_height, max_width, max_height);
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state)
      return;
    GdkGeometry geometry = {};
    int mask = 0;
    if (c.min_width > 0 || c.min_height > 0) {
      geometry.min_width = c.min_width;
      geometry.min_height = c.min_height;
      mask |= GDK_HINT_MIN_SIZE;
    }
    if (c.max_width > 0 || c.max_height > 0) {
      geometry.max_width = c.max_width > 0 ? c.max_width : G_MAXSHORT;
      geometry.max_height = c.max_height > 0 ? c.max_height : G_MAXSHORT;
      mask |= GDK_HINT_MAX_SIZE;
    }
    gtk_window_set_geometry_hints(GTK_WINDOW(state->window), nullptr,
                                  mask ? &geometry : nullptr,
                                  static_cast<GdkWindowHints>(mask));
    int w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(state->window), &w, &h);
    if (laufey_common::ClampSize(c, &w, &h))
      gtk_window_resize(GTK_WINDOW(state->window), w, h);
  });
}

void WebKitGTKBackend::GetWindowSizeConstraints(uint32_t window_id,
                                                int* min_width, int* min_height,
                                                int* max_width,
                                                int* max_height) {
  laufey_common::SizeConstraints c =
      laufey_common::GetSizeConstraints(window_id);
  if (min_width)
    *min_width = c.min_width;
  if (min_height)
    *min_height = c.min_height;
  if (max_width)
    *max_width = c.max_width;
  if (max_height)
    *max_height = c.max_height;
}

size_t WebKitGTKBackend::GetScreens(laufey_screen_t* out, size_t capacity) {
  std::vector<laufey_screen_t> screens;
  gtk_invoke_sync([&] {
    GdkDisplay* display = gdk_display_get_default();
    if (!display)
      return;
    int n = gdk_display_get_n_monitors(display);
    bool any_primary = false;
    for (int i = 0; i < n; ++i) {
      GdkMonitor* m = gdk_display_get_monitor(display, i);
      if (!m)
        continue;
      laufey_screen_t s = {};
      s.id = LaufeyMonitorId(display, m);
      GdkRectangle g, w;
      gdk_monitor_get_geometry(m, &g);
      gdk_monitor_get_workarea(m, &w);
      s.x = g.x;
      s.y = g.y;
      s.width = g.width;
      s.height = g.height;
      s.work_x = w.x;
      s.work_y = w.y;
      s.work_width = w.width;
      s.work_height = w.height;
      s.scale_factor = gdk_monitor_get_scale_factor(m);
      s.is_primary = gdk_monitor_is_primary(m);
      any_primary |= s.is_primary;
      screens.push_back(s);
    }
    // Wayland has no primary monitor; the first one stands in for it.
    if (!any_primary && !screens.empty())
      screens[0].is_primary = true;
    std::stable_partition(
        screens.begin(), screens.end(),
        [](const laufey_screen_t& s) { return s.is_primary; });
  });
  return laufey_common::CopyScreens(screens, out, capacity);
}

int64_t WebKitGTKBackend::GetWindowScreen(uint32_t window_id) {
  int64_t result = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state)
      return;
    GdkWindow* gw = gtk_widget_get_window(state->window);
    GdkDisplay* display = gdk_display_get_default();
    if (!gw || !display)
      return;
    GdkMonitor* m = gdk_display_get_monitor_at_window(display, gw);
    if (m)
      result = LaufeyMonitorId(display, m);
  });
  return result;
}

void WebKitGTKBackend::SetDisplayChangedHandler(
    laufey_display_changed_fn handler, void* user_data) {
  laufey_common::SetDisplayChangedHandler(handler, user_data);
  if (!handler)
    return;
  gtk_invoke_sync([] {
    static bool installed = false;
    if (installed)
      return;
    installed = true;
    GdkDisplay* display = gdk_display_get_default();
    if (!display)
      return;
    int n = gdk_display_get_n_monitors(display);
    for (int i = 0; i < n; ++i)
      LaufeyWatchMonitor(gdk_display_get_monitor(display, i));
    g_signal_connect(display, "monitor-added",
                     G_CALLBACK(+[](GdkDisplay*, GdkMonitor* m, gpointer) {
                       LaufeyWatchMonitor(m);
                       on_display_monitors_changed();
                     }),
                     nullptr);
    g_signal_connect(display, "monitor-removed",
                     G_CALLBACK(+[](GdkDisplay*, GdkMonitor*, gpointer) {
                       on_display_monitors_changed();
                     }),
                     nullptr);
  });
}

bool WebKitGTKBackend::GetWindowNormalBounds(uint32_t window_id, int* x, int* y,
                                             int* width, int* height) {
  bool found = false;
  laufey_common::Bounds b;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state)
      return;
    found = true;
    uint32_t s = 0;
    if (GdkWindow* gw = gtk_widget_get_window(state->window))
      s = LaufeyStateFromGdk(gdk_window_get_state(gw));
    if (s != 0 && laufey_common::GetCommittedNormalBounds(window_id, &b))
      return;
    gtk_window_get_position(GTK_WINDOW(state->window), &b.x, &b.y);
    gtk_window_get_size(GTK_WINDOW(state->window), &b.width, &b.height);
  });
  if (!found)
    return false;
  if (x)
    *x = b.x;
  if (y)
    *y = b.y;
  if (width)
    *width = b.width;
  if (height)
    *height = b.height;
  return true;
}

void WebKitGTKBackend::SetResizable(uint32_t window_id, bool resizable) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_set_resizable(GTK_WINDOW(state->window), resizable);
    }
  });
}

bool WebKitGTKBackend::IsResizable(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = gtk_window_get_resizable(GTK_WINDOW(state->window)) != FALSE;
    }
  });
  return result;
}

void WebKitGTKBackend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_set_keep_above(GTK_WINDOW(state->window), always_on_top);
    }
  });
}

bool WebKitGTKBackend::IsAlwaysOnTop(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      GdkWindow* gdk_window = gtk_widget_get_window(state->window);
      if (gdk_window) {
        GdkWindowState wstate = gdk_window_get_state(gdk_window);
        result = (wstate & GDK_WINDOW_STATE_ABOVE) != 0;
      }
    }
  });
  return result;
}

void WebKitGTKBackend::SetWindowOpacity(uint32_t window_id, double opacity) {
  if (opacity < 0.0)
    opacity = 0.0;
  if (opacity > 1.0)
    opacity = 1.0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_widget_set_opacity(state->window, opacity);
    }
  });
}

double WebKitGTKBackend::GetWindowOpacity(uint32_t window_id) {
  double result = 1.0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = gtk_widget_get_opacity(state->window);
    }
  });
  return result;
}

void WebKitGTKBackend::SetClickPassthrough(uint32_t window_id, bool enabled) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state)
      return;
    if (enabled) {
      // An empty input shape makes the whole window transparent to pointer
      // events; they fall through to whatever is beneath. Best-effort under a
      // reparenting X11 window manager (the WM frame may still catch clicks),
      // so pair it with a frameless window.
      cairo_region_t* empty = cairo_region_create();
      gtk_widget_input_shape_combine_region(state->window, empty);
      cairo_region_destroy(empty);
    } else {
      // NULL restores the default input shape (the full window).
      gtk_widget_input_shape_combine_region(state->window, nullptr);
    }
    state->click_passthrough = enabled;
  });
}

bool WebKitGTKBackend::IsClickPassthrough(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = state->click_passthrough;
    }
  });
  return result;
}

bool WebKitGTKBackend::IsVisible(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = gtk_widget_get_visible(state->window) != FALSE;
    }
  });
  return result;
}

void WebKitGTKBackend::Show(uint32_t window_id) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_widget_show_all(state->window);
    }
  });
}

void WebKitGTKBackend::Hide(uint32_t window_id) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_widget_hide(state->window);
    }
  });
}

void WebKitGTKBackend::Focus(uint32_t window_id) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_widget_show(state->window);
      gtk_window_present(GTK_WINDOW(state->window));
    }
  });
}

bool WebKitGTKBackend::PostUiTask(void (*task)(void*), void* data) {
  struct TaskData {
    void (*task)(void*);
    void* data;
  };
  auto* td = new TaskData{task, data};
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* td = static_cast<TaskData*>(data);
        td->task(td->data);
        delete td;
        return G_SOURCE_REMOVE;
      },
      td);
  return true;
}

void WebKitGTKBackend::InvokeJsCallback(uint32_t window_id,
                                        uint64_t callback_id,
                                        laufey::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = BuildInvokeCallbackScript(callback_id, argsJson);
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    if (window_id == 0) {
      for (auto& [wid, state] : windows_) {
        webkit_web_view_run_javascript(state.webview, script.c_str(), nullptr,
                                       nullptr, nullptr);
      }
    } else {
      auto* state = GetWindow(window_id);
      if (state) {
        webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                       nullptr, nullptr);
      }
    }
  });
}

void WebKitGTKBackend::ReleaseJsCallback(uint32_t window_id,
                                         uint64_t callback_id) {
  std::string script = BuildReleaseCallbackScript(callback_id);
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    if (window_id == 0) {
      for (auto& [wid, state] : windows_) {
        webkit_web_view_run_javascript(state.webview, script.c_str(), nullptr,
                                       nullptr, nullptr);
      }
    } else {
      auto* state = GetWindow(window_id);
      if (state) {
        webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                       nullptr, nullptr);
      }
    }
  });
}

void WebKitGTKBackend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                       laufey::ValuePtr result,
                                       laufey::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson =
      (error && !error->IsNull()) ? json::Serialize(error) : "null";
  std::string script =
      BuildRespondScript(call_id, resultJson, errorJson, errorJson != "null");
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                     nullptr, nullptr);
    }
  });
}

void WebKitGTKBackend::Run() {
  gtk_main();
}

void WebKitGTKBackend::HandleJsMessage(uint32_t window_id,
                                       const char* jsonStr) {
  laufey::ValuePtr msg = json::ParseJson(jsonStr);
  if (!msg || !msg->IsDict())
    return;

  const auto& dict = msg->GetDict();

  auto callIdIt = dict.find("callId");
  auto methodIt = dict.find("method");
  auto argsIt = dict.find("args");

  if (callIdIt == dict.end() || methodIt == dict.end())
    return;

  uint64_t call_id = 0;
  if (callIdIt->second->IsInt()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetInt());
  } else if (callIdIt->second->IsDouble()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetDouble());
  }

  std::string method =
      methodIt->second->IsString() ? methodIt->second->GetString() : "";
  laufey::ValuePtr args =
      (argsIt != dict.end()) ? argsIt->second : laufey::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// ============================================================================
// Application Menu / Context Menu
// ============================================================================
//
// Menu construction lives in backend-common
// (laufey_common::BuildGtkMenuFromValue).

void WebKitGTKBackend::SetApplicationMenu(uint32_t window_id,
                                          laufey_value_t* menu_template,
                                          const laufey_backend_api_t* api,
                                          laufey_menu_click_fn on_click,
                                          void* on_click_data) {
  if (!menu_template)
    return;
  std::vector<laufey_common::MenuEntry> entries =
      laufey_common::ParseMenuTemplate(menu_template, api, false);
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !state->vbox)
      return;

    // Remove the old menu bar and its accelerators.
    if (state->menu_bar) {
      gtk_container_remove(GTK_CONTAINER(state->vbox), state->menu_bar);
      state->menu_bar = nullptr;
    }
    if (state->accel_group) {
      gtk_window_remove_accel_group(GTK_WINDOW(state->window),
                                    state->accel_group);
      g_object_unref(state->accel_group);
      state->accel_group = nullptr;
    }
    if (entries.empty())
      return;

    // GtkWindow runs accelerators before the focused widget (the web view)
    // sees the key, as a native menu bar's.
    GtkAccelGroup* group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(state->window), group);
    state->accel_group = group;
    GtkWidget* menu_bar = laufey_common::BuildGtkMenuFromEntries(
        entries, window_id, on_click, on_click_data, true, group);
    // Pack menu bar at the top (before the webview)
    gtk_box_pack_start(GTK_BOX(state->vbox), menu_bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(state->vbox), menu_bar, 0);
    state->menu_bar = menu_bar;
    gtk_widget_show_all(menu_bar);
  });
}

void WebKitGTKBackend::ShowContextMenu(uint32_t window_id, int x, int y,
                                       laufey_value_t* menu_template,
                                       const laufey_backend_api_t* api,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template)
    return;
  ShowContextMenuEx(window_id, x, y, menu_template, api, on_click,
                    on_click_data, nullptr, nullptr);
}

void WebKitGTKBackend::ShowContextMenuEx(uint32_t window_id, int x, int y,
                                         laufey_value_t* menu_template,
                                         const laufey_backend_api_t* api,
                                         laufey_menu_click_fn on_click,
                                         void* on_click_data,
                                         laufey_menu_closed_fn on_closed,
                                         void* on_closed_data) {
  // Parsed here: the template is the caller's only for this call.
  auto entries = std::make_shared<std::vector<laufey_common::MenuEntry>>(
      laufey_common::ParseMenuTemplate(menu_template, api, false));
  laufey_common::GtkRunAsync([this, window_id, x, y, entries, on_click,
                              on_click_data, on_closed, on_closed_data] {
    GtkWidget* anchor = nullptr;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        anchor = state->webview ? GTK_WIDGET(state->webview) : state->window;
    }
    laufey_common::ShowGtkContextMenu(anchor, x, y, *entries, window_id,
                                      on_click, on_click_data, on_closed,
                                      on_closed_data);
  });
}

bool WebKitGTKBackend::TestTriggerMenuAccelerator(uint32_t window_id,
                                                  const char* accelerator) {
  bool fired = false;
  gtk_invoke_sync([&] {
    GtkWidget* window = nullptr;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        window = state->window;
    }
    fired = laufey_common::TestTriggerMenuAcceleratorGtk(window, accelerator);
  });
  return fired;
}

// ============================================================================
// DevTools
// ============================================================================

void WebKitGTKBackend::OpenDevTools(uint32_t window_id) {
  if (!laufey_common::LaunchInspectable())
    return;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state && state->webview) {
      WebKitWebInspector* inspector =
          webkit_web_view_get_inspector(state->webview);
      webkit_web_inspector_show(inspector);
    }
  });
}

void WebKitGTKBackend::CloseDevTools(uint32_t window_id) {
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state && state->webview)
      webkit_web_inspector_close(webkit_web_view_get_inspector(state->webview));
  });
}

bool WebKitGTKBackend::IsDevToolsOpen(uint32_t window_id) {
  bool open = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !state->webview)
      return;
    // The inspector's own web view exists while it is shown (attached or in
    // its window) and is dropped when it closes.
    open = webkit_web_inspector_get_web_view(
               webkit_web_view_get_inspector(state->webview)) != nullptr;
  });
  return open;
}

bool WebKitGTKBackend::IsDevToolsEnabled(uint32_t window_id) {
  if (window_id == 0)
    return laufey_common::LaunchInspectable();
  bool enabled = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state && state->webview) {
      enabled = webkit_settings_get_enable_developer_extras(
                    webkit_web_view_get_settings(state->webview)) != FALSE;
    }
  });
  return enabled;
}

// WebKitGTK has no in-memory PDF API; WebKitPrintOperation only writes to a
// GtkPrintSettings output URI, and the write happens in the separate
// WebKitWebProcess -- so the target must be a real filesystem path that
// process can open by name (an fd owned by this UI process, e.g. a memfd's
// /proc/self/fd/N, would resolve against the web process's own fd table and
// name the wrong file or none at all). We print into a private temp file,
// read the bytes back, and delete it; the capi layer owns writing the
// caller's requested output path.
struct PdfPrintData {
  laufey_pdf_result_fn callback;
  void* callback_data;
  std::string output_path;  // temp file the print job writes into
  bool has_error = false;
  std::string error_message;
};

static void on_pdf_print_failed(WebKitPrintOperation* /*op*/, GError* error,
                                gpointer user_data) {
  auto* d = static_cast<PdfPrintData*>(user_data);
  d->has_error = true;
  d->error_message = error && error->message ? error->message : "print failed";
}

static void on_pdf_print_finished(WebKitPrintOperation* op,
                                  gpointer user_data) {
  auto* d = static_cast<PdfPrintData*>(user_data);
  if (d->has_error) {
    d->callback(nullptr, 0, d->error_message.c_str(), d->callback_data);
  } else {
    gchar* contents = nullptr;
    gsize length = 0;
    GError* read_error = nullptr;
    if (g_file_get_contents(d->output_path.c_str(), &contents, &length,
                            &read_error)) {
      d->callback(length ? reinterpret_cast<const uint8_t*>(contents) : nullptr,
                  length, nullptr, d->callback_data);
      g_free(contents);
    } else {
      d->callback(nullptr, 0,
                  read_error && read_error->message ? read_error->message
                                                    : "failed to read PDF",
                  d->callback_data);
    }
    if (read_error)
      g_error_free(read_error);
  }
  unlink(d->output_path.c_str());
  g_object_unref(op);
  delete d;
}

void WebKitGTKBackend::PrintToPdf(uint32_t window_id,
                                  laufey_pdf_result_fn callback,
                                  void* callback_data) {
  if (!callback)
    return;
  gtk_invoke_sync([&] {
    // Scope the lock to the lookup only: the callback is an arbitrary user
    // FnOnce that may re-enter backend APIs taking this same non-recursive
    // mutex on this thread (the "failed" signal can fire synchronously from
    // webkit_print_operation_print below). Using the webview after unlock is
    // safe because window teardown also runs on this (main) thread.
    WebKitWebView* webview = nullptr;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        webview = state->webview;
    }
    if (!webview) {
      callback(nullptr, 0, "window not found", callback_data);
      return;
    }

    // Private (0600, unpredictable name) temp file for the print output;
    // close the fd right away -- the web process opens the file by path.
    gchar* tmp_path = nullptr;
    GError* tmp_error = nullptr;
    int tmp_fd = g_file_open_tmp("laufey-pdf-XXXXXX", &tmp_path, &tmp_error);
    if (tmp_fd < 0) {
      callback(nullptr, 0,
               tmp_error && tmp_error->message ? tmp_error->message
                                               : "failed to create temp file",
               callback_data);
      if (tmp_error)
        g_error_free(tmp_error);
      return;
    }
    close(tmp_fd);

    auto* d = new PdfPrintData{callback, callback_data, tmp_path, false, ""};
    gchar* uri = g_filename_to_uri(tmp_path, nullptr, nullptr);
    g_free(tmp_path);
    if (!uri) {
      // Cannot happen for g_file_open_tmp's absolute path, but stay safe.
      callback(nullptr, 0, "invalid temp file path", callback_data);
      unlink(d->output_path.c_str());
      delete d;
      return;
    }

    GtkPrintSettings* settings = gtk_print_settings_new();
    gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
    gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT,
                           "pdf");
    g_free(uri);

    WebKitPrintOperation* op = webkit_print_operation_new(webview);
    webkit_print_operation_set_print_settings(op, settings);
    g_object_unref(settings);

    // "finished" is WebKitPrintOperation's guaranteed completion signal: it is
    // emitted for every terminated operation, and always AFTER "failed" when an
    // error occurred (WebKit forwards GtkPrintOperation's always-emitted "done"
    // signal). So on_pdf_print_finished is the single owner of invoking the
    // callback, deleting the temp file, unref-ing `op`, and freeing `d` -- the
    // same "completion callback always fires" contract the macOS backend
    // relies on. "failed" only records the error for that handler to report.
    g_signal_connect(op, "failed", G_CALLBACK(on_pdf_print_failed), d);
    g_signal_connect(op, "finished", G_CALLBACK(on_pdf_print_finished), d);

    webkit_print_operation_print(op);
  });
}

// ============================================================================
// Dialog
// ============================================================================

int WebKitGTKBackend::ShowDialog(uint32_t /*window_id*/, int dialog_type,
                                 const std::string& title,
                                 const std::string& message,
                                 const std::string& default_value,
                                 char** out_input_value) {
  // Native modal must run on the GTK main thread. gtk_invoke_sync blocks the
  // calling (runtime) thread until the modal's nested loop returns on main.
  int result = 0;
  gtk_invoke_sync([&] {
    result = laufey_common::ShowDialogLinux(dialog_type, title, message,
                                            default_value, out_input_value);
  });
  return result;
}

// ============================================================================
// Dock / taskbar (Linux — X11 urgency hint)
// ============================================================================

void WebKitGTKBackend::BounceDock(int /*type*/) {
  // X11 urgency hint is binary (no informational vs critical). Set it on
  // every LAUFEY window; the WM surfaces attention (flash the taskbar button,
  // highlight the window in overview, etc.).
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    for (auto& [wid, state] : windows_) {
      if (!state.window)
        continue;
      gtk_window_set_urgency_hint(GTK_WINDOW(state.window), TRUE);
    }
  });
}

// Badge via title prefix. Saved-titles map lives in
// laufey_common::ApplyTitlePrefixBadge.
void WebKitGTKBackend::SetDockBadge(const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> wlock(windows_mutex_);
    for (auto& [wid, state] : windows_) {
      if (!state.window)
        continue;
      GtkWindow* gw = GTK_WINDOW(state.window);
      const char* current = gtk_window_get_title(gw);
      std::string next = laufey_common::ApplyTitlePrefixBadge(
          wid, current ? std::string(current) : std::string(), badge);
      gtk_window_set_title(gw, next.c_str());
    }
  });
}

// ============================================================================
// Tray / status bar (Linux) — libappindicator if available
// ============================================================================

// Thin trampolines over backend-common/src/tray_linux.cc.

uint32_t WebKitGTKBackend::CreateTrayIcon() {
  return laufey_common::CreateTrayIconLinux();
}
void WebKitGTKBackend::DestroyTrayIcon(uint32_t tray_id) {
  laufey_common::DestroyTrayIconLinux(tray_id);
}
void WebKitGTKBackend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                   size_t len) {
  laufey_common::SetTrayIconLinux(tray_id, png_bytes, len);
}
void WebKitGTKBackend::SetTrayTooltip(uint32_t tray_id,
                                      const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipLinux(tray_id, tooltip_or_null);
}
void WebKitGTKBackend::SetTrayMenu(uint32_t tray_id,
                                   laufey_value_t* menu_template,
                                   const laufey_backend_api_t* api,
                                   laufey_menu_click_fn on_click,
                                   void* on_click_data) {
  laufey_common::SetTrayMenuLinux(tray_id, menu_template, api, on_click,
                                  on_click_data);
}
void WebKitGTKBackend::SetTrayClickHandler(uint32_t tray_id,
                                           laufey_tray_click_fn handler,
                                           void* user_data) {
  laufey_common::SetTrayClickHandlerLinux(tray_id, handler, user_data);
}

// ============================================================================
// Notifications (WebKitGTK Linux)
// ============================================================================
//
// Thin trampolines over backend-common (laufey_notifications.h: the
// org.freedesktop.Notifications D-Bus client).

uint32_t WebKitGTKBackend::ShowNotification(
    laufey_value_t* options, const laufey_backend_api_t* api,
    laufey_notification_event_fn on_event, void* user_data) {
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, api);
  return laufey_common::ShowNotification(opts, on_event, user_data);
}

void WebKitGTKBackend::CloseNotification(uint32_t notification_id) {
  laufey_common::CloseNotification(notification_id);
}

// ============================================================================
// Custom URL scheme handling (in-process transport)
// ============================================================================
//
// Serves "app" and every scheme the embedder registered through
// register_scheme_handler. Each one is registered on the default
// WebKitWebContext as secure (isSecureContext, crypto.subtle) and CORS-enabled
// (a page on another origin may fetch it with CORS headers); with a host in
// the URL WebKit gives it a `<scheme>://<host>` origin and per-origin storage.

namespace {

// Exchange wrapping a WebKitURISchemeRequest. The response body is a
// laufey_common::SchemeBodyWriter's stream, handed to WebKit (on the GTK main
// thread) with the head: the runtime's writes append to it and return at
// once, whatever WebKit has read so far, and WebKit reads it from the GTK
// main loop. (A pipe used to sit here: its write blocked the runtime's event
// loop whenever 64 KiB were unread.) A page that stops reading makes the
// next write fail; a body that grows past kSchemeBodyMaxQueued unread fails
// the response, as on WebView2.
//
// The request body is buffered before the exchange is created (see
// OnAppSchemeRequest), so ReadRequestBody is a non-blocking copy, as on the
// other backends.
class LinuxSchemeExchange : public SchemeExchangeBase {
 public:
  LinuxSchemeExchange(WebKitURISchemeRequest* request,
                      std::vector<uint8_t> request_body)
      : request_(WEBKIT_URI_SCHEME_REQUEST(g_object_ref(request))),
        request_body_(std::move(request_body)),
        body_(std::make_shared<laufey_common::SchemeBodyWriter>()),
        gate_(std::make_shared<laufey_common::SchemeCancelGate>()) {
    // WebKit let the body stream go before it ended: the page aborted the
    // request (or the document / window went away). WebKitGTK says nothing
    // about a request cancelled before its head was sent, so a cancel is
    // only seen once the head is.
    std::shared_ptr<laufey_common::SchemeCancelGate> gate = gate_;
    LinuxSchemeExchange* self = this;
    body_->SetReaderGoneHandler([gate, self] {
      gate->Cancel(
          [self] { RuntimeLoader::GetInstance()->DispatchSchemeCancel(self); });
    });
  }

  ~LinuxSchemeExchange() override {
    body_->End();
    if (request_)
      g_object_unref(request_);
  }

  intptr_t ReadRequestBody(uint8_t* buf, size_t cap) override {
    if (cap == 0)
      return 0;
    size_t remaining = request_body_.size() - req_cursor_;
    if (remaining == 0)
      return 0;
    size_t n = std::min(cap, remaining);
    memcpy(buf, request_body_.data() + req_cursor_, n);
    req_cursor_ += n;
    return static_cast<intptr_t>(n);
  }

  void Begin(int status, const char* headers, size_t headers_len) override {
    began_ = true;
    auto* d = new BeginData;
    d->request = WEBKIT_URI_SCHEME_REQUEST(g_object_ref(request_));
    d->body = body_;
    d->status = status;
    d->headers = LaufeyParseFlatHeaders(headers, headers_len);
    g_idle_add(BeginOnMain, d);
  }

  // Never blocks (see the class comment).
  intptr_t WriteResponse(const uint8_t* buf, size_t len) override {
    return body_->Write(buf, len);
  }

  void Finish() override {
    // No on_cancel from now on (and one in progress has returned).
    gate_->Finish();
    body_->End();  // EOF for WebKit, after what was written
    if (!began_) {
      // Finished without a head: no response, so the request fails (it was
      // never finished at all, which left the page's request pending).
      g_idle_add(FailOnMain, g_object_ref(request_));
    }
    delete this;
  }

 private:
  struct BeginData {
    WebKitURISchemeRequest* request;
    // Shared: the exchange may finish before this runs.
    std::shared_ptr<laufey_common::SchemeBodyWriter> body;
    int status;
    std::vector<std::pair<std::string, std::string>> headers;
  };

  static gboolean FailOnMain(gpointer data) {
    auto* request = static_cast<WebKitURISchemeRequest*>(data);
    GError* error =
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                            "the scheme handler finished without a response");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    g_object_unref(request);
    return G_SOURCE_REMOVE;
  }

  static gboolean BeginOnMain(gpointer data) {
    auto* d = static_cast<BeginData*>(data);
    // On the GTK main thread: WebKit reads the stream from this context.
    GInputStream* stream = d->body->CreateStream();
    WebKitURISchemeResponse* resp = webkit_uri_scheme_response_new(stream, -1);
    webkit_uri_scheme_response_set_status(resp, d->status, nullptr);
    SoupMessageHeaders* hdrs =
        soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    for (const auto& [k, v] : d->headers) {
      soup_message_headers_append(hdrs, k.c_str(), v.c_str());
      // WebKit takes the response's MIME type from the content type set on
      // the WebKitURISchemeResponse, not from the HTTP header list. Without
      // it the type is empty, the default response policy treats the
      // navigation as a download, and the load fails with "Frame load
      // interrupted" — so mirror the Content-Type header into it.
      if (g_ascii_strcasecmp(k.c_str(), "content-type") == 0) {
        webkit_uri_scheme_response_set_content_type(resp, v.c_str());
      }
    }
    // set_http_headers takes ownership of `hdrs`.
    webkit_uri_scheme_response_set_http_headers(resp, hdrs);
    webkit_uri_scheme_request_finish_with_response(d->request, resp);
    g_object_unref(resp);
    // WebKit holds the stream now; when it lets go (the load ended or was
    // cancelled) the stream closes and the next write fails.
    g_object_unref(stream);
    g_object_unref(d->request);
    delete d;
    return G_SOURCE_REMOVE;
  }

  WebKitURISchemeRequest* request_;
  std::vector<uint8_t> request_body_;
  size_t req_cursor_ = 0;
  std::shared_ptr<laufey_common::SchemeBodyWriter> body_;
  std::shared_ptr<laufey_common::SchemeCancelGate> gate_;
  // Begin and Finish come from the runtime's thread, in that order.
  bool began_ = false;
};

// A scheme request whose body is still being read (see OnAppSchemeRequest).
struct PendingSchemeRequest {
  WebKitURISchemeRequest* request;  // owned reference
  std::string method;
  std::string uri;
  std::string flat_headers;
  std::vector<uint8_t> body;
};

// Hand a request (with its complete body) to the runtime. Takes `pending`.
void DispatchPendingSchemeRequest(PendingSchemeRequest* pending) {
  // window_id is unused by the desktop bridge (it serves a single named
  // channel), so 0 is fine.
  auto* exchange =
      new LinuxSchemeExchange(pending->request, std::move(pending->body));
  RuntimeLoader::GetInstance()->DispatchSchemeRequest(
      0, exchange, pending->method, pending->uri, pending->flat_headers);
  g_object_unref(pending->request);
  delete pending;
}

#if WEBKIT_CHECK_VERSION(2, 40, 0)
// Bytes requested per asynchronous read of a request body.
constexpr gsize kSchemeBodyChunk = 256 * 1024;

void ReadSchemeRequestBodyChunk(GInputStream* body,
                                PendingSchemeRequest* pending);

// Completion of one body read, on the GTK main thread: append the chunk and
// read on, dispatch at end of stream, or fail the request on a read error
// (forwarding a truncated body would be worse than failing the fetch).
void OnSchemeRequestBodyChunk(GObject* source, GAsyncResult* result,
                              gpointer data) {
  GInputStream* body = G_INPUT_STREAM(source);
  auto* pending = static_cast<PendingSchemeRequest*>(data);
  GError* error = nullptr;
  GBytes* chunk = g_input_stream_read_bytes_finish(body, result, &error);
  if (!chunk) {
    std::cerr << "laufey: failed to read the request body of "
              << pending->method << " " << pending->uri << ": "
              << (error ? error->message : "unknown error") << std::endl;
    if (error) {
      webkit_uri_scheme_request_finish_error(pending->request, error);
      g_error_free(error);
    } else {
      GError* fallback = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "failed to read the request body");
      webkit_uri_scheme_request_finish_error(pending->request, fallback);
      g_error_free(fallback);
    }
    g_object_unref(pending->request);
    delete pending;
    g_object_unref(body);
    return;
  }
  gsize size = 0;
  const auto* bytes =
      static_cast<const uint8_t*>(g_bytes_get_data(chunk, &size));
  if (size == 0) {
    // End of stream.
    g_bytes_unref(chunk);
    g_object_unref(body);
    DispatchPendingSchemeRequest(pending);
    return;
  }
  pending->body.insert(pending->body.end(), bytes, bytes + size);
  g_bytes_unref(chunk);
  ReadSchemeRequestBodyChunk(body, pending);
}

void ReadSchemeRequestBodyChunk(GInputStream* body,
                                PendingSchemeRequest* pending) {
  g_input_stream_read_bytes_async(body, kSchemeBodyChunk, G_PRIORITY_DEFAULT,
                                  nullptr, OnSchemeRequestBodyChunk, pending);
}
#endif  // WEBKIT_CHECK_VERSION(2, 40, 0)

// Runs on the GTK main thread. The request body (POST/PUT/PATCH from the page)
// is exposed by WebKitGTK >= 2.40 as a GInputStream; it is read to the end
// with asynchronous reads — never blocking the main loop — and the request is
// dispatched to the runtime once it is complete, so ReadRequestBody is a plain
// copy (the WKWebView, WebView2 and CEF backends buffer the body up front
// too). A request without a body (NULL stream) is dispatched at once. Built
// against WebKitGTK < 2.40, which has no body accessor, every request is
// forwarded with an empty body.
void OnAppSchemeRequest(WebKitURISchemeRequest* request, gpointer) {
  const char* uri = webkit_uri_scheme_request_get_uri(request);
  const char* method = webkit_uri_scheme_request_get_http_method(request);
  std::vector<std::pair<std::string, std::string>> headers;
  SoupMessageHeaders* req_headers =
      webkit_uri_scheme_request_get_http_headers(request);
  if (req_headers) {
    SoupMessageHeadersIter it;
    soup_message_headers_iter_init(&it, req_headers);
    const char* name;
    const char* value;
    while (soup_message_headers_iter_next(&it, &name, &value)) {
      headers.emplace_back(name, value);
    }
  }
  auto* pending = new PendingSchemeRequest;
  pending->request = WEBKIT_URI_SCHEME_REQUEST(g_object_ref(request));
  pending->method = method ? method : "GET";
  pending->uri = uri ? uri : "";
  pending->flat_headers = LaufeyFlattenHeaders(headers);
#if WEBKIT_CHECK_VERSION(2, 40, 0)
  // (transfer full), NULL when the request has no body.
  if (GInputStream* body = webkit_uri_scheme_request_get_http_body(request)) {
    ReadSchemeRequestBodyChunk(body, pending);
    return;
  }
#endif
  DispatchPendingSchemeRequest(pending);
}

}  // namespace

void WebKitGTKBackend::RegisterSchemeHandler(const std::string& scheme) {
  if (!laufey_common::IsValidSchemeName(scheme)) {
    std::cerr << "laufey: ignoring invalid URL scheme name \"" << scheme
              << "\" passed to register_scheme_handler" << std::endl;
    return;
  }
  std::string normalized = laufey_common::NormalizeSchemeName(scheme);
  bool added = laufey_common::SchemeRegistry::GetInstance()->Add(normalized);
  if (added && any_web_view_created_.load()) {
    // WebKitGTK applies the registration to existing web views too (the
    // schemes live on the shared web context), but the other engines do not;
    // flag the portability hazard.
    std::cerr << "laufey: scheme \"" << normalized
              << "\" was registered after a window was created; WebKitGTK "
                 "serves it, but other backends will not (register schemes "
                 "before the first window)"
              << std::endl;
  }

  // Register on the web context shared by all webviews (LaufeyWebContext).
  // Must run on the GTK main thread; the runtime calls this from its own
  // thread. Window creation is queued on the same main loop, so a scheme
  // registered before the first CreateWindow is installed before that window's
  // web view exists.
  g_idle_add(
      [](gpointer) -> gboolean {
        // Main thread only. Install every registered scheme that isn't yet
        // known to WebKit — the built-in "app" included, so it is served
        // whether or not the runtime registered it by name (as the other
        // backends do). Each name is registered exactly once; a second
        // registration of the same name is an error.
        static std::set<std::string> installed;
        WebKitWebContext* ctx = LaufeyWebContext();
        WebKitSecurityManager* sm =
            webkit_web_context_get_security_manager(ctx);
        for (const std::string& s :
             laufey_common::SchemeRegistry::GetInstance()->Snapshot()) {
          if (!installed.insert(s).second) {
            continue;
          }
          webkit_web_context_register_uri_scheme(
              ctx, s.c_str(), OnAppSchemeRequest, nullptr, nullptr);
          webkit_security_manager_register_uri_scheme_as_secure(sm, s.c_str());
          webkit_security_manager_register_uri_scheme_as_cors_enabled(
              sm, s.c_str());
        }
        return G_SOURCE_REMOVE;
      },
      nullptr);
}

// ============================================================================
// Factory Function
// ============================================================================

LaufeyBackend* CreateLaufeyBackend() {
  return new WebKitGTKBackend();
}
