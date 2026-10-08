// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux launcher badge (launcher_entry_linux.cc) on a private D-Bus
// session bus (GTestDBus):
//
//   - no dock that reads com.canonical.Unity.LauncherEntry: not shown, with
//     the reason (the backends keep the title prefix);
//   - a dock (com.canonical.Unity owned) but no `<app id>.desktop`: not
//     shown, the reason names the file;
//   - both: a count is an Update(application://<app id>.desktop,
//     {count, count-visible}), an empty badge hides it, and a badge that
//     isn't a number falls back (false) after hiding the count shown;
//   - org.kde.plasmashell counts as a dock too (Plasma's task manager).
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "laufey_backend_common.h"

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

struct Update {
  std::string uri;
  int64_t count = -1;
  bool visible = false;
};
std::mutex g_mutex;
std::vector<Update> g_updates;

void OnUpdate(GDBusConnection*, const gchar*, const gchar*, const gchar*,
              const gchar*, GVariant* params, gpointer) {
  Update u;
  const char* uri = nullptr;
  GVariant* props = nullptr;
  g_variant_get(params, "(&s@a{sv})", &uri, &props);
  u.uri = uri;
  gint64 count = -1;
  if (g_variant_lookup(props, "count", "x", &count))
    u.count = count;
  gboolean visible = FALSE;
  if (g_variant_lookup(props, "count-visible", "b", &visible))
    u.visible = visible;
  g_variant_unref(props);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_updates.push_back(u);
}

size_t UpdateCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_updates.size();
}
Update LastUpdate() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_updates.back();
}

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

GDBusConnection* NewClient(GTestDBus* bus) {
  GDBusConnection* c = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(bus),
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, nullptr);
  EXPECT(c);
  return c;
}

void Own(GDBusConnection* c, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", name, 4u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  g_variant_unref(r);
}

void Release(GDBusConnection* c, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", name),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  g_variant_unref(r);
}

}  // namespace

int main() {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_launcher_entry_dbus_test: dbus-daemon missing, "
        "skipped\n");
    return 77;
  }
  g_free(daemon);
  char data_template[] = "/tmp/laufey-badge-xdg-XXXXXX";
  std::string xdg = mkdtemp(data_template);
  setenv("XDG_DATA_HOME", xdg.c_str(), 1);
  setenv("XDG_DATA_DIRS", (xdg + "/none").c_str(), 1);
  setenv("LAUFEY_APP_ID", "dev.laufey.badgetest", 1);
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);

  // The dock: subscribes to Update from anyone, on its own thread.
  GDBusConnection* dock = nullptr;
  std::atomic<bool> ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    dock = NewClient(bus);
    g_dbus_connection_signal_subscribe(
        dock, nullptr, "com.canonical.Unity.LauncherEntry", "Update", nullptr,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, OnUpdate, nullptr, nullptr);
    ready = true;
    g_main_loop_run(g_main_loop_new(ctx, FALSE));
  }).detach();
  EXPECT(WaitFor([&] { return ready.load(); }));

  // No dock owns a name: not shown.
  std::string reason;
  EXPECT(!LauncherEntryAvailable(&reason));
  EXPECT(Contains(reason, "no dock reads launcher badges"));
  EXPECT(!SetLauncherEntryBadge("3"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT(UpdateCount() == 0);

  // A dock, but no desktop entry to match.
  Own(dock, "com.canonical.Unity");
  EXPECT(!LauncherEntryAvailable(&reason));
  EXPECT(Contains(reason, "dev.laufey.badgetest.desktop"));
  EXPECT(!SetLauncherEntryBadge("3"));

  // Both.
  EXPECT(g_mkdir_with_parents((xdg + "/applications").c_str(), 0700) == 0);
  EXPECT(g_file_set_contents(
      (xdg + "/applications/dev.laufey.badgetest.desktop").c_str(),
      "[Desktop Entry]\nType=Application\nName=Badge\nExec=/bin/true\n", -1,
      nullptr));
  EXPECT(LauncherEntryAvailable(&reason) && reason.empty());
  EXPECT(SetLauncherEntryBadge("3"));
  EXPECT(WaitFor([] { return UpdateCount() == 1; }));
  Update u = LastUpdate();
  EXPECT(u.uri == "application://dev.laufey.badgetest.desktop");
  EXPECT(u.count == 3 && u.visible);
  EXPECT(SetLauncherEntryBadge(""));
  EXPECT(WaitFor([] { return UpdateCount() == 2; }));
  EXPECT(!LastUpdate().visible);
  // Text can't be a count: the title shows it, and the count shown before
  // is hidden.
  EXPECT(SetLauncherEntryBadge("12"));
  EXPECT(WaitFor([] { return UpdateCount() == 3; }));
  EXPECT(LastUpdate().count == 12);
  EXPECT(!SetLauncherEntryBadge("new"));
  EXPECT(WaitFor([] { return UpdateCount() == 4; }));
  EXPECT(!LastUpdate().visible);

  // Plasma's task manager: plasmashell on the bus.
  Release(dock, "com.canonical.Unity");
  EXPECT(!LauncherEntryAvailable(nullptr));
  Own(dock, "org.kde.plasmashell");
  EXPECT(SetLauncherEntryBadge("5"));
  EXPECT(WaitFor([] { return UpdateCount() == 5; }));
  EXPECT(LastUpdate().count == 5 && LastUpdate().visible);

  std::printf("laufey_launcher_entry_dbus_test: ok\n");
  std::fflush(stdout);
  std::_Exit(0);
}
