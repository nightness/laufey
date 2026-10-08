// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// Win32 application and context menus built from laufey menu templates.
//
// API 41: labels are UTF-16 (AppendMenuW), each app menu gets an accelerator
// table (CreateAcceleratorTableW) that the message loop runs through
// TranslateAccelerator and WebView2's AcceleratorKeyPressed matches while the
// page has the focus, item clicks are registered for test_click_menu_item,
// and a context menu reports its closing (laufey_menu.h session).

#ifndef LAUFEY_WIN32_MENU_H_
#define LAUFEY_WIN32_MENU_H_

#include <windows.h>
#include <wincodec.h>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <laufey.h>
#include <laufey_backend_common.h>
#include <laufey_menu.h>

namespace win32_menu {

// Per-window menu state — maps command IDs to item string IDs.
struct MenuState {
  std::map<UINT, std::string> command_to_id;
  // GDI bitmaps for item icons (hbmpItem). Owned here so they outlive the menu
  // and are freed when the menu is rebuilt/destroyed.
  std::vector<HBITMAP> bitmaps;
  // The app menu's accelerators (enabled items with a parsed accelerator and
  // a key Windows accelerator tables can express: no Win-key combinations).
  std::vector<ACCEL> accels;
  HACCEL haccel = nullptr;
  laufey_menu_click_fn on_click = nullptr;
  void* on_click_data = nullptr;
  uint32_t window_id = 0;
  UINT next_command_id = 0x8000;  // Start above standard IDs

  UINT AllocCommandId(const std::string& item_id) {
    UINT id = next_command_id++;
    command_to_id[id] = item_id;
    return id;
  }

  void HandleCommand(UINT cmd) {
    if (on_click) {
      auto it = command_to_id.find(cmd);
      if (it != command_to_id.end()) {
        on_click(on_click_data, window_id, it->second.c_str());
      }
    }
  }
};

// The app menus, by top-level window. The HMENUs are UI-thread objects; the
// mutex guards the map for the accelerator lookups.
inline std::map<HWND, MenuState>& GetMenuStates() {
  static std::map<HWND, MenuState> states;
  return states;
}
inline std::recursive_mutex& MenuStatesMutex() {
  static std::recursive_mutex m;
  return m;
}

// ACCEL fVirt flags for a parsed accelerator, or 0 when a table can't hold it
// (accelerator tables have no Windows-key modifier).
inline BYTE AccelFlags(const laufey_common::Accelerator& a) {
  if (a.mods & laufey_common::kModSuper)
    return 0;
  BYTE f = FVIRTKEY;
  if (a.mods & laufey_common::kModCtrl)
    f |= FCONTROL;
  if (a.mods & laufey_common::kModAlt)
    f |= FALT;
  if (a.mods & laufey_common::kModShift)
    f |= FSHIFT;
  return f;
}

// Create a role-based menu item (standard operations). Roles are forwarded
// to on_click as their name.
inline bool CreateRoleMenuItem(HMENU menu, const laufey_common::MenuEntry& e,
                               MenuState& state) {
  struct RoleEntry {
    const char* role;
    const wchar_t* label;
  };
  static const RoleEntry roles[] = {
      {"quit", L"E&xit"},
      {"copy", L"&Copy"},
      {"paste", L"&Paste"},
      {"cut", L"Cu&t"},
      {"selectall", L"&Select All"},
      {"undo", L"&Undo"},
      {"redo", L"&Redo"},
      {"minimize", L"Mi&nimize"},
      {"close", L"&Close"},
      {"about", L"&About"},
  };

  for (const auto& entry : roles) {
    if (laufey_common::RoleIs(e.role, entry.role)) {
      UINT id = state.AllocCommandId(e.role);
      std::wstring label = e.label.empty() ? std::wstring(entry.label)
                                           : laufey_common::Utf8ToWide(e.label);
      AppendMenuW(menu, MF_STRING, id, label.c_str());
      return true;
    }
  }
  return false;
}

// Decode PNG bytes into a 32bpp premultiplied-alpha top-down DIB section
// suitable for a menu item bitmap (hbmpItem). Scaled to the small-icon metric
// so it lines up with the menu gutter. Returns nullptr on any failure; the
// caller owns the returned HBITMAP and must DeleteObject it. Unlike macOS,
// the image is rendered as-is — there is no template tinting on selection.
inline HBITMAP LoadMenuIconBitmap(const void* bytes, size_t len) {
  if (!bytes || len == 0)
    return nullptr;

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }

  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  if (SUCCEEDED(factory->CreateStream(&stream)) && stream &&
      SUCCEEDED(stream->InitializeFromMemory((BYTE*)const_cast<void*>(bytes),
                                             (DWORD)len))) {
    factory->CreateDecoderFromStream(stream, nullptr,
                                     WICDecodeMetadataCacheOnLoad, &decoder);
  }
  IWICBitmapFrameDecode* frame = nullptr;
  if (decoder)
    decoder->GetFrame(0, &frame);
  IWICFormatConverter* conv = nullptr;
  factory->CreateFormatConverter(&conv);
  if (frame && conv) {
    conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                     WICBitmapDitherTypeNone, nullptr, 0.0,
                     WICBitmapPaletteTypeCustom);
  }

  int metric = GetSystemMetrics(SM_CXSMICON);
  UINT side = metric > 0 ? static_cast<UINT>(metric) : 16;
  IWICBitmapScaler* scaler = nullptr;
  factory->CreateBitmapScaler(&scaler);
  if (conv && scaler) {
    scaler->Initialize(conv, side, side,
                       WICBitmapInterpolationModeHighQualityCubic);
  }

  HBITMAP hbmp = nullptr;
  if (scaler) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(side);
    bi.bmiHeader.biHeight = -static_cast<LONG>(side);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    UINT stride = side * 4;
    if (hbmp && bits &&
        FAILED(scaler->CopyPixels(nullptr, stride, stride * side,
                                  static_cast<BYTE*>(bits)))) {
      DeleteObject(hbmp);
      hbmp = nullptr;
    } else if (hbmp && !bits) {
      DeleteObject(hbmp);
      hbmp = nullptr;
    }
  }

  if (scaler)
    scaler->Release();
  if (conv)
    conv->Release();
  if (frame)
    frame->Release();
  if (decoder)
    decoder->Release();
  if (stream)
    stream->Release();
  factory->Release();
  return hbmp;
}

