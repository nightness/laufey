// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux secure store (secret_store_linux.cc) on a private D-Bus session
// bus (GTestDBus), against mock Secret Services and, where it is installed,
// a real gnome-keyring-daemon:
//   - libsecret missing (a child process pointed at a library that isn't
//     there): unavailable, naming the package;
//   - no session bus: unavailable at once;
//   - no provider: unavailable, "install gnome-keyring"; with KWallet
//     running but not serving org.freedesktop.secrets: "enable KWallet's
//     Secret Service";
//   - a provider that never answers: unavailable after the timeout, no hang;
//   - a locked keyring where no one can answer the unlock prompt (a mock
//     whose matching item and default collection are locked): lookup, store
//     and delete are refused at once, never "not found";
//   - gnome-keyring (its login keyring, unlocked): store / lookup / replace /
//     delete round trips,
//     non-ASCII text, a missing item is "not found", deleting nothing is
//     fine; a second gnome-keyring-daemon taking org.freedesktop.secrets
//     over (two daemons in one session) is written to and read from, not
//     sent the first one's transfer session; locked: refused at once
//     without a prompter, and with one that
//     never answers, unavailable after the timeout (not a hang), the prompt
//     never dismissed (gnome-keyring stays up) and a second call joining the
//     pending unlock instead of a second prompt.
// Exits 77 (skipped) without dbus-daemon or libsecret; the gnome-keyring
// part is skipped without gnome-keyring-daemon.

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "laufey_platform_features.h"
#include "laufey_secret_store.h"

using namespace laufey_common;

namespace {

pid_t g_keyring = -1;

[[noreturn]] void Finish(int code) {
  if (g_keyring > 0) {
    kill(g_keyring, SIGTERM);
    waitpid(g_keyring, nullptr, 0);
  }
  std::fflush(stdout);
  std::_Exit(code);
}

}  // namespace

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Finish(1);                                                             \
    }                                                                        \
  } while (0)

namespace {

using Clock = std::chrono::steady_clock;

const char kXml[] =
    "<node>"
    " <interface name='org.freedesktop.Secret.Service'>"
    "  <method name='SearchItems'>"
    "   <arg type='a{ss}' direction='in'/>"
    "   <arg type='ao' direction='out'/><arg type='ao' direction='out'/>"
    "  </method>"
    "  <method name='ReadAlias'>"
    "   <arg type='s' direction='in'/><arg type='o' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.Secret.Collection'>"
    "  <property name='Locked' type='b' access='read'/>"
    " </interface>"
    "</node>";

GDBusConnection* g_conn = nullptr;  // the mocks' connection
GDBusNodeInfo* g_info = nullptr;
// The mock Secret Service: never answers, or answers one locked item.
std::atomic<bool> g_hang{false};
std::atomic<int> g_searches{0};

void CallMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                const gchar* method, GVariant*,
                GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "ReadAlias") == 0) {
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(o)", "/org/freedesktop/secrets/aliases/default"));
    return;
  }
  g_searches++;
  if (g_hang) {
    // Never answered (the invocation leaks on purpose: the caller waits).
    return;
  }
  const gchar* locked[] = {"/org/freedesktop/secrets/collection/login/1",
                           nullptr};
  const gchar* none[] = {nullptr};
  g_dbus_method_invocation_return_value(
      invocation, g_variant_new("(^ao^ao)", none, locked));
}

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar*, GError**, gpointer) {
  return g_variant_new_boolean(TRUE);
}

const GDBusInterfaceVTable kVTable = {CallMethod, GetProperty, nullptr, {}};

// gcr's system prompter (what gnome-keyring asks to show an unlock
// prompt): it takes the prompt and never shows it (no one answers).
const char kPrompterXml[] =
    "<node>"
    " <interface name='org.gnome.keyring.internal.Prompter'>"
    "  <method name='BeginPrompting'><arg type='o' direction='in'/></method>"
    "  <method name='PerformPrompt'>"
    "   <arg type='o' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/><arg type='s' direction='in'/>"
    "  </method>"
    "  <method name='StopPrompting'><arg type='o' direction='in'/></method>"
    " </interface>"
    "</node>";
