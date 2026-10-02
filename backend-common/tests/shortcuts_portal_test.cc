// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The XDG GlobalShortcuts portal client (shortcuts_linux.cc, the Wayland
// path) against a mock portal on a private D-Bus session bus (GTestDBus):
// the version probe, the host app registration, CreateSession and
// BindShortcuts through Request/Response objects with the accelerator as the
// preferred trigger, Activated reaching the shortcut handler, a declined
// binding (DENIED), and Session.Close on unregister. Exits 77 (skipped)
// without dbus-daemon.

#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "laufey_io.h"
#include "laufey_system.h"

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
    " <interface name='org.freedesktop.portal.GlobalShortcuts'>"
    "  <method name='CreateSession'>"
    "   <arg type='a{sv}' name='options' direction='in'/>"
    "   <arg type='o' name='handle' direction='out'/>"
    "  </method>"
    "  <method name='BindShortcuts'>"
    "   <arg type='o' name='session_handle' direction='in'/>"
    "   <arg type='a(sa{sv})' name='shortcuts' direction='in'/>"
    "   <arg type='s' name='parent_window' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/>"
    "   <arg type='o' name='handle' direction='out'/>"
    "  </method>"
    "  <signal name='Activated'>"
    "   <arg type='o' name='session_handle'/>"
    "   <arg type='s' name='shortcut_id'/>"
    "   <arg type='t' name='timestamp'/>"
    "   <arg type='a{sv}' name='options'/>"
    "  </signal>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.host.portal.Registry'>"
    "  <method name='Register'>"
    "   <arg type='s' name='app_id' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Session'>"
    "  <method name='Close'/>"
    " </interface>"
    "</node>";

struct Mock {
  GDBusConnection* conn = nullptr;
  GDBusNodeInfo* info = nullptr;
  std::mutex mutex;
  std::string registered_app_id;
  std::vector<std::string> sessions;
  std::vector<std::string> closed;
  std::map<std::string, std::string> triggers;      // id -> preferred_trigger
  std::map<std::string, std::string> descriptions;  // id -> description
  std::map<std::string, std::string> session_of;    // id -> session
  int counter = 0;
};

Mock g_mock;

std::string SenderPath(const gchar* sender) {
  std::string s = sender;
  if (!s.empty() && s[0] == ':')
    s = s.substr(1);
  for (auto& c : s) {
    if (c == '.')
      c = '_';
  }
  return s;
}

void EmitResponse(const std::string& path, guint32 code, GVariant* results) {
  g_dbus_connection_emit_signal(
      g_mock.conn, nullptr, path.c_str(), "org.freedesktop.portal.Request",
      "Response", g_variant_new("(u@a{sv})", code, results), nullptr);
}

void RegisterSessionObject(const std::string& path);

