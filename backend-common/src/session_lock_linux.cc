// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// logind's LockedHint for our session; see the header. Over the shared
// system bus connection (GDBus serves it from its own worker thread, so a
// synchronous call works from any thread without a main loop).

#include "session_lock_linux.h"

#include <gio/gio.h>

#include <cstdlib>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

constexpr char kLogind[] = "org.freedesktop.login1";
constexpr int kTimeoutMs = 500;

GDBusConnection* SystemBus() {
  static GDBusConnection* bus =
      g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
  return bus;
}

// logind's object for a session id ("auto": the caller's session, or the
// user's display session when the caller is in none), or "".
std::string GetSession(GDBusConnection* bus, const char* id) {
  GVariant* reply = g_dbus_connection_call_sync(
      bus, kLogind, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
      "GetSession", g_variant_new("(s)", id), G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, kTimeoutMs, nullptr, nullptr);
  if (!reply)
    return "";
  const gchar* path = nullptr;
  g_variant_get(reply, "(&o)", &path);
  std::string out = path ? path : "";
  g_variant_unref(reply);
  return out;
}

// Our session's object path, found once (retried while it can't be).
std::string SessionPath(GDBusConnection* bus) {
  static std::mutex mutex;
  static std::string path;
  std::lock_guard<std::mutex> lock(mutex);
  if (path.empty()) {
    const char* id = getenv("XDG_SESSION_ID");
    if (id && *id)
      path = GetSession(bus, id);
    if (path.empty())
      path = GetSession(bus, "auto");
  }
  return path;
}

}  // namespace

bool SessionLockedLinux() {
  GDBusConnection* bus = SystemBus();
  if (!bus)
    return false;
  std::string path = SessionPath(bus);
  if (path.empty())
    return false;
  GVariant* reply = g_dbus_connection_call_sync(
      bus, kLogind, path.c_str(), "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.freedesktop.login1.Session", "LockedHint"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, kTimeoutMs,
      nullptr, nullptr);
  if (!reply)
    return false;
  GVariant* value = nullptr;
  g_variant_get(reply, "(v)", &value);
  bool locked = value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
                g_variant_get_boolean(value);
  if (value)
    g_variant_unref(value);
  g_variant_unref(reply);
  return locked;
}

}  // namespace laufey_common
