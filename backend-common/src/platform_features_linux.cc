// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Platform features (API 45), the Linux probe. Everything is read from the
// session itself, never from the desktop's name (the one exception mirrors
// Chromium: whether its cookie store would use KWallet):
//
//   - the tray host: an owner of org.kde.StatusNotifierWatcher, followed
//     through NameOwnerChanged so a watcher that starts late (or restarts)
//     counts at once, or on X11 an XEmbed system tray (the
//     _NET_SYSTEM_TRAY_S<screen> selection, which appindicator falls back
//     to);
//   - the Secret Service: whether org.freedesktop.secrets runs (or can be
//     started), whether its default collection is locked (a property read:
//     no unlock, no prompt), and whether anyone could answer an unlock
//     prompt (a graphical session, and gnome-keyring's prompter where
//     gnome-keyring is the provider); and whether Chromium would use
//     KWallet instead (a KDE desktop by Chromium's own rule), which
//     kwalletd answers (the one Chromium's rule names first, at its own
//     object path), and whether its wallet is open (kwalletd's isEnabled /
//     networkWallet / isOpen: only an open wallet answers Chromium);
//   - the session type: XDG_SESSION_TYPE as set, never made graphical by
//     the display variables alone (Xvfb, cron and systemd services that run
//     `xvfb-run` have a $DISPLAY and no one in front of it); a graphical one
//     names the display that is there (ReportedSessionType: GDM's autologin
//     into Xorg XFCE or i3 says "wayland" with only $DISPLAY). A session is
//     graphical (someone could answer a prompt) only when XDG_SESSION_TYPE
//     says x11 or wayland and, where logind can say, the process's logind
//     session is of that type and active;
//   - the notification server: who owns org.freedesktop.Notifications now
//     (the Notification portal alone is no proof: on Sway with no daemon it
//     is offered with nothing behind it);
//   - the xdg-desktop-portal interface versions.
//
// Every D-Bus call is synchronous with a short timeout. The probe never
// starts the Secret Service or kwalletd (NO_AUTO_START) and never asks
// either to unlock.

#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_notifications.h"
#include "laufey_platform_features.h"

