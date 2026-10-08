// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux notification platform's portal path, D-Bus activation and
// systemd timers (notifications_linux.cc), against mock services on a
// private D-Bus session bus (GTestDBus): a notification server, the
// xdg-desktop-portal (its host registry and Notification v1) and a systemd
// user manager, with `<app id>.desktop` and `<app id>.service` installed in
// a private XDG data directory.
//
//   - a scheduled launch (`--laufey-notify <id>`) nudges a running app
//     (another process owns the app's name) with ActivateAction
//     "laufey-schedule-due" and leaves the notification to it when it
//     claims it; when the owner answers without claiming it (it never armed
//     it), or doesn't answer, the launch posts it itself, once; with no app
//     running it posts the stored notification through the portal and
//     claims it from the file;
//   - the running app answers that nudge by posting a due entry it never
//     armed;
//   - at launch the app takes its D-Bus name and exports
//     org.freedesktop.Application; capabilities report the cold start and
//     persisted schedules; facts say "portal";
//   - the registry is called with the app id before any notification, and
//     AddNotification carries the title, body, priority and every click as
//     "app.laufey-notification" with an encoded target;
//   - ActivateAction on the app's name is the click: the first one after a
//     D-Bus-activated start is the launch; the portal's own ActionInvoked
//     for the same click is dropped; a live notification gets its click;
//   - Open and Activate on a running app are second launches;
//   - schedule: a transient timer named laufey-<app id>-<tag id>.timer at
//     the time (OnCalendar, UTC) that runs `<exe> --laufey-notify <tag
//     id>` with the app id in its environment; cancel stops it; close
//     removes the portal notification.
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "laufey_launch_config.h"
#include "laufey_notifications.h"
#include "laufey_single_instance.h"
#include "laufey_ui_tasks.h"

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

const char kAppId[] = "dev.laufey.portaltest";

const char kXml[] =
    "<node>"
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='Notify'>"
    "   <arg type='s' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='u' direction='out'/>"
    "  </method>"
    "  <method name='GetCapabilities'>"
    "   <arg type='as' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.host.portal.Registry'>"
    "  <method name='Register'>"
    "   <arg type='s' direction='in'/><arg type='a{sv}' direction='in'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Notification'>"
    "  <method name='AddNotification'>"
    "   <arg type='s' direction='in'/><arg type='a{sv}' direction='in'/>"
    "  </method>"
    "  <method name='RemoveNotification'>"
    "   <arg type='s' direction='in'/>"
    "  </method>"
    "  <signal name='ActionInvoked'>"
    "   <arg type='s'/><arg type='s'/><arg type='s'/><arg type='av'/>"
    "  </signal>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.systemd1.Manager'>"
    "  <method name='StartTransientUnit'>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='a(sv)' direction='in'/>"
    "   <arg type='a(sa(sv))' direction='in'/>"
    "   <arg type='o' direction='out'/>"
    "  </method>"
    "  <method name='StopUnit'>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='o' direction='out'/>"
    "  </method>"
    " </interface>"
    "</node>";

struct PortalNotification {
  std::string id;
  std::string title, body, priority;
  std::string default_action, default_target;
  std::vector<std::string> button_labels, button_actions, button_targets;
  bool has_icon = false;
};

struct Transient {
  std::string name;
  std::string calendar;
  std::vector<std::string> argv;
  std::vector<std::string> env;
  std::string service;
};

struct Mock {
  GDBusConnection* conn = nullptr;
  std::mutex mutex;
  std::vector<std::string> calls;  // method names, in order
  std::vector<std::string> registered;
  std::vector<PortalNotification> added;
  std::vector<std::string> removed;
  std::vector<Transient> started;
  std::vector<std::string> stopped;
  int fdo_notified = 0;
};
Mock g_mock;

std::string Str(GVariant* dict, const char* key) {
  const char* v = nullptr;
  if (g_variant_lookup(dict, key, "&s", &v))
    return v;
  return "";
}

