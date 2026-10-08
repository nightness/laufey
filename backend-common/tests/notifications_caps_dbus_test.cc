// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux notification platform follows the server's GetCapabilities
// (notifications_linux.cc), against a mock org.freedesktop.Notifications
// server on a private D-Bus session bus (GTestDBus) that reports
// "body-markup" and no "actions", as a minimal daemon does:
//
//   - capabilities report showing and scheduling but no clicks, no action
//     buttons and no cold start;
//   - Notify carries no actions (not even "default") and the body escaped,
//     since the server reads it as markup;
//   - a server whose GetCapabilities fails gets the body escaped too (it may
//     read markup); one known not to read markup ("body" alone, a new
//     owner of the name) gets it as it is;
//   - with no portal and no `<app id>.desktop`, the transport is
//     org.freedesktop.Notifications, and the facts say why there is no
//     cold start; with no systemd user manager, why a scheduled one isn't
//     posted while the app is closed.
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "laufey_notifications.h"
#include "laufey_platform_features.h"
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
    "  <method name='GetServerInformation'>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "  </method>"
    " </interface>"
    "</node>";

struct Notified {
  std::string body;
  std::vector<std::string> actions;
};

std::mutex g_mutex;
std::vector<Notified> g_notified;

// What GetCapabilities answers: an error, then {"body", "body-markup"}, then
// (a new server) {"body"}.
enum CapsMode { kCapsFail, kCapsMarkup, kCapsPlain };
std::atomic<int> g_caps_mode{kCapsFail};

void HandleMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                  const gchar* method, GVariant* params,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "GetCapabilities") == 0) {
    if (g_caps_mode == kCapsFail) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.freedesktop.DBus.Error.Failed", "not now");
      return;
    }
    const gchar* markup[] = {"body", "body-markup", nullptr};
    const gchar* plain[] = {"body", nullptr};
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@as)", g_variant_new_strv(
                                   g_caps_mode == kCapsPlain ? plain : markup,
                                   -1)));
    return;
  }
  if (strcmp(method, "GetServerInformation") == 0) {
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(ssss)", "minimal", "test", "1", "1.2"));
    return;
  }
  if (strcmp(method, "Notify") == 0) {
    Notified n;
    const gchar *app = nullptr, *icon = nullptr, *summary = nullptr,
                *body = nullptr;
    guint32 replaces = 0;
    gint32 timeout = 0;
    GVariantIter* actions = nullptr;
    g_variant_get(params, "(&su&s&s&sasa{sv}i)", &app, &replaces, &icon,
                  &summary, &body, &actions, nullptr, &timeout);
    n.body = body;
    const gchar* a = nullptr;
    while (g_variant_iter_loop(actions, "&s", &a))
      n.actions.push_back(a);
    g_variant_iter_free(actions);
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_notified.push_back(n);
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 7u));
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

bool Contains(const std::string& s, const std::string& part) {
  return s.find(part) != std::string::npos;
}

size_t NotifiedCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_notified.size();
}

// A notification server on a thread of its own: its connection, once it
// owns org.freedesktop.Notifications.
GDBusConnection* StartServer(GTestDBus* bus) {
  std::atomic<GDBusConnection*> ready{nullptr};
  std::thread([&ready, bus] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GError* error = nullptr;
    GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(bus),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    EXPECT(conn);
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kXml, &error);
    EXPECT(info);
    EXPECT(g_dbus_connection_register_object(
               conn, "/org/freedesktop/Notifications", info->interfaces[0],
               &kVTable, nullptr, nullptr, &error) != 0);
    GVariant* r = g_dbus_connection_call_sync(
        conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.Notifications", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    EXPECT(r);
    g_variant_unref(r);
    ready = conn;
    g_main_loop_run(g_main_loop_new(ctx, FALSE));
  }).detach();
  EXPECT(WaitFor([&] { return ready.load() != nullptr; }));
  return ready.load();
}

}  // namespace

