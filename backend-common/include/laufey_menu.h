// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Menus (API 41): the portable half shared by every CEF and WebView backend.
//
// - ParseMenuTemplate turns a laufey_value_t menu template into plain
//   MenuEntry structs once (freeing every value it reads), with each item's
//   "accelerator" parsed by the global-shortcut parser (laufey_system.h), so
//   the per-OS builders (menu_mac.mm, menu_linux.cc, win32_menu.h, the CEF
//   Views menu) and their accelerator tables agree on one syntax.
// - The context-menu session tracks the one open context menu so its close
//   callback fires exactly once and the test hook can dismiss it.
// - The accelerator key helpers map a parsed accelerator to each platform's
//   key code (layout-aware where the platform offers it).
//
// See docs/menus.md.

#ifndef LAUFEY_MENU_H_
#define LAUFEY_MENU_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_system.h"

namespace laufey_common {

// One entry of a menu template.
struct MenuEntry {
  enum class Kind { kItem, kSubmenu, kSeparator, kRole };
  Kind kind = Kind::kItem;
  std::string label;
  std::string id;    // kItem: the "id" ("" when the template has none)
  std::string role;  // kRole: the "role" as written ("selectAll", "quit", ...)
  std::string tooltip;
  // kItem: the "accelerator" as written, and whether it parsed (for this OS)
  // into `accel`. One that doesn't parse is dropped (with a warning once per
  // spelling): the item works, without a key.
  std::string accelerator_text;
  bool has_accel = false;
  Accelerator accel;
  bool enabled = true;
  bool checked = false;
  std::vector<uint8_t> icon_png;
  std::vector<MenuEntry> children;  // kSubmenu

  // The string a click reports: the id, else the label (the Linux and
  // Windows builders' long-standing fallback; macOS only reports ids).
  const std::string& ClickKey() const {
    return id.empty() ? label : id;
  }
};

// Parses `menu_template` (a list of item dicts, see laufey.h
// set_application_menu). Does not take ownership of `menu_template`; every
// value fetched from it is freed. Returns an empty list for NULL or a
// non-list. `for_mac` resolves Command / CommandOrControl.
std::vector<MenuEntry> ParseMenuTemplate(laufey_value_t* menu_template,
                                         const laufey_backend_api_t* api,
                                         bool for_mac);

// Whether `role` (case-insensitive) is `name` ("selectall" == "selectAll").
bool RoleIs(const std::string& role, const char* name);

// The accelerator as menus display it on Windows and Linux: the canonical
// form with "Super" spelled "Win" on Windows ("Ctrl+Shift+N", "Alt+F4").
std::string AcceleratorDisplayText(const Accelerator& accel);

// A Windows virtual-key code for the accelerator's key using the US layout
// (what CEF reports as windows_key_code on every platform), or 0 for a key
// without one.
int AcceleratorUsVirtualKey(const Accelerator& accel);

#ifdef _WIN32
// The virtual-key code in the current keyboard layout (so "Ctrl+Z" is the key
// labelled Z on any layout), or 0. Shared with global shortcuts.
unsigned AcceleratorVirtualKey(const Accelerator& accel);
#endif

#ifdef __linux__
// The X11 keysym / GDK keyval for the accelerator's key, or 0. Shared with
// global shortcuts.
uint32_t AcceleratorKeysym(const Accelerator& accel);
#endif

// --- Context-menu session
// -----------------------------------------------------
//
// A backend calls BeginContextMenu right before showing a context menu, with
// a `dismiss` function that closes the menu as Escape would (run on any
// thread: it must hop to the UI thread itself), and EndContextMenu once the
// menu has closed (after the item's click, if any). EndContextMenu fires the
// close callback exactly once per session; ending an unknown or ended
// session is a no-op.

uint64_t BeginContextMenu(uint32_t window_id, laufey_menu_closed_fn on_closed,
                          void* on_closed_data, std::function<void()> dismiss);
void EndContextMenu(uint64_t session);

// Backs test_dismiss_context_menu: runs the newest open session's dismiss
// function. Returns false when no context menu is open.
bool DismissOpenContextMenu();

// Fires `on_closed` (if any) right away: for a request that showed nothing.
void FireContextMenuClosedNow(uint32_t window_id,
                              laufey_menu_closed_fn on_closed,
                              void* on_closed_data);

// --- Native modal loops
// ------------------------------------------------------
//
// A menu that runs the OS's own modal loop on the UI thread
// (TrackPopupMenu) holds that thread until it closes. The loop still
// dispatches the thread's window messages, but a backend whose task queue
// doesn't run inside a nested OS loop starves while it is open: Chromium,
// and so CEF, runs no tasks there unless told to. Everything the runtime
// sends through that queue then waits for the menu (a binding call from the
// page, a synchronous UI-thread call, dispatch_ui_task), and a runtime that
// blocks on one of them stops altogether.
//
// Such a backend installs a hook, which is told when the outermost of these
// loops starts (true) and ends (false); CEF uses it to allow its tasks to
// run inside the loop (CefSetNestableTasksAllowed). The menu code brackets
// its modal loop with a ScopedNativeModalLoop. Nesting is counted, so the
// hook sees one true / false pair per outermost loop. Only a loop on the UI
// thread (UiTaskDispatcher::IsUiThread) reaches the hook: a modal run on any
// other thread holds no backend task, and the hook is a UI-thread call.

using NativeModalLoopHook = void (*)(bool entering);
void SetNativeModalLoopHook(NativeModalLoopHook hook);

class ScopedNativeModalLoop {
 public:
  ScopedNativeModalLoop();
  ~ScopedNativeModalLoop();
  ScopedNativeModalLoop(const ScopedNativeModalLoop&) = delete;
  ScopedNativeModalLoop& operator=(const ScopedNativeModalLoop&) = delete;

 private:
  bool on_ui_thread_;
};

// --- Accelerator table (portable)
// -----------------------------------------------
//
// The accelerators of one window's app menu, for backends that match key
// presses themselves (CEF Views, WebView2's AcceleratorKeyPressed): each
// enabled item with a parsed accelerator, and the click to run.

struct MenuAccelBinding {
  Accelerator accel;
  std::string click_key;
};

// Collects the bindings of enabled items in `entries` (submenus included).
// A combination bound twice keeps its first item.
std::vector<MenuAccelBinding> CollectMenuAccelerators(
    const std::vector<MenuEntry>& entries);

// The binding in `bindings` whose accelerator equals `accel` (same key, same
// modifiers), or nullptr.
const MenuAccelBinding* FindMenuAccelerator(
    const std::vector<MenuAccelBinding>& bindings, const Accelerator& accel);

}  // namespace laufey_common

#endif  // LAUFEY_MENU_H_
