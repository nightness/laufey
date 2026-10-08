// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences on Linux (title_bar_linux.cc) against a mock
// xdg-desktop-portal Settings interface on a private D-Bus session bus
// (GTestDBus) and GSettings on its memory backend, with test schemas named
// like GNOME's (org.gnome.desktop.wm.preferences / .interface) compiled
// into a private schema directory:
//   - no portal: nothing from it, without waiting for a portal that isn't
//     there;
//   - GSettings alone (no portal running);
//   - the portal and GSettings disagree on the button layout (Ubuntu GNOME:
//     GSettings appmenu:close, the portal :minimize,maximize,close): the
//     portal wins; a key the portal doesn't answer comes from GSettings; the
//     appearance namespace's colour scheme and accent colour; the system font
//     when titlebar-uses-system-font is set;
//   - the change handler: a SettingChanged that alters the answer fires it
//     once (a burst of changes settles into one call), on the watcher's own
//     thread, not on the thread that runs the default main context; a
//     SettingChanged of an unrelated key, or one that leaves the answer as it
//     was, doesn't; a GSettings change of a key the portal doesn't answer
//     does; a portal that starts after the handler was set is heard.
// Exits 77 (skipped) without dbus-daemon or glib-compile-schemas.

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "laufey_title_bar.h"

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

const char kXml[] =
    "<node>"
    " <interface name='org.freedesktop.portal.Settings'>"
    "  <method name='ReadAll'>"
    "   <arg type='as' direction='in'/>"
    "   <arg type='a{sa{sv}}' direction='out'/>"
    "  </method>"
    "  <property name='version' type='u' access='read'/>"
    "  <signal name='SettingChanged'>"
    "   <arg type='s'/><arg type='s'/><arg type='v'/>"
    "  </signal>"
    " </interface>"
    "</node>";

// The schemas, named like GNOME's, with the keys title_bar_linux.cc reads.
const char kSchemas[] =
    "<schemalist>"
    " <schema id='org.gnome.desktop.wm.preferences'"
    "         path='/org/gnome/desktop/wm/preferences/'>"
    "  <key name='button-layout' type='s'>"
    "   <default>'appmenu:close'</default></key>"
    "  <key name='action-double-click-titlebar' type='s'>"
    "   <default>'toggle-maximize'</default></key>"
    "  <key name='titlebar-font' type='s'>"
    "   <default>'Cantarell Bold 11'</default></key>"
    "  <key name='titlebar-uses-system-font' type='b'>"
    "   <default>false</default></key>"
    "  <key name='theme' type='s'><default>'Adwaita'</default></key>"
    " </schema>"
    " <schema id='org.gnome.desktop.interface'"
    "         path='/org/gnome/desktop/interface/'>"
    "  <key name='color-scheme' type='s'>"
    "   <default>'default'</default></key>"
    "  <key name='accent-color' type='s'><default>'blue'</default></key>"
    "  <key name='font-name' type='s'>"
    "   <default>'Cantarell 11'</default></key>"
    " </schema>"
    "</schemalist>";

GDBusConnection* g_conn = nullptr;  // the mock portal's connection
GDBusNodeInfo* g_info = nullptr;
GMainContext* g_mock_ctx = nullptr;

// What the mock portal answers: namespace -> key -> value (owned).
std::mutex g_mutex;
std::map<std::string, std::map<std::string, GVariant*>> g_values;
std::atomic<int> g_read_alls{0};

void SetValue(const char* ns, const char* key, GVariant* value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  GVariant*& slot = g_values[ns][key];
  if (slot)
    g_variant_unref(slot);
  slot = g_variant_ref_sink(value);
}

GVariant* ReadAll(GVariant* params) {
  GVariantIter* iter = nullptr;
  g_variant_get(params, "(as)", &iter);
  GVariantBuilder all;
  g_variant_builder_init(&all, G_VARIANT_TYPE("a{sa{sv}}"));
  const gchar* ns = nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  while (g_variant_iter_next(iter, "&s", &ns)) {
    auto it = g_values.find(ns);
    if (it == g_values.end())
      continue;
    GVariantBuilder keys;
    g_variant_builder_init(&keys, G_VARIANT_TYPE("a{sv}"));
    for (const auto& [key, value] : it->second)
      g_variant_builder_add(&keys, "{sv}", key.c_str(), value);
    g_variant_builder_add(&all, "{sa{sv}}", ns, &keys);
  }
  g_variant_iter_free(iter);
  return g_variant_new("(a{sa{sv}})", &all);
}

void CallMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                const gchar* method, GVariant* params,
                GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "ReadAll") == 0) {
    g_read_alls++;
    g_dbus_method_invocation_return_value(invocation, ReadAll(params));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar*, GError**, gpointer) {
  return g_variant_new_uint32(2);
}

const GDBusInterfaceVTable kVTable = {CallMethod, GetProperty, nullptr, {}};

void BusName(const char* method, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method,
      strcmp(method, "RequestName") == 0 ? g_variant_new("(su)", name, 0u)
                                         : g_variant_new("(s)", name),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  g_variant_unref(r);
}