namespace laufey_common {

namespace {

constexpr int kCallTimeoutMs = 1000;
// The first portal call may start xdg-desktop-portal.
constexpr int kPortalStartTimeoutMs = 3000;

constexpr char kWatcherName[] = "org.kde.StatusNotifierWatcher";
constexpr char kSecretsName[] = "org.freedesktop.secrets";
constexpr char kGnomeKeyringName[] = "org.gnome.keyring";
constexpr char kGcrPrompterName[] = "org.gnome.keyring.SystemPrompter";
// kwalletd's names and object paths, as Chromium addresses them, newest
// first (Plasma 6 runs kwalletd6, Plasma 5 kwalletd5, KDE 4 kwalletd). A
// daemon answers only at its own path: kwalletd6 also owns
// org.kde.kwalletd, with no /modules/kwalletd object.
struct KWalletDaemon {
  KWalletService service;
  const char* name;
  const char* path;
};
constexpr KWalletDaemon kKWalletDaemons[] = {
    {KWalletService::kKWalletd6, "org.kde.kwalletd6", "/modules/kwalletd6"},
    {KWalletService::kKWalletd5, "org.kde.kwalletd5", "/modules/kwalletd5"},
    {KWalletService::kKWalletd, "org.kde.kwalletd", "/modules/kwalletd"},
};
constexpr char kKWalletInterface[] = "org.kde.KWallet";
constexpr char kNotificationsName[] = "org.freedesktop.Notifications";
constexpr char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr const char* kPortalInterfaces[] = {
    "Notification",
    "FileChooser",
    "GlobalShortcuts",
    "Settings",
};

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

// XDG_SESSION_TYPE as set ("unknown" when unset), a graphical one corrected
// to the display that is there (ReportedSessionType). Never made graphical
// by $DISPLAY or $WAYLAND_DISPLAY alone: a display (Xvfb, xvfb-run under
// cron or a systemd service, a forwarded X connection) says nothing about
// who is there.
std::string SessionType() {
  return ReportedSessionType(Env("XDG_SESSION_TYPE"), DisplayBackend());
}

bool HasDisplay() {
  return !Env("WAYLAND_DISPLAY").empty() || !Env("DISPLAY").empty();
}

// A session bus is configured: the address variable, or systemd's per-user
// socket. Checked first so that GIO never autolaunches a bus through
// dbus-launch on a bare X display.
bool HasSessionBusAddress() {
  if (!Env("DBUS_SESSION_BUS_ADDRESS").empty())
    return true;
  std::string runtime = Env("XDG_RUNTIME_DIR");
  if (runtime.empty())
    return false;
  struct stat st;
  return stat((runtime + "/bus").c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

struct State {
  std::mutex mutex;
  bool probed = false;  // session facts + secret service
  PlatformFeatures base;
  bool portals_probed = false;
  std::map<std::string, uint32_t> portal_versions;
  GDBusConnection* bus = nullptr;  // owned; kept for the watcher subscription
  guint watcher_sub = 0;
};

State& S() {
  static State* s = new State();
  return *s;
}

// -1 unknown (no subscription), 0 no watcher, 1 a watcher.
std::atomic<int> g_watcher{-1};

// The platform-features change handler (SetPlatformFeaturesChangedHandler).
std::mutex g_changed_mutex;
void (*g_changed_handler)(void*) = nullptr;
void* g_changed_user_data = nullptr;

void FirePlatformFeaturesChanged() {
  void (*handler)(void*) = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_changed_mutex);
    handler = g_changed_handler;
    user_data = g_changed_user_data;
  }
  if (handler)
    handler(user_data);
}

GDBusConnection* SessionBusLocked(State& s) {
  if (s.bus)
    return s.bus;
  if (!HasSessionBusAddress())
    return nullptr;
  GError* error = nullptr;
  s.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  g_clear_error(&error);
  return s.bus;
}

std::set<std::string> ListNames(GDBusConnection* bus, const char* method) {
  std::set<std::string> names;
  GVariant* r = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method, nullptr, G_VARIANT_TYPE("(as)"),
      G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr);
  if (!r)
    return names;
  GVariantIter* iter = nullptr;
  const gchar* name = nullptr;
  g_variant_get(r, "(as)", &iter);
  while (g_variant_iter_next(iter, "&s", &name))
    names.insert(name);
  g_variant_iter_free(iter);
  g_variant_unref(r);
  return names;
}

bool NameHasOwner(GDBusConnection* bus, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
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

// A string or boolean property, read without starting the service. False
// when it can't be read.
bool GetProperty(GDBusConnection* bus, const char* dest, const char* path,
                 const char* iface, const char* prop, const GVariantType* type,
                 GVariant** out) {
  GVariant* r = g_dbus_connection_call_sync(
      bus, dest, path, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", iface, prop), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, kCallTimeoutMs, nullptr, nullptr);
  if (!r)
    return false;
  GVariant* inner = nullptr;
  g_variant_get(r, "(v)", &inner);
  g_variant_unref(r);
  if (!inner || !g_variant_is_of_type(inner, type)) {
    if (inner)
      g_variant_unref(inner);
    return false;
  }
  *out = inner;
  return true;
}

// A private connection to the system bus, given up after kCallTimeoutMs (a
// bus daemon that accepts the socket and never finishes the handshake can't
// hold the probe): an asynchronous connect on `ctx` (this thread's
// thread-default context, a private one), cancelled by a timer.
GDBusConnection* ConnectSystemBus(GMainContext* ctx) {
  GError* error = nullptr;
  gchar* address =
      g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  g_clear_error(&error);
  if (!address)
    return nullptr;
  struct Pending {
    GDBusConnection* conn = nullptr;
    bool done = false;
  } pending;
  GCancellable* cancel = g_cancellable_new();
  g_dbus_connection_new_for_address(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, cancel,
      [](GObject*, GAsyncResult* result, gpointer data) {
        auto* p = static_cast<Pending*>(data);
        p->conn = g_dbus_connection_new_for_address_finish(result, nullptr);
        p->done = true;
      },
      &pending);
  GSource* timer = g_timeout_source_new(kCallTimeoutMs);
  g_source_set_callback(
      timer,
      [](gpointer c) -> gboolean {
        g_cancellable_cancel(G_CANCELLABLE(c));
        return G_SOURCE_REMOVE;
      },
      cancel, nullptr);
  g_source_attach(timer, ctx);
  while (!pending.done)
    g_main_context_iteration(ctx, TRUE);
  g_source_destroy(timer);
  g_source_unref(timer);
  g_object_unref(cancel);
  g_free(address);
  return pending.conn;
}

// The logind part of LogindSessionGraphical, on a system-bus connection.
int LogindSessionOn(GDBusConnection* sys) {
  std::string id = Env("XDG_SESSION_ID");
  GVariant* r = g_dbus_connection_call_sync(
      sys, "org.freedesktop.login1", "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager",
      id.empty() ? "GetSessionByPID" : "GetSession",
      id.empty() ? g_variant_new("(u)", static_cast<guint32>(getpid()))
                 : g_variant_new("(s)", id.c_str()),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, kCallTimeoutMs,
      nullptr, nullptr);
  int result = -1;
  if (r) {
    const gchar* path = nullptr;
    g_variant_get(r, "(&o)", &path);
    GVariant* type = nullptr;
    GVariant* active = nullptr;
    if (GetProperty(sys, "org.freedesktop.login1", path,
                    "org.freedesktop.login1.Session", "Type",
                    G_VARIANT_TYPE_STRING, &type) &&
        GetProperty(sys, "org.freedesktop.login1", path,
                    "org.freedesktop.login1.Session", "Active",
                    G_VARIANT_TYPE_BOOLEAN, &active)) {
      std::string t = g_variant_get_string(type, nullptr);
      result = (t == "x11" || t == "wayland") && g_variant_get_boolean(active)
                   ? 1
                   : 0;
    }
    if (type)
      g_variant_unref(type);
    if (active)
      g_variant_unref(active);
    g_variant_unref(r);
  }
  return result;
}

// What logind says about this process's session: 1 a graphical (x11 /
// wayland) session that is active, 0 not (a tty or ssh session, or an
// inactive one: someone else's seat is in front), -1 logind can't say (no
// system bus, no logind, or the process belongs to no session: a systemd
// user service, a container). The session is XDG_SESSION_ID's, else the
// process's own. Property reads only, on the system bus, never starting
// logind.
int LogindSessionGraphical() {
  // A private context for the connect (and whatever the connection queues
  // on it), drained before it goes.
  GMainContext* ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  int result = -1;
  if (GDBusConnection* sys = ConnectSystemBus(ctx)) {
    result = LogindSessionOn(sys);
    g_dbus_connection_close_sync(sys, nullptr, nullptr);
    g_object_unref(sys);
  }
  while (g_main_context_iteration(ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  return result;
}

// Someone could answer a prompt here: XDG_SESSION_TYPE says x11 or wayland,
// a display variable is set, and logind (where it can say) agrees that this
// is an active graphical session.
bool GraphicalSession(const std::string& session) {
  if (session != "x11" && session != "wayland")
    return false;
  if (!HasDisplay())
    return false;
  return LogindSessionGraphical() != 0;
}

// A method of kwalletd's org.kde.KWallet returning one value of `type`,
// never starting it. Null when it fails.
GVariant* KWalletCall(GDBusConnection* bus, const char* name, const char* path,
                      const char* method, GVariant* args, const char* type) {
  GVariant* r = g_dbus_connection_call_sync(
      bus, name, path, kKWalletInterface, method, args, G_VARIANT_TYPE(type),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, kCallTimeoutMs, nullptr, nullptr);
  return r;
}

// KWallet's state, from the daemon Chromium's choice names (`chromium`,
// ChromiumKWalletService) when it answers, else from the first other one
// that does: enabled, and whether its network wallet (the one Chromium
// opens) is open. A daemon counts only when isEnabled answers at its object
// path: a name with nothing there (kwalletd6's org.kde.kwalletd) is no
// daemon, as Chromium finds. `*service` is the daemon that answered (kNone:
// none did), the one the cookie store is pinned to. Reads only: nothing is
// opened, nothing prompts, nothing is started.
KWalletState ProbeKWallet(GDBusConnection* bus,
                          const std::set<std::string>& owned,
                          KWalletService chromium, KWalletService* service) {
  *service = KWalletService::kNone;
  std::vector<const KWalletDaemon*> order;
  for (const auto& daemon : kKWalletDaemons) {
    if (daemon.service == chromium)
      order.insert(order.begin(), &daemon);
    else
      order.push_back(&daemon);
  }
  for (const KWalletDaemon* daemon : order) {
    const char* name = daemon->name;
    const char* path = daemon->path;
    if (!owned.count(name))
      continue;
    GVariant* r = KWalletCall(bus, name, path, "isEnabled", nullptr, "(b)");
    if (!r)
      continue;  // no object there (or no answer): not this daemon
    gboolean enabled = FALSE;
    g_variant_get(r, "(b)", &enabled);
    g_variant_unref(r);
    *service = daemon->service;
    if (!enabled)
      return KWalletState::kDisabled;
    std::string wallet;
    r = KWalletCall(bus, name, path, "networkWallet", nullptr, "(s)");
    if (r) {
      const gchar* w = nullptr;
      g_variant_get(r, "(&s)", &w);
      wallet = w ? w : "";
      g_variant_unref(r);
    }
    gboolean open = FALSE;
    if (!wallet.empty()) {
      r = KWalletCall(bus, name, path, "isOpen",
                      g_variant_new("(s)", wallet.c_str()), "(b)");
      if (r) {
        g_variant_get(r, "(b)", &open);
        g_variant_unref(r);
      }
    }
    return open ? KWalletState::kOpen : KWalletState::kClosed;
  }
  return KWalletState::kNotRunning;
}

// The default collection's Locked property. -1 when it can't be read (no
// default collection: creating one is a prompt too).
int DefaultCollectionLocked(GDBusConnection* bus) {
  GVariant* r = g_dbus_connection_call_sync(
      bus, kSecretsName, "/org/freedesktop/secrets/aliases/default",
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.freedesktop.Secret.Collection", "Locked"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, kCallTimeoutMs,
      nullptr, nullptr);
  if (!r)
    return -1;
  GVariant* inner = nullptr;
  g_variant_get(r, "(v)", &inner);
  int locked = -1;
  if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_BOOLEAN))
    locked = g_variant_get_boolean(inner) ? 1 : 0;
  if (inner)
    g_variant_unref(inner);
  g_variant_unref(r);
  return locked;
}

void ProbeSecretServiceOn(GDBusConnection* bus, const std::string& session,
                          PlatformFeatures* out) {
  bool graphical = GraphicalSession(session);
  if (!bus) {
    out->secret_service = SecretServiceState::kNoSessionBus;
    out->secret_prompter = false;
    return;
  }
  std::set<std::string> owned = ListNames(bus, "ListNames");
  std::set<std::string> activatable = ListNames(bus, "ListActivatableNames");
  auto known = [&](const char* name) {
    return owned.count(name) || activatable.count(name);
  };
  if (owned.count(kSecretsName)) {
    int locked = DefaultCollectionLocked(bus);
    out->secret_service = locked == 0 ? SecretServiceState::kAvailable
                                      : SecretServiceState::kLocked;
  } else if (activatable.count(kSecretsName)) {
    out->secret_service = SecretServiceState::kActivatable;
  } else {
    out->secret_service = SecretServiceState::kAbsent;
  }
  // KWallet only where Chromium would pick it: a KDE desktop by Chromium's
  // own rule (GetDesktopEnvironment). On any other desktop it uses the
  // Secret Service, with kwalletd running or not.
  // The environment read here is the process's own, the one Chromium reads
  // in this (the browser) process.
  KWalletService chromium = ChromiumKWalletService(
      [](const char* name) -> const char* { return std::getenv(name); });
  out->kwallet = chromium != KWalletService::kNone;
  out->kwallet_service = KWalletService::kNone;
  out->kwallet_state =
      out->kwallet ? ProbeKWallet(bus, owned, chromium, &out->kwallet_service)
                   : KWalletState::kNotUsed;
  // gnome-keyring asks through gcr's prompter (gnome-shell's own, or
  // gcr-prompter); KWallet's provider prompts by itself.
  bool needs_gcr = known(kGnomeKeyringName);
  out->secret_prompter = graphical && (!needs_gcr || known(kGcrPrompterName));
}

// The watcher's owner changed: a host started, quit or restarted.
void OnWatcherOwnerChanged(GDBusConnection*, const gchar*, const gchar*,
                           const gchar*, const gchar*, GVariant* params,
                           gpointer) {
  const gchar* name = nullptr;
  const gchar* old_owner = nullptr;
  const gchar* new_owner = nullptr;
  g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (!name || std::strcmp(name, kWatcherName) != 0)
    return;
  int now = new_owner && *new_owner ? 1 : 0;
  int before = g_watcher.exchange(now);
  // The tray host appeared or went away (a restart is two changes).
  if (before != now)
    FirePlatformFeaturesChanged();
}

// The watcher state, subscribing on first use. The subscription delivers on
// the calling thread's thread-default main context, which is the process's
// default context (every backend's UI loop runs it) on any thread that has
// not pushed one of its own: the callers are the UI thread and the runtime
// thread, neither of which pushes one. Never push the default context here:
// that needs to acquire it, which fails (a GLib critical) on any thread
// while the UI thread runs it.
bool TrayWatcherLocked(State& s, GDBusConnection* bus) {
  if (!bus)
    return false;
  if (!s.watcher_sub) {
    s.watcher_sub = g_dbus_connection_signal_subscribe(
        bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", kWatcherName, G_DBUS_SIGNAL_FLAGS_NONE,
        OnWatcherOwnerChanged, nullptr, nullptr);
    // After subscribing, so an owner change in between is not lost.
    g_watcher.store(NameHasOwner(bus, kWatcherName) ? 1 : 0);
  }
  return g_watcher.load() == 1;
}

std::atomic<int> g_xembed_probes{0};

// An XEmbed system tray on the X display (the selection a tray manager
// owns). appindicator falls back to it when no watcher runs. Only in an X11
// session: in a Wayland session $DISPLAY is Xwayland's, and connecting to it
// can start Xwayland (an on-demand Xwayland, as Mutter and KWin run it) just
// to find there is no tray.
bool XEmbedTrayPresent(const std::string& session_type) {
  if (session_type != "x11" || Env("DISPLAY").empty())
    return false;
  g_xembed_probes++;
  int screen = 0;
  xcb_connection_t* c = xcb_connect(nullptr, &screen);
  if (!c || xcb_connection_has_error(c)) {
    if (c)
      xcb_disconnect(c);
    return false;
  }
  std::string atom_name = "_NET_SYSTEM_TRAY_S" + std::to_string(screen);
  xcb_intern_atom_reply_t* atom = xcb_intern_atom_reply(
      c,
      xcb_intern_atom(c, 1, static_cast<uint16_t>(atom_name.size()),
                      atom_name.c_str()),
      nullptr);
  bool present = false;
  if (atom && atom->atom != XCB_ATOM_NONE) {
    xcb_get_selection_owner_reply_t* owner = xcb_get_selection_owner_reply(
        c, xcb_get_selection_owner(c, atom->atom), nullptr);
    present = owner && owner->owner != XCB_WINDOW_NONE;
    free(owner);
  }
  free(atom);
  xcb_disconnect(c);
  return present;
}

// Who owns org.freedesktop.Notifications now (never starting one), and
// whether D-Bus could start one. Read live: a desktop's server can claim the
// name after the app starts.
void ProbeNotificationServer(GDBusConnection* bus, PlatformFeatures* f) {
  f->notification_server.clear();
  f->notification_activatable = false;
  if (!bus)
    return;
  if (NameHasOwner(bus, kNotificationsName)) {
    GVariant* r = g_dbus_connection_call_sync(
        bus, kNotificationsName, "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "GetServerInformation", nullptr,
        G_VARIANT_TYPE("(ssss)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        kCallTimeoutMs, nullptr, nullptr);
    std::string name;
    if (r) {
      const gchar* n = nullptr;
      g_variant_get(r, "(&s&s&s&s)", &n, nullptr, nullptr, nullptr);
      name = n ? n : "";
      g_variant_unref(r);
    }
    f->notification_server = name.empty() ? "unknown" : name;
    return;
  }
  f->notification_activatable =
      ListNames(bus, "ListActivatableNames").count(kNotificationsName) > 0;
}

std::map<std::string, uint32_t> ProbePortals(GDBusConnection* bus) {
  std::map<std::string, uint32_t> versions;
  if (!bus)
    return versions;
  if (!NameHasOwner(bus, kPortalName) &&
      !ListNames(bus, "ListActivatableNames").count(kPortalName)) {
    return versions;
  }
  bool first = true;
  for (const char* iface : kPortalInterfaces) {
    std::string full = std::string("org.freedesktop.portal.") + iface;
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        bus, kPortalName, kPortalPath, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", full.c_str(), "version"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, first ? kPortalStartTimeoutMs : kCallTimeoutMs,
        nullptr, &error);
    first = false;
    if (!r) {
      // A portal that doesn't answer at all (it failed to start, or hangs)
      // won't answer the next interface either.
      bool dead =
          error &&
          (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_FAILED) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY));
      g_clear_error(&error);
      if (dead)
        break;
      continue;
    }
    GVariant* inner = nullptr;
    g_variant_get(r, "(v)", &inner);
    if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32))
      versions[iface] = g_variant_get_uint32(inner);
    if (inner)
      g_variant_unref(inner);
    g_variant_unref(r);
  }
  return versions;
}