// Recursively build an HMENU (a menu bar when `popup` is false) from parsed
// entries. `collect_accels` binds the items' accelerators into
// state.accels (the app menu); a context menu only shows them.
inline HMENU BuildMenuFromEntries(
    const std::vector<laufey_common::MenuEntry>& entries, MenuState& state,
    bool popup, bool collect_accels) {
  using laufey_common::MenuEntry;
  HMENU menu = popup ? CreatePopupMenu() : CreateMenu();

  for (const MenuEntry& e : entries) {
    switch (e.kind) {
      case MenuEntry::Kind::kSeparator:
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        continue;
      case MenuEntry::Kind::kRole:
        CreateRoleMenuItem(menu, e, state);
        continue;
      case MenuEntry::Kind::kSubmenu: {
        HMENU submenu =
            BuildMenuFromEntries(e.children, state, true, collect_accels);
        std::wstring label = laufey_common::Utf8ToWide(e.label);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu),
                    label.c_str());
        continue;
      }
      case MenuEntry::Kind::kItem:
        break;
    }

    std::string label = e.label;
    if (e.has_accel)
      label += "\t" + laufey_common::AcceleratorDisplayText(e.accel);

    const std::string& key = e.ClickKey();
    UINT cmdId = state.AllocCommandId(key);
    laufey_common::RegisterMenuClick(key, state.on_click, state.on_click_data,
                                     state.window_id);

    UINT flags = MF_STRING;
    if (!e.enabled)
      flags |= MF_GRAYED;
    if (e.checked)
      flags |= MF_CHECKED;

    std::wstring wlabel = laufey_common::Utf8ToWide(label);
    AppendMenuW(menu, flags, cmdId, wlabel.c_str());

    if (collect_accels && e.enabled && e.has_accel) {
      BYTE fvirt = AccelFlags(e.accel);
      WORD vk =
          static_cast<WORD>(laufey_common::AcceleratorVirtualKey(e.accel));
      bool taken = false;
      for (const ACCEL& a : state.accels)
        taken |= (a.fVirt == fvirt && a.key == vk);
      if (fvirt && vk && !taken) {
        ACCEL a = {};
        a.fVirt = fvirt;
        a.key = vk;
        a.cmd = static_cast<WORD>(cmdId);
        state.accels.push_back(a);
      }
    }

    if (!e.icon_png.empty()) {
      HBITMAP hbmp = LoadMenuIconBitmap(e.icon_png.data(), e.icon_png.size());
      if (hbmp) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = hbmp;
        SetMenuItemInfoW(menu, cmdId, FALSE, &mii);
        state.bitmaps.push_back(hbmp);
      }
    }
  }

  return menu;
}