// The portal says `key` in `ns` is now `value` (and answers it from now on).
void Change(const char* ns, const char* key, GVariant* value) {
  g_variant_ref_sink(value);
  SetValue(ns, key, value);
  EXPECT(g_dbus_connection_emit_signal(
      g_conn, nullptr, "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Settings", "SettingChanged",
      g_variant_new("(ssv)", ns, key, value), nullptr));
  g_variant_unref(value);
}

std::atomic<int> g_changed{0};
std::atomic<bool> g_changed_on_default_owner{false};
std::thread::id g_main_thread;
std::atomic<bool> g_changed_on_main{false};

void OnChanged(void* user_data) {
  EXPECT(user_data == &g_changed);
  if (std::this_thread::get_id() == g_main_thread)
    g_changed_on_main = true;
  if (g_main_context_is_owner(g_main_context_default()))
    g_changed_on_default_owner = true;
  g_changed++;
}

bool WaitFor(const std::function<bool()>& done, int ms = 5000) {
  for (int i = 0; i < ms / 10; ++i) {
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

std::string Json() {
  return TitleBarPreferencesToJson(ProbeTitleBarPreferences());
}

bool Contains(const std::string& s, const char* part) {
  return s.find(part) != std::string::npos;
}

}  // namespace

int main() {
  g_main_thread = std::this_thread::get_id();
  g_log_set_always_fatal(
      static_cast<GLogLevelFlags>(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL));
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  gchar* compiler = g_find_program_in_path("glib-compile-schemas");
  if (!daemon || !compiler) {
    std::printf("laufey_title_bar_dbus_test: %s missing, skipped\n",
                daemon ? "glib-compile-schemas" : "dbus-daemon");
    return 77;
  }
  g_free(daemon);

  // GSettings: the test schemas, on the memory backend (nothing written to
  // the user's dconf). Before any GSettings use.
  gchar* schemas = g_dir_make_tmp("laufey-tb-XXXXXX", nullptr);
  EXPECT(schemas);
  std::string xml = std::string(schemas) + "/laufey-test.gschema.xml";
  EXPECT(g_file_set_contents(xml.c_str(), kSchemas, -1, nullptr));
  gchar* argv[] = {compiler, schemas, nullptr};
  gint status = -1;
  EXPECT(g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                      nullptr, nullptr, &status, nullptr));
  EXPECT(g_spawn_check_wait_status(status, nullptr));
  g_free(compiler);

  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);  // sets DBUS_SESSION_BUS_ADDRESS

  // --- No portal: nothing from it, at once ----------------------------------
  // GSettings reads the test schemas from the memory backend (set before
  // GLib reads the schema directory, on first use).
  setenv("GSETTINGS_SCHEMA_DIR", schemas, 1);
  setenv("GSETTINGS_BACKEND", "memory", 1);
  // The private bus has no portal (nor an activatable one): the call fails
  // at once instead of waiting out the portal's start-up timeout.
  auto start = std::chrono::steady_clock::now();
  TitleBarSettings none = PortalTitleBarSettings();
  EXPECT(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  EXPECT(!none.button_layout && !none.color_scheme);

  TitleBarSettings gs = GSettingsTitleBarSettings();
  EXPECT(gs.button_layout && *gs.button_layout == "appmenu:close");
  EXPECT(gs.double_click && *gs.double_click == "toggle-maximize");
  EXPECT(gs.color_scheme && *gs.color_scheme == "no-preference");
  EXPECT(gs.accent_color && *gs.accent_color == "#3584e4");
  EXPECT(gs.font && *gs.font == "Cantarell Bold 11");

  GSettings* wm = g_settings_new("org.gnome.desktop.wm.preferences");
  GSettings* iface = g_settings_new("org.gnome.desktop.interface");
  g_settings_set_string(wm, "action-double-click-titlebar", "minimize");
  g_settings_set_boolean(wm, "titlebar-uses-system-font", TRUE);
  g_settings_set_string(iface, "color-scheme", "prefer-dark");

  // --- GSettings alone (no portal running) ---------------------------------
  std::string j = Json();
  EXPECT(Contains(j, "\"left\":[\"appmenu\"],\"right\":[\"close\"]"));
  EXPECT(Contains(j, "\"doubleClick\":\"minimize\""));
  EXPECT(Contains(j, "\"colorScheme\":\"dark\""));
  // titlebar-uses-system-font: the interface font.
  EXPECT(Contains(j, "\"font\":\"Cantarell 11\""));
  EXPECT(Contains(j, "\"source\":\"gsettings\""));

  // --- The mock portal -----------------------------------------------------
  GError* error = nullptr;
  g_conn = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(bus),
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  EXPECT(g_conn);
  g_info = g_dbus_node_info_new_for_xml(kXml, &error);
  EXPECT(g_info);
  std::atomic<bool> ready{false};
  std::thread([&] {
    g_mock_ctx = g_main_context_new();
    g_main_context_push_thread_default(g_mock_ctx);
    EXPECT(g_dbus_connection_register_object(
               g_conn, "/org/freedesktop/portal/desktop",
               g_dbus_node_info_lookup_interface(
                   g_info, "org.freedesktop.portal.Settings"),
               &kVTable, nullptr, nullptr, &error) != 0);
    ready = true;
    GMainLoop* loop = g_main_loop_new(g_mock_ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  EXPECT(WaitFor([&] { return ready.load(); }));

  // The change handler, set while no portal runs yet: a portal that starts
  // later is heard (the subscription follows the well-known name's owner).
  SetTitleBarPreferencesChangedHandler(OnChanged, &g_changed);
  EXPECT(g_changed == 0);

  // Ubuntu GNOME: the portal's button layout differs from GSettings'.
  SetValue("org.gnome.desktop.wm.preferences", "button-layout",
           g_variant_new_string(":minimize,maximize,close"));
  SetValue("org.freedesktop.appearance", "color-scheme",
           g_variant_new_uint32(2));
  SetValue("org.freedesktop.appearance", "accent-color",
           g_variant_new("(ddd)", 0.2, 0.4, 0.6));
  BusName("RequestName", "org.freedesktop.portal.Desktop");

  j = Json();
  EXPECT(Contains(j,
                  "\"left\":[],\"right\":[\"minimize\",\"maximize\","
                  "\"close\"]"));
  EXPECT(Contains(j, "\"side\":\"right\""));
  EXPECT(Contains(j, "\"source\":\"portal\""));
  // The portal didn't answer the double-click action: GSettings'.
  EXPECT(Contains(j, "\"doubleClick\":\"minimize\""));
  // The appearance namespace wins over GSettings' prefer-dark.
  EXPECT(Contains(j, "\"colorScheme\":\"light\""));
  EXPECT(Contains(j, "\"accentColor\":\"#336699\""));

  // --- Live changes ----------------------------------------------------------
  // The button order moves to the left: one change, fired on the watcher's
  // thread.
  int before = g_changed;
  Change("org.gnome.desktop.wm.preferences", "button-layout",
         g_variant_new_string("close,minimize,maximize:"));
  EXPECT(WaitFor([&] { return g_changed == before + 1; }));
  EXPECT(!g_changed_on_main && !g_changed_on_default_owner);
  j = Json();
  EXPECT(Contains(j, "\"left\":[\"close\",\"minimize\",\"maximize\"]"));
  EXPECT(Contains(j, "\"side\":\"left\""));

  // A burst (the colour scheme and the accent colour at once) settles into
  // one call.
  before = g_changed;
  Change("org.freedesktop.appearance", "color-scheme", g_variant_new_uint32(1));
  Change("org.freedesktop.appearance", "accent-color",
         g_variant_new("(ddd)", 1.0, 0.0, 0.0));
  EXPECT(WaitFor([&] { return g_changed == before + 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT(g_changed == before + 1);
  j = Json();
  EXPECT(Contains(j, "\"colorScheme\":\"dark\""));
  EXPECT(Contains(j, "\"accentColor\":\"#ff0000\""));

  // Unrelated keys, and a change that leaves the answer as it was: nothing.
  before = g_changed;
  Change("org.gnome.desktop.wm.preferences", "theme",
         g_variant_new_string("HighContrast"));
  Change("org.gnome.desktop.wm.preferences", "button-layout",
         g_variant_new_string("close,minimize,maximize:"));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  EXPECT(g_changed == before);

  // A GSettings change of a key the portal doesn't answer.
  before = g_changed;
  g_settings_set_string(wm, "action-double-click-titlebar", "toggle-shade");
  EXPECT(WaitFor([&] { return g_changed == before + 1; }));
  EXPECT(Contains(Json(), "\"doubleClick\":\"shade\""));

  // The portal starts answering the double-click action itself: it wins.
  before = g_changed;
  Change("org.gnome.desktop.wm.preferences", "action-double-click-titlebar",
         g_variant_new_string("none"));
  EXPECT(WaitFor([&] { return g_changed == before + 1; }));
  EXPECT(Contains(Json(), "\"doubleClick\":\"none\""));

  // The C ABI's JSON is the probe's.
  char* abi = TitleBarPreferencesJsonForAbi();
  EXPECT(abi && Contains(abi, "\"doubleClick\":\"none\""));
  std::free(abi);

  // A cleared handler isn't called.
  SetTitleBarPreferencesChangedHandler(nullptr, nullptr);
  before = g_changed;
  Change("org.gnome.desktop.wm.preferences", "button-layout",
         g_variant_new_string(":close"));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  EXPECT(g_changed == before);

  ResetTitleBarPreferencesForTesting();
  g_object_unref(wm);
  g_object_unref(iface);
  BusName("ReleaseName", "org.freedesktop.portal.Desktop");
  g_dbus_connection_close_sync(g_conn, nullptr, nullptr);
  g_test_dbus_down(bus);
  g_object_unref(bus);
  std::printf("laufey_title_bar_dbus_test: ok\n");
  return 0;
}
