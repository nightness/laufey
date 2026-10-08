// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Global shortcuts on Windows (API 40): RegisterHotKey on a message-only
// window owned by the backend's UI thread, so WM_HOTKEY is dispatched by the
// UI thread's message loop (and by any modal loop running on it). MOD_NOREPEAT
// keeps a held key from repeating. RegisterHotKey fails with
// ERROR_HOTKEY_ALREADY_REGISTERED when another app (or another window or
// thread of this one) holds the combination: LAUFEY_SHORTCUT_CONFLICT.

#include <windows.h>

#include <map>
#include <mutex>

#include "laufey_menu.h"
#include "laufey_system.h"

namespace laufey_common {

namespace {

const wchar_t kClassName[] = L"LaufeyGlobalShortcuts";

// The VK code for a character in the current keyboard layout (so "Ctrl+Z" is
// the key labelled Z on any layout); the US OEM codes as a fallback.
UINT VkForChar(char ch) {
  if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
    return static_cast<UINT>(ch);
  SHORT scan = VkKeyScanExW(static_cast<wchar_t>(ch), GetKeyboardLayout(0));
  if (scan != -1 && LOBYTE(scan) != 0xFF)
    return LOBYTE(scan);
  switch (ch) {
    case '-': return VK_OEM_MINUS;
    case '=': return VK_OEM_PLUS;
    case '[': return VK_OEM_4;
    case ']': return VK_OEM_6;
    case '\\': return VK_OEM_5;
    case ';': return VK_OEM_1;
    case '\'': return VK_OEM_7;
    case ',': return VK_OEM_COMMA;
    case '.': return VK_OEM_PERIOD;
    case '/': return VK_OEM_2;
    case '`': return VK_OEM_3;
  }
  return 0;
}

}  // namespace

// Shared with the menu accelerators (laufey_menu.h).
unsigned AcceleratorVirtualKey(const Accelerator& a) {
  switch (a.kind) {
    case KeyKind::kLetter:
    case KeyKind::kDigit:
    case KeyKind::kPunct:
      return VkForChar(a.ch);
    case KeyKind::kFunction:
      return VK_F1 + (a.number - 1);
    case KeyKind::kNumpad:
      switch (a.named) {
        case NamedKey::kNone: return VK_NUMPAD0 + a.number;
        case NamedKey::kNumDecimal: return VK_DECIMAL;
        case NamedKey::kNumAdd: return VK_ADD;
        case NamedKey::kNumSubtract: return VK_SUBTRACT;
        case NamedKey::kNumMultiply: return VK_MULTIPLY;
        case NamedKey::kNumDivide: return VK_DIVIDE;
        default: return 0;
      }
    case KeyKind::kNamed:
      switch (a.named) {
        case NamedKey::kSpace: return VK_SPACE;
        case NamedKey::kTab: return VK_TAB;
        case NamedKey::kBackspace: return VK_BACK;
        case NamedKey::kDelete: return VK_DELETE;
        case NamedKey::kInsert: return VK_INSERT;
        case NamedKey::kEnter: return VK_RETURN;
        case NamedKey::kEscape: return VK_ESCAPE;
        case NamedKey::kUp: return VK_UP;
        case NamedKey::kDown: return VK_DOWN;
        case NamedKey::kLeft: return VK_LEFT;
        case NamedKey::kRight: return VK_RIGHT;
        case NamedKey::kHome: return VK_HOME;
        case NamedKey::kEnd: return VK_END;
        case NamedKey::kPageUp: return VK_PRIOR;
        case NamedKey::kPageDown: return VK_NEXT;
        case NamedKey::kPrintScreen: return VK_SNAPSHOT;
        case NamedKey::kMediaPlayPause: return VK_MEDIA_PLAY_PAUSE;
        case NamedKey::kMediaNextTrack: return VK_MEDIA_NEXT_TRACK;
        case NamedKey::kMediaPreviousTrack: return VK_MEDIA_PREV_TRACK;
        case NamedKey::kMediaStop: return VK_MEDIA_STOP;
        case NamedKey::kVolumeUp: return VK_VOLUME_UP;
        case NamedKey::kVolumeDown: return VK_VOLUME_DOWN;
        case NamedKey::kVolumeMute: return VK_VOLUME_MUTE;
        default: return 0;
      }
  }
  return 0;
}

namespace {

UINT HotKeyModifiers(uint32_t mods) {
  UINT m = MOD_NOREPEAT;
  if (mods & kModCtrl)
    m |= MOD_CONTROL;
  if (mods & kModAlt)
    m |= MOD_ALT;
  if (mods & kModShift)
    m |= MOD_SHIFT;
  if (mods & kModSuper)
    m |= MOD_WIN;
  return m;
}

LRESULT CALLBACK HotKeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_HOTKEY) {
    DispatchShortcut(static_cast<uint32_t>(wp));
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

class WinShortcuts : public ShortcutPlatform {
 public:
  explicit WinShortcuts(std::function<void(std::function<void()>)> run_on_ui)
      : run_on_ui_(std::move(run_on_ui)) {}

  uint32_t Capabilities() override {
    return run_on_ui_ ? LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS : 0;
  }

  void Bind(uint32_t sid, const Accelerator& accel,
            std::function<void(int)> done) override {
    UINT vk = AcceleratorVirtualKey(accel);
    UINT mods = HotKeyModifiers(accel.mods);
    run_on_ui_([this, sid, vk, mods, done = std::move(done)] {
      if (!vk) {
        done(LAUFEY_SHORTCUT_INVALID);
        return;
      }
      HWND hwnd = EnsureWindow();
      if (!hwnd) {
        done(LAUFEY_SHORTCUT_FAILED);
        return;
      }
      // Ids are small (the registry reuses them), well under 0xC000.
      if (RegisterHotKey(hwnd, static_cast<int>(sid), mods, vk)) {
        done(LAUFEY_SHORTCUT_OK);
        return;
      }
      DWORD err = GetLastError();
      done(err == ERROR_HOTKEY_ALREADY_REGISTERED ? LAUFEY_SHORTCUT_CONFLICT
                                                  : LAUFEY_SHORTCUT_FAILED);
    });
  }

  void Unbind(uint32_t sid) override {
    run_on_ui_([this, sid] {
      if (hwnd_)
        UnregisterHotKey(hwnd_, static_cast<int>(sid));
    });
  }

 private:
  // UI thread.
  HWND EnsureWindow() {
    if (hwnd_)
      return hwnd_;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HotKeyWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly if already registered
    hwnd_ = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, inst, nullptr);
    return hwnd_;
  }

  std::function<void(std::function<void()>)> run_on_ui_;
  HWND hwnd_ = nullptr;  // UI thread
};

}  // namespace

std::unique_ptr<ShortcutPlatform> CreateShortcutPlatformWin(
    std::function<void(std::function<void()>)> run_on_ui) {
  return std::make_unique<WinShortcuts>(std::move(run_on_ui));
}

}  // namespace laufey_common
