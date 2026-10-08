// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_platform_features.h, the decisions made from a probe (no bus): when
// Chromium's cookie store must not wait for the Secret Service, when a tray
// icon can be seen and why not, and the JSON platform_features hands out.
// Plain asserts, no framework.

#include "laufey_platform_features.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

static bool Contains(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

static PlatformFeatures Linux() {
  PlatformFeatures f;
  f.os = "linux";
  f.session_type = "wayland";
  f.session_bus = true;
  f.tray_watcher = true;
  f.tray_clicks = false;
  f.secret_service = SecretServiceState::kAvailable;
  f.secret_prompter = true;
  return f;
}

int main() {
#ifndef _WIN32
  // The ABI string below runs the real probe: keep it off this machine's
  // session (no bus, no display), so it answers at once.
  unsetenv("DBUS_SESSION_BUS_ADDRESS");
  unsetenv("XDG_RUNTIME_DIR");
  unsetenv("DISPLAY");
  unsetenv("WAYLAND_DISPLAY");
#endif

  // --- The display that is there (not XDG_SESSION_TYPE) --------------------
  {
    using Vars = std::map<std::string, std::string>;
    auto backend = [](const Vars& vars, const std::vector<std::string>& socks) {
      return DisplayBackend(
          [&vars](const char* name) -> const char* {
            auto it = vars.find(name);
            return it == vars.end() ? nullptr : it->second.c_str();
          },
          [&socks](const std::string& path) {
            for (const auto& s : socks)
              if (s == path)
                return true;
            return false;
          });
    };
    // GDM's autologin into Xorg XFCE / i3: logind and the env say
    // "wayland", only $DISPLAY is there. X11 (CEF opened no window when
    // Chromium's ozone hint followed XDG_SESSION_TYPE).
    EXPECT(backend({{"XDG_SESSION_TYPE", "wayland"}, {"DISPLAY", ":0"}}, {}) ==
           "x11");
    // A stale $WAYLAND_DISPLAY with no socket behind it.
    EXPECT(backend({{"WAYLAND_DISPLAY", "wayland-0"},
                    {"XDG_RUNTIME_DIR", "/run/user/1000"},
                    {"DISPLAY", ":0"}},
                   {}) == "x11");
    EXPECT(backend({{"WAYLAND_DISPLAY", "wayland-0"},
                    {"XDG_RUNTIME_DIR", "/run/user/1000"},
                    {"DISPLAY", ":0"}},
                   {"/run/user/1000/wayland-0"}) == "wayland");
    EXPECT(backend({{"WAYLAND_DISPLAY", "/tmp/wl"}}, {"/tmp/wl"}) == "wayland");
    // Relative without XDG_RUNTIME_DIR: libwayland can't find it either.
    EXPECT(backend({{"WAYLAND_DISPLAY", "wayland-0"}}, {"wayland-0"}).empty());
    EXPECT(backend({{"WAYLAND_SOCKET", "5"}}, {}) == "wayland");
    EXPECT(backend({{"XDG_SESSION_TYPE", "x11"}}, {}).empty());
    EXPECT(backend({{"DISPLAY", ""}, {"WAYLAND_DISPLAY", ""}}, {}).empty());

    EXPECT(ReportedSessionType("wayland", "x11") == "x11");
    EXPECT(ReportedSessionType("x11", "wayland") == "wayland");
    EXPECT(ReportedSessionType("wayland", "wayland") == "wayland");
    EXPECT(ReportedSessionType("wayland", "") == "wayland");
    // A display never makes a tty / unknown session graphical.
    EXPECT(ReportedSessionType("tty", "x11") == "tty");
    EXPECT(ReportedSessionType("", "x11") == "unknown");

    // WebKitGTK on X11: shared-memory frames unless the user chose.
    auto none = [](const char*) -> const char* { return nullptr; };
    EXPECT(ShouldForceWebKitShm(true, none));
    EXPECT(!ShouldForceWebKitShm(false, none));
    for (const char* user :
         {"WEBKIT_DMABUF_RENDERER_FORCE_SHM", "WEBKIT_DISABLE_DMABUF_RENDERER",
          "WEBKIT_DISABLE_COMPOSITING_MODE"}) {
      std::string set = user;
      EXPECT(!ShouldForceWebKitShm(true, [&set](const char* name) {
        return set == name ? "0" : nullptr;
      }));
    }
  }

  // --- The cookie store ------------------------------------------------------
  PlatformFeatures f = Linux();
  EXPECT(!NeedsBasicPasswordStore(f));
  EXPECT(BasicPasswordStoreReason(f).empty());

  // No provider, or no bus: Chromium finds no keystore and falls back to
  // basic by itself, without waiting. Left to it.
  f.secret_service = SecretServiceState::kAbsent;
  f.secret_prompter = false;
  EXPECT(!NeedsBasicPasswordStore(f));
  f.secret_service = SecretServiceState::kNoSessionBus;
  EXPECT(!NeedsBasicPasswordStore(f));
  f.secret_prompter = true;

  // Locked (or not yet started): fine while someone can answer the prompt,
  // a hang when no one can.
  for (SecretServiceState s :
       {SecretServiceState::kLocked, SecretServiceState::kActivatable}) {
    f.secret_service = s;
    f.secret_prompter = true;
    EXPECT(!NeedsBasicPasswordStore(f));
    f.secret_prompter = false;
    f.session_type = "tty";
    EXPECT(NeedsBasicPasswordStore(f));
    EXPECT(Contains(BasicPasswordStoreReason(f), "tty session"));
    f.session_type = "wayland";
  }

  // KWallet (Chromium's store on KDE): only an open wallet answers, whatever
  // the Secret Service or a prompter (on Plasma a request for a closed
  // wallet's key is never answered, a person in front or not).
  f = Linux();
  f.kwallet = true;
  f.kwallet_state = KWalletState::kOpen;
  EXPECT(!NeedsBasicPasswordStore(f));
  EXPECT(BasicPasswordStoreReason(f).empty());
  for (KWalletState k : {KWalletState::kClosed, KWalletState::kDisabled,
                         KWalletState::kNotRunning}) {
    f.kwallet_state = k;
    f.secret_prompter = true;  // a graphical session: still no answer
    EXPECT(NeedsBasicPasswordStore(f));
    EXPECT(Contains(BasicPasswordStoreReason(f), "KWallet"));
  }
  f.kwallet_state = KWalletState::kClosed;
  EXPECT(Contains(BasicPasswordStoreReason(f), "wallet is closed"));
  f.kwallet_state = KWalletState::kNotRunning;
  EXPECT(Contains(BasicPasswordStoreReason(f), "kwalletd is not running"));
  EXPECT(std::string(KWalletStateName(KWalletState::kNotRunning)) ==
         "not-running");
  EXPECT(KWalletStateName(KWalletState::kNotUsed) == nullptr);

  // Whether Chromium's cookie store would use KWallet: its own rule over
  // XDG_CURRENT_DESKTOP (the first desktop it knows), then DESKTOP_SESSION
  // and the older variables.
  {
    // A variable in the map is set (possibly to ""); one not in it is unset.
    auto picks = [](std::map<std::string, std::string> vars) {
      return ChromiumPicksKWallet([&vars](const char* name) -> const char* {
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
      });
    };
    EXPECT(picks({{"XDG_CURRENT_DESKTOP", "KDE"}}));
    EXPECT(
        picks({{"XDG_CURRENT_DESKTOP", "KDE"}, {"KDE_SESSION_VERSION", "6"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "GNOME"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "ubuntu:GNOME"}}));
    // The first desktop Chromium knows decides; unknown ones are skipped.
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "GNOME:KDE"}}));
    EXPECT(picks({{"XDG_CURRENT_DESKTOP", "Hyprland: KDE :GNOME"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "sway"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "KDE-ish"}}));
    EXPECT(!picks({}));
    // Nothing it knows in XDG_CURRENT_DESKTOP: DESKTOP_SESSION.
    EXPECT(picks({{"XDG_CURRENT_DESKTOP", "sway"},
                  {"DESKTOP_SESSION", "plasma"},
                  {"KDE_FULL_SESSION", "true"},
                  {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(picks({{"DESKTOP_SESSION", "kde-plasma"}}));
    EXPECT(picks({{"DESKTOP_SESSION", "kde4"}}));
    EXPECT(picks({{"DESKTOP_SESSION", "kde"}, {"KDE_SESSION_VERSION", "5"}}));
    // KDE 3 (no KDE_SESSION_VERSION) is KWallet too.
    EXPECT(picks({{"DESKTOP_SESSION", "kde"}}));
    EXPECT(!picks({{"DESKTOP_SESSION", "gnome"}, {"KDE_FULL_SESSION", "1"}}));
    // The older variables count when set, even empty (HasVar).
    EXPECT(!picks({{"GNOME_DESKTOP_SESSION_ID", "x"},
                   {"KDE_FULL_SESSION", "true"},
                   {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(!picks(
        {{"GNOME_DESKTOP_SESSION_ID", ""}, {"KDE_FULL_SESSION", "true"}}));
    EXPECT(picks({{"KDE_FULL_SESSION", "true"}, {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(picks({{"KDE_FULL_SESSION", "true"}}));  // KDE 3
    EXPECT(picks({{"KDE_FULL_SESSION", ""}}));
    EXPECT(picks({{"KDE_FULL_SESSION", ""}, {"KDE_SESSION_VERSION", ""}}));
    EXPECT(!picks({{"KDE_SESSION_VERSION", "5"}}));  // not on its own
    // An unknown DESKTOP_SESSION falls through to the older variables.
    EXPECT(picks({{"DESKTOP_SESSION", "default"}, {"KDE_FULL_SESSION", ""}}));
  }

  // Which kwalletd it asks (M149 FreedesktopSecretKeyProvider::GetKey over
  // GetDesktopEnvironment): KDE_SESSION_VERSION "6" kwalletd6, "5"
  // kwalletd5, anything else (unset included) KDE 4's org.kde.kwalletd;
  // DESKTOP_SESSION and KDE_FULL_SESSION only ever KDE 4 / 3.
  {
    auto service = [](std::map<std::string, std::string> vars) {
      return ChromiumKWalletService([&vars](const char* name) -> const char* {
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
      });
    };
    const KWalletService k4 = KWalletService::kKWalletd;
    const KWalletService k5 = KWalletService::kKWalletd5;
    const KWalletService k6 = KWalletService::kKWalletd6;
    const KWalletService none = KWalletService::kNone;
    // Plasma 6 launched without KDE_SESSION_VERSION (ssh, a systemd unit,
    // a wrapper): KDE 4's daemon, the bug's trigger.
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "KDE"}}) == k4);
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "KDE"},
                    {"KDE_SESSION_VERSION", "6"}}) == k6);
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "KDE"},
                    {"KDE_SESSION_VERSION", "5"}}) == k5);
    // Compared as is: no trimming, no other versions.
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "KDE"},
                    {"KDE_SESSION_VERSION", " 6"}}) == k4);
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "KDE"},
                    {"KDE_SESSION_VERSION", "7"}}) == k4);
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "KDE"},
                    {"KDE_SESSION_VERSION", ""}}) == k4);
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "Hyprland: KDE"},
                    {"KDE_SESSION_VERSION", "6"}}) == k6);
    EXPECT(service({{"XDG_CURRENT_DESKTOP", "GNOME"},
                    {"KDE_SESSION_VERSION", "6"}}) == none);
    EXPECT(service({}) == none);
    // DESKTOP_SESSION kde-plasma / kde4 / kde and KDE_FULL_SESSION: KDE 4
    // (or 3), whatever KDE_SESSION_VERSION says.
    EXPECT(service({{"DESKTOP_SESSION", "kde-plasma"},
                    {"KDE_SESSION_VERSION", "6"}}) == k4);
    EXPECT(service({{"DESKTOP_SESSION", "kde"}}) == k4);
    EXPECT(service({{"KDE_FULL_SESSION", "true"},
                    {"KDE_SESSION_VERSION", "6"}}) == k4);
    EXPECT(service({{"DESKTOP_SESSION", "plasma"},
                    {"KDE_SESSION_VERSION", "6"}}) == none);
    // The switch value that names each daemon to Chromium.
    EXPECT(std::string(KWalletPasswordStore(k4)) == "kwallet");
    EXPECT(std::string(KWalletPasswordStore(k5)) == "kwallet5");
    EXPECT(std::string(KWalletPasswordStore(k6)) == "kwallet6");
    EXPECT(KWalletPasswordStore(none) == nullptr);
  }

  // macOS and Windows: the OS keystore.
  PlatformFeatures mac;
  mac.os = "macos";
  EXPECT(mac.secret_service == SecretServiceState::kNotApplicable);
  EXPECT(!NeedsBasicPasswordStore(mac));
  EXPECT(TrayAvailable(mac));
  EXPECT(TrayUnavailableReason(mac).empty());

  // --- The cookie store: never basic on a profile with OS-key cookies ------
  {
    int probes = 0;
    PlatformFeatures locked = Linux();
    locked.secret_service = SecretServiceState::kLocked;
    locked.secret_prompter = false;  // headless: no one to unlock it
    PlatformFeatures activatable = locked;
    activatable.secret_service = SecretServiceState::kActivatable;
    PlatformFeatures kwallet_closed = Linux();
    kwallet_closed.kwallet = true;
    kwallet_closed.kwallet_state = KWalletState::kClosed;
    auto probe_locked = [&] {
      ++probes;
      return locked;
    };
    auto probe_activatable = [&] {
      ++probes;
      return activatable;
    };
    auto probe_unlocked = [&] {
      ++probes;
      return Linux();
    };
    auto probe_kwallet_closed = [&] {
      ++probes;
      return kwallet_closed;
    };
    const ProfileCookieKeys kNone = ProfileCookieKeys::kNone;
    const ProfileCookieKeys kOsKey = ProfileCookieKeys::kOsKey;
    const ProfileCookieKeys kUnknown = ProfileCookieKeys::kUnknown;
    // No OS-key cookies: the probe decides, nothing is lost either way.
    // Basic is not recorded (basic -> os later is lossless: Chromium reads
    // v10 cookies under os); os is.
    PasswordStoreChoice c =
        ChoosePasswordStore(nullptr, "", kNone, probe_locked);
    EXPECT(c.store == "basic" && c.append_basic && !c.record && !c.wait);
    EXPECT(c.source == "probe" && Contains(c.reason, "locked"));
    EXPECT(Contains(PasswordStoreWarning(c, kNone, ""),
                    "--password-store=basic (not encrypted"));
    c = ChoosePasswordStore(nullptr, "", kNone, probe_unlocked);
    EXPECT(c.store == "os" && !c.append_basic && c.record && !c.wait);
    EXPECT(c.source == "probe" && c.reason.empty());
    EXPECT(PasswordStoreWarning(c, kNone, "").empty());
    EXPECT(probes == 2);
    // An "os" profile whose key is reachable: os, nothing rewritten.
    c = ChoosePasswordStore(nullptr, "os", kOsKey, probe_unlocked);
    EXPECT(c.store == "os" && !c.append_basic && !c.record && !c.wait);
    EXPECT(c.source == "profile");
    EXPECT(PasswordStoreWarning(c, kOsKey, "").empty());
    // OS-key cookies (v11 rows) and no one to unlock the key: never basic.
    // The OS store is kept and the launch waits for the key, with the
    // reason and one warning. Locked, activatable, a closed KWallet; with
    // the marker or without it (the database decides).
    std::vector<std::function<PlatformFeatures()>> unreachable = {
        probe_locked, probe_activatable, probe_kwallet_closed};
    for (const auto& probe : unreachable) {
      for (const char* marker : {"os", ""}) {
        c = ChoosePasswordStore(nullptr, marker, kOsKey, probe);
        EXPECT(c.store == "os" && !c.append_basic && c.wait);
        EXPECT(c.source == "cookies" && !c.reason.empty());
        EXPECT(c.record == (std::string(marker) != "os"));
        std::string w = PasswordStoreWarning(c, kOsKey, "");
        EXPECT(Contains(w, "holds cookies encrypted with the OS key"));
        EXPECT(Contains(w, "waits until it is unlocked"));
        EXPECT(Contains(w, "would delete those cookies"));
        EXPECT(Contains(w, c.reason));
      }
    }
    // A cookie database that can't be read counts as holding them.
    c = ChoosePasswordStore(nullptr, "", kUnknown, probe_locked);
    EXPECT(c.store == "os" && !c.append_basic && c.wait);
    EXPECT(Contains(PasswordStoreWarning(c, kUnknown, "file is not a database"),
                    "can't be read (file is not a database)"));
    // The marker alone never forces the wait: an "os" profile with no
    // OS-key cookies takes basic when the key can't be reached.
    c = ChoosePasswordStore(nullptr, "os", kNone, probe_activatable);
    EXPECT(c.store == "basic" && c.append_basic && !c.wait && !c.record);
    EXPECT(Contains(c.reason, "not running"));
    // A closed KWallet on a profile without OS-key cookies: basic.
    c = ChoosePasswordStore(nullptr, "", kNone, probe_kwallet_closed);
    EXPECT(c.store == "basic" && c.append_basic && !c.wait);
    EXPECT(Contains(c.reason, "KWallet"));
    // KWallet: the OS store is pinned to the daemon the probe found, the
    // wait included (Chromium waits on that daemon rather than one picked
    // from the environment that may not answer). No daemon, no pin; basic
    // and the Secret Service, no pin.
    {
      PlatformFeatures kwallet_open = Linux();
      kwallet_open.kwallet = true;
      kwallet_open.kwallet_state = KWalletState::kOpen;
      kwallet_open.kwallet_service = KWalletService::kKWalletd6;
      auto probe_kwallet_open = [&] { return kwallet_open; };
      for (ProfileCookieKeys keys : {kNone, kOsKey, kUnknown}) {
        for (const char* marker : {"os", ""}) {
          c = ChoosePasswordStore(nullptr, marker, keys, probe_kwallet_open);
          EXPECT(c.store == "os" && !c.append_basic && !c.wait);
          EXPECT(c.append_store == "kwallet6");
        }
      }
      kwallet_open.kwallet_service = KWalletService::kKWalletd5;
      c = ChoosePasswordStore(nullptr, "", kNone, probe_kwallet_open);
      EXPECT(c.append_store == "kwallet5");
      kwallet_closed.kwallet_service = KWalletService::kKWalletd6;
      c = ChoosePasswordStore(nullptr, "", kOsKey, probe_kwallet_closed);
      EXPECT(c.store == "os" && c.wait && c.append_store == "kwallet6");
      c = ChoosePasswordStore(nullptr, "", kNone, probe_kwallet_closed);
      EXPECT(c.store == "basic" && c.append_basic && c.append_store.empty());
      kwallet_closed.kwallet_service = KWalletService::kNone;
      kwallet_closed.kwallet_state = KWalletState::kNotRunning;
      c = ChoosePasswordStore(nullptr, "", kOsKey, probe_kwallet_closed);
      EXPECT(c.store == "os" && c.wait && c.append_store.empty());
      kwallet_closed.kwallet_state = KWalletState::kClosed;
      c = ChoosePasswordStore(nullptr, "", kOsKey, probe_unlocked);
      EXPECT(c.store == "os" && c.append_store.empty());
      c = ChoosePasswordStore(nullptr, "", kOsKey, probe_locked);
      EXPECT(c.store == "os" && c.wait && c.append_store.empty());
    }
    // An explicit --password-store wins and is not appended again (Chromium
    // reads it). An OS store is recorded when the profile lacks it; basic
    // never is, and leaves an "os" marker alone. The probe doesn't run.
    probes = 0;
    std::string explicit_store = "gnome-libsecret";
    c = ChoosePasswordStore(&explicit_store, "", kOsKey, probe_locked);
    EXPECT(c.store == "os" && !c.append_basic && c.record && !c.wait);
    EXPECT(c.source == "explicit" && !c.explicit_basic_deletes);
    c = ChoosePasswordStore(&explicit_store, "os", kNone, probe_locked);
    EXPECT(c.store == "os" && !c.record);
    explicit_store = "basic";
    c = ChoosePasswordStore(&explicit_store, "os", kNone, probe_unlocked);
    EXPECT(c.store == "basic" && !c.append_basic && !c.record);
    EXPECT(!c.explicit_basic_deletes);
    EXPECT(PasswordStoreWarning(c, kNone, "").empty());
    // Basic asked for on a profile with OS-key cookies: honoured, with the
    // warning that they will be deleted.
    for (ProfileCookieKeys keys : {kOsKey, kUnknown}) {
      c = ChoosePasswordStore(&explicit_store, "os", keys, probe_unlocked);
      EXPECT(c.store == "basic" && !c.append_basic && !c.wait);
      EXPECT(c.explicit_basic_deletes);
      std::string w = PasswordStoreWarning(c, keys, "locked");
      EXPECT(Contains(w, "--password-store=basic was given"));
      EXPECT(Contains(w, "deletes them"));
    }
    EXPECT(probes == 0);
    EXPECT(std::string(ProfileCookieKeysName(kOsKey)) == "os-key");

    // The marker file round trip: only "os" is written, atomically.
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("laufey-pf-marker-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    EXPECT(!WritePasswordStoreMarker(dir.string(), "basic"));
    EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    EXPECT(WritePasswordStoreMarker(dir.string(), "os"));
    EXPECT(ReadPasswordStoreMarker(dir.string()) == "os");
    EXPECT(WritePasswordStoreMarker(dir.string(), "os"));  // replaces it
    EXPECT(ReadPasswordStoreMarker(dir.string()) == "os");
    EXPECT(!WritePasswordStoreMarker(dir.string(), "kwallet"));
    EXPECT(!WritePasswordStoreMarker("", "os"));  // a profile in memory
    EXPECT(ReadPasswordStoreMarker("").empty());
    // Written through a temporary file renamed over it: nothing else is
    // left in the directory.
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      EXPECT(entry.path().filename() == kPasswordStoreMarkerName);
      ++entries;
    }
    EXPECT(entries == 1);
    // A directory that can't be written to: false, and nothing half done.
    EXPECT(!WritePasswordStoreMarker((dir / "missing").string(), "os"));
    // Anything else in the file counts as no choice (an older "basic"
    // marker included).
    for (const char* other : {"gnome-libsecret\n", "basic\n"}) {
      FILE* file =
          std::fopen((dir / kPasswordStoreMarkerName).string().c_str(), "wb");
      EXPECT(file);
      std::fputs(other, file);
      std::fclose(file);
      EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    }
#ifndef _WIN32
    // Temporary files a crashed write left: those older than this launch
    // go; a newer one (another instance writing now) and the marker stay.
    {
      std::string marker = (dir / kPasswordStoreMarkerName).string();
      std::string stale = marker + ".tmp4242";
      std::string fresh = marker + ".tmp4343";
      for (const std::string& p : {stale, fresh}) {
        FILE* file = std::fopen(p.c_str(), "wb");
        EXPECT(file);
        std::fclose(file);
      }
      auto now = std::filesystem::last_write_time(fresh);
      std::filesystem::last_write_time(stale, now - std::chrono::hours(1));
      int64_t launched_at = static_cast<int64_t>(std::time(nullptr)) - 60;
      RemoveStalePasswordStoreTemps(dir.string(), launched_at);
      EXPECT(!std::filesystem::exists(stale));
      EXPECT(std::filesystem::exists(fresh));
      EXPECT(std::filesystem::exists(marker));
      RemoveStalePasswordStoreTemps("", launched_at);  // a profile in memory
      std::filesystem::remove(fresh);
    }
#endif
    std::filesystem::remove_all(dir);
  }

  // --- The tray
  // ----------------------------------------------------------------
  f = Linux();
  EXPECT(TrayAvailable(f));
  EXPECT(TrayUnavailableReason(f).empty());
  // Stock GNOME: no watcher, no XEmbed tray.
  f.tray_watcher = false;
  f.desktop_hint = "GNOME";
  EXPECT(!TrayAvailable(f));
  std::string reason = TrayUnavailableReason(f);
  EXPECT(reason ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension (XDG_CURRENT_DESKTOP=GNOME; GNOME shows tray "
         "icons only with the AppIndicator extension enabled)");
  EXPECT(!Contains(reason, "XEmbed"));  // a Wayland session has none
  // No desktop is named outside the hint's suffix, which is the reason's
  // last part: cut there (a runtime that may not show the hint), the
  // wording is neutral.
  EXPECT(reason.substr(0, reason.find(" (XDG_CURRENT_DESKTOP=")) ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension");
  f.desktop_hint = "ubuntu:GNOME";
  EXPECT(Contains(TrayUnavailableReason(f), "AppIndicator extension"));
  f.desktop_hint = "sway";
  EXPECT(TrayUnavailableReason(f) ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension (XDG_CURRENT_DESKTOP=sway)");
  f.desktop_hint = "";
  EXPECT(TrayUnavailableReason(f) ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension");
  // X11: an XEmbed tray is a host too (appindicator falls back to it).
  f.session_type = "x11";
  EXPECT(Contains(TrayUnavailableReason(f), "XEmbed"));
  f.tray_xembed = true;
  EXPECT(TrayAvailable(f));
  // The library is required either way.
  f.tray_library = false;
  EXPECT(!TrayAvailable(f));
  EXPECT(Contains(TrayUnavailableReason(f), "libayatana-appindicator3"));

  // --- JSON
  // ----------------------------------------------------------------------
  f = Linux();
  f.desktop_hint = "KDE";
  f.portal_versions = {{"Notification", 2}, {"Settings", 2}};
  f.cookie_encryption = "os";
  f.notification_server = "Plasma";
  std::string json = PlatformFeaturesToJson(f);
  EXPECT(json ==
         "{\"os\":\"linux\",\"sessionType\":\"wayland\","
         "\"desktopHint\":\"KDE\",\"sessionBus\":true,\"trayHost\":true,"
         "\"trayReason\":null,\"trayClicks\":false,\"trayTooltip\":true,"
         "\"secretService\":\"available\",\"secretServicePrompt\":true,"
         "\"kwallet\":null,"
         "\"notificationServer\":\"Plasma\",\"notificationReason\":null,"
         "\"portalVersions\":{\"Notification\":2,\"Settings\":2},"
         "\"cookieEncryption\":\"os\",\"cookieEncryptionWait\":null,"
         "\"sandbox\":null,\"sandboxReason\":null,"
         "\"notificationTransport\":null,\"notificationColdStart\":false,"
         "\"notificationColdStartReason\":null,"
         "\"notificationScheduleWhileClosed\":false,"
         "\"notificationScheduleReason\":null,"
         "\"notificationServerCapabilities\":null,\"badge\":null,"
         "\"badgeReason\":null,\"fileChooser\":null,"
         "\"fileChooserReason\":null}");
  // The file chooser (L2): the portal's, or GTK's with the reason.
  {
    PlatformFeatures c = f;
    c.file_chooser = "gtk";
    c.file_chooser_reason = "xdg-desktop-portal offers no FileChooser here";
    EXPECT(Contains(PlatformFeaturesToJson(c),
                    "\"fileChooser\":\"gtk\",\"fileChooserReason\":"
                    "\"xdg-desktop-portal offers no FileChooser here\"}"));
  }
  // The notification platform's facts and the badge (L1).
  {
    PlatformFeatures l = f;
    l.notification_transport = "portal";
    l.notification_cold_start = true;
    l.notification_cold_start_reason = "ignored when it can";
    l.notification_schedule_reason = "no systemd user manager";
    l.notification_caps_known = true;
    l.notification_server_caps = {"actions", "body-markup"};
    l.badge = "title";
    l.badge_reason = "no dock reads launcher badges";
    std::string lj = PlatformFeaturesToJson(l);
    EXPECT(Contains(lj, "\"notificationTransport\":\"portal\""));
    EXPECT(Contains(lj,
                    "\"notificationColdStart\":true,"
                    "\"notificationColdStartReason\":null"));
    EXPECT(Contains(lj,
                    "\"notificationScheduleWhileClosed\":false,"
                    "\"notificationScheduleReason\":\"no systemd user "
                    "manager\""));
    EXPECT(Contains(lj,
                    "\"notificationServerCapabilities\":[\"actions\","
                    "\"body-markup\"]"));
    EXPECT(Contains(lj, "\"badge\":\"title\",\"badgeReason\":\"no dock"));
  }
  {
    PlatformFeatures k = f;
    k.kwallet = true;
    k.kwallet_state = KWalletState::kClosed;
    k.cookie_encryption_wait = "the wallet is closed";
    std::string kj = PlatformFeaturesToJson(k);
    EXPECT(Contains(kj, "\"kwallet\":\"closed\""));
    EXPECT(Contains(kj, "\"cookieEncryptionWait\":\"the wallet is closed\""));
  }

  // No notification server (Sway with no daemon): the Notification portal
  // is no proof; the reason says what is missing.
  {
    PlatformFeatures n = f;
    n.notification_server.clear();
    std::string reason = NotificationUnavailableReason(n);
    EXPECT(Contains(reason, "nothing owns org.freedesktop.Notifications"));
    EXPECT(Contains(PlatformFeaturesToJson(n),
                    "\"notificationServer\":null,\"notificationReason\":\"no "
                    "notification server"));
    n.notification_activatable = true;
    EXPECT(Contains(NotificationUnavailableReason(n), "can start one"));
    // Activatable, but the start failed (two units for the name): D-Bus's
    // reason, not "can start one".
    n.notification_activation_error =
        "D-Bus could not start the notification server for "
        "org.freedesktop.Notifications: unit is invalid";
    EXPECT(Contains(NotificationUnavailableReason(n), "unit is invalid"));
    EXPECT(!Contains(NotificationUnavailableReason(n), "can start one"));
    n.notification_activation_error.clear();
    n.session_bus = false;
    EXPECT(NotificationUnavailableReason(n) == "no D-Bus session bus");
    EXPECT(NotificationUnavailableReason(mac).empty());
  }

  f.tray_watcher = false;
  f.desktop_hint = "a\"b\\c\n";
  f.secret_service = SecretServiceState::kLocked;
  f.portal_versions.clear();
  f.cookie_encryption.clear();
  json = PlatformFeaturesToJson(f);
  EXPECT(Contains(json, "\"desktopHint\":\"a\\\"b\\\\c\\n\""));
  EXPECT(Contains(json, "\"trayHost\":false"));
  EXPECT(Contains(json, "\"trayReason\":\"no tray host"));
  EXPECT(Contains(json, "\"secretService\":\"locked\""));
  EXPECT(Contains(json, "\"portalVersions\":{}"));
  EXPECT(Contains(json, "\"cookieEncryption\":null"));

  json = PlatformFeaturesToJson(mac);
  EXPECT(Contains(json, "\"sessionType\":null"));
  EXPECT(Contains(json,
                  "\"notificationServer\":null,\"notificationReason\":null"));
  EXPECT(Contains(json, "\"secretService\":\"os\""));
  // Linux-only notification facts are null elsewhere.
  EXPECT(Contains(json, "\"notificationColdStart\":null"));
  EXPECT(Contains(json, "\"notificationScheduleWhileClosed\":null"));
  EXPECT(Contains(json, "\"notificationServerCapabilities\":null"));

  // The ABI string carries the backend's cookie-store decision.
  SetCookieEncryption("basic");
  char* abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi, "\"cookieEncryption\":\"basic\""));
  std::free(abi);
  SetCookieEncryption(nullptr);
  abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi, "\"cookieEncryption\":null"));
  EXPECT(abi && Contains(abi, "\"cookieEncryptionWait\":null"));
  std::free(abi);
  // A launch that waits for the OS key says why.
  SetCookieEncryption("os", "the keyring is locked");
  abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi,
                         "\"cookieEncryption\":\"os\","
                         "\"cookieEncryptionWait\":\"the keyring is "
                         "locked\""));
  std::free(abi);
  SetCookieEncryption(nullptr);
  // The CEF host's sandbox choice (Linux) and why.
  SetSandboxMode("off", "no chrome-sandbox helper");
  abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi,
                         "\"sandbox\":\"off\","
                         "\"sandboxReason\":\"no chrome-sandbox helper\""));
  std::free(abi);
  SetSandboxMode(nullptr, nullptr);
  abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi, "\"sandbox\":null,\"sandboxReason\":null"));
  std::free(abi);

  std::printf("laufey_platform_features_test: OK\n");
  return 0;
}
