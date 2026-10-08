// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux platform-features probe (platform_features_linux.cc) against
// mock services on a private D-Bus session bus (GTestDBus):
//   - no tray watcher, then a watcher that appears late (the probe follows
//     NameOwnerChanged), then one that quits;
//   - the watcher subscription made from a thread that does not own the
//     default main context while another thread runs it (as the runtime
//     thread asks while the UI thread runs the loop), under
//     G_DEBUG=fatal-criticals (ctest sets it; set here too);
//   - the Secret Service activatable (not running), running with its
//     default collection locked, with and without someone to answer the
//     unlock prompt, unlocked, and no session bus at all;
//   - KWallet counted only where Chromium would use it, a KDE desktop
//     (kwalletd running or activatable on GNOME is not), and there only
//     an open wallet answers: kwalletd not running, disabled, or its
//     network wallet closed means no one can hand out the key (mock
//     kwalletd6 / kwalletd5 objects, read without starting anything); the
//     daemon asked is Chromium's own pick (KDE_SESSION_VERSION 6 / 5 /
//     unset), counted only where it answers at its object path (kwalletd6
//     owns org.kde.kwalletd with no /modules/kwalletd), else the one that
//     answers, and the cookie store is pinned to it (--password-store=
//     kwallet6 / kwallet5): a v11 profile started without
//     KDE_SESSION_VERSION is never left to Chromium's failing pick;
//   - the portal interface versions (an interface the portal lacks is
//     absent);
//   - when a session counts as graphical (someone could answer the unlock
//     prompt): only with XDG_SESSION_TYPE x11 / wayland, never from $DISPLAY
//     alone (Xvfb, cron, xvfb-run under systemd), and, where logind answers
//     (a mock org.freedesktop.login1 on the same bus, standing in for the
//     system bus), only for an active x11 / wayland logind session.
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "laufey_platform_features.h"

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
    " <interface name='org.freedesktop.Secret.Collection'>"
    "  <property name='Locked' type='b' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Notification'>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.FileChooser'>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Settings'>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='GetServerInformation'>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "   <arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.login1.Manager'>"
    "  <method name='GetSession'>"
    "   <arg type='s' direction='in'/><arg type='o' direction='out'/>"
    "  </method>"
    "  <method name='GetSessionByPID'>"
    "   <arg type='u' direction='in'/><arg type='o' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.login1.Session'>"
    "  <property name='Type' type='s' access='read'/>"
    "  <property name='Active' type='b' access='read'/>"
    " </interface>"
    " <interface name='org.kde.KWallet'>"
    "  <method name='isEnabled'><arg type='b' direction='out'/></method>"
    "  <method name='networkWallet'><arg type='s' direction='out'/></method>"
    "  <method name='isOpen'>"
    "   <arg type='s' direction='in'/><arg type='b' direction='out'/>"
    "  </method>"
    " </interface>"
    "</node>";

GDBusConnection* g_conn = nullptr;  // the mock services' connection
GDBusNodeInfo* g_info = nullptr;
std::atomic<bool> g_locked{true};
// The mock logind session.
std::atomic<const char*> g_login_type{"x11"};
std::atomic<bool> g_login_active{true};
std::atomic<int> g_login_lookups{0};
// The mock kwalletd.
std::atomic<bool> g_kwallet_enabled{true};
std::atomic<bool> g_kwallet_open{false};
std::atomic<int> g_kwallet_is_open_calls{0};
// The object path the last isOpen was asked at (which daemon answered).
std::mutex g_kwallet_path_mutex;
std::string g_kwallet_path;
constexpr char kLoginSession[] = "/org/freedesktop/login1/session/c7";

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar* interface, const gchar* property, GError**,
                      gpointer) {
  if (strcmp(interface, "org.freedesktop.Secret.Collection") == 0)
    return g_variant_new_boolean(g_locked.load());
  if (strcmp(interface, "org.freedesktop.login1.Session") == 0) {
    if (strcmp(property, "Type") == 0)
      return g_variant_new_string(g_login_type.load());
    return g_variant_new_boolean(g_login_active.load());
  }
  if (strcmp(property, "version") == 0) {
    if (strcmp(interface, "org.freedesktop.portal.Notification") == 0)
      return g_variant_new_uint32(2);
    if (strcmp(interface, "org.freedesktop.portal.FileChooser") == 0)
      return g_variant_new_uint32(4);
    return g_variant_new_uint32(2);  // Settings
  }
  return nullptr;
}