std::atomic<int> g_prompts{0};

void PrompterCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                  const gchar* method, GVariant*,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "BeginPrompting") == 0)
    g_prompts++;
  // Accepted; the prompt is never shown, so its callback never comes.
  g_dbus_method_invocation_return_value(invocation, nullptr);
}

const GDBusInterfaceVTable kPrompterVTable = {
    PrompterCall, nullptr, nullptr, {}};

void BusName(const char* method, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method,
      strcmp(method, "RequestName") == 0 ? g_variant_new("(su)", name, 0u)
                                         : g_variant_new("(s)", name),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  g_variant_unref(r);
}

bool HasOwner(const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  gboolean owned = FALSE;
  if (r) {
    g_variant_get(r, "(b)", &owned);
    g_variant_unref(r);
  }
  return owned;
}

bool Contains(const std::string& s, const char* part) {
  return s.find(part) != std::string::npos;
}

long Ms(Clock::time_point start) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count());
}

// A headless session (no one can answer a prompt) or a graphical one.
void Session(bool graphical) {
  setenv("XDG_SESSION_TYPE", graphical ? "x11" : "tty", 1);
  if (graphical)
    setenv("DISPLAY", ":97", 1);
  else
    unsetenv("DISPLAY");
  ResetPlatformFeaturesForTesting();
}