// Set the application menu on a given HWND. Call this from the UI thread.
// `entries` come from laufey_common::ParseMenuTemplate.
inline void SetApplicationMenu(
    HWND hwnd, const std::vector<laufey_common::MenuEntry>& entries,
    laufey_menu_click_fn on_click, void* on_click_data,
    uint32_t window_id = 0) {
  if (!hwnd)
    return;

  std::lock_guard<std::recursive_mutex> lock(MenuStatesMutex());
  MenuState& state = GetMenuStates()[hwnd];
  state.command_to_id.clear();
  // The previously built menu still references its bitmaps until DestroyMenu
  // below, so hold them aside and free them only after the old menu is gone.
  std::vector<HBITMAP> oldBitmaps = std::move(state.bitmaps);
  state.bitmaps.clear();
  HACCEL oldAccel = state.haccel;
  state.haccel = nullptr;
  state.accels.clear();
  state.next_command_id = 0x8000;
  state.on_click = on_click;
  state.on_click_data = on_click_data;
  state.window_id = window_id;

  // Destroy the old menu to avoid HMENU leak
  HMENU oldMenu = GetMenu(hwnd);

  HMENU menubar = BuildMenuFromEntries(entries, state, false, true);
  if (menubar) {
    SetMenu(hwnd, menubar);
    DrawMenuBar(hwnd);
  }
  if (!state.accels.empty()) {
    state.haccel = CreateAcceleratorTableW(
        state.accels.data(), static_cast<int>(state.accels.size()));
  }

  if (oldMenu) {
    DestroyMenu(oldMenu);
  }
  if (oldAccel)
    DestroyAcceleratorTable(oldAccel);
  for (HBITMAP bmp : oldBitmaps) {
    if (bmp)
      DeleteObject(bmp);
  }
}

// Forget a window's menu state (the window is being destroyed).
inline void ForgetWindow(HWND hwnd) {
  std::lock_guard<std::recursive_mutex> lock(MenuStatesMutex());
  auto it = GetMenuStates().find(hwnd);
  if (it == GetMenuStates().end())
    return;
  if (it->second.haccel)
    DestroyAcceleratorTable(it->second.haccel);
  for (HBITMAP bmp : it->second.bitmaps) {
    if (bmp)
      DeleteObject(bmp);
  }
  GetMenuStates().erase(it);
}

// Call this from WndProc on WM_COMMAND to dispatch menu clicks.
inline bool HandleMenuCommand(HWND hwnd, WPARAM wParam) {
  UINT cmd = LOWORD(wParam);
  laufey_menu_click_fn fn = nullptr;
  void* data = nullptr;
  uint32_t window_id = 0;
  std::string id;
  {
    std::lock_guard<std::recursive_mutex> lock(MenuStatesMutex());
    auto& states = GetMenuStates();
    auto it = states.find(hwnd);
    if (it == states.end())
      return false;
    auto cmd_it = it->second.command_to_id.find(cmd);
    if (cmd_it == it->second.command_to_id.end())
      return false;
    fn = it->second.on_click;
    data = it->second.on_click_data;
    window_id = it->second.window_id;
    id = cmd_it->second;
  }
  // Outside the lock: the handler may set a new menu.
  if (fn)
    fn(data, window_id, id.c_str());
  return true;
}

// The message loop's accelerator step: runs `msg` through the accelerator
// table of the top-level window it is addressed to (or a child of). Returns
// true when it was translated (a WM_COMMAND was sent; skip
// TranslateMessage / DispatchMessage). UI thread.
inline bool TranslateWindowAccelerator(MSG* msg) {
  if (!msg || !msg->hwnd ||
      (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN))
    return false;
  HWND root = GetAncestor(msg->hwnd, GA_ROOT);
  HACCEL haccel = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(MenuStatesMutex());
    auto it = GetMenuStates().find(root);
    if (it != GetMenuStates().end())
      haccel = it->second.haccel;
  }
  return haccel && TranslateAcceleratorW(root, haccel, msg) != 0;
}

// The current modifier keys as ACCEL fVirt bits (with FVIRTKEY).
inline BYTE CurrentAccelModifiers() {
  BYTE f = FVIRTKEY;
  if (GetKeyState(VK_CONTROL) & 0x8000)
    f |= FCONTROL;
  if (GetKeyState(VK_MENU) & 0x8000)
    f |= FALT;
  if (GetKeyState(VK_SHIFT) & 0x8000)
    f |= FSHIFT;
  return f;
}

