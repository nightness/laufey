// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The dock badge on Linux: the com.canonical.Unity.LauncherEntry API. The
// app emits Update(app_uri, {count, count-visible}) on the session bus and
// any dock that listens draws the count on the app's launcher: Ubuntu's dock
// and Dash to Dock (both own com.canonical.Unity in gnome-shell) and
// Plasma's task manager (in plasmashell). No consumer, no badge: the
// backends then keep the "(N) " title prefix (title_badge.cc).
//
// A dock matches app_uri ("application://<app id>.desktop") to a launcher,
// so the app id must name an installed desktop entry. The signal is sent
// from the shared session-bus connection: Plasma clears a badge when its
// sender leaves the bus, which is what should happen when the app quits.

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"
#include "laufey_notifications.h"

#include <gio/gio.h>
#include <sys/stat.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace laufey_common {
namespace {

constexpr gint kCallTimeoutMs = 2000;

std::mutex& BadgeMutex() {
  static std::mutex m;
  return m;
}

bool NameHasOwner(GDBusConnection* conn, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
      nullptr);
  if (!r)
    return false;
  gboolean owned = FALSE;
  g_variant_get(r, "(b)", &owned);
  g_variant_unref(r);
  return owned;
}

bool DesktopEntryInstalled(const std::string& app_id) {
  std::string file = app_id + ".desktop";
  std::string path =
      std::string(g_get_user_data_dir()) + "/applications/" + file;
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    return true;
  for (const gchar* const* d = g_get_system_data_dirs(); d && *d; ++d) {
    path = std::string(*d) + "/applications/" + file;
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      return true;
  }
  return false;
}

// The session bus (shared), or null. Any thread.
GDBusConnection* SessionBus() {
  return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

// Whether a launcher shows counts here; `reason` why not.
bool Available(GDBusConnection* conn, std::string* reason) {
  std::string app_id = LaunchAppId();
  if (!IsValidApplicationId(app_id)) {
    *reason = "no app id to name the launcher by";
    return false;
  }
  if (!conn) {
    *reason = "no session bus";
    return false;
  }
  if (!NameHasOwner(conn, "com.canonical.Unity") &&
      !NameHasOwner(conn, "org.kde.plasmashell")) {
    *reason =
        "no dock reads launcher badges (com.canonical.Unity.LauncherEntry: "
        "Ubuntu's dock, Dash to Dock or Plasma's task manager); the badge is "
        "a \"(N) \" prefix on the window title";
    return false;
  }
  if (!DesktopEntryInstalled(app_id)) {
    *reason = "no " + app_id +
              ".desktop is installed for the dock to match (the .deb and "
              ".rpm install it); the badge is a \"(N) \" prefix on the "
              "window title";
    return false;
  }
  return true;
}

// The count a badge stands for: digits only (at most 18), else -1.
long long BadgeCount(const std::string& badge) {
  if (badge.empty() || badge.size() > 18)
    return -1;
  long long n = 0;
  for (char c : badge) {
    if (c < '0' || c > '9')
      return -1;
    n = n * 10 + (c - '0');
  }
  return n;
}

void EmitUpdate(GDBusConnection* conn, const std::string& app_id,
                long long count, bool visible) {
  std::string uri = "application://" + app_id + ".desktop";
  std::string path =
      "/com/canonical/unity/launcherentry/" + NotificationTagId(app_id);
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&props, "{sv}", "count",
                        g_variant_new_int64(count < 0 ? 0 : count));
  g_variant_builder_add(&props, "{sv}", "count-visible",
                        g_variant_new_boolean(visible));
  g_dbus_connection_emit_signal(
      conn, nullptr, path.c_str(), "com.canonical.Unity.LauncherEntry",
      "Update", g_variant_new("(sa{sv})", uri.c_str(), &props), nullptr);
  g_dbus_connection_flush_sync(conn, nullptr, nullptr);
}

// A count is showing on the launcher (so a fallback must clear it).
bool g_count_shown = false;

}  // namespace

bool LauncherEntryAvailable(std::string* reason) {
  GDBusConnection* conn = SessionBus();
  std::string why;
  bool ok = Available(conn, &why);
  if (conn)
    g_object_unref(conn);
  if (reason)
    *reason = ok ? std::string() : why;
  return ok;
}

bool SetLauncherEntryBadge(const std::string& badge) {
  std::lock_guard<std::mutex> lock(BadgeMutex());
  GDBusConnection* conn = SessionBus();
  std::string why;
  bool available = Available(conn, &why);
  long long count = BadgeCount(badge);
  bool shown = available && (badge.empty() || count >= 0);
  if (available) {
    if (shown)
      EmitUpdate(conn, LaunchAppId(), count, !badge.empty());
    else if (g_count_shown)
      EmitUpdate(conn, LaunchAppId(), 0, false);  // text: the title shows it
    g_count_shown = shown && !badge.empty();
  }
  if (conn)
    g_object_unref(conn);
  return shown;
}

}  // namespace laufey_common