// The session facts and the secret service, once per process.
void EnsureBaseLocked(State& s) {
  if (s.probed)
    return;
  s.probed = true;
  PlatformFeatures& f = s.base;
  f.os = "linux";
  f.session_type = SessionType();
  f.desktop_hint = Env("XDG_CURRENT_DESKTOP");
  GDBusConnection* bus = SessionBusLocked(s);
  f.session_bus = bus != nullptr;
  ProbeSecretServiceOn(bus, f.session_type, &f);
  // appindicator: no click events, the title is the hover text.
  f.tray_clicks = false;
  f.tray_tooltip = true;
}

}  // namespace

void ProbeSecretService(PlatformFeatures* out) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  EnsureBaseLocked(s);
  out->session_type = s.base.session_type;
  out->session_bus = s.base.session_bus;
  out->secret_service = s.base.secret_service;
  out->secret_prompter = s.base.secret_prompter;
  out->kwallet = s.base.kwallet;
  out->kwallet_state = s.base.kwallet_state;
  out->kwallet_service = s.base.kwallet_service;
}

void ProbeTray(PlatformFeatures* out) {
  State& s = S();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    EnsureBaseLocked(s);
    out->os = s.base.os;
    out->session_type = s.base.session_type;
    out->desktop_hint = s.base.desktop_hint;
    out->session_bus = s.base.session_bus;
    out->tray_clicks = s.base.tray_clicks;
    out->tray_tooltip = s.base.tray_tooltip;
    out->tray_watcher = TrayWatcherLocked(s, s.bus);
  }
  out->tray_xembed = XEmbedTrayPresent(out->session_type);
  out->tray_library = AppIndicatorLibraryAvailableLinux();
}