void HandleMethod(GDBusConnection*, const gchar* sender,
                  const gchar* object_path, const gchar* interface,
                  const gchar* method, GVariant* params,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(interface, "org.freedesktop.host.portal.Registry") == 0) {
    const gchar* app_id = nullptr;
    GVariant* options = nullptr;
    g_variant_get(params, "(&s@a{sv})", &app_id, &options);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.registered_app_id = app_id ? app_id : "";
    }
    g_variant_unref(options);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(interface, "org.freedesktop.portal.Session") == 0) {
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.closed.push_back(object_path);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  std::string request_base =
      "/org/freedesktop/portal/desktop/request/" + SenderPath(sender) + "/";
  if (strcmp(method, "CreateSession") == 0) {
    GVariant* options = nullptr;
    g_variant_get(params, "(@a{sv})", &options);
    const gchar* token = nullptr;
    const gchar* session_token = nullptr;
    g_variant_lookup(options, "handle_token", "&s", &token);
    g_variant_lookup(options, "session_handle_token", "&s", &session_token);
    std::string request = request_base + (token ? token : "none");
    std::string session = "/org/freedesktop/portal/desktop/session/" +
                          SenderPath(sender) + "/" +
                          (session_token ? session_token : "none");
    g_variant_unref(options);
    RegisterSessionObject(session);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.sessions.push_back(session);
    }
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(o)", request.c_str()));
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&results, "{sv}", "session_handle",
                          g_variant_new_string(session.c_str()));
    EmitResponse(request, 0, g_variant_builder_end(&results));
    return;
  }
  if (strcmp(method, "BindShortcuts") == 0) {
    const gchar* session = nullptr;
    GVariant* shortcuts = nullptr;
    const gchar* parent = nullptr;
    GVariant* options = nullptr;
    g_variant_get(params, "(&o@a(sa{sv})&s@a{sv})", &session, &shortcuts,
                  &parent, &options);
    const gchar* token = nullptr;
    g_variant_lookup(options, "handle_token", "&s", &token);
    std::string request = request_base + (token ? token : "none");
    bool deny = false;
    GVariantBuilder bound;
    g_variant_builder_init(&bound, G_VARIANT_TYPE("a(sa{sv})"));
    GVariantIter iter;
    g_variant_iter_init(&iter, shortcuts);
    const gchar* id = nullptr;
    GVariant* props = nullptr;
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &props)) {
      const gchar* trigger = nullptr;
      const gchar* description = nullptr;
      g_variant_lookup(props, "preferred_trigger", "&s", &trigger);
      g_variant_lookup(props, "description", "&s", &description);
      {
        std::lock_guard<std::mutex> lock(g_mock.mutex);
        g_mock.triggers[id] = trigger ? trigger : "";
        g_mock.descriptions[id] = description ? description : "";
        g_mock.session_of[id] = session;
      }
      // The user declines anything with a D key in this mock.
      if (strstr(id, "+D"))
        deny = true;
      GVariantBuilder out;
      g_variant_builder_init(&out, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&out, "{sv}", "trigger_description",
                            g_variant_new_string(trigger ? trigger : ""));
      g_variant_builder_add(&bound, "(s@a{sv})", id,
                            g_variant_builder_end(&out));
      g_variant_unref(props);
    }
    g_variant_unref(shortcuts);
    g_variant_unref(options);
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(o)", request.c_str()));
    if (deny) {
      g_variant_builder_clear(&bound);
      EmitResponse(request, 1,
                   g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
      return;
    }
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&results, "{sv}", "shortcuts",
                          g_variant_builder_end(&bound));
    EmitResponse(request, 0, g_variant_builder_end(&results));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar* property, GError**,
                      gpointer) {
  if (strcmp(property, "version") == 0)
    return g_variant_new_uint32(1);
  return nullptr;
}

const GDBusInterfaceVTable kVTable = {HandleMethod, GetProperty, nullptr, {}};

void RegisterSessionObject(const std::string& path) {
  g_dbus_connection_register_object(
      g_mock.conn, path.c_str(),
      g_dbus_node_info_lookup_interface(g_mock.info,
                                        "org.freedesktop.portal.Session"),
      &kVTable, nullptr, nullptr, nullptr);
}