void HandleMethod(GDBusConnection*, const gchar*, const gchar*,
                  const gchar* iface, const gchar* method, GVariant* params,
                  GDBusMethodInvocation* invocation, gpointer) {
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    g_mock.calls.push_back(method);
  }
  if (strcmp(method, "GetCapabilities") == 0) {
    const gchar* caps[] = {"actions", "body", nullptr};
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(@as)", g_variant_new_strv(caps, -1)));
    return;
  }
  if (strcmp(method, "Notify") == 0) {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    g_mock.fdo_notified++;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 1u));
    return;
  }
  if (strcmp(method, "Register") == 0) {
    const char* app = nullptr;
    g_variant_get(params, "(&sa{sv})", &app, nullptr);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.registered.push_back(app);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method, "AddNotification") == 0) {
    PortalNotification n;
    const char* id = nullptr;
    GVariant* dict = nullptr;
    g_variant_get(params, "(&s@a{sv})", &id, &dict);
    n.id = id;
    n.title = Str(dict, "title");
    n.body = Str(dict, "body");
    n.priority = Str(dict, "priority");
    n.default_action = Str(dict, "default-action");
    n.default_target = Str(dict, "default-action-target");
    GVariant* icon = g_variant_lookup_value(dict, "icon", nullptr);
    if (icon) {
      n.has_icon = true;
      g_variant_unref(icon);
    }
    GVariant* buttons =
        g_variant_lookup_value(dict, "buttons", G_VARIANT_TYPE("aa{sv}"));
    if (buttons) {
      GVariantIter iter;
      g_variant_iter_init(&iter, buttons);
      GVariant* b = nullptr;
      while ((b = g_variant_iter_next_value(&iter))) {
        n.button_labels.push_back(Str(b, "label"));
        n.button_actions.push_back(Str(b, "action"));
        n.button_targets.push_back(Str(b, "target"));
        g_variant_unref(b);
      }
      g_variant_unref(buttons);
    }
    g_variant_unref(dict);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.added.push_back(n);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method, "RemoveNotification") == 0) {
    const char* id = nullptr;
    g_variant_get(params, "(&s)", &id);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.removed.push_back(id);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (strcmp(method, "StartTransientUnit") == 0) {
    Transient t;
    const char *name = nullptr, *mode = nullptr;
    GVariant *props = nullptr, *aux = nullptr;
    g_variant_get(params, "(&s&s@a(sv)@a(sa(sv)))", &name, &mode, &props, &aux);
    t.name = name;
    GVariantIter iter;
    g_variant_iter_init(&iter, props);
    const char* key = nullptr;
    GVariant* value = nullptr;
    while (g_variant_iter_next(&iter, "(&sv)", &key, &value)) {
      if (strcmp(key, "TimersCalendar") == 0) {
        GVariantIter cal;
        g_variant_iter_init(&cal, value);
        const char *k = nullptr, *spec = nullptr;
        while (g_variant_iter_next(&cal, "(&s&s)", &k, &spec))
          t.calendar = spec;
      }
      g_variant_unref(value);
    }
    GVariantIter aux_iter;
    g_variant_iter_init(&aux_iter, aux);
    const char* service = nullptr;
    GVariantIter* service_props = nullptr;
    while (
        g_variant_iter_next(&aux_iter, "(&sa(sv))", &service, &service_props)) {
      t.service = service;
      while (g_variant_iter_next(service_props, "(&sv)", &key, &value)) {
        if (strcmp(key, "ExecStart") == 0) {
          GVariantIter exec;
          g_variant_iter_init(&exec, value);
          const char* path = nullptr;
          GVariantIter* argv = nullptr;
          gboolean ignore = FALSE;
          while (g_variant_iter_next(&exec, "(&sasb)", &path, &argv, &ignore)) {
            const char* a = nullptr;
            while (g_variant_iter_next(argv, "&s", &a))
              t.argv.push_back(a);
            g_variant_iter_free(argv);
          }
        } else if (strcmp(key, "Environment") == 0) {
          GVariantIter env;
          g_variant_iter_init(&env, value);
          const char* e = nullptr;
          while (g_variant_iter_next(&env, "&s", &e))
            t.env.push_back(e);
        }
        g_variant_unref(value);
      }
      g_variant_iter_free(service_props);
    }
    g_variant_unref(props);
    g_variant_unref(aux);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.started.push_back(t);
    }
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(o)", "/org/freedesktop/systemd1/job/1"));
    return;
  }
  if (strcmp(method, "StopUnit") == 0) {
    const char *name = nullptr, *mode = nullptr;
    g_variant_get(params, "(&s&s)", &name, &mode);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.stopped.push_back(name);
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.systemd1.NoSuchUnit", name);
    return;
  }
  (void)iface;
  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar* property, GError**, gpointer) {
  if (strcmp(property, "version") == 0)
    return g_variant_new_uint32(1);
  return nullptr;
}

