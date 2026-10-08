// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences (API 47), the Linux sources and the change watcher.
//
//   1. xdg-desktop-portal's Settings interface: one ReadAll of
//      org.gnome.desktop.wm.preferences (button-layout,
//      action-double-click-titlebar, titlebar-font / titlebar-uses-system-
//      font), org.freedesktop.appearance (color-scheme, accent-color) and
//      org.gnome.desktop.interface (its color-scheme / accent-color /
//      font-name, for a portal without the appearance keys). Every desktop
//      portal answers these: GNOME's from GSettings, Plasma's from KWin's
//      decoration settings and its colour scheme.
//   2. GSettings, for a key the portal didn't answer (no portal running, an
//      old one).
//   3. GTK's defaults.
//
// The portal wins key by key: on Ubuntu GNOME, GSettings' button-layout can
// differ from the portal's, and the portal's is what sandboxed apps and GTK 4
// use.
//
// The watcher runs on a thread of its own with a private GMainContext (the
// Winit backend runs no GLib loop, and the CEF / WebView UI loops must not
// block on it): it follows the portal's SettingChanged and GSettings'
// "changed", and, after a short settle (a desktop changes several keys at
// once), fires the change handler when the answer differs from the last
// one it reported.

#include <gio/gio.h>
#include <sys/stat.h>

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "laufey_title_bar.h"