PlatformFeatures ProbePlatformFeatures() {
  State& s = S();
  PlatformFeatures f;
  GDBusConnection* bus = nullptr;
  bool need_portals = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    EnsureBaseLocked(s);
    f = s.base;
    bus = s.bus ? static_cast<GDBusConnection*>(g_object_ref(s.bus)) : nullptr;
    f.tray_watcher = TrayWatcherLocked(s, bus);
    need_portals = !s.portals_probed;
    if (!need_portals)
      f.portal_versions = s.portal_versions;
  }
  // Outside the lock: the first portal call may wait for the portal to
  // start.
  if (need_portals) {
    std::map<std::string, uint32_t> versions = ProbePortals(bus);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.portals_probed) {
      s.portals_probed = true;
      s.portal_versions = versions;
    }
    f.portal_versions = s.portal_versions;
  }
  ProbeNotificationServer(bus, &f);
  if (bus)
    g_object_unref(bus);
  // The file chooser a dialog would use (the same choice io_linux.cc makes).
  auto chooser = f.portal_versions.find("FileChooser");
  FileChooserChoice choice = ChooseFileChooser(
      chooser == f.portal_versions.end() ? 0 : chooser->second, nullptr,
      std::getenv("LAUFEY_FILE_CHOOSER"));
  f.file_chooser = choice.portal ? "portal" : "gtk";
  f.file_chooser_reason = choice.reason;
  // The notification platform's own view (its transport, the cold start,
  // timers, the server's capabilities), and the launcher badge.
  NotificationFacts facts = LinuxNotificationFacts();
  f.notification_transport = facts.transport;
  f.notification_activation_error = facts.activation_error;
  f.notification_cold_start = facts.cold_start;
  f.notification_cold_start_reason = facts.cold_start_reason;
  f.notification_schedule_while_closed = facts.schedule_while_closed;
  f.notification_schedule_reason = facts.schedule_reason;
  f.notification_caps_known = facts.server_caps_known;
  f.notification_server_caps = facts.server_caps;
  f.badge =
      LauncherEntryAvailable(&f.badge_reason) ? "launcher-entry" : "title";
  f.tray_xembed = XEmbedTrayPresent(f.session_type);
  f.tray_library = AppIndicatorLibraryAvailableLinux();
  return f;
}

void SetPlatformFeaturesChangedHandler(void (*handler)(void* user_data),
                                       void* user_data) {
  {
    std::lock_guard<std::mutex> lock(g_changed_mutex);
    g_changed_handler = handler;
    g_changed_user_data = user_data;
  }
  if (!handler)
    return;
  // Follow the watcher from now on (the subscription the tray probe makes).
  // Only the bus: none of the base probe (the Secret Service, logind).
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  TrayWatcherLocked(s, SessionBusLocked(s));
}

int XEmbedProbeCountForTesting() {
  return g_xembed_probes.load();
}

void ResetPlatformFeaturesForTesting() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.bus && s.watcher_sub)
    g_dbus_connection_signal_unsubscribe(s.bus, s.watcher_sub);
  s.watcher_sub = 0;
  g_watcher.store(-1);
  if (s.bus)
    g_object_unref(s.bus);
  s.bus = nullptr;
  s.probed = false;
  s.base = PlatformFeatures();
  s.portals_probed = false;
  s.portal_versions.clear();
}

}  // namespace laufey_common
