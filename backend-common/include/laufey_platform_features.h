// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Platform features (API 45): what THIS session provides, probed instead of
// guessed from the desktop's name. Linux desktops differ in what they offer
// (a tray host, a secret service that can answer without a prompt, which
// xdg-desktop-portal interfaces at which version), and a feature the session
// lacks must be reported with a reason, never fail silently.
//
//   platform_features.cc        the portable part: the cookie-store decision,
//                               the tray reason and the JSON the C ABI hands
//                               out; the static answer on macOS and Windows
//   platform_features_linux.cc  the probe over the session bus (GDBus) and,
//                               for the XEmbed tray, the X server (XCB)
//
// XDG_CURRENT_DESKTOP is read as a hint for wording a reason, and only to
// mirror Chromium's own password-store choice (ChromiumPicksKWallet); no
// feature is guessed from it. See docs/platform-features.md.

#ifndef LAUFEY_PLATFORM_FEATURES_H_
#define LAUFEY_PLATFORM_FEATURES_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace laufey_common {

// Whether the Secret Service (org.freedesktop.secrets) can hand out a key.
enum class SecretServiceState {
  kAvailable,     // running, its default collection is unlocked
  kLocked,        // running, the default collection is locked or missing:
                  // using it means an unlock (or create) prompt
  kActivatable,   // not running, D-Bus can start it (its state is unknown
                  // until it runs; starting it is left to its first user)
  kAbsent,        // no provider on the session bus
  kNoSessionBus,  // no session bus to ask
  kNotApplicable  // not Linux: the OS keystore (Keychain, DPAPI) is always
                  // there and never blocks on a prompt
};

const char* SecretServiceStateName(SecretServiceState state);

// KWallet, where Chromium's cookie store would use it (`kwallet`): whether
// it can hand out the key without anyone answering anything. Read from
// kwalletd over D-Bus (org.kde.KWallet isEnabled / localWallet / isOpen),
// never starting it. Only an open wallet answers: on Plasma, Chromium's
// request for a closed wallet's key is never answered, with a person in
// front or not (no unlock prompt reaches anyone).
enum class KWalletState {
  kNotUsed,     // Chromium wouldn't use KWallet here
  kOpen,        // kwalletd runs, enabled, its local wallet is open
  kClosed,      // kwalletd runs, enabled, its local wallet is closed
  kDisabled,    // kwalletd runs, KWallet is disabled
  kNotRunning,  // no kwalletd (kwalletd6 / kwalletd5 / kwalletd) answers
                // at its own object path (none runs, or only a name with no
                // object behind it, as kwalletd6's org.kde.kwalletd)
};

// "open", "closed", "disabled", "not-running"; nullptr for kNotUsed.
const char* KWalletStateName(KWalletState state);

// A kwalletd as Chromium's cookie store addresses it: one D-Bus name and
// one object path (FreedesktopSecretKeyProvider::InitializeKWallet). A
// daemon answers only at its own path: Plasma 6's kwalletd6 also owns
// org.kde.kwalletd but has no /modules/kwalletd object, so Chromium's
// KDE 4 choice reaches nothing there.
enum class KWalletService {
  kNone,       // not KWallet: the Secret Service
  kKWalletd,   // org.kde.kwalletd   /modules/kwalletd   (KDE 3 / KDE 4)
  kKWalletd5,  // org.kde.kwalletd5  /modules/kwalletd5  (KDE 5)
  kKWalletd6,  // org.kde.kwalletd6  /modules/kwalletd6  (KDE 6)
};

// The --password-store value that makes Chromium use exactly this daemon
// ("kwallet", "kwallet5", "kwallet6"); nullptr for kNone.
const char* KWalletPasswordStore(KWalletService service);

// What the probe found. Strings are empty when not applicable.
struct PlatformFeatures {
  std::string os;            // "linux", "macos", "windows"
  std::string session_type;  // Linux: XDG_SESSION_TYPE ("wayland", "x11",
                             // "tty"), "unknown" when unset; a graphical
                             // one follows the display that is there
                             // (ReportedSessionType)
  std::string desktop_hint;  // XDG_CURRENT_DESKTOP, verbatim (a hint only)
  bool session_bus = false;  // Linux: a session bus answered