// For a key pressed while the focus is in a web view whose keys don't pass
// through the host's message loop (WebView2's AcceleratorKeyPressed): if
// `vk` with the current modifiers is one of `root`'s menu accelerators, post
// the WM_COMMAND TranslateAccelerator would send and return true (mark the
// key handled). UI thread.
inline bool HandleAcceleratorKey(HWND root, UINT vk) {
  BYTE mods = CurrentAccelModifiers();
  UINT cmd = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(MenuStatesMutex());
    auto it = GetMenuStates().find(root);
    if (it == GetMenuStates().end())
      return false;
    for (const ACCEL& a : it->second.accels) {
      if (a.key == vk && a.fVirt == mods) {
        cmd = a.cmd;
        break;
      }
    }
  }
  if (!cmd)
    return false;
  PostMessageW(root, WM_COMMAND, MAKEWPARAM(cmd, 1), 0);
  return true;
}

// Test hook: presses `accelerator` on `hwnd` through TranslateAccelerator,
// with this thread's keyboard state holding exactly its modifiers for the
// call (TranslateAccelerator reads them with GetKeyState). UI thread. Returns
// true if a menu command was sent.
inline bool TestTriggerAccelerator(HWND hwnd, const char* accelerator) {
  if (!hwnd || !accelerator)
    return false;
  laufey_common::Accelerator accel;
  if (!laufey_common::ParseAccelerator(accelerator, false, &accel, nullptr))
    return false;
  UINT vk = laufey_common::AcceleratorVirtualKey(accel);
  HACCEL haccel = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(MenuStatesMutex());
    auto it = GetMenuStates().find(hwnd);
    if (it != GetMenuStates().end())
      haccel = it->second.haccel;
  }
  if (!vk || !haccel)
    return false;
  BYTE saved[256];
  if (!GetKeyboardState(saved))
    return false;
  BYTE keys[256];
  memcpy(keys, saved, sizeof(keys));
  auto set = [&](int key, bool down) { keys[key] = down ? 0x80 : 0; };
  bool ctrl = (accel.mods & laufey_common::kModCtrl) != 0;
  bool alt = (accel.mods & laufey_common::kModAlt) != 0;
  bool shift = (accel.mods & laufey_common::kModShift) != 0;
  set(VK_CONTROL, ctrl);
  set(VK_LCONTROL, ctrl);
  set(VK_RCONTROL, false);
  set(VK_MENU, alt);
  set(VK_LMENU, alt);
  set(VK_RMENU, false);
  set(VK_SHIFT, shift);
  set(VK_LSHIFT, shift);
  set(VK_RSHIFT, false);
  SetKeyboardState(keys);
  MSG msg = {};
  msg.hwnd = hwnd;
  msg.message = alt ? WM_SYSKEYDOWN : WM_KEYDOWN;
  msg.wParam = vk;
  msg.lParam = 1;
  bool translated = TranslateAcceleratorW(hwnd, haccel, &msg) != 0;
  SetKeyboardState(saved);
  return translated;
}

// --- Context menus ---

constexpr UINT kEndMenuMessage = WM_APP + 0x51;

// A context menu that is waiting for the thread's active menu to go (see
// ShowContextMenu). Owned by the deferral timer while it waits.
struct PendingContextMenu {
  HWND hwnd = nullptr;
  int x = 0;
  int y = 0;
  std::vector<laufey_common::MenuEntry> entries;
  laufey_menu_click_fn on_click = nullptr;
  void* on_click_data = nullptr;
  uint32_t window_id = 0;
  laufey_menu_closed_fn on_closed = nullptr;
  void* on_closed_data = nullptr;
  int deferrals = 0;
};

inline void TrackContextMenu(PendingContextMenu* menu);

// A message-only window on the UI thread that ends the active menu: the
// context menu's modal loop dispatches the thread's messages, so a message
// posted here from any thread reaches EndMenu on the menu's own thread. Its
// timers (id = a PendingContextMenu*) show a deferred context menu.
// UI thread (it is created on the first call).
inline HWND EndMenuWindow() {
  static HWND hwnd = nullptr;
  if (hwnd)
    return hwnd;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == kEndMenuMessage) {
      EndMenu();
      return 0;
    }
    if (m == WM_TIMER) {
      KillTimer(h, w);
      TrackContextMenu(reinterpret_cast<PendingContextMenu*>(w));
      return 0;
    }
    return DefWindowProcW(h, m, w, l);
  };
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LaufeyEndMenuWindow";
  RegisterClassExW(&wc);
  hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                         nullptr, wc.hInstance, nullptr);
  return hwnd;
}

