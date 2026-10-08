// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The XDG GlobalShortcuts portal client (shortcuts_linux.cc, the Wayland
// path) against a mock portal on a private D-Bus session bus (GTestDBus):
// the version probe, the host app registration, CreateSession and
// BindShortcuts through Request/Response objects with the accelerator as the
// preferred trigger, Activated reaching the shortcut handler, a declined
// binding (DENIED), an approval dialog nobody answers (closed through
// Request.Close after LAUFEY_SHORTCUT_PROMPT_TIMEOUT_MS and reported DENIED,
// instead of a registration that never finishes), a CreateSession that never
// answers or answers after LAUFEY_SHORTCUT_SESSION_TIMEOUT_MS (DENIED, and the
// session closed by its predictable handle or by the late Response's), and
// Session.Close on unregister. The mock ties an app id
// to a connection the way xdg-desktop-portal 1.19+ does: Register fails on a
// connection that already talked to the portal, and CreateSession refuses a
// connection without an app id, so the client must register on a connection
// of its own (GTK reads the Settings portal over the shared one first).
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
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
    " <interface name='org.freedesktop.portal.Request'>"
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
  std::vector<std::string> closed_requests;
  std::map<std::string, std::string> triggers;      // id -> preferred_trigger
  std::map<std::string, std::string> descriptions;  // id -> description
  std::map<std::string, std::string> session_of;    // id -> session
  std::set<std::string> talked;                      // senders seen
  std::map<std::string, std::string> app_of;         // sender -> app id
  int counter = 0;
  GMainContext* ctx = nullptr;
  // How CreateSession answers: kCreateNow, kCreateNever (the session exists,
  // no Response ever comes) or kCreateLate (session and Response only after
  // kLateMs, past the client's deadline).
  std::atomic<int> create_mode{0};
};

constexpr int kCreateNow = 0;
constexpr int kCreateNever = 1;
constexpr int kCreateLate = 2;
constexpr guint kLateMs = 1200;

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
void RegisterRequestObject(const std::string& path);

void HandleMethod(GDBusConnection*, const gchar* sender,
                  const gchar* object_path, const gchar* interface,
                  const gchar* method, GVariant* params,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(interface, "org.freedesktop.host.portal.Registry") == 0) {
    const gchar* app_id = nullptr;
    GVariant* options = nullptr;
    g_variant_get(params, "(&s@a{sv})", &app_id, &options);
    g_variant_unref(options);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      if (!g_mock.talked.insert(sender).second) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.portal.Error.Failed",
            "Could not register app ID: Connection already associated with "
            "an application ID");
        return;
      }
      g_mock.registered_app_id = app_id ? app_id : "";
      g_mock.app_of[sender] = g_mock.registered_app_id;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    g_mock.talked.insert(sender);
    if (strcmp(method, "CreateSession") == 0 &&
        g_mock.app_of[sender].empty()) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.freedesktop.portal.Error.NotAllowed",
          "An app id is required");
      return;
    }
  }
  if (strcmp(interface, "org.freedesktop.portal.Request") == 0) {
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.closed_requests.push_back(object_path);
    }
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
    int mode = g_mock.create_mode.load();
    if (mode == kCreateLate) {
      g_dbus_method_invocation_return_value(
          invocation, g_variant_new("(o)", request.c_str()));
      struct Late {
        std::string request, session;
      };
      GSource* timer = g_timeout_source_new(kLateMs);
      g_source_set_callback(
          timer,
          +[](gpointer data) -> gboolean {
            auto* late = static_cast<Late*>(data);
            RegisterSessionObject(late->session);
            {
              std::lock_guard<std::mutex> lock(g_mock.mutex);
              g_mock.sessions.push_back(late->session);
            }
            GVariantBuilder results;
            g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&results, "{sv}", "session_handle",
                                  g_variant_new_string(late->session.c_str()));
            EmitResponse(late->request, 0, g_variant_builder_end(&results));
            return G_SOURCE_REMOVE;
          },
          new Late{request, session},
          [](gpointer data) { delete static_cast<Late*>(data); });
      g_source_attach(timer, g_mock.ctx);
      g_source_unref(timer);
      return;
    }
    RegisterSessionObject(session);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.sessions.push_back(session);
    }
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(o)", request.c_str()));
    if (mode == kCreateNever)
      return;
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
    bool deny = false, unanswered = false;
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
      // The user declines anything with a D key in this mock, and never
      // answers the dialog for an N key.
      if (strstr(id, "+D"))
        deny = true;
      if (strstr(id, "+N"))
        unanswered = true;
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
    if (unanswered) {
      // The dialog stays up: no Response until the client closes it.
      g_variant_builder_clear(&bound);
      RegisterRequestObject(request);
      return;
    }
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