const GDBusInterfaceVTable kVTable = {HandleMethod, GetProperty, nullptr, {}};

template <typename F>
bool WaitFor(F cond, int ms = 5000) {
  for (int i = 0; i < ms / 10; i++) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

std::mutex g_responses_mutex;
std::vector<std::string> g_responses;
void OnResponse(void*, const char* json) {
  std::lock_guard<std::mutex> lock(g_responses_mutex);
  g_responses.push_back(json);
}
size_t ResponseCount() {
  std::lock_guard<std::mutex> lock(g_responses_mutex);
  return g_responses.size();
}

struct Event {
  uint32_t id;
  int reason;
  std::string action;
};
std::mutex g_events_mutex;
std::vector<Event> g_events;
void OnEvent(void*, uint32_t id, int reason, const char* action) {
  std::lock_guard<std::mutex> lock(g_events_mutex);
  g_events.push_back({id, reason, action ? action : ""});
}
size_t CountEvents(uint32_t id, int reason, const std::string& action = "") {
  std::lock_guard<std::mutex> lock(g_events_mutex);
  size_t n = 0;
  for (const Event& e : g_events)
    n += e.id == id && e.reason == reason && e.action == action;
  return n;
}

std::mutex g_second_mutex;
std::vector<std::vector<std::string>> g_second;
void OnSecondInstance(void*, const char* const* args, size_t count,
                      const char* /*cwd*/) {
  std::lock_guard<std::mutex> lock(g_second_mutex);
  g_second.emplace_back(args, args + count);
}

size_t AddedCount() {
  std::lock_guard<std::mutex> lock(g_mock.mutex);
  return g_mock.added.size();
}
PortalNotification LastAdded() {
  std::lock_guard<std::mutex> lock(g_mock.mutex);
  return g_mock.added.back();
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

GDBusConnection* NewClient(GTestDBus* bus) {
  GError* error = nullptr;
  GDBusConnection* c = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(bus),
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  EXPECT(c);
  return c;
}

uint32_t RequestName(GDBusConnection* c, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", name, 4u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  uint32_t reply = 0;
  g_variant_get(r, "(u)", &reply);
  g_variant_unref(r);
  return reply;
}

void CallApp(GDBusConnection* c, const char* method, GVariant* params) {
  GError* error = nullptr;
  GVariant* r = g_dbus_connection_call_sync(
      c, kAppId, "/dev/laufey/portaltest", "org.freedesktop.Application",
      method, params, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, nullptr,
      &error);
  if (!r) {
    std::fprintf(stderr, "%s failed: %s\n", method,
                 error ? error->message : "?");
    std::exit(1);
  }
  g_variant_unref(r);
}

void ActivateAction(GDBusConnection* c, const std::string& target) {
  GVariantBuilder param;
  g_variant_builder_init(&param, G_VARIANT_TYPE("av"));
  g_variant_builder_add(&param, "v", g_variant_new_string(target.c_str()));
  GVariantBuilder pd;
  g_variant_builder_init(&pd, G_VARIANT_TYPE("a{sv}"));
  CallApp(c, "ActivateAction",
          g_variant_new("(sava{sv})", "laufey-notification", &param, &pd));
}

void WriteFile(const std::string& path, const std::string& text) {
  EXPECT(g_file_set_contents(path.c_str(), text.c_str(), -1, nullptr));
}

// Another process that owns the app's name and exports
// org.freedesktop.Application, as a running instance would. What it does
// with a scheduled launch's "laufey-schedule-due" nudge:
enum class OwnerMode {
  kClaim,   // a running app: claims the entry from the file (and posts it)
  kIgnore,  // answers, but never armed the entry: leaves it in the file
  kSilent,  // never answers
};
struct Owner {
  std::atomic<OwnerMode> mode{OwnerMode::kClaim};
  std::string file;
  std::mutex mutex;
  std::vector<std::string> nudges;  // the tag ids it was nudged with
};
Owner g_owner;

const char kOwnerXml[] =
    "<node>"
    " <interface name='org.freedesktop.Application'>"
    "  <method name='ActivateAction'>"
    "   <arg type='s' direction='in'/><arg type='av' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/>"
    "  </method>"
    " </interface>"
    "</node>";

void HandleOwnerMethod(GDBusConnection*, const gchar*, const gchar*,
                       const gchar*, const gchar* method, GVariant* params,
                       GDBusMethodInvocation* invocation, gpointer) {
  const char* name = nullptr;
  GVariantIter* param = nullptr;
  g_variant_get(params, "(&sava{sv})", &name, &param, nullptr);
  GVariant* v = nullptr;
  std::string id;
  if (g_variant_iter_next(param, "v", &v)) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
      id = g_variant_get_string(v, nullptr);
    g_variant_unref(v);
  }
  g_variant_iter_free(param);
  EXPECT(strcmp(method, "ActivateAction") == 0 &&
         strcmp(name, "laufey-schedule-due") == 0);
  {
    std::lock_guard<std::mutex> lock(g_owner.mutex);
    g_owner.nudges.push_back(id);
  }
  switch (g_owner.mode.load()) {
    case OwnerMode::kSilent:
      return;  // the invocation is never answered
    case OwnerMode::kClaim: {
      std::vector<ScheduledNotification> list;
      std::string error;
      gchar* text = nullptr;
      if (g_file_get_contents(g_owner.file.c_str(), &text, nullptr, nullptr)) {
        ParseSchedule(text, &list, &error);
        g_free(text);
      }
      list.erase(std::remove_if(list.begin(), list.end(),
                                [&](const ScheduledNotification& e) {
                                  return NotificationTagId(e.tag) == id;
                                }),
                 list.end());
      WriteFile(g_owner.file, SerializeSchedule(list));
      break;
    }
    case OwnerMode::kIgnore:
      break;
  }
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

const GDBusInterfaceVTable kOwnerVTable = {HandleOwnerMethod, nullptr, nullptr,
                                           {}};

// Starts the owner on a thread of its own: its connection, once it owns the
// app's name.
GDBusConnection* StartOwner(GTestDBus* bus) {
  std::atomic<GDBusConnection*> conn{nullptr};
  std::thread([&conn, bus] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GDBusConnection* c = NewClient(bus);
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kOwnerXml, &error);
    EXPECT(info);
    EXPECT(g_dbus_connection_register_object(c, "/dev/laufey/portaltest",
                                             info->interfaces[0],
                                             &kOwnerVTable, nullptr, nullptr,
                                             &error) != 0);
    EXPECT(RequestName(c, kAppId) == 1);
    conn = c;
    g_main_loop_run(g_main_loop_new(ctx, FALSE));
  }).detach();
  EXPECT(WaitFor([&] { return conn.load() != nullptr; }));
  return conn.load();
}