  // Tray icons: a StatusNotifierItem host (org.kde.StatusNotifierWatcher)
  // or, on X11, an XEmbed system tray (_NET_SYSTEM_TRAY_S<n>), and the
  // appindicator library laufey drives them with.
  bool tray_watcher = false;  // org.kde.StatusNotifierWatcher has an owner
  bool tray_xembed = false;   // X11 sessions only: a system tray selection
                              // owner (never probed in a Wayland session,
                              // whose $DISPLAY is Xwayland's)
  bool tray_library = true;   // libayatana-appindicator3 / libappindicator3
  bool tray_clicks = true;    // the icon reports left / double clicks
  bool tray_tooltip = true;   // set_tray_tooltip shows something

  SecretServiceState secret_service = SecretServiceState::kNotApplicable;
  // Chromium's cookie store would use KWallet here, not the Secret Service:
  // the desktop is KDE as Chromium reads it (ChromiumPicksKWallet). Only
  // the desktop decides, as in Chromium: on GNOME (or any other desktop)
  // it uses the Secret Service even with kwalletd running.
  bool kwallet = false;
  // KWallet's state when `kwallet` (kNotUsed otherwise): the state of the
  // daemon in `kwallet_service`.
  KWalletState kwallet_state = KWalletState::kNotUsed;
  // When `kwallet`: the kwalletd that answered at its own object path, the
  // one Chromium's choice names (ChromiumKWalletService) when it answers,
  // else another that runs (Plasma 6's kwalletd6 when KDE_SESSION_VERSION
  // is missing). The cookie store is pinned to it (--password-store=
  // KWalletPasswordStore), so Chromium asks the daemon probed here. kNone
  // when none answers (kNotRunning) or KWallet isn't used.
  KWalletService kwallet_service = KWalletService::kNone;
  // A person can answer an unlock prompt here: a graphical session
  // (XDG_SESSION_TYPE x11 / wayland with a display and, where logind can
  // say, an active x11 / wayland logind session), and the provider's
  // prompter (gnome-keyring's gcr-prompter) exists.
  bool secret_prompter = true;

  // Notifications: the name of the server that owns
  // org.freedesktop.Notifications right now (GetServerInformation; "unknown"
  // when it doesn't say), empty when nothing owns it; and whether D-Bus
  // could start one. The Notification portal's version is no proof: on
  // Sway with no daemon the portal still offers it, with nothing behind it.
  std::string notification_server;
  bool notification_activatable = false;
  // Activatable, but this process's start of it failed (D-Bus's error).
  std::string notification_activation_error;
  // Linux, from the notification platform (LinuxNotificationFacts): how
  // notifications are sent ("portal" or "freedesktop"; empty with no
  // server), whether a click starts the app when it isn't running, whether
  // a scheduled one is posted while it is closed (a systemd user timer),
  // each with why not, and the server's GetCapabilities.
  std::string notification_transport;
  bool notification_cold_start = false;
  std::string notification_cold_start_reason;
  bool notification_schedule_while_closed = false;
  std::string notification_schedule_reason;
  bool notification_caps_known = false;
  std::vector<std::string> notification_server_caps;

  // How the dock badge shows: "dock" (macOS), "launcher-entry" (Linux, a
  // dock reads com.canonical.Unity.LauncherEntry), "title" (a "(N) "
  // prefix on the window titles: Windows, and Linux without such a dock),
  // and why not a launcher (Linux).
  std::string badge;
  std::string badge_reason;

  // xdg-desktop-portal interface -> version ("Notification" -> 2). An
  // interface the portal lacks is absent from the map.
  std::map<std::string, uint32_t> portal_versions;

  // Linux: which file chooser a file dialog uses (API 47): "portal" (the
  // portal's FileChooser, the desktop's own dialog) or "gtk" (GTK's own
  // chooser), and why GTK's. Empty elsewhere.
  std::string file_chooser;
  std::string file_chooser_reason;

  // CEF only: "os" (OSCrypt keeps its key in the OS keystore) or "basic"
  // (the key is fixed, cookies are only obfuscated: --password-store=basic on
  // Linux, Chromium's mock keychain on macOS).
  // Empty on engines that don't encrypt with an OS key (WebKit, Winit).
  std::string cookie_encryption;
  // CEF only: why this launch keeps the OS key store although no one may be
  // able to unlock it (the profile holds cookies encrypted with the OS key,
  // which basic would delete): requests that carry cookies wait until the
  // keystore is unlocked. Empty otherwise.
  std::string cookie_encryption_wait;
  // CEF on Linux: the Chromium sandbox this launch runs web content in
  // ("namespace", "setuid", "chromium": on, Chromium picks the layer; "off")
  // and why (laufey_cef_sandbox.h). Empty elsewhere.
  std::string sandbox;
  std::string sandbox_reason;
};