void EmitActivated(const std::string& session, const std::string& id) {
  g_dbus_connection_emit_signal(
      g_mock.conn, nullptr, "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.GlobalShortcuts", "Activated",
      g_variant_new("(ost@a{sv})", session.c_str(), id.c_str(),
                    static_cast<guint64>(1),
                    g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
      nullptr);
  g_dbus_connection_flush_sync(g_mock.conn, nullptr, nullptr);
}

template <typename F>
bool WaitFor(F cond) {
  for (int i = 0; i < 500; i++) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

struct Result {
  std::atomic<int> calls{0};
  std::atomic<int> status{-1};
};

void OnResult(void* data, int status, const char*) {
  auto* r = static_cast<Result*>(data);
  r->status = status;
  r->calls++;
}

std::mutex g_pressed_mutex;
std::vector<std::string> g_pressed;
void OnPress(void*, const char* accelerator) {
  std::lock_guard<std::mutex> lock(g_pressed_mutex);
  g_pressed.push_back(accelerator);
}

}  // namespace

int main() {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_shortcuts_portal_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);

  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);  // sets DBUS_SESSION_BUS_ADDRESS
  setenv("LAUFEY_GLOBAL_SHORTCUTS", "portal", 1);
  setenv("LAUFEY_APP_ID", "dev.laufey.portaltest", 1);

  // The mock portal: its own connection and main loop on a thread.
  std::atomic<bool> mock_ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GError* error = nullptr;
    g_mock.conn = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(bus),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    EXPECT(g_mock.conn);
    g_mock.info = g_dbus_node_info_new_for_xml(kXml, &error);
    EXPECT(g_mock.info);
    for (const char* iface : {"org.freedesktop.portal.GlobalShortcuts",
                              "org.freedesktop.host.portal.Registry"}) {
      EXPECT(g_dbus_connection_register_object(
                 g_mock.conn, "/org/freedesktop/portal/desktop",
                 g_dbus_node_info_lookup_interface(g_mock.info, iface),
                 &kVTable, nullptr, nullptr, &error) != 0);
    }
    GVariant* r = g_dbus_connection_call_sync(
        g_mock.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    EXPECT(r);
    g_variant_unref(r);
    mock_ready = true;
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  EXPECT(WaitFor([&] { return mock_ready.load(); }));

  // Presses are delivered "on the GTK thread"; here, inline.
  SetGtkThread([](std::function<void()> fn) { fn(); }, [] { return true; });
  InstallShortcutPlatform(CreateShortcutPlatformLinux());
  SetShortcutHandler(OnPress, nullptr);

  // The probe found the portal.
  EXPECT(ShortcutCapabilities() == (LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS |
                                    LAUFEY_SYSTEM_CAP_SHORTCUTS_USER_BINDS));
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(g_mock.registered_app_id == "dev.laufey.portaltest");
  }

  // Bind: one session, the accelerator as id + description, the XDG trigger.
  Result ok;
  RegisterShortcut("Ctrl+Shift+K", OnResult, &ok);
  EXPECT(WaitFor([&] { return ok.calls.load() == 1; }));
  EXPECT(ok.status == LAUFEY_SHORTCUT_OK);
  std::string session;
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(g_mock.triggers["Ctrl+Shift+K"] == "CTRL+SHIFT+k");
    EXPECT(g_mock.descriptions["Ctrl+Shift+K"] == "Ctrl+Shift+K");
    session = g_mock.session_of["Ctrl+Shift+K"];
  }
  EXPECT(!session.empty());
  char* list = ListShortcuts();
  EXPECT(std::string(list) == "Ctrl+Shift+K");
  free(list);

  // Activated -> the handler, with the canonical accelerator.
  EmitActivated(session, "Ctrl+Shift+K");
  EXPECT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(g_pressed_mutex);
    return g_pressed.size() == 1;
  }));
  EXPECT(g_pressed[0] == "Ctrl+Shift+K");
  // An Activated for a session we don't own is ignored.
  EmitActivated("/org/freedesktop/portal/desktop/session/x/y", "Ctrl+Shift+K");

  // A binding the user declines.
  Result denied;
  RegisterShortcut("Ctrl+Alt+D", OnResult, &denied);
  EXPECT(WaitFor([&] { return denied.calls.load() == 1; }));
  EXPECT(denied.status == LAUFEY_SHORTCUT_DENIED);
  list = ListShortcuts();
  EXPECT(std::string(list) == "Ctrl+Shift+K");
  free(list);

  // Other trigger syntax.
  Accelerator a;
  EXPECT(ParseAccelerator("Alt+Super+F5", false, &a, nullptr));
  EXPECT(PortalTriggerFor(a) == "ALT+LOGO+F5");
  EXPECT(ParseAccelerator("Ctrl+PageDown", false, &a, nullptr));
  EXPECT(PortalTriggerFor(a) == "CTRL+Page_Down");
  EXPECT(ParseAccelerator("Ctrl+Num7", false, &a, nullptr));
  EXPECT(PortalTriggerFor(a) == "CTRL+KP_7");
  EXPECT(ParseAccelerator("Ctrl+/", false, &a, nullptr));
  EXPECT(PortalTriggerFor(a) == "CTRL+slash");

  // Unregister closes the portal session.
  EXPECT(UnregisterShortcut("Shift+Ctrl+K"));
  EXPECT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    for (const auto& c : g_mock.closed) {
      if (c == session)
        return true;
    }
    return false;
  }));
  EmitActivated(session, "Ctrl+Shift+K");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::lock_guard<std::mutex> lock(g_pressed_mutex);
    EXPECT(g_pressed.size() == 1);
  }

  std::printf("laufey_shortcuts_portal_test: ok\n");
  std::fflush(stdout);
  // The worker and mock threads are detached; skip teardown (g_test_dbus_down
  // would wait for connections those threads hold).
  std::_Exit(0);
}
