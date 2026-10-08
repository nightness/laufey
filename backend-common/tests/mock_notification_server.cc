// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A minimal org.freedesktop.Notifications server for the Linux native e2e
// (scripts/native-e2e-run.sh --menus-notifications), run on the private
// session bus of the test. It accepts Notify / CloseNotification /
// GetCapabilities / GetServerInformation, emits NotificationClosed for a
// CloseNotification, and, for a notification whose body contains
// "[[invoke:<key>]]", emits ActionInvoked(<id>, <key>) shortly after (a
// stand-in for the user clicking that action). Prints "ready" once it owns
// the name.

#include <gio/gio.h>

#include <cstdio>
#include <cstring>
#include <string>

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
    "  <method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "  <method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "  <method name='GetServerInformation'>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "  </method>"
    "  <signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "  <signal name='NotificationClosed'><arg type='u'/><arg "
    "type='u'/></signal>"
    " </interface>"
    "</node>";

GDBusConnection* g_conn = nullptr;
guint32 g_next_id = 1;

struct Invoke {
  guint32 id;
  std::string key;
};

void Emit(const char* signal, GVariant* params) {
  g_dbus_connection_emit_signal(
      g_conn, nullptr, "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", signal, params, nullptr);
}

void HandleMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                  const gchar* method, GVariant* params,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "GetCapabilities") == 0) {
    const gchar* caps[] = {"actions", "body", nullptr};
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(@as)", g_variant_new_strv(caps, -1)));
  } else if (strcmp(method, "GetServerInformation") == 0) {
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(ssss)", "laufey-mock", "laufey", "1", "1.2"));
  } else if (strcmp(method, "CloseNotification") == 0) {
    guint32 id = 0;
    g_variant_get(params, "(u)", &id);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    Emit("NotificationClosed", g_variant_new("(uu)", id, 3u));
  } else if (strcmp(method, "Notify") == 0) {
    const gchar *app = nullptr, *icon = nullptr, *summary = nullptr,
                *body = nullptr;
    guint32 replaces = 0;
    gint32 timeout = 0;
    GVariant* actions = nullptr;
    GVariant* hints = nullptr;
    g_variant_get(params, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon,
                  &summary, &body, &actions, &hints, &timeout);
    guint32 id = replaces ? replaces : g_next_id++;
    std::printf("Notify id=%u app=%s summary=%s\n", id, app, summary);
    std::fflush(stdout);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    const char* marker = strstr(body, "[[invoke:");
    if (marker) {
      std::string rest = marker + 9;
      size_t end = rest.find("]]");
      auto* inv = new Invoke{id, rest.substr(0, end)};
      g_timeout_add(
          200,
          [](gpointer data) -> gboolean {
            auto* inv = static_cast<Invoke*>(data);
            Emit("ActionInvoked",
                 g_variant_new("(us)", inv->id, inv->key.c_str()));
            delete inv;
            return G_SOURCE_REMOVE;
          },
          inv);
    }
    g_variant_unref(actions);
    g_variant_unref(hints);
  } else {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
  }
}

const GDBusInterfaceVTable kVTable = {HandleMethod, nullptr, nullptr, {}};

}  // namespace

int main() {
  GError* error = nullptr;
  g_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!g_conn) {
    std::fprintf(stderr, "mock: no session bus\n");
    return 1;
  }
  GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kXml, &error);
  g_dbus_connection_register_object(g_conn, "/org/freedesktop/Notifications",
                                    info->interfaces[0], &kVTable, nullptr,
                                    nullptr, &error);
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.freedesktop.Notifications", 4u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (!r) {
    std::fprintf(stderr, "mock: RequestName failed\n");
    return 1;
  }
  g_variant_unref(r);
  std::printf("ready\n");
  std::fflush(stdout);
  g_main_loop_run(g_main_loop_new(nullptr, FALSE));
  return 0;
}
