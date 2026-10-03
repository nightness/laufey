// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Global shortcuts on macOS (API 40): Carbon RegisterEventHotKey, the API
// Chromium and Electron use. It needs no Accessibility permission. Hot keys
// are registered with kEventHotKeyExclusive, so a combination another app
// (or another part of this process) holds comes back as eventHotKeyExistsErr
// -> LAUFEY_SHORTCUT_CONFLICT. Registration and the event handler run on the
// main thread; presses arrive there as kEventHotKeyPressed.

#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

#include <map>

#include "laufey_system.h"

namespace laufey_common {

namespace {

// 'LFEY'
constexpr OSType kSignature = 0x4C464559;

// Keypad key codes, skipped when looking a character up in the layout (the
// main keys of the same characters come first anyway).
bool IsKeypadKeyCode(UInt16 kc) {
  return kc == kVK_ANSI_KeypadDecimal || kc == kVK_ANSI_KeypadMultiply ||
         kc == kVK_ANSI_KeypadPlus || kc == kVK_ANSI_KeypadClear ||
         kc == kVK_ANSI_KeypadDivide || kc == kVK_ANSI_KeypadEnter ||
         kc == kVK_ANSI_KeypadMinus || kc == kVK_ANSI_KeypadEquals ||
         (kc >= kVK_ANSI_Keypad0 && kc <= kVK_ANSI_Keypad9);
}

// The ANSI (US) position of a character, the fallback when the current
// layout has no Unicode layout data or doesn't produce the character.
int AnsiKeyCode(char ch) {
  switch (ch) {
    case 'A': return kVK_ANSI_A;
    case 'B': return kVK_ANSI_B;
    case 'C': return kVK_ANSI_C;
    case 'D': return kVK_ANSI_D;
    case 'E': return kVK_ANSI_E;
    case 'F': return kVK_ANSI_F;
    case 'G': return kVK_ANSI_G;
    case 'H': return kVK_ANSI_H;
    case 'I': return kVK_ANSI_I;
    case 'J': return kVK_ANSI_J;
    case 'K': return kVK_ANSI_K;
    case 'L': return kVK_ANSI_L;
    case 'M': return kVK_ANSI_M;
    case 'N': return kVK_ANSI_N;
    case 'O': return kVK_ANSI_O;
    case 'P': return kVK_ANSI_P;
    case 'Q': return kVK_ANSI_Q;
    case 'R': return kVK_ANSI_R;
    case 'S': return kVK_ANSI_S;
    case 'T': return kVK_ANSI_T;
    case 'U': return kVK_ANSI_U;
    case 'V': return kVK_ANSI_V;
    case 'W': return kVK_ANSI_W;
    case 'X': return kVK_ANSI_X;
    case 'Y': return kVK_ANSI_Y;
    case 'Z': return kVK_ANSI_Z;
    case '0': return kVK_ANSI_0;
    case '1': return kVK_ANSI_1;
    case '2': return kVK_ANSI_2;
    case '3': return kVK_ANSI_3;
    case '4': return kVK_ANSI_4;
    case '5': return kVK_ANSI_5;
    case '6': return kVK_ANSI_6;
    case '7': return kVK_ANSI_7;
    case '8': return kVK_ANSI_8;
    case '9': return kVK_ANSI_9;
    case '-': return kVK_ANSI_Minus;
    case '=': return kVK_ANSI_Equal;
    case '[': return kVK_ANSI_LeftBracket;
    case ']': return kVK_ANSI_RightBracket;
    case '\\': return kVK_ANSI_Backslash;
    case ';': return kVK_ANSI_Semicolon;
    case '\'': return kVK_ANSI_Quote;
    case ',': return kVK_ANSI_Comma;
    case '.': return kVK_ANSI_Period;
    case '/': return kVK_ANSI_Slash;
    case '`': return kVK_ANSI_Grave;
  }
  return -1;
}

// The key that types `ch` (unmodified) in the current keyboard layout, so
// "Cmd+Z" is the key labelled Z on an AZERTY keyboard too. Main thread.
int LayoutKeyCode(char ch) {
  UniChar want = static_cast<UniChar>(tolower(static_cast<unsigned char>(ch)));
  int found = -1;
  TISInputSourceRef sources[2] = {TISCopyCurrentKeyboardLayoutInputSource(),
                                  nullptr};
  for (int s = 0; s < 2 && found < 0; s++) {
    if (s == 1)
      sources[1] = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    TISInputSourceRef src = sources[s];
    if (!src)
      continue;
    CFDataRef data = static_cast<CFDataRef>(
        TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData));
    if (!data)
      continue;
    const UCKeyboardLayout* layout =
        reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
    for (UInt16 kc = 0; kc < 128 && found < 0; kc++) {
      if (IsKeypadKeyCode(kc))
        continue;
      UInt32 dead = 0;
      UniChar chars[4];
      UniCharCount len = 0;
      OSStatus st =
          UCKeyTranslate(layout, kc, kUCKeyActionDown, 0, LMGetKbdType(),
                         kUCKeyTranslateNoDeadKeysMask, &dead, 4, &len, chars);
      if (st == noErr && len == 1 && chars[0] == want)
        found = kc;
    }
  }
  for (TISInputSourceRef src : sources) {
    if (src)
      CFRelease(src);
  }
  return found >= 0 ? found : AnsiKeyCode(ch);
}

int FunctionKeyCode(int n) {
  static const int kCodes[] = {
      kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
      kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
      kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
  return n >= 1 && n <= 20 ? kCodes[n - 1] : -1;
}

// The virtual key code for `a`, or -1 when macOS has no such key a hot key
// can use (F21-F24; the media keys, which arrive as system-defined events
// only a CGEventTap with Accessibility permission sees). Main thread.
int KeyCodeFor(const Accelerator& a) {
  switch (a.kind) {
    case KeyKind::kLetter:
    case KeyKind::kDigit:
    case KeyKind::kPunct:
      return LayoutKeyCode(a.ch);
    case KeyKind::kFunction:
      return FunctionKeyCode(a.number);
    case KeyKind::kNumpad:
      switch (a.named) {
        case NamedKey::kNone: {
          static const int kDigits[] = {
              kVK_ANSI_Keypad0, kVK_ANSI_Keypad1, kVK_ANSI_Keypad2,
              kVK_ANSI_Keypad3, kVK_ANSI_Keypad4, kVK_ANSI_Keypad5,
              kVK_ANSI_Keypad6, kVK_ANSI_Keypad7, kVK_ANSI_Keypad8,
              kVK_ANSI_Keypad9};
          return kDigits[a.number];
        }
        case NamedKey::kNumDecimal: return kVK_ANSI_KeypadDecimal;
        case NamedKey::kNumAdd: return kVK_ANSI_KeypadPlus;
        case NamedKey::kNumSubtract: return kVK_ANSI_KeypadMinus;
        case NamedKey::kNumMultiply: return kVK_ANSI_KeypadMultiply;
        case NamedKey::kNumDivide: return kVK_ANSI_KeypadDivide;
        default: return -1;
      }
    case KeyKind::kNamed:
      switch (a.named) {
        case NamedKey::kSpace: return kVK_Space;
        case NamedKey::kTab: return kVK_Tab;
        case NamedKey::kBackspace: return kVK_Delete;
        case NamedKey::kDelete: return kVK_ForwardDelete;
        case NamedKey::kInsert: return kVK_Help;
        case NamedKey::kEnter: return kVK_Return;
        case NamedKey::kEscape: return kVK_Escape;
        case NamedKey::kUp: return kVK_UpArrow;
        case NamedKey::kDown: return kVK_DownArrow;
        case NamedKey::kLeft: return kVK_LeftArrow;
        case NamedKey::kRight: return kVK_RightArrow;
        case NamedKey::kHome: return kVK_Home;
        case NamedKey::kEnd: return kVK_End;
        case NamedKey::kPageUp: return kVK_PageUp;
        case NamedKey::kPageDown: return kVK_PageDown;
        case NamedKey::kPrintScreen: return kVK_F13;
        case NamedKey::kVolumeUp: return kVK_VolumeUp;
        case NamedKey::kVolumeDown: return kVK_VolumeDown;
        case NamedKey::kVolumeMute: return kVK_Mute;
        default: return -1;
      }
  }
  return -1;
}

UInt32 CarbonModifiers(uint32_t mods) {
  UInt32 m = 0;
  if (mods & kModCtrl)
    m |= controlKey;
  if (mods & kModAlt)
    m |= optionKey;
  if (mods & kModShift)
    m |= shiftKey;
  if (mods & kModSuper)
    m |= cmdKey;
  return m;
}

OSStatus HotKeyHandler(EventHandlerCallRef /*next*/, EventRef event,
                       void* /*data*/) {
  EventHotKeyID hk = {};
  if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                        nullptr, sizeof(hk), nullptr, &hk) != noErr ||
      hk.signature != kSignature) {
    return eventNotHandledErr;
  }
  DispatchShortcut(hk.id);
  return noErr;
}