// The libsecret-missing case, in a child (the library is loaded once per
// process).
void LibsecretMissing() {
  pid_t pid = fork();
  if (pid == 0) {
    SetLibsecretPathForTesting("/nonexistent/libsecret-1.so.0");
    std::string value, reason;
    SecretStatus s = SecretLookup("svc", "acct", 1000, &value, &reason);
    bool ok = s == SecretStatus::kUnavailable &&
              Contains(reason, "libsecret isn't installed") &&
              Contains(reason, "libsecret-1-0");
    if (!ok)
      std::fprintf(stderr, "libsecret missing: %s\n", reason.c_str());
    std::_Exit(ok ? 0 : 1);
  }
  int status = 0;
  EXPECT(waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// A real gnome-keyring-daemon on the private bus, its login keyring created
// and unlocked (the default collection). False when it isn't installed.
std::string NameOwner(const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  std::string owner;
  if (r) {
    const gchar* s = nullptr;
    g_variant_get(r, "(&s)", &s);
    owner = s;
    g_variant_unref(r);
  }
  return owner;
}

// Starts gnome-keyring-daemon (its login keyring unlocked) and waits until
// it owns org.freedesktop.secrets; with `replace`, until it has taken the
// name over from the one that had it.
bool StartGnomeKeyring(const std::string& home, bool replace = false) {
  const std::string before =
      replace ? NameOwner("org.freedesktop.secrets") : std::string();
  gchar* daemon = g_find_program_in_path("gnome-keyring-daemon");
  if (!daemon)
    return false;
  std::string run = home + "/run";
  g_mkdir_with_parents(run.c_str(), 0700);
  int in[2];
  EXPECT(pipe(in) == 0);
  g_keyring = fork();
  if (g_keyring == 0) {
    dup2(in[0], STDIN_FILENO);
    close(in[1]);
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_DATA_HOME", (home + "/data").c_str(), 1);
    setenv("XDG_RUNTIME_DIR", run.c_str(), 1);
    if (replace)
      execl(daemon, daemon, "--foreground", "--components=secrets", "--unlock",
            "--replace", static_cast<char*>(nullptr));
    else
      execl(daemon, daemon, "--foreground", "--components=secrets", "--unlock",
            static_cast<char*>(nullptr));
    _exit(127);
  }
  g_free(daemon);
  close(in[0]);
  // The login keyring's password (an empty one creates no keyring).
  EXPECT(write(in[1], "laufey", 6) == 6);
  close(in[1]);
  auto owned = [&] {
    std::string owner = NameOwner("org.freedesktop.secrets");
    return !owner.empty() && owner != before;
  };
  for (int i = 0; i < 500 && !owned(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return owned();
}

// The gnome-keyring-daemon this test started is still running (not exited,
// not a zombie: it is our child).
bool KeyringAlive() {
  int status = 0;
  return g_keyring > 0 && waitpid(g_keyring, &status, WNOHANG) == 0;
}

// Locks every collection of the running Secret Service (no prompt).
void LockAll() {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.secrets", "/org/freedesktop/secrets",
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.freedesktop.Secret.Service", "Collections"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  GVariant* collections = nullptr;
  g_variant_get(r, "(v)", &collections);
  GVariant* locked = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.secrets", "/org/freedesktop/secrets",
      "org.freedesktop.Secret.Service", "Lock",
      g_variant_new("(@ao)", collections), G_VARIANT_TYPE("(aoo)"),
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(locked);
  g_variant_unref(locked);
  g_variant_unref(r);
}

}  // namespace

int main() {
  // --- The pieces with no bus
  // -------------------------------------------------
  EXPECT(SecretProviderFromComm("gnome-keyring-d") == "gnome-keyring");
  EXPECT(SecretProviderFromComm("ksecretd") == "KWallet");
  EXPECT(SecretProviderFromComm("kwalletd6") == "KWallet");
  EXPECT(SecretProviderFromComm("keepassxc") == "KeePassXC");
  EXPECT(SecretProviderFromComm("oo7-daemon") == "oo7-daemon");
  EXPECT(SecretProviderFromComm("").empty());
  EXPECT(Contains(NoSecretProviderReason(false), "install gnome-keyring"));
  EXPECT(Contains(NoSecretProviderReason(true),
                  "enable KWallet's Secret Service"));
  EXPECT(Contains(LockedSecretReason("KWallet", false, false, 0),
                  "the KWallet wallet is closed"));
  EXPECT(Contains(LockedSecretReason("KWallet", false, false, 0),
                  "open the wallet"));
  EXPECT(Contains(LockedSecretReason("gnome-keyring", false, false, 0),
                  "sign in to the desktop with a password"));
  EXPECT(Contains(LockedSecretReason("", true, true, 20000),
                  "the keyring is locked and its unlock prompt went "
                  "unanswered for 20 s"));
  EXPECT(Contains(LockedSecretReason("gnome-keyring", true, false, 5),
                  "the unlock prompt was dismissed"));

  // --- No bus at all ------------------------------------------------------
  LibsecretMissing();
  {
    gchar* bus_env = nullptr;
    const char* a = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    bus_env = a ? g_strdup(a) : nullptr;
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("XDG_RUNTIME_DIR", "/nonexistent-laufey", 1);
    std::string value, reason;
    SecretStatus s = SecretLookup("svc", "acct", 1000, &value, &reason);
    if (s == SecretStatus::kUnavailable &&
        Contains(reason, "libsecret isn't installed")) {
      std::printf(
          "laufey_secret_store_dbus_test: libsecret missing, skipped\n");
      return 77;
    }
    EXPECT(s == SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "no D-Bus session bus"));
    g_free(bus_env);
  }
  // Bad arguments.
  {
    std::string value, reason;
    EXPECT(SecretLookup("", "acct", 1000, &value, &reason) ==
           SecretStatus::kFailed);
    EXPECT(SecretStore("svc", "acct", "", std::string("a\0b", 3), 1000,
                       &reason) == SecretStatus::kFailed);
  }

  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_secret_store_dbus_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  // The system bus (logind, for the "graphical session" probe) is the same
  // private bus, where no logind answers.
  setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), 1);
  unsetenv("XDG_SESSION_ID");
  for (const char* name : {"XDG_CURRENT_DESKTOP", "KDE_FULL_SESSION",
                           "DESKTOP_SESSION", "WAYLAND_DISPLAY"})
    unsetenv(name);
  Session(false);

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
  std::atomic<bool> ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    EXPECT(g_dbus_connection_register_object(
               g_conn, "/org/freedesktop/secrets",
               g_dbus_node_info_lookup_interface(
                   g_info, "org.freedesktop.Secret.Service"),
               &kVTable, nullptr, nullptr, &error) != 0);
    EXPECT(g_dbus_connection_register_object(
               g_conn, "/org/freedesktop/secrets/aliases/default",
               g_dbus_node_info_lookup_interface(
                   g_info, "org.freedesktop.Secret.Collection"),
               &kVTable, nullptr, nullptr, &error) != 0);
    GDBusNodeInfo* prompter =
        g_dbus_node_info_new_for_xml(kPrompterXml, &error);
    EXPECT(prompter);
    EXPECT(g_dbus_connection_register_object(
               g_conn, "/org/gnome/keyring/Prompter",
               g_dbus_node_info_lookup_interface(
                   prompter, "org.gnome.keyring.internal.Prompter"),
               &kPrompterVTable, nullptr, nullptr, &error) != 0);
    ready = true;
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  for (int i = 0; i < 500 && !ready; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(ready);

  // --- No provider ----------------------------------------------------------
  {
    std::string value, reason;
    EXPECT(SecretLookup("svc", "acct", 1000, &value, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "no Secret Service provider"));
    EXPECT(Contains(reason, "install gnome-keyring"));
    EXPECT(SecretStore("svc", "acct", "", "v", 1000, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(SecretDelete("svc", "acct", 1000, &reason) ==
           SecretStatus::kUnavailable);
  }
  // KWallet runs, without the Secret Service.
  BusName("RequestName", "org.kde.kwalletd6");
  {
    std::string value, reason;
    EXPECT(SecretLookup("svc", "acct", 1000, &value, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "KWallet runs but doesn't serve"));
    EXPECT(Contains(reason, "enable KWallet's Secret Service"));
  }
  BusName("ReleaseName", "org.kde.kwalletd6");

  // --- A provider that never answers ----------------------------------------
  g_hang = true;
  BusName("RequestName", "org.freedesktop.secrets");
  {
    std::string value, reason;
    auto start = Clock::now();
    EXPECT(SecretLookup("svc", "acct", 1500, &value, &reason) ==
           SecretStatus::kUnavailable);
    long took = Ms(start);
    EXPECT(took >= 1000 && took < 5000);
    EXPECT(Contains(reason, "did not answer"));
  }

  // --- Locked, and no one here can answer the prompt -----------------------
  g_hang = false;
  {
    std::string value, reason;
    auto start = Clock::now();
    // A matching item exists, locked: never "not found".
    EXPECT(SecretLookup("svc", "acct", 20000, &value, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "locked"));
    EXPECT(Contains(reason, "no one here can answer its unlock prompt"));
    EXPECT(SecretStore("svc", "acct", "", "v", 20000, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "locked"));
    EXPECT(SecretDelete("svc", "acct", 20000, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "locked"));
    // Refused at once, without waiting out the timeout.
    EXPECT(Ms(start) < 5000);
  }
  BusName("ReleaseName", "org.freedesktop.secrets");

  // --- gnome-keyring
  // ----------------------------------------------------------
  gchar* home = g_dir_make_tmp("laufey-ss-XXXXXX", nullptr);
  EXPECT(home);
  if (!StartGnomeKeyring(home)) {
    std::printf(
        "laufey_secret_store_dbus_test: gnome-keyring-daemon missing; its "
        "part skipped\n");
    std::printf("laufey_secret_store_dbus_test: ok\n");
    Finish(0);
  }
  {
    std::string value, reason;
    const std::string service = "dev.laufey.test";
    EXPECT(SecretLookup(service, "nobody", 5000, &value, &reason) ==
           SecretStatus::kNotFound);
    EXPECT(SecretStore(service, "alice", "Laufey test", "first", 5000,
                       &reason) == SecretStatus::kOk);
    EXPECT(SecretLookup(service, "alice", 5000, &value, &reason) ==
           SecretStatus::kOk);
    EXPECT(value == "first");
    // Replaced, not added.
    std::string text = "zweite Zeile \xe2\x9c\x93 \"quoted\"\nline 2";
    EXPECT(SecretStore(service, "alice", "Laufey test", text, 5000, &reason) ==
           SecretStatus::kOk);
    EXPECT(SecretLookup(service, "alice", 5000, &value, &reason) ==
           SecretStatus::kOk);
    EXPECT(value == text);
    // Another account is another item.
    EXPECT(SecretStore(service, "bob", "", "b", 5000, &reason) ==
           SecretStatus::kOk);
    EXPECT(SecretDelete(service, "alice", 5000, &reason) == SecretStatus::kOk);
    EXPECT(SecretLookup(service, "alice", 5000, &value, &reason) ==
           SecretStatus::kNotFound);
    EXPECT(SecretLookup(service, "bob", 5000, &value, &reason) ==
           SecretStatus::kOk);
    EXPECT(value == "b");
    // Deleting nothing is fine.
    EXPECT(SecretDelete(service, "alice", 5000, &reason) == SecretStatus::kOk);

    // A second gnome-keyring takes org.freedesktop.secrets over, as when a
    // session has two (PAM's --login one and a D-Bus-activated
    // --components=secrets one). The secret of a write goes encrypted with
    // a transfer session; the one libsecret opened with the first daemon
    // means nothing to the second, so each call must use a session with
    // whoever owns the name now.
    {
      const pid_t first = g_keyring;
      gchar* home2 = g_dir_make_tmp("laufey-ss2-XXXXXX", nullptr);
      EXPECT(home2);
      EXPECT(StartGnomeKeyring(home2, /*replace=*/true));
      const SecretStatus stored =
          SecretStore(service, "carol", "", "c", 5000, &reason);
      if (stored != SecretStatus::kOk)
        std::fprintf(stderr, "  write after the owner changed: %s\n",
                     reason.c_str());
      EXPECT(stored == SecretStatus::kOk);
      EXPECT(SecretLookup(service, "carol", 5000, &value, &reason) ==
             SecretStatus::kOk);
      EXPECT(value == "c");
      // The first daemon's items aren't the new owner's.
      EXPECT(SecretLookup(service, "bob", 5000, &value, &reason) ==
             SecretStatus::kNotFound);
      EXPECT(SecretStore(service, "bob", "", "b", 5000, &reason) ==
             SecretStatus::kOk);
      kill(first, SIGTERM);
      waitpid(first, nullptr, 0);
      g_free(home2);
    }

    // Locked, and no one here can answer the prompt: at once.
    LockAll();
    auto start = Clock::now();
    EXPECT(SecretLookup(service, "bob", 20000, &value, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "gnome-keyring keyring is locked"));
    EXPECT(Ms(start) < 5000);
    // A graphical session with a prompter (a stand-in that never answers):
    // libsecret asks to unlock, and the timeout ends the wait.
    Session(true);
    BusName("RequestName", "org.gnome.keyring.SystemPrompter");
    start = Clock::now();
    EXPECT(SecretLookup(service, "bob", 2000, &value, &reason) ==
           SecretStatus::kUnavailable);
    long took = Ms(start);
    EXPECT(took >= 1500 && took < 8000);
    EXPECT(g_prompts == 1);
    EXPECT(Contains(reason, "its unlock prompt went unanswered"));
    // The prompt was never dismissed from here: gnome-keyring is still up
    // (it aborts when its unlock prompt is dismissed while it is shown).
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT(KeyringAlive());
    EXPECT(HasOwner("org.freedesktop.secrets"));
    // A call while that unlock is still pending waits for it: no second
    // prompt, and a write is refused the same way.
    start = Clock::now();
    EXPECT(SecretStore(service, "bob", "", "x", 1500, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(Contains(reason, "its unlock prompt went unanswered"));
    EXPECT(Ms(start) < 6000);
    EXPECT(g_prompts == 1);
    EXPECT(KeyringAlive());
    std::printf("  locked with an unanswered prompt: %ld ms: %s\n", took,
                reason.c_str());
  }
  std::printf("laufey_secret_store_dbus_test: ok\n");
  Finish(0);
}