// --- The probe
// ----------------------------------------------------------------

// The secret-service part on its own, for a decision made before the event
// loop runs (CEF's command line). Synchronous, bounded by short D-Bus
// timeouts, never starts a service and never shows a prompt. Any thread.
void ProbeSecretService(PlatformFeatures* out);

// The tray part on its own (session facts plus the tray fields), for
// create_tray_icon: no portal calls. Any thread.
void ProbeTray(PlatformFeatures* out);

// The full probe. The session facts and the secret service are probed once
// per process; the tray host is re-read on every call (a watcher that
// appears late counts as soon as it does: the Linux probe follows the
// watcher's NameOwnerChanged); the portal versions are probed once, on the
// first call. Any thread.
PlatformFeatures ProbePlatformFeatures();

// --- Decisions (pure; tested without a bus)
// ------------------------------------

// The kwalletd Chromium's cookie store asks when no --password-store is
// given (M149 os_crypt_async FreedesktopSecretKeyProvider::GetKey over
// base::nix::GetDesktopEnvironment), kNone where it uses the Secret
// Service. The desktop is the first one XDG_CURRENT_DESKTOP names that
// Chromium knows; "KDE" is KDE 5 with KDE_SESSION_VERSION "5", KDE 6 with
// "6", and KDE 4 otherwise (unset included: ssh, a systemd unit, a wrapper
// that copies part of the environment). With none it knows, DESKTOP_SESSION
// kde4 / kde-plasma / kde (KDE 4 or 3); else (no GNOME_DESKTOP_SESSION_ID
// set) KDE_FULL_SESSION set (KDE 4 or 3). KDE 3 / 4 is org.kde.kwalletd at
// /modules/kwalletd, KDE 5 kwalletd5, KDE 6 kwalletd6. A variable counts as
// set even when empty, as Chromium's HasVar does. `env` returns a
// variable's value, nullptr when unset. The one place the desktop's name
// decides anything: it is Chromium's own rule.
KWalletService ChromiumKWalletService(
    const std::function<const char*(const char*)>& env);

// Whether Chromium's cookie store would use KWallet at all
// (ChromiumKWalletService is not kNone).
bool ChromiumPicksKWallet(const std::function<const char*(const char*)>& env);

// True when no one here can hand Chromium's cookie store its OS key: the
// Secret Service is locked (or not running and may start locked) and no one
// can answer its unlock prompt; or, where Chromium would use KWallet, the
// wallet isn't open (KWalletState). Chromium would wait for that key
// forever, holding every request that carries cookies. Then the store is
// --password-store=basic, unless the profile holds cookies encrypted with
// the OS key (ChoosePasswordStore). When there is no Secret Service at all
// (or no session bus), Chromium falls back to basic by itself, so the
// choice is left to it.
bool NeedsBasicPasswordStore(const PlatformFeatures& f);

// --- The display a Linux process reaches (pure; tested without a display)
// ----------------------------------------------------------------------

// The display server this process's windows go to, from the display that
// is actually there: "wayland" when $WAYLAND_DISPLAY names a socket that
// exists (an absolute path, else one under $XDG_RUNTIME_DIR, as libwayland
// resolves it) or $WAYLAND_SOCKET hands one over; else "x11" when $DISPLAY
// is set; else "" (no display). $XDG_SESSION_TYPE is never read: it is only
// a hint, and a wrong one is common (GDM's autologin into an Xorg session,
// XFCE or i3, leaves it "wayland" with only $DISPLAY set). Everything keyed
// on the session's display (Chromium's Ozone platform, the clipboard and
// global shortcut backends, the session type platform_features reports)
// follows this. `env` returns a variable's value, nullptr when unset;
// `is_socket` says whether a path is a Unix socket.
std::string DisplayBackend(
    const std::function<const char*(const char*)>& env,
    const std::function<bool(const std::string&)>& is_socket);
// The same, from this process's environment and file system ("" on
// Windows; only meaningful on Linux).
std::string DisplayBackend();