// logind's Manager: every session lookup answers the mock session. The
// notification server names itself.
void CallMethod(GDBusConnection*, const gchar*, const gchar* object_path,
                const gchar*, const gchar* method, GVariant* params,
                GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "isEnabled") == 0) {
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(b)", g_kwallet_enabled.load()));
    return;
  }
  if (strcmp(method, "networkWallet") == 0) {
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(s)", "kdewallet"));
    return;
  }
  if (strcmp(method, "isOpen") == 0) {
    g_kwallet_is_open_calls++;
    {
      std::lock_guard<std::mutex> lock(g_kwallet_path_mutex);
      g_kwallet_path = object_path;
    }
    const gchar* wallet = nullptr;
    g_variant_get(params, "(&s)", &wallet);
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(b)", g_kwallet_open.load() &&
                                             strcmp(wallet, "kdewallet") == 0));
    return;
  }
  if (strcmp(method, "GetServerInformation") == 0) {
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(ssss)", "mock-notifyd", "laufey", "1", "1.2"));
    return;
  }
  g_login_lookups++;
  g_dbus_method_invocation_return_value(invocation,
                                        g_variant_new("(o)", kLoginSession));
}

const GDBusInterfaceVTable kVTable = {CallMethod, GetProperty, nullptr, {}};

void Register(const char* path, const char* iface) {
  GError* error = nullptr;
  EXPECT(g_dbus_connection_register_object(
             g_conn, path, g_dbus_node_info_lookup_interface(g_info, iface),
             &kVTable, nullptr, nullptr, &error) != 0);
}

PlatformFeatures SecretServiceProbe() {
  ResetPlatformFeaturesForTesting();
  PlatformFeatures s;
  ProbeSecretService(&s);
  return s;
}

std::string KWalletPathAsked() {
  std::lock_guard<std::mutex> lock(g_kwallet_path_mutex);
  std::string path = g_kwallet_path;
  g_kwallet_path.clear();
  return path;
}

void BusCall(const char* method, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method,
      strcmp(method, "RequestName") == 0 ? g_variant_new("(su)", name, 0u)
                                         : g_variant_new("(s)", name),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  g_variant_unref(r);
}

// Runs the default main context (where the probe's NameOwnerChanged
// subscription delivers) until `done`, up to ~5 s.
bool SpinUntil(const std::function<bool()>& done) {
  for (int i = 0; i < 500; ++i) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool TrayWatcher() {
  PlatformFeatures f;
  ProbeTray(&f);
  return f.tray_watcher;
}

void WriteService(const std::string& dir, const char* name) {
  std::string path = dir + "/" + name + ".service";
  std::string body =
      std::string("[D-BUS Service]\nName=") + name + "\nExec=/bin/false\n";
  EXPECT(g_file_set_contents(path.c_str(), body.c_str(), -1, nullptr));
}

}  // namespace

std::atomic<int> g_changed{0};
std::atomic<bool> g_changed_on_owner{true};
void OnChanged(void* user_data) {
  EXPECT(user_data == &g_changed);
  // Delivered where the default context runs (the UI thread in a backend).
  if (!g_main_context_is_owner(g_main_context_default()))
    g_changed_on_owner = false;
  g_changed++;
}