int main() {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_notifications_caps_dbus_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);
  UiTaskDispatcher::Get().Bind([](void (*)(void*), void*) { return false; });

  // Nothing installed: no desktop entry, no service file.
  char data_template[] = "/tmp/laufey-caps-xdg-XXXXXX";
  std::string xdg = mkdtemp(data_template);
  setenv("XDG_DATA_HOME", xdg.c_str(), 1);
  setenv("XDG_DATA_DIRS", (xdg + "/none").c_str(), 1);
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  char dir_template[] = "/tmp/laufey-caps-test-XXXXXX";
  std::string dir = mkdtemp(dir_template);
  setenv("LAUFEY_DATA_DIR", dir.c_str(), 1);
  setenv("LAUFEY_APP_ID", "dev.laufey.capstest", 1);

  GDBusConnection* server = StartServer(bus);

  InitNotificationsAtLaunch();
  NotificationOptions o;
  o.title = "Build";
  o.body = "a < b & c";
  o.actions = {{"rebuild", "Rebuild"}};

  // The server's capabilities can't be read: the body is escaped all the
  // same (it may read markup).
  EXPECT(ShowNotification(o, nullptr, nullptr) > 0);
  EXPECT(WaitFor([] { return NotifiedCount() == 1; }));
  EXPECT(g_notified[0].body == "a &lt; b &amp; c");

  // Now it answers: no "actions" means no clicks, no buttons, no cold
  // start.
  g_caps_mode = kCapsMarkup;
  EXPECT(NotificationCapabilities() ==
         (LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE));
  EXPECT(ShowNotification(o, nullptr, nullptr) > 0);
  EXPECT(WaitFor([] { return NotifiedCount() == 2; }));
  EXPECT(g_notified[1].actions.empty());
  EXPECT(g_notified[1].body == "a &lt; b &amp; c");

  NotificationFacts facts = LinuxNotificationFacts();
  EXPECT(facts.transport == "freedesktop");
  EXPECT(!facts.cold_start);
  EXPECT(Contains(facts.cold_start_reason, "dev.laufey.capstest.desktop"));
  EXPECT(!facts.schedule_while_closed);
  EXPECT(Contains(facts.schedule_reason, "no systemd user manager"));
  EXPECT(facts.server_caps_known &&
         facts.server_caps ==
             std::vector<std::string>({"body", "body-markup"}));

  // The same, through platform_features.
  std::string json = PlatformFeaturesToJson(ProbePlatformFeatures());
  EXPECT(Contains(json, "\"notificationTransport\":\"freedesktop\""));
  EXPECT(Contains(json, "\"notificationColdStart\":false"));
  EXPECT(Contains(json,
                  "\"notificationServerCapabilities\":[\"body\","
                  "\"body-markup\"]"));
  EXPECT(Contains(json, "\"notificationScheduleWhileClosed\":false"));
  // No dock reads launcher badges on this bus.
  EXPECT(Contains(json, "\"badge\":\"title\",\"badgeReason\":\"no dock"));

  // Another server, known to show the body as it is ("body", no
  // "body-markup"): no escaping.
  g_dbus_connection_close_sync(server, nullptr, nullptr);
  g_caps_mode = kCapsPlain;
  GDBusConnection* plain = StartServer(bus);
  EXPECT(WaitFor([&] {
    GVariant* r = g_dbus_connection_call_sync(
        plain, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", "org.freedesktop.Notifications"),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    if (!r)
      return false;
    const char* owner = nullptr;
    g_variant_get(r, "(&s)", &owner);
    bool ours = g_strcmp0(owner, g_dbus_connection_get_unique_name(plain)) == 0;
    g_variant_unref(r);
    return ours;
  }));
  EXPECT(ShowNotification(o, nullptr, nullptr) > 0);
  EXPECT(WaitFor([] { return NotifiedCount() == 3; }));
  EXPECT(g_notified[2].body == "a < b & c");

  std::printf("laufey_notifications_caps_dbus_test: ok\n");
  std::fflush(stdout);
  std::_Exit(0);
}