// The session type platform_features reports: $XDG_SESSION_TYPE as set
// ("unknown" when unset), except that a graphical one ("x11" / "wayland")
// says which display there is (DisplayBackend's `display_backend`) when one
// is there. A display alone never makes a session graphical: "tty" and
// "unknown" stay as they are (Xvfb under cron or a systemd service has a
// $DISPLAY and no one in front of it).
std::string ReportedSessionType(const std::string& xdg_session_type,
                                const std::string& display_backend);

// Whether the WebKitGTK host sets WEBKIT_DMABUF_RENDERER_FORCE_SHM=1: on an
// X11 display (GDK's, `gdk_display_is_x11`), unless the user chose a
// renderer setting of their own (WEBKIT_DMABUF_RENDERER_FORCE_SHM,
// WEBKIT_DISABLE_DMABUF_RENDERER or WEBKIT_DISABLE_COMPOSITING_MODE set,
// even empty). GTK 3 can't show a GPU buffer on X11: WebKit's UI process
// maps each GBM buffer to the CPU (gbm_bo_map) to draw it with cairo. With
// shared memory the web process does the same read back itself, so the
// frame costs the same copy, and the UI process doesn't use libgbm (whose
// teardown during exit() crashed a paint before the exit guard parked the
// UI thread first; docs/backends.md).
bool ShouldForceWebKitShm(bool gdk_display_is_x11,
                          const std::function<const char*(const char*)>& env);

// --- The cookie store per profile (CEF on Linux)
// -------------------------------------------------------------------

// Whether a CEF profile holds cookies encrypted with the OS key. Chromium
// writes those with a "v11" prefix (basic ones "v10"). A v11 cookie it can't
// decrypt (--password-store=basic has no v11 key) is dropped, and its whole
// eTLD+1 group is then deleted from the database
// (sqlite_persistent_cookie_store.cc): switching such a profile to basic,
// even for one launch, loses those cookies for good. Cookies written under
// basic stay readable under the OS key, so basic -> os loses nothing.
enum class ProfileCookieKeys {
  kNone,     // no cookie database, or one without v11 rows
  kOsKey,    // at least one v11 row
  kUnknown,  // a database that can't be read (corrupt, locked by a running
             // instance, no libsqlite3): counts as kOsKey
};

const char* ProfileCookieKeysName(ProfileCookieKeys keys);

// Reads `<root_cache_dir>/Default/Cookies` (and Default/Network/Cookies)
// read-only with SQLite (the system libsqlite3, loaded at run time): any row
// whose encrypted_value starts with "v11". `detail` (optional) says why
// kUnknown. kNone for an empty root (a profile kept in memory). Linux only;
// kNone elsewhere.
ProfileCookieKeys ReadProfileCookieKeys(const std::string& root_cache_dir,
                                        std::string* detail = nullptr);

// The file in a CEF root cache directory that records that its profile
// chose the OS key ("os": the store was left to Chromium). Only "os" is
// recorded. It is a hint for the reader: the profile's cookie database
// (ReadProfileCookieKeys) decides whether the profile may use basic.
extern const char kPasswordStoreMarkerName[];

// "os" when the profile recorded it, else "" (none, unreadable, or any
// other value: an older "basic" marker counts as none).
std::string ReadPasswordStoreMarker(const std::string& root_cache_dir);

// Records "os", atomically and durably (a temporary file, synced, renamed
// over the marker, and the directory synced). Any other store is refused.
// False when it can't be written (no directory: a profile kept in memory).
bool WritePasswordStoreMarker(const std::string& root_cache_dir,
                              const std::string& store);

// Removes temporary marker files (`<marker>.tmp*`) last modified before
// `launched_at` (a launch that crashed mid-write left them). A newer one
// may be another instance's write in progress and is left. POSIX only.
void RemoveStalePasswordStoreTemps(const std::string& root_cache_dir,
                                   int64_t launched_at);