// The probe's NameOwnerChanged subscription made from this thread while
// another thread runs the default main context: no GLib critical (fatal
// here), and the owner thread delivers the watcher's owner changes, and the
// platform-features change handler (API 45) with them.
void SubscribeOffTheOwnerThread() {
  ResetPlatformFeaturesForTesting();
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);  // the default context
  std::thread owner([loop] { g_main_loop_run(loop); });
  for (int i = 0; i < 500 && !g_main_loop_is_running(loop); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(g_main_loop_is_running(loop));
  EXPECT(!g_main_context_is_owner(g_main_context_default()));
  // A third thread, like the runtime's, subscribes first: setting the
  // change handler makes the subscription.
  bool first = true;
  std::thread([&first] {
    SetPlatformFeaturesChangedHandler(OnChanged, &g_changed);
    first = TrayWatcher();
  }).join();
  EXPECT(!first);
  EXPECT(g_changed == 0);
  auto wait = [](bool want) {
    for (int i = 0; i < 500; ++i) {
      if (TrayWatcher() == want)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };
  BusCall("RequestName", "org.kde.StatusNotifierWatcher");
  EXPECT(wait(true));  // delivered on the owner thread
  // A tray host appeared: the handler fired (create the tray again).
  for (int i = 0; i < 500 && g_changed < 1; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(g_changed == 1);
  // The reason follows the host only where the appindicator library loads
  // (without it the reason is the missing library, host or not: the webview
  // CI job installs no appindicator).
  PlatformFeatures lib_probe;
  ProbeTray(&lib_probe);
  const bool tray_library = lib_probe.tray_library;
  if (tray_library) {
    char* none = TrayUnavailableReasonForAbi();
    EXPECT(none == nullptr);  // a host: no reason
    free(none);
  }
  BusCall("ReleaseName", "org.kde.StatusNotifierWatcher");
  EXPECT(wait(false));
  for (int i = 0; i < 500 && g_changed < 2; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(g_changed == 2);
  EXPECT(g_changed_on_owner);
  char* reason = TrayUnavailableReasonForAbi();
  EXPECT(reason && strstr(reason, tray_library ? "StatusNotifierWatcher"
                                               : "no tray library"));
  free(reason);
  SetPlatformFeaturesChangedHandler(nullptr, nullptr);
  g_main_loop_quit(loop);
  owner.join();
  g_main_loop_unref(loop);
  ResetPlatformFeaturesForTesting();
}

int main() {
  // Any GLib critical (a failed g_main_context_push_thread_default
  // included) ends the test.
  g_log_set_always_fatal(
      static_cast<GLogLevelFlags>(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL));
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_platform_features_dbus_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);

  // Activatable (never startable) Secret Service and gnome-keyring, as a
  // session where gnome-keyring is installed but not running.
  gchar* services = g_dir_make_tmp("laufey-pf-XXXXXX", nullptr);
  EXPECT(services);
  WriteService(services, "org.freedesktop.secrets");
  WriteService(services, "org.gnome.keyring");
  // KDE's wallet installed (activatable) on a GNOME session.
  WriteService(services, "org.kde.kwalletd5");
  WriteService(services, "org.kde.kwalletd6");

  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_add_service_dir(bus, services);
  g_test_dbus_up(bus);  // sets DBUS_SESSION_BUS_ADDRESS
  // logind lives on the system bus: the same private bus stands in for it,
  // so the machine's own logind never answers (no org.freedesktop.login1
  // until the logind section below owns it).
  setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), 1);
  unsetenv("XDG_SESSION_ID");
  // A headless (ssh / CI) session: no display, no graphical session.
  setenv("XDG_SESSION_TYPE", "tty", 1);
  setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
  for (const char* name : {"DESKTOP_SESSION", "KDE_FULL_SESSION",
                           "KDE_SESSION_VERSION", "GNOME_DESKTOP_SESSION_ID"})
    unsetenv(name);
  unsetenv("DISPLAY");
  unsetenv("WAYLAND_DISPLAY");

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
  // The mock's method and property calls are served on its own thread.
  std::atomic<bool> ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    // Objects registered here deliver on `ctx`.
    Register("/org/freedesktop/secrets/aliases/default",
             "org.freedesktop.Secret.Collection");
    for (const char* iface : {"org.freedesktop.portal.Notification",
                              "org.freedesktop.portal.FileChooser",
                              "org.freedesktop.portal.Settings"}) {
      Register("/org/freedesktop/portal/desktop", iface);
    }
    Register("/org/freedesktop/login1", "org.freedesktop.login1.Manager");
    Register("/org/freedesktop/Notifications", "org.freedesktop.Notifications");
    Register(kLoginSession, "org.freedesktop.login1.Session");
    Register("/modules/kwalletd6", "org.kde.KWallet");
    Register("/modules/kwalletd5", "org.kde.KWallet");
    ready = true;
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  EXPECT(SpinUntil([&] { return ready.load(); }));

  // --- Subscribing off the thread that runs the default context ----------
  SubscribeOffTheOwnerThread();

  // --- Nothing running ----------------------------------------------------
  PlatformFeatures f = ProbePlatformFeatures();
  EXPECT(f.os == "linux");
  EXPECT(f.session_type == "tty");
  EXPECT(f.desktop_hint == "GNOME");
  EXPECT(f.session_bus);
  EXPECT(!f.tray_watcher);
  EXPECT(!f.tray_xembed);  // no X display
  EXPECT(!TrayAvailable(f));
  // The no-host wording, as where the appindicator library loads (without
  // it the reason is the missing library: the webview CI job has none).
  f.tray_library = true;
  EXPECT(TrayUnavailableReason(f).find("XDG_CURRENT_DESKTOP=GNOME") !=
         std::string::npos);
  EXPECT(!f.tray_clicks);
  // Installed, not running: what it would ask, no one here could answer.
  EXPECT(f.secret_service == SecretServiceState::kActivatable);
  EXPECT(!f.secret_prompter);
  EXPECT(NeedsBasicPasswordStore(f));
  // The portal isn't running (nor activatable).
  EXPECT(f.portal_versions.empty());
  // No notification server (Sway with no daemon).
  EXPECT(f.notification_server.empty());
  EXPECT(!f.notification_activatable);
  EXPECT(NotificationUnavailableReason(f).find(
             "nothing owns org.freedesktop.Notifications") !=
         std::string::npos);
  // One claims the name later (plasmashell, say): read live, by name.
  BusCall("RequestName", "org.freedesktop.Notifications");
  f = ProbePlatformFeatures();
  EXPECT(f.notification_server == "mock-notifyd");
  EXPECT(NotificationUnavailableReason(f).empty());
  EXPECT(PlatformFeaturesToJson(f).find(
             "\"notificationServer\":\"mock-notifyd\","
             "\"notificationReason\":null") != std::string::npos);
  BusCall("ReleaseName", "org.freedesktop.Notifications");
  EXPECT(ProbePlatformFeatures().notification_server.empty());

  // --- A tray watcher that starts late, then quits ---------------------------
  BusCall("RequestName", "org.kde.StatusNotifierWatcher");
  EXPECT(SpinUntil([] { return TrayWatcher(); }));
  BusCall("ReleaseName", "org.kde.StatusNotifierWatcher");
  EXPECT(SpinUntil([] { return !TrayWatcher(); }));

  // --- The XEmbed tray: X11 sessions only -----------------------------------
  // In a Wayland session $DISPLAY is Xwayland's: the probe never connects to
  // it (that can start Xwayland). The display here is a socket-less :97, so
  // an X11 probe connects, fails and finds no tray. A Wayland session is one
  // with a Wayland socket that exists (a bound one here).
  std::string wl_dir = "/tmp/laufey-pf-wl-XXXXXX";
  EXPECT(mkdtemp(wl_dir.data()) != nullptr);
  std::string wl_path = wl_dir + "/wayland-test";
  int wl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un wl_addr{};
  wl_addr.sun_family = AF_UNIX;
  std::snprintf(wl_addr.sun_path, sizeof(wl_addr.sun_path), "%s",
                wl_path.c_str());
  EXPECT(bind(wl_fd, reinterpret_cast<sockaddr*>(&wl_addr), sizeof(wl_addr)) ==
         0);
  setenv("DISPLAY", ":97", 1);
  setenv("WAYLAND_DISPLAY", wl_path.c_str(), 1);
  for (const char* session : {"wayland", "tty"}) {
    setenv("XDG_SESSION_TYPE", session, 1);
    ResetPlatformFeaturesForTesting();
    int probes = XEmbedProbeCountForTesting();
    PlatformFeatures t;
    ProbeTray(&t);
    ProbePlatformFeatures();
    EXPECT(!t.tray_xembed);
    EXPECT(XEmbedProbeCountForTesting() == probes);
  }
  unsetenv("WAYLAND_DISPLAY");
  close(wl_fd);
  unlink(wl_path.c_str());
  rmdir(wl_dir.c_str());
  // XDG_SESSION_TYPE=x11, and an Xorg session that says "wayland" with only
  // $DISPLAY (GDM's autologin into XFCE or i3): both are X11 sessions, and
  // the XEmbed tray (i3bar's, xfce4-panel's) is looked for.
  for (const char* session : {"x11", "wayland"}) {
    setenv("XDG_SESSION_TYPE", session, 1);
    ResetPlatformFeaturesForTesting();
    int probes = XEmbedProbeCountForTesting();
    PlatformFeatures t;
    ProbeTray(&t);
    EXPECT(t.session_type == "x11");
    EXPECT(XEmbedProbeCountForTesting() == probes + 1);
  }
  setenv("XDG_SESSION_TYPE", "x11", 1);
  ResetPlatformFeaturesForTesting();
  int probes = XEmbedProbeCountForTesting();
  PlatformFeatures x11;
  ProbeTray(&x11);
  EXPECT(XEmbedProbeCountForTesting() == probes + 1);
  EXPECT(!x11.tray_xembed);
  x11.tray_library = true;  // the no-host wording, as above
  EXPECT(TrayUnavailableReason(x11).find("XEmbed") != std::string::npos);
  setenv("XDG_SESSION_TYPE", "tty", 1);
  unsetenv("DISPLAY");

  // --- The Secret Service running, its keyring locked
  // ---------------------------
  BusCall("RequestName", "org.freedesktop.secrets");
  g_locked = true;
  ResetPlatformFeaturesForTesting();
  PlatformFeatures s;
  ProbeSecretService(&s);
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(!s.secret_prompter);  // a tty session
  EXPECT(NeedsBasicPasswordStore(s));

  // A graphical session, but gnome-keyring's prompter is missing.
  setenv("XDG_SESSION_TYPE", "x11", 1);
  setenv("DISPLAY", ":97", 1);  // nothing there: no XEmbed tray either
  ResetPlatformFeaturesForTesting();
  s = PlatformFeatures();
  ProbeSecretService(&s);
  EXPECT(s.session_type == "x11");
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(!s.secret_prompter);
  EXPECT(NeedsBasicPasswordStore(s));

  // ...and with it: the user unlocks the keyring when asked.
  BusCall("RequestName", "org.gnome.keyring.SystemPrompter");
  ResetPlatformFeaturesForTesting();
  s = PlatformFeatures();
  ProbeSecretService(&s);
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(s.secret_prompter);
  EXPECT(!NeedsBasicPasswordStore(s));

  // --- Graphical only by XDG_SESSION_TYPE, confirmed by logind -------------
  // A display alone is no one in front of it: Xvfb, cron, a systemd service
  // running xvfb-run. XDG_SESSION_TYPE unset gives an unknown session, and
  // the locked keyring falls back to basic even with gcr's prompter there.
  unsetenv("XDG_SESSION_TYPE");
  s = SecretServiceProbe();
  EXPECT(s.session_type == "unknown");
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(!s.secret_prompter);
  EXPECT(NeedsBasicPasswordStore(s));
  setenv("WAYLAND_DISPLAY", "wayland-97", 1);
  s = SecretServiceProbe();
  EXPECT(s.session_type == "unknown");
  EXPECT(!s.secret_prompter);
  unsetenv("WAYLAND_DISPLAY");
  // XDG_SESSION_TYPE set but no display at all: no one can see a prompt.
  setenv("XDG_SESSION_TYPE", "x11", 1);
  unsetenv("DISPLAY");
  EXPECT(!SecretServiceProbe().secret_prompter);
  setenv("DISPLAY", ":97", 1);

  // logind answers (XDG_SESSION_ID's session, else the process's own): it
  // must be an active x11 / wayland session.
  BusCall("RequestName", "org.freedesktop.login1");
  setenv("XDG_SESSION_ID", "c7", 1);
  g_login_type = "tty";  // an ssh session that exported XDG_SESSION_TYPE
  g_login_active = true;
  int lookups = g_login_lookups.load();
  s = SecretServiceProbe();
  EXPECT(g_login_lookups.load() > lookups);  // logind was asked
  EXPECT(s.session_type == "x11");
  EXPECT(!s.secret_prompter);
  EXPECT(NeedsBasicPasswordStore(s));
  g_login_type = "x11";
  g_login_active = false;  // another seat's session is in front
  EXPECT(!SecretServiceProbe().secret_prompter);
  g_login_active = true;
  s = SecretServiceProbe();
  EXPECT(s.secret_prompter);
  EXPECT(!NeedsBasicPasswordStore(s));
  // No XDG_SESSION_ID: the process's own session (GetSessionByPID).
  unsetenv("XDG_SESSION_ID");
  g_login_type = "wayland";
  setenv("XDG_SESSION_TYPE", "wayland", 1);
  unsetenv("DISPLAY");
  setenv("WAYLAND_DISPLAY", "wayland-97", 1);
  lookups = g_login_lookups.load();
  EXPECT(SecretServiceProbe().secret_prompter);
  EXPECT(g_login_lookups.load() > lookups);
  g_login_type = "tty";
  EXPECT(!SecretServiceProbe().secret_prompter);
  unsetenv("WAYLAND_DISPLAY");
  BusCall("ReleaseName", "org.freedesktop.login1");

  // KWallet. On GNOME an activatable kwalletd (KDE apps installed) is not
  // Chromium's store: it picks libsecret there, so a locked gnome-keyring
  // with no one to answer still means basic.
  setenv("XDG_SESSION_TYPE", "tty", 1);
  setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
  s = SecretServiceProbe();
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(!s.secret_prompter);
  EXPECT(!s.kwallet && NeedsBasicPasswordStore(s));
  EXPECT(s.kwallet_state == KWalletState::kNotUsed);
  // Nor is a running one, open wallet and all: on GNOME Chromium uses the
  // Secret Service whatever kwalletd does.
  g_kwallet_open = true;
  for (const char* kwallet : {"org.kde.kwalletd5", "org.kde.kwalletd6"}) {
    BusCall("RequestName", kwallet);
    s = SecretServiceProbe();
    EXPECT(!s.kwallet);
    EXPECT(s.kwallet_state == KWalletState::kNotUsed);
    EXPECT(s.kwallet_service == KWalletService::kNone);
    EXPECT(NeedsBasicPasswordStore(s));
    BusCall("ReleaseName", kwallet);
  }
  g_kwallet_open = false;

  // A KDE desktop (Chromium's rule): KWallet, and only an open wallet
  // answers. A graphical session with someone in front doesn't change that
  // (on Plasma a closed wallet's key request is never answered).
  setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
  setenv("KDE_SESSION_VERSION", "6", 1);
  setenv("XDG_SESSION_TYPE", "wayland", 1);
  setenv("WAYLAND_DISPLAY", "wayland-97", 1);
  BusCall("RequestName", "org.freedesktop.login1");
  g_login_type = "wayland";
  g_login_active = true;
  // kwalletd not running (activatable only): never started by the probe.
  s = SecretServiceProbe();
  EXPECT(s.kwallet);
  EXPECT(s.secret_prompter);  // a person is there; it doesn't matter
  EXPECT(s.kwallet_state == KWalletState::kNotRunning);
  EXPECT(NeedsBasicPasswordStore(s));
  EXPECT(BasicPasswordStoreReason(s).find("kwalletd is not running") !=
         std::string::npos);
  for (const char* name : {"org.kde.kwalletd6", "org.kde.kwalletd5"}) {
    BusCall("RequestName", name);
    // Running, enabled, its wallet closed.
    g_kwallet_enabled = true;
    g_kwallet_open = false;
    int calls = g_kwallet_is_open_calls.load();
    s = SecretServiceProbe();
    EXPECT(s.kwallet_state == KWalletState::kClosed);
    EXPECT(g_kwallet_is_open_calls.load() > calls);
    // The daemon that runs: Chromium's own (kwalletd6) or, without it, the
    // one that answers (the cookie store is pinned to it).
    EXPECT(s.kwallet_service == (strcmp(name, "org.kde.kwalletd6") == 0
                                     ? KWalletService::kKWalletd6
                                     : KWalletService::kKWalletd5));
    EXPECT(NeedsBasicPasswordStore(s));
    EXPECT(BasicPasswordStoreReason(s).find("wallet is closed") !=
           std::string::npos);
    EXPECT(PlatformFeaturesToJson(s).find("\"kwallet\":\"closed\"") !=
           std::string::npos);
    // Open: Chromium gets its key; never basic.
    g_kwallet_open = true;
    s = SecretServiceProbe();
    EXPECT(s.kwallet_state == KWalletState::kOpen);
    EXPECT(!NeedsBasicPasswordStore(s));
    // Disabled.
    g_kwallet_enabled = false;
    s = SecretServiceProbe();
    EXPECT(s.kwallet_state == KWalletState::kDisabled);
    EXPECT(NeedsBasicPasswordStore(s));
    g_kwallet_enabled = true;
    g_kwallet_open = false;
    BusCall("ReleaseName", name);
  }
  // The cookie store's choice on KDE with the wallet closed: basic for a
  // profile without OS-key cookies, the OS store and a wait for one with.
  {
    PasswordStoreChoice c = ChoosePasswordStore(
        nullptr, "", ProfileCookieKeys::kNone, SecretServiceProbe);
    EXPECT(c.store == "basic" && c.append_basic);
    c = ChoosePasswordStore(nullptr, "os", ProfileCookieKeys::kOsKey,
                            SecretServiceProbe);
    EXPECT(c.store == "os" && !c.append_basic && c.wait);
    EXPECT(c.reason.find("KWallet") != std::string::npos);
  }

  // Which kwalletd Chromium asks: KDE_SESSION_VERSION "6" is kwalletd6, "5"
  // kwalletd5, unset KDE 4's org.kde.kwalletd at /modules/kwalletd. Plasma
  // 6's kwalletd6 owns that name too, with no object there: the mock owns
  // org.kde.kwalletd and has no /modules/kwalletd (calls there fail, as
  // with the real daemon). The probe asks Chromium's daemon first, counts a
  // daemon only where it answers at its own path, and the cookie store is
  // pinned to the one that answered.
  {
    g_kwallet_enabled = true;
    g_kwallet_open = true;
    auto probe = [] { return SecretServiceProbe(); };
    // Both kwalletd6 and kwalletd5 run: KDE_SESSION_VERSION picks.
    BusCall("RequestName", "org.kde.kwalletd6");
    BusCall("RequestName", "org.kde.kwalletd5");
    setenv("KDE_SESSION_VERSION", "6", 1);
    KWalletPathAsked();
    s = SecretServiceProbe();
    EXPECT(s.kwallet && s.kwallet_state == KWalletState::kOpen);
    EXPECT(s.kwallet_service == KWalletService::kKWalletd6);
    EXPECT(KWalletPathAsked() == "/modules/kwalletd6");
    PasswordStoreChoice c =
        ChoosePasswordStore(nullptr, "", ProfileCookieKeys::kNone, probe);
    EXPECT(c.store == "os" && !c.append_basic && c.append_store == "kwallet6");
    setenv("KDE_SESSION_VERSION", "5", 1);
    s = SecretServiceProbe();
    EXPECT(s.kwallet_state == KWalletState::kOpen);
    EXPECT(s.kwallet_service == KWalletService::kKWalletd5);
    EXPECT(KWalletPathAsked() == "/modules/kwalletd5");
    c = ChoosePasswordStore(nullptr, "", ProfileCookieKeys::kNone, probe);
    EXPECT(c.store == "os" && c.append_store == "kwallet5");
    BusCall("ReleaseName", "org.kde.kwalletd5");

    // Plasma 6 started without KDE_SESSION_VERSION (ssh, a systemd unit, a
    // wrapper): Chromium would ask org.kde.kwalletd at /modules/kwalletd,
    // which kwalletd6 owns with nothing behind it. That is no daemon; the
    // one that answers is kwalletd6, and Chromium is pinned to it.
    unsetenv("KDE_SESSION_VERSION");
    BusCall("RequestName", "org.kde.kwalletd");
    s = SecretServiceProbe();
    EXPECT(s.kwallet && s.kwallet_state == KWalletState::kOpen);
    EXPECT(s.kwallet_service == KWalletService::kKWalletd6);
    EXPECT(KWalletPathAsked() == "/modules/kwalletd6");
    EXPECT(!NeedsBasicPasswordStore(s));
    c = ChoosePasswordStore(nullptr, "", ProfileCookieKeys::kNone, probe);
    EXPECT(c.store == "os" && !c.append_basic && c.append_store == "kwallet6");
    // The negative control (the bug): a profile holding OS-key (v11)
    // cookies, KDE_SESSION_VERSION missing. Never basic, and the OS store
    // goes to kwalletd6, so Chromium finds the key it wrote them with
    // instead of falling back to basic and deleting them.
    for (ProfileCookieKeys keys :
         {ProfileCookieKeys::kOsKey, ProfileCookieKeys::kUnknown}) {
      c = ChoosePasswordStore(nullptr, "os", keys, probe);
      EXPECT(c.store == "os" && !c.append_basic && !c.wait);
      EXPECT(c.append_store == "kwallet6");
      EXPECT(PasswordStoreWarning(c, keys, "").empty());
    }
    // Its wallet closed: a v11 profile still waits on kwalletd6 (a pinned
    // OS store), never basic; a profile without v11 rows takes basic.
    g_kwallet_open = false;
    c = ChoosePasswordStore(nullptr, "os", ProfileCookieKeys::kOsKey, probe);
    EXPECT(c.store == "os" && !c.append_basic && c.wait);
    EXPECT(c.append_store == "kwallet6");
    c = ChoosePasswordStore(nullptr, "", ProfileCookieKeys::kNone, probe);
    EXPECT(c.store == "basic" && c.append_basic && c.append_store.empty());
    g_kwallet_open = true;

    // Only the name, no daemon behind it: nothing answers. Not usable, so
    // no OS store is claimed for a fresh profile, and a v11 profile keeps
    // the OS store (never basic) without a pin.
    BusCall("ReleaseName", "org.kde.kwalletd6");
    s = SecretServiceProbe();
    EXPECT(s.kwallet && s.kwallet_state == KWalletState::kNotRunning);
    EXPECT(s.kwallet_service == KWalletService::kNone);
    EXPECT(NeedsBasicPasswordStore(s));
    c = ChoosePasswordStore(nullptr, "", ProfileCookieKeys::kNone, probe);
    EXPECT(c.store == "basic" && c.append_basic && c.append_store.empty());
    c = ChoosePasswordStore(nullptr, "os", ProfileCookieKeys::kOsKey, probe);
    EXPECT(c.store == "os" && !c.append_basic && c.wait);
    EXPECT(c.append_store.empty());
    BusCall("ReleaseName", "org.kde.kwalletd");

    // KDE_SESSION_VERSION=5 on a Plasma 6 session (a stale environment):
    // kwalletd5 isn't there, kwalletd6 answers and is pinned.
    setenv("KDE_SESSION_VERSION", "5", 1);
    BusCall("RequestName", "org.kde.kwalletd6");
    s = SecretServiceProbe();
    EXPECT(s.kwallet_service == KWalletService::kKWalletd6);
    c = ChoosePasswordStore(nullptr, "", ProfileCookieKeys::kNone, probe);
    EXPECT(c.store == "os" && c.append_store == "kwallet6");
    // An explicit --password-store is Chromium's to read: nothing added.
    std::string explicit_store = "kwallet5";
    c = ChoosePasswordStore(&explicit_store, "", ProfileCookieKeys::kOsKey,
                            probe);
    EXPECT(c.store == "os" && c.append_store.empty() && !c.append_basic);
    BusCall("ReleaseName", "org.kde.kwalletd6");
    setenv("KDE_SESSION_VERSION", "6", 1);
    g_kwallet_open = false;
  }
  BusCall("ReleaseName", "org.freedesktop.login1");
  unsetenv("WAYLAND_DISPLAY");
  unsetenv("KDE_SESSION_VERSION");
  setenv("XDG_SESSION_TYPE", "tty", 1);
  setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);

  // Unlocked: no prompt at all, even headless.
  g_locked = false;
  setenv("XDG_SESSION_TYPE", "tty", 1);
  unsetenv("DISPLAY");
  ResetPlatformFeaturesForTesting();
  s = PlatformFeatures();
  ProbeSecretService(&s);
  EXPECT(s.secret_service == SecretServiceState::kAvailable);
  EXPECT(!NeedsBasicPasswordStore(s));

  // --- Portal versions
  // ---------------------------------------------------------
  BusCall("RequestName", "org.freedesktop.portal.Desktop");
  ResetPlatformFeaturesForTesting();
  f = ProbePlatformFeatures();
  EXPECT(f.portal_versions.size() == 3);
  EXPECT(f.portal_versions["Notification"] == 2);
  EXPECT(f.portal_versions["FileChooser"] == 4);
  EXPECT(f.portal_versions["Settings"] == 2);
  EXPECT(f.portal_versions.count("GlobalShortcuts") == 0);
  std::string json = PlatformFeaturesToJson(f);
  EXPECT(json.find("\"portalVersions\":{\"FileChooser\":4,"
                   "\"Notification\":2,\"Settings\":2}") != std::string::npos);

  // --- No session bus
  // ------------------------------------------------------------
  std::string address = g_test_dbus_get_bus_address(bus);
  unsetenv("DBUS_SESSION_BUS_ADDRESS");
  setenv("XDG_RUNTIME_DIR", services, 1);  // no "bus" socket in it
  ResetPlatformFeaturesForTesting();
  f = ProbePlatformFeatures();
  EXPECT(!f.session_bus);
  EXPECT(f.secret_service == SecretServiceState::kNoSessionBus);
  // Chromium falls back to basic by itself here: left to it.
  EXPECT(!NeedsBasicPasswordStore(f));
  EXPECT(!f.tray_watcher);
  EXPECT(f.portal_versions.empty());
  setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);

  // Drop the probe's bus before stopping the daemon (g_test_dbus_down waits
  // for the session bus to go), then the service directory it watched.
  ResetPlatformFeaturesForTesting();
  g_test_dbus_down(bus);
  g_object_unref(bus);
  for (const char* name : {"org.freedesktop.secrets", "org.gnome.keyring",
                           "org.kde.kwalletd5", "org.kde.kwalletd6"}) {
    std::string path = std::string(services) + "/" + name + ".service";
    g_unlink(path.c_str());
  }
  g_rmdir(services);
  g_free(services);
  std::printf("laufey_platform_features_dbus_test: OK\n");
  std::fflush(stdout);
  // The mock thread is detached and still holds its connection: end here.
  std::_Exit(0);
}
