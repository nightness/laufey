// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Global shortcuts and launch at login (API 40). See
// docs/global-shortcuts.md and docs/launch-at-login.md.
//
// The portable part (shortcuts.cc, login_item.cc) is what every CEF and
// WebView backend shares: accelerator parsing and its canonical form, the
// registry of this app's shortcuts (ids, the exactly-once registration
// result, the handler and its dispatch, the test hook), and the login-entry
// naming and file formats. The per-OS parts talk to the OS:
// shortcuts_mac.mm (Carbon RegisterEventHotKey), shortcuts_win.cc
// (RegisterHotKey on the UI thread), shortcuts_linux.cc (XGrabKey through
// XCB on X11, the XDG GlobalShortcuts portal on Wayland), and
// login_item_mac.mm (SMAppService), login_item_win.cc (HKCU Run),
// login_item_linux.cc (XDG autostart).

#ifndef LAUFEY_SYSTEM_H_
#define LAUFEY_SYSTEM_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "laufey.h"

namespace laufey_common {

// --- Accelerators ------------------------------------------------------------

// Modifier bits of a parsed accelerator. kModSuper is Command on macOS, the
// Windows key on Windows and Super on Linux; "CommandOrControl" resolves to
// kModSuper on macOS and kModCtrl elsewhere.
constexpr uint32_t kModCtrl = 1u << 0;
constexpr uint32_t kModAlt = 1u << 1;
constexpr uint32_t kModShift = 1u << 2;
constexpr uint32_t kModSuper = 1u << 3;

// The kind of key an accelerator names.
enum class KeyKind {
  kLetter,    // "A".."Z"; `ch` is the upper-case letter
  kDigit,     // "0".."9"; `ch` is the digit
  kPunct,     // one of - = [ ] \ ; ' , . / `; `ch` is the character
  kFunction,  // "F1".."F24"; `number` is 1..24
  kNumpad,    // "Num0".."Num9" (`number` 0..9) and NumDecimal / NumAdd /
              // NumSubtract / NumMultiply / NumDivide (`named`)
  kNamed,     // Space, Tab, Enter, ... (`named`)
};

// Named keys (kNamed, plus the numpad operators under kNumpad).
enum class NamedKey {
  kNone,
  kSpace,
  kTab,
  kBackspace,
  kDelete,
  kInsert,
  kEnter,
  kEscape,
  kUp,
  kDown,
  kLeft,
  kRight,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
  kPrintScreen,
  kMediaPlayPause,
  kMediaNextTrack,
  kMediaPreviousTrack,
  kMediaStop,
  kVolumeUp,
  kVolumeDown,
  kVolumeMute,
  kNumDecimal,
  kNumAdd,
  kNumSubtract,
  kNumMultiply,
  kNumDivide,
};

struct Accelerator {
  uint32_t mods = 0;  // kMod* bits
  KeyKind kind = KeyKind::kNamed;
  char ch = 0;
  int number = 0;
  NamedKey named = NamedKey::kNone;
};

// Parses the menu accelerator syntax ("CommandOrControl+Shift+K",
// "Alt+F4", "Ctrl+Num5"): '+'-separated, case-insensitive, modifiers first,
// exactly one key last. Modifiers: Command / Cmd (macOS only), Control /
// Ctrl, CommandOrControl / CmdOrCtrl, Alt / Option, Shift, Super / Meta.
// Keys name physical keys, so shifted symbols ("Plus", "!") are not keys:
// write "Shift+=" or "Shift+1". `for_mac` resolves Command /
// CommandOrControl (tests pass either). Returns false (with a reason in `error`, if non-null) for an
// empty or unknown token, a repeated modifier, no key or two keys, or a
// Command modifier off macOS.
bool ParseAccelerator(const std::string& text, bool for_mac, Accelerator* out,
                      std::string* error);

// The canonical spelling: "Ctrl+Alt+Shift+Super+<Key>" with only the
// modifiers present, keys as "K", "5", "F12", "Num5", "NumAdd", "Space",
// "PageUp", "-", ... Two spellings of one combination have one canonical
// form.
std::string CanonicalAccelerator(const Accelerator& accel);

// Whether a parsed accelerator may be a global shortcut: a key used while
// typing (a letter, digit, punctuation, Space, any numpad key, Enter, Tab,
// Backspace, Delete) needs a modifier other than Shift, so a shortcut can't
// swallow (or observe) ordinary typing in other apps.
bool IsAllowedGlobalShortcut(const Accelerator& accel);

// --- Global shortcuts ----------------------------------------------------------

// The OS side of global shortcuts, one per process (see
// InstallShortcutPlatform). The registry calls Bind / Unbind with ids it
// allocates (1..LAUFEY_MAX_SHORTCUTS * 2, reused after release).
class ShortcutPlatform {
 public:
  virtual ~ShortcutPlatform() = default;
  // LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS / _SHORTCUTS_USER_BINDS. May block
  // briefly the first time (the Linux portal probe).
  virtual uint32_t Capabilities() = 0;
  // Binds `accel` under `id`. Calls `done` exactly once with a
  // LAUFEY_SHORTCUT_* status, from any thread (never synchronously while
  // holding a lock the registry holds). Presses then go to
  // DispatchShortcut(id) on the UI thread.
  virtual void Bind(uint32_t id, const Accelerator& accel,
                    std::function<void(int)> done) = 0;
  // Releases the binding made under `id` (a successful Bind). Any thread;
  // may complete asynchronously.
  virtual void Unbind(uint32_t id) = 0;
};

// Installs the platform (once, at backend start; later calls replace it).
// Without one, every registration answers NOT_SUPPORTED.
void InstallShortcutPlatform(std::unique_ptr<ShortcutPlatform> platform);

// The laufey_backend_api_t entry points (see laufey.h); any thread.
uint32_t ShortcutCapabilities();
void SetShortcutHandler(laufey_shortcut_fn handler, void* user_data);
void RegisterShortcut(const char* accelerator,
                      laufey_shortcut_result_fn callback, void* user_data);
bool UnregisterShortcut(const char* accelerator);
void UnregisterAllShortcuts();
char* ListShortcuts();
// malloc'd canonical form for this OS, or NULL when it doesn't parse.
char* CanonicalizeAccelerator(const char* accelerator);
bool TestTriggerShortcut(const char* accelerator);

// From the platform: the shortcut bound under `id` was pressed. Calls the
// handler (if any, and if `id` is still registered). UI thread.
void DispatchShortcut(uint32_t id);

#ifdef __APPLE__
std::unique_ptr<ShortcutPlatform> CreateShortcutPlatformMac();
#endif
#ifdef _WIN32
// `run_on_ui` runs a task on the backend's UI thread (inline when already
// there); the hotkey window lives there, so WM_HOTKEY arrives on it.
std::unique_ptr<ShortcutPlatform> CreateShortcutPlatformWin(
    std::function<void(std::function<void()>)> run_on_ui);
#endif
#ifdef __linux__
// X11 (XCB key grabs on the root window) or, in a Wayland session, the XDG
// GlobalShortcuts portal, on laufey's own thread; presses reach the handler
// through GtkRunAsync. LAUFEY_GLOBAL_SHORTCUTS=x11|portal|off forces a
// choice.
std::unique_ptr<ShortcutPlatform> CreateShortcutPlatformLinux();

// The portal's trigger syntax for an accelerator ("CTRL+SHIFT+k"), from the
// XDG shortcuts specification. Exposed for tests.
std::string PortalTriggerFor(const Accelerator& accel);
#endif

// --- Launch at login -------------------------------------------------------------

// The name the login entry goes by: LAUFEY_APP_ID / the launch file's
// "appId" when set, else the executable's file name without its extension
// ("" if neither is known).
std::string LoginItemName();

// The laufey_backend_api_t entry points: LAUFEY_LOGIN_ITEM_* states. Any
// thread. `error` (may be null) receives a message on FAILED.
int GetLaunchAtLogin();
int SetLaunchAtLogin(bool enabled, std::string* error);

// The XDG autostart entry for `exec_path` (Linux; portable for tests):
// a [Desktop Entry] with the Exec line quoted and escaped per the Desktop
// Entry specification.
std::string BuildAutostartEntry(const std::string& name,
                                const std::string& exec_path);
// Whether an autostart entry's text turns the app on: no "Hidden=true" and
// no "X-GNOME-Autostart-enabled=false" in its [Desktop Entry] group.
bool AutostartEntryEnabled(const std::string& text);
// The Exec value for a path: quoted, with '"', '`', '$' and '\\' escaped,
// '%' doubled, then the backslashes escaped again for the string value.
std::string DesktopExecQuote(const std::string& path);

}  // namespace laufey_common

#endif  // LAUFEY_SYSTEM_H_