namespace laufey_common {

namespace {

constexpr int kCallTimeoutMs = 1000;
// The first portal call may start xdg-desktop-portal.
constexpr int kPortalStartTimeoutMs = 3000;
// How long the watcher lets a burst of changes settle before it re-reads.
constexpr int kSettleMs = 150;

constexpr char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kSettingsInterface[] = "org.freedesktop.portal.Settings";
constexpr char kWmNamespace[] = "org.gnome.desktop.wm.preferences";
constexpr char kAppearanceNamespace[] = "org.freedesktop.appearance";
constexpr char kInterfaceNamespace[] = "org.gnome.desktop.interface";

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

// A session bus is configured (the address, or systemd's per-user socket):
// checked first so GIO never autolaunches one on a bare X display.
bool HasSessionBusAddress() {
  if (!Env("DBUS_SESSION_BUS_ADDRESS").empty())
    return true;
  std::string runtime = Env("XDG_RUNTIME_DIR");
  if (runtime.empty())
    return false;
  struct stat st;
  return stat((runtime + "/bus").c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

GDBusConnection* SessionBus() {
  if (!HasSessionBusAddress())
    return nullptr;
  return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

std::mutex g_first_mutex;
bool g_portal_called = false;  // a call already waited for the portal

// One namespace's keys from a ReadAll answer (a{sa{sv}}), or null.
GVariant* Namespace(GVariant* all, const char* ns) {
  return g_variant_lookup_value(all, ns, G_VARIANT_TYPE("a{sv}"));
}

std::optional<std::string> StringKey(GVariant* dict, const char* key) {
  if (!dict)
    return std::nullopt;
  GVariant* v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
  if (!v)
    return std::nullopt;
  std::string s = g_variant_get_string(v, nullptr);
  g_variant_unref(v);
  return s;
}

std::optional<bool> BoolKey(GVariant* dict, const char* key) {
  if (!dict)
    return std::nullopt;
  GVariant* v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_BOOLEAN);
  if (!v)
    return std::nullopt;
  bool b = g_variant_get_boolean(v);
  g_variant_unref(v);
  return b;
}

// The titlebar font as mutter picks it: the system font when
// titlebar-uses-system-font is set (and known), else titlebar-font.
std::optional<std::string> TitlebarFont(GVariant* wm, GVariant* iface) {
  std::optional<bool> system = BoolKey(wm, "titlebar-uses-system-font");
  if (system.value_or(false)) {
    if (auto f = StringKey(iface, "font-name"))
      return f;
  }
  return StringKey(wm, "titlebar-font");
}

}  // namespace

// What the portal answers, or nothing when no portal runs (or it has no
// Settings interface, or doesn't answer in time). Never starts anything but
// the portal itself (D-Bus activation, as any portal client does).
TitleBarSettings PortalTitleBarSettings() {
  TitleBarSettings out;
  GDBusConnection* bus = SessionBus();
  if (!bus)
    return out;
  bool first;
  {
    std::lock_guard<std::mutex> lock(g_first_mutex);
    first = !g_portal_called;
    g_portal_called = true;
  }
  const char* namespaces[] = {kWmNamespace, kAppearanceNamespace,
                              kInterfaceNamespace, nullptr};
  GVariant* r = g_dbus_connection_call_sync(
      bus, kPortalName, kPortalPath, kSettingsInterface, "ReadAll",
      g_variant_new("(^as)", namespaces), G_VARIANT_TYPE("(a{sa{sv}})"),
      G_DBUS_CALL_FLAGS_NONE, first ? kPortalStartTimeoutMs : kCallTimeoutMs,
      nullptr, nullptr);
  g_object_unref(bus);
  if (!r)
    return out;
  GVariant* all = g_variant_get_child_value(r, 0);
  GVariant* wm = Namespace(all, kWmNamespace);
  GVariant* appearance = Namespace(all, kAppearanceNamespace);
  GVariant* iface = Namespace(all, kInterfaceNamespace);
  out.button_layout = StringKey(wm, "button-layout");
  out.double_click = StringKey(wm, "action-double-click-titlebar");
  out.font = TitlebarFont(wm, iface);
  // The appearance namespace's keys first; GNOME's interface keys for a
  // portal without them.
  if (appearance) {
    GVariant* v = g_variant_lookup_value(appearance, "color-scheme",
                                         G_VARIANT_TYPE_UINT32);
    if (v) {
      std::string scheme = ColorSchemeFromPortal(g_variant_get_uint32(v));
      if (!scheme.empty())
        out.color_scheme = scheme;
      g_variant_unref(v);
    }
    v = g_variant_lookup_value(appearance, "accent-color",
                               G_VARIANT_TYPE("(ddd)"));
    if (v) {
      double cr = -1, cg = -1, cb = -1;
      g_variant_get(v, "(ddd)", &cr, &cg, &cb);
      std::string accent = AccentFromPortalRgb(cr, cg, cb);
      if (!accent.empty())
        out.accent_color = accent;
      g_variant_unref(v);
    }
  }
  if (!out.color_scheme) {
    if (auto s = StringKey(iface, "color-scheme")) {
      std::string scheme = ColorSchemeFromGnome(*s);
      if (!scheme.empty())
        out.color_scheme = scheme;
    }
  }
  if (!out.accent_color) {
    if (auto s = StringKey(iface, "accent-color")) {
      std::string accent = AccentFromGnomeName(*s);
      if (!accent.empty())
        out.accent_color = accent;
    }
  }
  for (GVariant* v : {wm, appearance, iface}) {
    if (v)
      g_variant_unref(v);
  }
  g_variant_unref(all);
  g_variant_unref(r);
  return out;
}

namespace {

// A GSettings object for `schema_id`, or null when the schema isn't
// installed (g_settings_new aborts on an unknown schema).
GSettings* SettingsFor(const char* schema_id) {
  GSettingsSchemaSource* source = g_settings_schema_source_get_default();
  if (!source)
    return nullptr;
  GSettingsSchema* schema =
      g_settings_schema_source_lookup(source, schema_id, TRUE);
  if (!schema)
    return nullptr;
  GSettings* settings = g_settings_new_full(schema, nullptr, nullptr);
  g_settings_schema_unref(schema);
  return settings;
}

bool HasKey(GSettings* settings, const char* key) {
  GSettingsSchema* schema = nullptr;
  g_object_get(settings, "settings-schema", &schema, nullptr);
  bool has = schema && g_settings_schema_has_key(schema, key);
  if (schema)
    g_settings_schema_unref(schema);
  return has;
}

std::optional<std::string> SettingsString(GSettings* settings,
                                          const char* key) {
  if (!settings || !HasKey(settings, key))
    return std::nullopt;
  GVariant* v = g_settings_get_value(settings, key);
  std::optional<std::string> out;
  // color-scheme / accent-color are enums: their values are strings too.
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
    out = g_variant_get_string(v, nullptr);
  if (v)
    g_variant_unref(v);
  return out;
}

std::optional<bool> SettingsBool(GSettings* settings, const char* key) {
  if (!settings || !HasKey(settings, key))
    return std::nullopt;
  return g_settings_get_boolean(settings, key) != FALSE;
}

TitleBarSettings ReadGSettings(GSettings* wm, GSettings* iface) {
  TitleBarSettings out;
  out.button_layout = SettingsString(wm, "button-layout");
  out.double_click = SettingsString(wm, "action-double-click-titlebar");
  if (SettingsBool(wm, "titlebar-uses-system-font").value_or(false))
    out.font = SettingsString(iface, "font-name");
  if (!out.font)
    out.font = SettingsString(wm, "titlebar-font");
  if (auto s = SettingsString(iface, "color-scheme")) {
    std::string scheme = ColorSchemeFromGnome(*s);
    if (!scheme.empty())
      out.color_scheme = scheme;
  }
  if (auto s = SettingsString(iface, "accent-color")) {
    std::string accent = AccentFromGnomeName(*s);
    if (!accent.empty())
      out.accent_color = accent;
  }
  return out;
}

}  // namespace

// What GSettings answers (nothing for a schema that isn't installed).
TitleBarSettings GSettingsTitleBarSettings() {
  GSettings* wm = SettingsFor(kWmNamespace);
  GSettings* iface = SettingsFor(kInterfaceNamespace);
  TitleBarSettings out = ReadGSettings(wm, iface);
  if (wm)
    g_object_unref(wm);
  if (iface)
    g_object_unref(iface);
  return out;
}

TitleBarPreferences ProbeTitleBarPreferences() {
  return ResolveTitleBarPreferences(PortalTitleBarSettings(),
                                    GSettingsTitleBarSettings());
}

namespace {

// The change watcher.
struct Watcher {
  std::mutex mutex;
  std::condition_variable ready_cv;
  bool started = false;
  bool ready = false;
  std::thread thread;
  GMainContext* ctx = nullptr;
  GMainLoop* loop = nullptr;
  // Owned by the watcher thread.
  GDBusConnection* bus = nullptr;
  guint subscription = 0;
  GSettings* wm = nullptr;
  GSettings* iface = nullptr;
  guint settle = 0;
  // Under `mutex`.
  void (*handler)(void*) = nullptr;
  void* user_data = nullptr;
  std::string last_json;
};

Watcher& W() {
  static Watcher* w = new Watcher();
  return *w;
}

bool Relevant(const char* ns, const char* key) {
  if (!ns || !key)
    return false;
  std::string n = ns, k = key;
  if (n == kWmNamespace) {
    return k == "button-layout" || k == "action-double-click-titlebar" ||
           k == "titlebar-font" || k == "titlebar-uses-system-font";
  }
  if (n == kAppearanceNamespace)
    return k == "color-scheme" || k == "accent-color";
  if (n == kInterfaceNamespace) {
    return k == "color-scheme" || k == "accent-color" || k == "font-name";
  }
  return false;
}

// Re-read and report when the answer changed. On the watcher thread.
gboolean Settled(gpointer) {
  Watcher& w = W();
  w.settle = 0;
  std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
  void (*handler)(void*) = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    if (json == w.last_json)
      return G_SOURCE_REMOVE;
    w.last_json = json;
    handler = w.handler;
    user_data = w.user_data;
  }
  if (handler)
    handler(user_data);
  return G_SOURCE_REMOVE;
}

void Changed() {
  Watcher& w = W();
  if (w.settle)
    return;
  GSource* timer = g_timeout_source_new(kSettleMs);
  g_source_set_callback(timer, Settled, nullptr, nullptr);
  w.settle = g_source_attach(timer, w.ctx);
  g_source_unref(timer);
}

void OnSettingChanged(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar*, GVariant* params, gpointer) {
  if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(ssv)")))
    return;
  const gchar* ns = nullptr;
  const gchar* key = nullptr;
  g_variant_get(params, "(&s&sv)", &ns, &key, nullptr);
  if (Relevant(ns, key))
    Changed();
}

void OnGSettingsChanged(GSettings*, const gchar*, gpointer) {
  Changed();
}

// GSettings reports a key only after it was read with a handler connected.
void ReadAllKeys(GSettings* settings) {
  if (!settings)
    return;
  GSettingsSchema* schema = nullptr;
  g_object_get(settings, "settings-schema", &schema, nullptr);
  if (!schema)
    return;
  gchar** keys = g_settings_schema_list_keys(schema);
  for (gchar** k = keys; k && *k; ++k) {
    GVariant* v = g_settings_get_value(settings, *k);
    if (v)
      g_variant_unref(v);
  }
  g_strfreev(keys);
  g_settings_schema_unref(schema);
}

void RunWatcher() {
  Watcher& w = W();
  g_main_context_push_thread_default(w.ctx);
  w.bus = SessionBus();
  if (w.bus) {
    // Subscribed by the portal's well-known name: GDBus follows its owner,
    // so a portal that starts (or restarts) later is heard too.
    w.subscription = g_dbus_connection_signal_subscribe(
        w.bus, kPortalName, kSettingsInterface, "SettingChanged", kPortalPath,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, OnSettingChanged, nullptr, nullptr);
  }
  w.wm = SettingsFor(kWmNamespace);
  w.iface = SettingsFor(kInterfaceNamespace);
  for (GSettings* s : {w.wm, w.iface}) {
    if (s) {
      g_signal_connect(s, "changed", G_CALLBACK(OnGSettingsChanged), nullptr);
      ReadAllKeys(s);
    }
  }
  // What the handler compares against: the answer as of now.
  std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.last_json = json;
    w.ready = true;
  }
  w.ready_cv.notify_all();
  g_main_loop_run(w.loop);
  if (w.settle) {
    GSource* s = g_main_context_find_source_by_id(w.ctx, w.settle);
    if (s)
      g_source_destroy(s);
    w.settle = 0;
  }
  for (GSettings** s : {&w.wm, &w.iface}) {
    if (*s) {
      g_signal_handlers_disconnect_by_func(
          *s, reinterpret_cast<gpointer>(OnGSettingsChanged), nullptr);
      g_object_unref(*s);
      *s = nullptr;
    }
  }
  if (w.bus) {
    if (w.subscription)
      g_dbus_connection_signal_unsubscribe(w.bus, w.subscription);
    w.subscription = 0;
    g_object_unref(w.bus);
    w.bus = nullptr;
  }
  while (g_main_context_iteration(w.ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(w.ctx);
}

// Starts the watcher once and waits until it follows changes (so a change
// right after the handler is set is not lost). Called under no lock.
void EnsureWatcher() {
  Watcher& w = W();
  std::unique_lock<std::mutex> lock(w.mutex);
  if (!w.started) {
    w.started = true;
    w.ready = false;
    w.ctx = g_main_context_new();
    w.loop = g_main_loop_new(w.ctx, FALSE);
    w.thread = std::thread(RunWatcher);
  }
  w.ready_cv.wait(lock, [&] { return w.ready; });
}

}  // namespace

void SetTitleBarPreferencesChangedHandler(void (*handler)(void* user_data),
                                          void* user_data) {
  Watcher& w = W();
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.handler = handler;
    w.user_data = user_data;
  }
  if (handler)
    EnsureWatcher();
}

void ResetTitleBarPreferencesForTesting() {
  Watcher& w = W();
  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.handler = nullptr;
    w.user_data = nullptr;
    if (!w.started)
      return;
    // Through the loop's own context: a quit made before the thread runs
    // the loop would be lost.
    g_main_context_invoke(
        w.ctx,
        [](gpointer loop) -> gboolean {
          g_main_loop_quit(static_cast<GMainLoop*>(loop));
          return G_SOURCE_REMOVE;
        },
        w.loop);
    thread = std::move(w.thread);
  }
  thread.join();
  std::lock_guard<std::mutex> lock(w.mutex);
  g_main_loop_unref(w.loop);
  g_main_context_unref(w.ctx);
  w.loop = nullptr;
  w.ctx = nullptr;
  w.started = false;
  w.ready = false;
  w.last_json.clear();
  std::lock_guard<std::mutex> first(g_first_mutex);
  g_portal_called = false;
}

}  // namespace laufey_common