void OnMain(void (^block)(void)) {
  if ([NSThread isMainThread])
    block();
  else
    dispatch_async(dispatch_get_main_queue(), block);
}

class MacShortcuts : public ShortcutPlatform {
 public:
  uint32_t Capabilities() override {
    return LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS;
  }

  void Bind(uint32_t sid, const Accelerator& accel,
            std::function<void(int)> done) override {
    Accelerator a = accel;
    __block std::function<void(int)> finish = std::move(done);
    OnMain(^{
      EnsureHandler();
      int code = KeyCodeFor(a);
      if (code < 0) {
        finish(a.kind == KeyKind::kNamed ? LAUFEY_SHORTCUT_NOT_SUPPORTED
                                         : LAUFEY_SHORTCUT_INVALID);
        return;
      }
      EventHotKeyID hk = {kSignature, sid};
      EventHotKeyRef ref = nullptr;
      OSStatus st = RegisterEventHotKey(
          static_cast<UInt32>(code), CarbonModifiers(a.mods), hk,
          GetApplicationEventTarget(), kEventHotKeyExclusive, &ref);
      if (st == noErr && ref) {
        refs_[sid] = ref;
        finish(LAUFEY_SHORTCUT_OK);
      } else if (st == eventHotKeyExistsErr) {
        finish(LAUFEY_SHORTCUT_CONFLICT);
      } else if (st == eventHotKeyInvalidErr) {
        finish(LAUFEY_SHORTCUT_INVALID);
      } else {
        finish(LAUFEY_SHORTCUT_FAILED);
      }
    });
  }

  void Unbind(uint32_t sid) override {
    OnMain(^{
      auto it = refs_.find(sid);
      if (it == refs_.end())
        return;
      UnregisterEventHotKey(it->second);
      refs_.erase(it);
    });
  }

 private:
  // Main thread.
  void EnsureHandler() {
    if (handler_)
      return;
    EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
    InstallApplicationEventHandler(NewEventHandlerUPP(HotKeyHandler), 1, &spec,
                                   nullptr, &handler_);
  }

  EventHandlerRef handler_ = nullptr;   // main thread
  std::map<uint32_t, EventHotKeyRef> refs_;  // main thread
};

}  // namespace

std::unique_ptr<ShortcutPlatform> CreateShortcutPlatformMac() {
  return std::make_unique<MacShortcuts>();
}

}  // namespace laufey_common