GVariant* GetProperty(GDBusConnection*, const gchar* sender, const gchar*,
                      const gchar*, const gchar* property, GError**,
                      gpointer) {
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    g_mock.talked.insert(sender);
  }
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

void RegisterRequestObject(const std::string& path) {
  g_dbus_connection_register_object(
      g_mock.conn, path.c_str(),
      g_dbus_node_info_lookup_interface(g_mock.info,
                                        "org.freedesktop.portal.Request"),
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
  setenv("LAUFEY_SHORTCUT_PROMPT_TIMEOUT_MS", "300", 1);
  setenv("LAUFEY_SHORTCUT_SESSION_TIMEOUT_MS", "400", 1);

  // The mock portal: its own connection and main loop on a thread.
  std::atomic<bool> mock_ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    g_mock.ctx = ctx;
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

  // What GTK / WebKitGTK do at startup: read from the portal over the
  // process's shared session connection, which ties that connection to the
  // (empty) app id of an unsandboxed process.
  {
    GDBusConnection* shared = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr,
                                             nullptr);
    EXPECT(shared);
    GVariant* v = g_dbus_connection_call_sync(
        shared, "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.freedesktop.portal.GlobalShortcuts",
                      "version"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    EXPECT(v);
    g_variant_unref(v);
  }

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

  // An approval dialog nobody answers: closed, the binding declined, the
  // session closed, nothing registered.
  Result unanswered;
  RegisterShortcut("Ctrl+Alt+N", OnResult, &unanswered);
  EXPECT(WaitFor([&] { return unanswered.calls.load() == 1; }));
  EXPECT(unanswered.status == LAUFEY_SHORTCUT_DENIED);
  EXPECT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    return g_mock.closed_requests.size() == 1 &&
           g_mock.closed.size() == 2;  // the declined and the unanswered one
  }));
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(g_mock.closed_requests[0].find("/request/") != std::string::npos);
  }
  list = ListShortcuts();
  EXPECT(std::string(list) == "Ctrl+Shift+K");
  free(list);

  auto closed_has = [](const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    for (const auto& c : g_mock.closed) {
      if (c == path)
        return true;
    }
    return false;
  };

  // CreateSession never answered: DENIED at the deadline, and the session
  // the portal did create is closed by its predictable handle.
  g_mock.create_mode = kCreateNever;
  Result no_session;
  auto start = std::chrono::steady_clock::now();
  RegisterShortcut("Ctrl+Alt+S", OnResult, &no_session);
  EXPECT(WaitFor([&] { return no_session.calls.load() == 1; }));
  EXPECT(no_session.status == LAUFEY_SHORTCUT_DENIED);
  EXPECT(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
  std::string never_session;
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    never_session = g_mock.sessions.back();
  }
  EXPECT(WaitFor([&] { return closed_has(never_session); }));

  // CreateSession answers after the deadline: DENIED at the deadline, before
  // the session exists; the session the late Response names is closed.
  g_mock.create_mode = kCreateLate;
  Result late;
  size_t sessions_before = 0;
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    sessions_before = g_mock.sessions.size();
  }
  start = std::chrono::steady_clock::now();
  RegisterShortcut("Ctrl+Alt+L", OnResult, &late);
  EXPECT(WaitFor([&] { return late.calls.load() == 1; }));
  EXPECT(late.status == LAUFEY_SHORTCUT_DENIED);
  EXPECT(std::chrono::steady_clock::now() - start <
         std::chrono::milliseconds(kLateMs));
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(g_mock.sessions.size() == sessions_before);  // not created yet
  }
  std::string late_session;
  EXPECT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    if (g_mock.sessions.size() == sessions_before)
      return false;
    late_session = g_mock.sessions.back();
    return true;
  }));
  EXPECT(WaitFor([&] { return closed_has(late_session); }));
  EXPECT(late.calls.load() == 1);  // answered once
  g_mock.create_mode = kCreateNow;
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