struct PasswordStoreChoice {
  std::string store;          // "basic" or "os": what this launch uses
  bool append_basic = false;  // add --password-store=basic to the command line
  bool record = false;        // write "os" to the marker
  // "explicit" (--password-store on the command line), "cookies" (the
  // profile holds OS-key cookies), "profile" (the marker, with a reachable
  // key), "probe" (this launch's platform features).
  std::string source;
  // Why no one can hand out the OS key (NeedsBasicPasswordStore), when that
  // decided anything: the store is basic, or `wait`.
  std::string reason;
  // The profile holds OS-key cookies (or a cookie database that can't be
  // read) and no one may be able to unlock the key: the OS store is kept
  // anyway, and requests that carry cookies wait until it is unlocked.
  // Basic would make Chromium delete those cookies.
  bool wait = false;
  // An explicit --password-store=basic on a profile that holds OS-key
  // cookies: honoured, but Chromium deletes them.
  bool explicit_basic_deletes = false;
  // The --password-store value to add ("kwallet", "kwallet5", "kwallet6"),
  // "" for none: the OS store on KWallet, pinned to the kwalletd the probe
  // found (PlatformFeatures::kwallet_service). Without it Chromium picks
  // its daemon from the environment alone and, where that one doesn't
  // answer, gives up on the OS key: it falls back to basic, and drops a
  // profile's OS-key cookies, while laufey reports "os".
  std::string append_store;
};

// Picks the store: an explicit --password-store wins (an OS store is
// recorded; basic on a profile with OS-key cookies sets
// explicit_basic_deletes). Otherwise the probe (NeedsBasicPasswordStore):
// with the key reachable, os (recorded); without, basic, unless `cookies`
// is kOsKey or kUnknown: then os and `wait`, never basic. The marker never
// overrides the database: an "os" profile without OS-key cookies still gets
// basic when the key can't be reached (nothing is lost). An OS store on
// KWallet is pinned to the daemon the probe found (append_store), the wait
// included: Chromium then waits on that daemon instead of picking one from
// the environment that may not answer.
PasswordStoreChoice ChoosePasswordStore(
    const std::string* explicit_store, const std::string& marker,
    ProfileCookieKeys cookies, const std::function<PlatformFeatures()>& probe);

// The stderr line for a choice ("" when there is nothing to say). `detail`
// is ReadProfileCookieKeys's.
std::string PasswordStoreWarning(const PasswordStoreChoice& choice,
                                 ProfileCookieKeys cookies,
                                 const std::string& detail);

// Whether a tray icon can be shown, and why not ("" when it can).
bool TrayAvailable(const PlatformFeatures& f);

std::string TrayUnavailableReason(const PlatformFeatures& f);

// Why no one can hand out the OS key, in a sentence (the Secret Service's or
// KWallet's state), for the warnings; "" when someone can.
std::string BasicPasswordStoreReason(const PlatformFeatures& f);

// Why notifications may not show ("" when a server runs; Linux only).
std::string NotificationUnavailableReason(const PlatformFeatures& f);

// The JSON object platform_features hands out (docs/platform-features.md).
std::string PlatformFeaturesToJson(const PlatformFeatures& f);

// --- Backend hooks
// ---------------------------------------------------------------

// The cookie-store decision a CEF backend made ("os" / "basic"), and why it
// waits for the OS key (PasswordStoreChoice::wait; nullptr: it doesn't);
// reported by PlatformFeaturesJsonForAbi. Any thread.
void SetCookieEncryption(const char* value, const char* wait = nullptr);

// The Chromium sandbox a CEF backend chose (LinuxSandboxModeName) and why;
// reported by PlatformFeaturesJsonForAbi. Any thread.
void SetSandboxMode(const char* mode, const char* reason);

// ProbePlatformFeatures() as JSON, malloc'd for the C ABI (freed with the
// backend's string_free). Any thread.
char* PlatformFeaturesJsonForAbi();

// The tray reason malloc'd for the C ABI (tray_unavailable_reason), or
// nullptr when a tray icon can be shown. The tray part of the probe only.
// Any thread.
char* TrayUnavailableReasonForAbi();

// The platform-features change handler (set_platform_features_changed_
// handler, API 45). On Linux it fires when the StatusNotifierWatcher's owner
// appears or goes away (the probe's NameOwnerChanged subscription, made
// here if it wasn't yet), on the thread that runs the default GLib main
// context. A null handler clears it. Any thread.
void SetPlatformFeaturesChangedHandler(void (*handler)(void* user_data),
                                       void* user_data);

// Test-only: forget every cached probe result (the next call probes again).
void ResetPlatformFeaturesForTesting();

// Test-only: how many times the probe connected to an X server for the
// XEmbed tray (only ever in an X11 session).
int XEmbedProbeCountForTesting();

}  // namespace laufey_common

#endif  // LAUFEY_PLATFORM_FEATURES_H_