bool InFile(const std::string& file, const std::string& tag) {
  gchar* text = nullptr;
  if (!g_file_get_contents(file.c_str(), &text, nullptr, nullptr))
    return false;
  bool found = std::string(text).find("\"" + tag + "\"") != std::string::npos;
  g_free(text);
  return found;
}

}  // namespace

int main() {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_notifications_portal_dbus_test: dbus-daemon missing, "
        "skipped\n");
    return 77;
  }
  g_free(daemon);
  UiTaskDispatcher::Get().Bind([](void (*)(void*), void*) { return false; });

  // The app's desktop entry and D-Bus service file, as a .deb installs
  // them, in a private XDG data directory (set before GLib reads it).
  char data_template[] = "/tmp/laufey-portal-xdg-XXXXXX";
  std::string xdg = mkdtemp(data_template);
  EXPECT(g_mkdir_with_parents((xdg + "/applications").c_str(), 0700) == 0);
  EXPECT(g_mkdir_with_parents((xdg + "/dbus-1/services").c_str(), 0700) == 0);
  WriteFile(xdg + "/applications/" + kAppId + ".desktop",
            "[Desktop Entry]\nType=Application\nName=Portal test\n"
            "Exec=/bin/true\n");
  WriteFile(xdg + "/dbus-1/services/" + std::string(kAppId) + ".service",
            "[D-BUS Service]\nName=" + std::string(kAppId) +
                "\nExec=/bin/true --laufey-dbus-activated\n");
  setenv("XDG_DATA_HOME", xdg.c_str(), 1);
  setenv("XDG_DATA_DIRS", (xdg + "/none").c_str(), 1);

  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  char dir_template[] = "/tmp/laufey-portal-test-XXXXXX";
  std::string dir = mkdtemp(dir_template);
  setenv("LAUFEY_DATA_DIR", dir.c_str(), 1);
  setenv("LAUFEY_APP_ID", kAppId, 1);
  std::string file = dir + "/laufey-notifications.json";

  // The mock services: one connection owning every name.
  std::atomic<bool> ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    g_mock.conn = NewClient(bus);
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kXml, &error);
    EXPECT(info);
    struct {
      const char* path;
      int iface;
    } objects[] = {
        {"/org/freedesktop/Notifications", 0},
        {"/org/freedesktop/portal/desktop", 1},
        {"/org/freedesktop/portal/desktop", 2},
        {"/org/freedesktop/systemd1", 3},
    };
    for (const auto& o : objects) {
      EXPECT(g_dbus_connection_register_object(
                 g_mock.conn, o.path, info->interfaces[o.iface], &kVTable,
                 nullptr, nullptr, &error) != 0);
    }
    for (const char* name :
         {"org.freedesktop.Notifications", "org.freedesktop.portal.Desktop",
          "org.freedesktop.systemd1"})
      EXPECT(RequestName(g_mock.conn, name) == 1);
    ready = true;
    g_main_loop_run(g_main_loop_new(ctx, FALSE));
  }).detach();
  EXPECT(WaitFor([&] { return ready.load(); }));
  GDBusConnection* client = NewClient(bus);

  // --- A scheduled launch ---
  ScheduledNotification due;
  due.tag = "daily";
  due.title = "Daily summary";
  due.body = "3 new";
  due.at_ms = UnixTimeMs() - 1000;
  due.has_data = true;
  due.data = "{\"d\":1}";
  WriteFile(file, SerializeSchedule({due}));
  std::string due_id = NotificationTagId("daily");

  // The app runs (another process owns its name): the launch nudges it,
  // and it claims and posts the notification; the launch posts nothing.
  g_owner.file = file;
  GDBusConnection* other = StartOwner(bus);
  EXPECT(RunNotifyLaunch(due_id) == 0);
  EXPECT(AddedCount() == 0);
  EXPECT(!InFile(file, "daily"));
  {
    std::lock_guard<std::mutex> lock(g_owner.mutex);
    EXPECT(g_owner.nudges == std::vector<std::string>({due_id}));
  }
  // The owner answers but never armed the entry (an instance that didn't
  // schedule it, a process that took the name): the launch posts it, once.
  WriteFile(file, SerializeSchedule({due}));
  g_owner.mode = OwnerMode::kIgnore;
  EXPECT(RunNotifyLaunch(due_id) == 0);
  EXPECT(AddedCount() == 1 && LastAdded().id == "daily");
  EXPECT(!InFile(file, "daily"));
  EXPECT(RunNotifyLaunch(due_id) == 0);
  EXPECT(AddedCount() == 1);
  // The owner never answers: after the nudge's timeout the launch posts it.
  WriteFile(file, SerializeSchedule({due}));
  g_owner.mode = OwnerMode::kSilent;
  auto nudged = std::chrono::steady_clock::now();
  EXPECT(RunNotifyLaunch(due_id) == 0);
  EXPECT(std::chrono::steady_clock::now() - nudged < std::chrono::seconds(4));
  EXPECT(AddedCount() == 2 && LastAdded().id == "daily");
  EXPECT(!InFile(file, "daily"));
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    g_mock.added.clear();
  }
  WriteFile(file, SerializeSchedule({due}));
  g_dbus_connection_close_sync(other, nullptr, nullptr);  // the app quit
  EXPECT(WaitFor([&] {
    GVariant* r = g_dbus_connection_call_sync(
        client, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", kAppId),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    gboolean owned = TRUE;
    g_variant_get(r, "(b)", &owned);
    g_variant_unref(r);
    return !owned;
  }));
  // An id that names nothing posts nothing.
  EXPECT(RunNotifyLaunch(NotificationTagId("nothing")) == 0);
  EXPECT(AddedCount() == 0);
  // Not running: posted through the portal and claimed from the file.
  EXPECT(RunNotifyLaunch(due_id) == 0);
  EXPECT(AddedCount() == 1);
  PortalNotification posted = LastAdded();
  EXPECT(posted.id == "daily" && posted.title == "Daily summary" &&
         posted.body == "3 new" && posted.priority == "normal");
  EXPECT(posted.default_action == "app.laufey-notification");
  EXPECT(posted.default_target ==
         EncodeToastArguments("daily", nullptr, &due.data));
  EXPECT(ReadFile(file).find("\"daily\"") == std::string::npos);
  {
    // The registry came first, with the app id; the fdo server got nothing.
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(g_mock.registered.size() == 1 && g_mock.registered[0] == kAppId);
    auto reg = std::find(g_mock.calls.begin(), g_mock.calls.end(), "Register");
    auto add =
        std::find(g_mock.calls.begin(), g_mock.calls.end(), "AddNotification");
    EXPECT(reg < add);
    EXPECT(g_mock.fdo_notified == 0);
  }
  // Again: already claimed.
  EXPECT(RunNotifyLaunch(due_id) == 0);
  EXPECT(AddedCount() == 1);

  // --- The app starts, as D-Bus activation started it ---
  SetDBusActivationLaunchForTesting(true);
  InitNotificationsAtLaunch();
  {
    // It owns its name now.
    GVariant* r = g_dbus_connection_call_sync(
        client, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", kAppId),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    gboolean owned = FALSE;
    g_variant_get(r, "(b)", &owned);
    g_variant_unref(r);
    EXPECT(owned);
  }
  EXPECT(NotificationCapabilities() ==
         (LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE |
          LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS |
          LAUFEY_NOTIFICATION_CAP_CLICKS | LAUFEY_NOTIFICATION_CAP_ACTIONS |
          LAUFEY_NOTIFICATION_CAP_COLD_START));
  NotificationFacts facts = LinuxNotificationFacts();
  EXPECT(facts.transport == "portal");
  EXPECT(facts.cold_start && facts.cold_start_reason.empty());
  EXPECT(facts.schedule_while_closed && facts.schedule_reason.empty());
  EXPECT(facts.server_caps_known && facts.server_caps.size() == 2);

  // The scheduled launch's target carries a MAC under the install's key,
  // which is in the data directory, owner-only: what a cold-started process
  // (another one) verifies with.
  {
    std::string key_path = dir + "/" + kNotificationClickKeyFile;
    struct stat st;
    EXPECT(stat(key_path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::string key;
    EXPECT(LoadOrCreateNotificationClickKey(dir, &key));
    EXPECT(VerifyToastArguments(key, posted.default_target));
  }

  // Forged clicks: any process on the session bus can call ActivateAction
  // on the app's name. Well-formed arguments without a MAC, and with one
  // under another key, are dropped: neither arrives, nor uses up the launch.
  ActivateAction(client, "laufey=1&tag=daily&data=%7B%22d%22%3A2%7D");
  ActivateAction(client, SignToastArguments(std::string(32, 'x'),
                                            "laufey=1&tag=daily&action=x"));
  // Over the size cap.
  ActivateAction(client, "laufey=1&tag=daily&data=" +
                             std::string(kMaxNotificationClickArgumentsBytes,
                                         'x'));

  // The click that started it: buffered until the handler registers, and
  // marked as the launch.
  ActivateAction(client, posted.default_target);
  // The portal's own ActionInvoked for the same click is the same click.
  {
    GVariantBuilder param;
    g_variant_builder_init(&param, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&param, "v",
                          g_variant_new_string(posted.default_target.c_str()));
    g_dbus_connection_emit_signal(
        g_mock.conn, nullptr, "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Notification", "ActionInvoked",
        g_variant_new("(sssav)", kAppId, "daily", "app.laufey-notification",
                      &param),
        nullptr);
    g_dbus_connection_flush_sync(g_mock.conn, nullptr, nullptr);
  }
  SetNotificationResponseHandler(OnResponse, nullptr);
  EXPECT(WaitFor([] { return ResponseCount() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT(ResponseCount() == 1);
  EXPECT(g_responses[0] ==
         "{\"tag\":\"daily\",\"action\":null,\"data\":\"{\\\"d\\\":1}\","
         "\"launch\":true}");

  // A notification shown now: its buttons, the urgent priority; a click on
  // a button reaches its live callback, not the launch.
  NotificationOptions o;
  o.title = "Build finished";
  o.body = "a < b";
  o.tag = "build";
  o.require_interaction = true;
  o.actions = {{"rebuild", "Rebuild"}, {"logs", "Logs"}};
  const uint8_t png[] = {0x89, 'P', 'N', 'G'};
  o.icon_png.assign(png, png + sizeof(png));
  uint32_t id = ShowNotification(o, OnEvent, nullptr);
  EXPECT(id > 0);
  EXPECT(WaitFor([] { return AddedCount() == 2; }));
  PortalNotification n = LastAdded();
  EXPECT(n.id == "build" && n.body == "a < b" && n.priority == "urgent");
  EXPECT(n.has_icon);
  EXPECT(n.button_labels == std::vector<std::string>({"Rebuild", "Logs"}));
  EXPECT(n.button_actions ==
         std::vector<std::string>(2, "app.laufey-notification"));
  EXPECT(n.button_targets[0] ==
         EncodeToastArguments("build", "rebuild", nullptr));
  EXPECT(WaitFor([&] { return CountEvents(id, LAUFEY_NOTIFICATION_SHOWN); }));
  ActivateAction(client, n.button_targets[1]);
  EXPECT(WaitFor(
      [&] { return CountEvents(id, LAUFEY_NOTIFICATION_ACTION, "logs"); }));
  // A forged click on the live notification's button never reaches it.
  ActivateAction(client, "laufey=1&tag=build&action=rebuild");
  ActivateAction(client, n.default_target);
  EXPECT(WaitFor([&] { return CountEvents(id, LAUFEY_NOTIFICATION_CLICKED); }));
  EXPECT(!CountEvents(id, LAUFEY_NOTIFICATION_ACTION, "rebuild"));
  EXPECT(ResponseCount() == 1);
  // Something that isn't a laufey target is ignored.
  ActivateAction(client, "tag=forged");

  // Close: removed from the portal.
  CloseNotification(id);
  EXPECT(WaitFor([] {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    return !g_mock.removed.empty() && g_mock.removed.back() == "build";
  }));

  // Open / Activate on the running app: second launches.
  SecondInstanceUiHooks hooks;
  hooks.post = [](void*, void (*task)(void*), void* data) { task(data); };
  SetSecondInstanceUiHooks(hooks);
  SetSecondInstanceHandler(OnSecondInstance, nullptr);
  {
    const gchar* uris[] = {"acme://open/1", "file:///tmp/a%20b.txt", nullptr};
    GVariantBuilder pd;
    g_variant_builder_init(&pd, G_VARIANT_TYPE("a{sv}"));
    CallApp(client, "Open", g_variant_new("(^asa{sv})", uris, &pd));
    GVariantBuilder pd2;
    g_variant_builder_init(&pd2, G_VARIANT_TYPE("a{sv}"));
    CallApp(client, "Activate", g_variant_new("(a{sv})", &pd2));
  }
  EXPECT(WaitFor([] {
    std::lock_guard<std::mutex> lock(g_second_mutex);
    return g_second.size() == 2;
  }));
  EXPECT(g_second[0] ==
         std::vector<std::string>({"--", "acme://open/1", "/tmp/a b.txt"}));
  EXPECT(g_second[1].empty());

  // A schedule: the persisted entry and its transient timer.
  NotificationOptions s;
  s.title = "Tomorrow";
  s.tag = "tomorrow";
  s.schedule_at_ms = UnixTimeMs() + 24LL * 3600 * 1000 + 250;
  EXPECT(ShowNotification(s, nullptr, nullptr) > 0);
  std::string unit = NotificationTimerUnit(kAppId, "tomorrow");
  EXPECT(unit ==
         std::string("laufey-") + kAppId + "-" + NotificationTagId("tomorrow"));
  Transient t;
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(!g_mock.started.empty());
    t = g_mock.started.back();
  }
  EXPECT(t.name == unit + ".timer" && t.service == unit + ".service");
  EXPECT(t.calendar == SystemdCalendarUtc(s.schedule_at_ms));
  EXPECT(t.argv ==
         std::vector<std::string>({ExecutablePath(), "--laufey-notify",
                                   NotificationTagId("tomorrow")}));
  EXPECT(std::find(t.env.begin(), t.env.end(),
                   std::string("LAUFEY_APP_ID=") + kAppId) != t.env.end());
  EXPECT(std::find(t.env.begin(), t.env.end(), "LAUFEY_DATA_DIR=" + dir) !=
         t.env.end());
  // A near one gets no timer (the running app's own timer is enough).
  size_t timers = 0;
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    timers = g_mock.started.size();
  }
  NotificationOptions soon;
  soon.title = "Soon";
  soon.tag = "soon";
  soon.schedule_at_ms = UnixTimeMs() + 500;
  EXPECT(ShowNotification(soon, nullptr, nullptr) > 0);
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(g_mock.started.size() == timers);
  }
  EXPECT(WaitFor([] { return AddedCount() == 3; }));
  EXPECT(LastAdded().id == "soon");

  // Cancel: out of the file, and its timer is stopped.
  CancelNotification("tomorrow");
  EXPECT(ReadFile(file).find("tomorrow") == std::string::npos);
  {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    EXPECT(std::find(g_mock.stopped.begin(), g_mock.stopped.end(),
                     unit + ".timer") != g_mock.stopped.end());
  }

  // A scheduled launch's nudge to this app (the name's owner) for an entry
  // it never armed (another instance scheduled it): posted, and claimed,
  // before the answer; not a click.
  {
    ScheduledNotification missed;
    missed.tag = "missed";
    missed.title = "Missed";
    missed.at_ms = UnixTimeMs() - 500;
    WriteFile(file, SerializeSchedule({missed}));
    GVariantBuilder param;
    g_variant_builder_init(&param, G_VARIANT_TYPE("av"));
    g_variant_builder_add(
        &param, "v",
        g_variant_new_string(NotificationTagId("missed").c_str()));
    GVariantBuilder pd;
    g_variant_builder_init(&pd, G_VARIANT_TYPE("a{sv}"));
    CallApp(client, "ActivateAction",
            g_variant_new("(sava{sv})", "laufey-schedule-due", &param, &pd));
    EXPECT(AddedCount() == 4 && LastAdded().id == "missed");
    EXPECT(!InFile(file, "missed"));
    EXPECT(ResponseCount() == 1);
    // A genuine click on it (a scheduled notification) arrives; not the
    // launch.
    ActivateAction(client, LastAdded().default_target);
    EXPECT(WaitFor([] { return ResponseCount() == 2; }));
    EXPECT(g_responses[1] ==
           "{\"tag\":\"missed\",\"action\":null,\"data\":null,"
           "\"launch\":false}");
  }

  std::printf("laufey_notifications_portal_dbus_test: ok\n");
  std::fflush(stdout);
  // Threads are detached; skip teardown (see shortcuts_portal_test).
  std::_Exit(0);
}