// Whether a menu is active on this thread: a popup menu, or a window's menu
// bar / system menu in menu mode (which a lone Alt press and release
// starts).
inline bool ThreadInMenuMode() {
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  return GetGUIThreadInfo(GetCurrentThreadId(), &info) &&
         (info.flags &
          (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0;
}

// Shows `menu` and blocks until it closes (UI thread); takes ownership.
inline void TrackContextMenu(PendingContextMenu* raw) {
  std::unique_ptr<PendingContextMenu> menu(raw);
  HWND hwnd = menu->hwnd;
  if (!hwnd || !IsWindow(hwnd) || menu->entries.empty()) {
    laufey_common::FireContextMenuClosedNow(menu->window_id, menu->on_closed,
                                            menu->on_closed_data);
    return;
  }
  // Only one menu can be active on a thread: while another one is (the
  // window's menu bar or system menu in menu mode, another context menu),
  // TrackPopupMenu fails at once and nothing shows. End that one and show
  // this menu once its loop has unwound, which the timer's message
  // (dispatched by the outer loop's caller, not inside it) waits for. A
  // menu that still can't show after ~1 s is shown anyway, so it reports
  // its close either way.
  constexpr int kMaxDeferrals = 20;
  if (menu->deferrals < kMaxDeferrals && ThreadInMenuMode()) {
    EndMenu();
    menu->deferrals++;
    HWND timer_window = EndMenuWindow();
    if (timer_window &&
        SetTimer(timer_window, reinterpret_cast<UINT_PTR>(menu.get()), 50,
                 nullptr)) {
      menu.release();
      return;
    }
  }

  MenuState state;
  state.on_click = menu->on_click;
  state.on_click_data = menu->on_click_data;
  state.window_id = menu->window_id;

  HMENU popup = BuildMenuFromEntries(menu->entries, state, true, false);
  if (!popup) {
    laufey_common::FireContextMenuClosedNow(menu->window_id, menu->on_closed,
                                            menu->on_closed_data);
    return;
  }

  // Convert client coordinates to screen coordinates
  POINT pt = {menu->x, menu->y};
  ClientToScreen(hwnd, &pt);

  HWND end_window = EndMenuWindow();
  uint64_t session = laufey_common::BeginContextMenu(
      menu->window_id, menu->on_closed, menu->on_closed_data, [end_window] {
        if (end_window)
          PostMessageW(end_window, kEndMenuMessage, 0, 0);
      });

  // The menu gets keyboard input, and closes on a click elsewhere, only
  // while its owner is the foreground window (KB135788). A no-op when it
  // already is; refused by the OS when this process may not take the
  // foreground.
  SetForegroundWindow(GetAncestor(hwnd, GA_ROOT));
  UINT cmd;
  {
    // Lets a backend keep running its own tasks inside the modal loop (CEF,
    // see laufey_menu.h).
    laufey_common::ScopedNativeModalLoop modal_loop;
    // TrackPopupMenu blocks until the user selects an item or dismisses.
    // TPM_RETURNCMD makes it return the selected command ID directly.
    cmd = static_cast<UINT>(TrackPopupMenu(
        popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr));
  }
  // The documented companion of a context menu (KB135788): lets the next
  // click outside close the next menu.
  PostMessageW(hwnd, WM_NULL, 0, 0);

  if (cmd != 0) {
    state.HandleCommand(cmd);
  }

  DestroyMenu(popup);
  for (HBITMAP bmp : state.bitmaps) {
    if (bmp)
      DeleteObject(bmp);
  }
  laufey_common::EndContextMenu(session);
}

// Show a context menu at the given position (client coordinates) and block
// until it closes (UI thread). `on_closed` fires once it closed, after the
// chosen item's click. When another menu is active on the thread, it is
// ended first and this one shows (from a timer) once it has gone.
inline void ShowContextMenu(
    HWND hwnd, int x, int y,
    const std::vector<laufey_common::MenuEntry>& entries,
    laufey_menu_click_fn on_click, void* on_click_data, uint32_t window_id,
    laufey_menu_closed_fn on_closed, void* on_closed_data) {
  if (!hwnd || entries.empty()) {
    laufey_common::FireContextMenuClosedNow(window_id, on_closed,
                                            on_closed_data);
    return;
  }
  auto* menu = new PendingContextMenu();
  menu->hwnd = hwnd;
  menu->x = x;
  menu->y = y;
  menu->entries = entries;
  menu->on_click = on_click;
  menu->on_click_data = on_click_data;
  menu->window_id = window_id;
  menu->on_closed = on_closed;
  menu->on_closed_data = on_closed_data;
  TrackContextMenu(menu);
}

}  // namespace win32_menu

#endif  // LAUFEY_WIN32_MENU_H_
