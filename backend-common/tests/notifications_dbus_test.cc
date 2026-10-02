// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux notification platform (notifications_linux.cc) against a mock
// org.freedesktop.Notifications server on a private D-Bus session bus
// (GTestDBus): capabilities and permission with and without a server, the
// Notify arguments (actions with "default", hints, replaces_id, the
// timeout), ActionInvoked as clicks and actions, NotificationClosed and
// CloseNotification, responses for notifications without a live callback,
// and the scheduler: re-arming a persisted schedule at launch (a past-due
// entry fires at once), delivery at the time, the schedule file, cancel.
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "laufey_notifications.h"

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
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='Notify'>"
    "   <arg type='s' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='u' direction='out'/>"
    "  </method>"
    "  <method name='CloseNotification'>"
    "   <arg type='u' direction='in'/>"
    "  </method>"
    "  <method name='GetCapabilities'>"
    "   <arg type='as' direction='out'/>"
    "  </method>"
    "  <signal name='ActionInvoked'>"
    "   <arg type='u'/><arg type='s'/>"
    "  </signal>"
    "  <signal name='NotificationClosed'>"
    "   <arg type='u'/><arg type='u'/>"
    "  </signal>"
    " </interface>"
    "</node>";

struct Notified {
  std::string app_name;
  uint32_t replaces = 0;
  std::string summary;
  std::string body;
  std::vector<std::string> actions;
  std::string desktop_entry;
  int urgency = -1;
  bool suppress_sound = false;
  bool has_image = false;
  int image_w = 0;
  int32_t timeout = 0;
  uint32_t id = 0;
};

struct Mock {
  GDBusConnection* conn = nullptr;
  std::mutex mutex;
  std::vector<Notified> notified;
  std::vector<uint32_t> closed_by_client;
  uint32_t next_id = 100;
};
Mock g_mock;

void Emit(const char* signal, GVariant* params) {
  g_dbus_connection_emit_signal(
      g_mock.conn, nullptr, "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", signal, params, nullptr);
  g_dbus_connection_flush_sync(g_mock.conn, nullptr, nullptr);
}

void HandleMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                  const gchar* method, GVariant* params,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "GetCapabilities") == 0) {
    const gchar* caps[] = {"actions", "body", nullptr};
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(@as)", g_variant_new_strv(caps, -1)));
    return;
  }
  if (strcmp(method, "CloseNotification") == 0) {
    uint32_t id = 0;
    g_variant_get(params, "(u)", &id);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      g_mock.closed_by_client.push_back(id);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Emit("NotificationClosed", g_variant_new("(uu)", id, 3u));
    return;
  }
  if (strcmp(method, "Notify") == 0) {
    Notified n;
    const gchar *app = nullptr, *icon = nullptr, *summary = nullptr,
                *body = nullptr;
    GVariantIter* actions = nullptr;
    GVariant* hints = nullptr;
    g_variant_get(params, "(&su&s&s&sas@a{sv}i)", &app, &n.replaces, &icon,
                  &summary, &body, &actions, &hints, &n.timeout);
    n.app_name = app;
    n.summary = summary;
    n.body = body;
    const gchar* a = nullptr;
    while (g_variant_iter_loop(actions, "&s", &a))
      n.actions.push_back(a);
    g_variant_iter_free(actions);
    const gchar* entry = nullptr;
    if (g_variant_lookup(hints, "desktop-entry", "&s", &entry))
      n.desktop_entry = entry;
    guchar urgency = 0;
    if (g_variant_lookup(hints, "urgency", "y", &urgency))
      n.urgency = urgency;
    gboolean suppress = FALSE;
    if (g_variant_lookup(hints, "suppress-sound", "b", &suppress))
      n.suppress_sound = suppress;
    GVariant* image = g_variant_lookup_value(hints, "image-data", nullptr);
    if (image) {
      n.has_image = true;
      g_variant_get_child(image, 0, "i", &n.image_w);
      g_variant_unref(image);
    }
    g_variant_unref(hints);
    {
      std::lock_guard<std::mutex> lock(g_mock.mutex);
      n.id = n.replaces ? n.replaces : g_mock.next_id++;
      g_mock.notified.push_back(n);
    }
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(u)", n.id));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

const GDBusInterfaceVTable kVTable = {HandleMethod, nullptr, nullptr, {}};

template <typename F>
bool WaitFor(F cond, int ms = 5000) {
  for (int i = 0; i < ms / 10; i++) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
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
bool SawEvent(uint32_t id, int reason, const std::string& action = "") {
  std::lock_guard<std::mutex> lock(g_events_mutex);
  for (const Event& e : g_events) {
    if (e.id == id && e.reason == reason && e.action == action)
      return true;
  }
  return false;
}
size_t CountEvents(uint32_t id, int reason) {
  std::lock_guard<std::mutex> lock(g_events_mutex);
  size_t n = 0;
  for (const Event& e : g_events)
    n += e.id == id && e.reason == reason;
  return n;
}

std::mutex g_responses_mutex;
std::vector<std::string> g_responses;
void OnResponse(void*, const char* json) {
  std::lock_guard<std::mutex> lock(g_responses_mutex);
  g_responses.push_back(json);
}

std::string g_list;
void OnList(void*, const char* json) {
  g_list = json;
}

std::atomic<int> g_perm{-1};
void OnPerm(void*, int status) {
  g_perm = status;
}

Notified LastNotified() {
  std::lock_guard<std::mutex> lock(g_mock.mutex);
  return g_mock.notified.back();
}
size_t NotifiedCount() {
  std::lock_guard<std::mutex> lock(g_mock.mutex);
  return g_mock.notified.size();
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const uint8_t kPng1x1[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

}  // namespace

int main() {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_notifications_dbus_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);

  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);  // sets DBUS_SESSION_BUS_ADDRESS
  char dir_template[] = "/tmp/laufey-notif-test-XXXXXX";
  std::string dir = mkdtemp(dir_template);
  setenv("LAUFEY_DATA_DIR", dir.c_str(), 1);
  setenv("LAUFEY_APP_ID", "dev.laufey.notiftest", 1);
  std::string file = dir + "/laufey-notifications.json";

  // No server yet: nothing works, and a permission query says so.
  EXPECT(NotificationCapabilities() == 0);
  QueryNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS, OnPerm, nullptr);
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  NotificationOptions none;
  none.title = "nobody listens";
  EXPECT(ShowNotification(none, nullptr, nullptr) == 0);

  // A schedule a previous run left: one past due, one later.
  {
    ScheduledNotification due;
    due.tag = "missed";
    due.title = "Missed while away";
    due.at_ms = UnixTimeMs() - 60000;
    ScheduledNotification later;
    later.tag = "tomorrow";
    later.title = "Tomorrow";
    later.at_ms = UnixTimeMs() + 24LL * 3600 * 1000;
    std::ofstream out(file);
    out << SerializeSchedule({due, later});
  }

  // The mock server.
  std::atomic<bool> ready{false};
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
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kXml, &error);
    EXPECT(info);
    EXPECT(g_dbus_connection_register_object(
               g_mock.conn, "/org/freedesktop/Notifications",
               info->interfaces[0], &kVTable, nullptr, nullptr, &error) != 0);
    GVariant* r = g_dbus_connection_call_sync(
        g_mock.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.Notifications", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    EXPECT(r);
    g_variant_unref(r);
    ready = true;
    g_main_loop_run(g_main_loop_new(ctx, FALSE));
  }).detach();
  EXPECT(WaitFor([&] { return ready.load(); }));

  // Launch: the past-due entry fires at once and leaves the file; the later
  // one stays scheduled.
  InitNotificationsAtLaunch();
  EXPECT(WaitFor([&] { return NotifiedCount() == 1; }));
  EXPECT(LastNotified().summary == "Missed while away");
  EXPECT(WaitFor(
      [&] { return ReadFile(file).find("missed") == std::string::npos; }));
  EXPECT(ReadFile(file).find("tomorrow") != std::string::npos);

  EXPECT(NotificationCapabilities() ==
         (LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE |
          LAUFEY_NOTIFICATION_CAP_CLICKS | LAUFEY_NOTIFICATION_CAP_ACTIONS));
  QueryNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS, OnPerm, nullptr);
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_GRANTED);
  RequestNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS, OnPerm,
                                nullptr);
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_GRANTED);

  // Notify: actions with "default", hints, the timeout.
  NotificationOptions o;
  o.title = "Build finished";
  o.body = "3 warnings";
  o.tag = "build";
  o.silent = true;
  o.require_interaction = true;
  o.actions = {{"rebuild", "Rebuild"}};
  o.icon_png.assign(kPng1x1, kPng1x1 + sizeof(kPng1x1));
  uint32_t id = ShowNotification(o, OnEvent, nullptr);
  EXPECT(id > 0);
  EXPECT(WaitFor([&] { return NotifiedCount() == 2; }));
  Notified n = LastNotified();
  EXPECT(n.app_name == "dev.laufey.notiftest");
  EXPECT(n.summary == "Build finished" && n.body == "3 warnings");
  EXPECT(n.replaces == 0);
  EXPECT(n.actions ==
         std::vector<std::string>({"default", "", "rebuild", "Rebuild"}));
  EXPECT(n.desktop_entry == "dev.laufey.notiftest");
  EXPECT(n.urgency == 2 && n.suppress_sound && n.timeout == 0);
  EXPECT(n.has_image && n.image_w == 1);
  EXPECT(WaitFor([&] { return SawEvent(id, LAUFEY_NOTIFICATION_SHOWN); }));

  // Clicks and actions from the server.
  Emit("ActionInvoked", g_variant_new("(us)", n.id, "rebuild"));
  EXPECT(WaitFor(
      [&] { return SawEvent(id, LAUFEY_NOTIFICATION_ACTION, "rebuild"); }));
  Emit("ActionInvoked", g_variant_new("(us)", n.id, "default"));
  EXPECT(WaitFor([&] { return SawEvent(id, LAUFEY_NOTIFICATION_CLICKED); }));
  // Another app's notification id is not ours.
  Emit("ActionInvoked", g_variant_new("(us)", 9999u, "default"));

  // The same tag replaces it on the server.
  o.require_interaction = false;
  o.silent = false;
  uint32_t id2 = ShowNotification(o, OnEvent, nullptr);
  EXPECT(WaitFor([&] { return NotifiedCount() == 3; }));
  Notified n2 = LastNotified();
  EXPECT(n2.replaces == n.id && n2.urgency == 1 && !n2.suppress_sound &&
         n2.timeout == -1);

  // close_notification -> CloseNotification, one CLOSED.
  CloseNotification(id2);
  EXPECT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(g_mock.mutex);
    return !g_mock.closed_by_client.empty() &&
           g_mock.closed_by_client.back() == n2.id;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT(CountEvents(id2, LAUFEY_NOTIFICATION_CLOSED) == 1);

  // The server closes one (dismissed).
  NotificationOptions d;
  d.title = "Dismiss me";
  uint32_t id3 = ShowNotification(d, OnEvent, nullptr);
  EXPECT(WaitFor([&] { return NotifiedCount() == 4; }));
  Emit("NotificationClosed", g_variant_new("(uu)", LastNotified().id, 2u));
  EXPECT(WaitFor([&] { return SawEvent(id3, LAUFEY_NOTIFICATION_CLOSED); }));

  // No live callback: the click is a response, with its data.
  SetNotificationResponseHandler(OnResponse, nullptr);
  NotificationOptions r;
  r.title = "Respond";
  r.tag = "resp";
  r.has_data = true;
  r.data = "{\"id\":7}";
  EXPECT(ShowNotification(r, nullptr, nullptr) > 0);
  EXPECT(WaitFor([&] { return NotifiedCount() == 5; }));
  Emit("ActionInvoked", g_variant_new("(us)", LastNotified().id, "default"));
  EXPECT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(g_responses_mutex);
    return g_responses.size() == 1;
  }));
  EXPECT(g_responses[0] ==
         "{\"tag\":\"resp\",\"action\":null,\"data\":\"{\\\"id\\\":7}\","
         "\"launch\":false}");

  // Scheduling: persisted, listed, delivered at its time, then gone.
  NotificationOptions s;
  s.title = "Soon";
  s.tag = "soon";
  s.schedule_at_ms = UnixTimeMs() + 400;
  uint32_t sid = ShowNotification(s, OnEvent, nullptr);
  EXPECT(sid > 0);
  EXPECT(ReadFile(file).find("\"soon\"") != std::string::npos);
  ListScheduledNotifications(OnList, nullptr);
  EXPECT(g_list.find("\"tag\":\"soon\"") != std::string::npos);
  EXPECT(g_list.find("\"tag\":\"tomorrow\"") != std::string::npos);
  EXPECT(g_list.find("\"soon\"") < g_list.find("\"tomorrow\""));
  EXPECT(WaitFor([&] { return NotifiedCount() == 6; }));
  EXPECT(LastNotified().summary == "Soon");
  EXPECT(WaitFor([&] { return SawEvent(sid, LAUFEY_NOTIFICATION_SHOWN); }));
  EXPECT(ReadFile(file).find("\"soon\"") == std::string::npos);

  // Cancel a pending one: out of the file and the list, never delivered.
  NotificationOptions c;
  c.title = "Cancelled";
  c.tag = "cancel-me";
  c.schedule_at_ms = UnixTimeMs() + 300;
  EXPECT(ShowNotification(c, OnEvent, nullptr) > 0);
  CancelNotification("cancel-me");
  ListScheduledNotifications(OnList, nullptr);
  EXPECT(g_list.find("cancel-me") == std::string::npos);
  EXPECT(ReadFile(file).find("cancel-me") == std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT(NotifiedCount() == 6);

  // A scheduled show replaces a pending one with the same tag.
  CancelNotification("tomorrow");
  ListScheduledNotifications(OnList, nullptr);
  EXPECT(g_list == "[]");

  std::printf("laufey_notifications_dbus_test: ok\n");
  std::fflush(stdout);
  // Threads are detached; skip teardown (see shortcuts_portal_test).
  std::_Exit(0);
}
