// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable half of global shortcuts (API 40): accelerator parsing, the
// canonical form, and the registry every CEF and WebView backend shares. See
// laufey_system.h and docs/global-shortcuts.md.

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "laufey_system.h"

namespace laufey_common {

namespace {

std::string Lower(const std::string& s) {
  std::string out = s;
  for (auto& c : out)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return out;
}

struct NamedEntry {
  const char* canonical;
  const char* aliases[4];  // lower-case; null-terminated
  KeyKind kind;
  NamedKey key;
};

const NamedEntry kNamedKeys[] = {
    {"Space", {"space", nullptr}, KeyKind::kNamed, NamedKey::kSpace},
    {"Tab", {"tab", nullptr}, KeyKind::kNamed, NamedKey::kTab},
    {"Backspace", {"backspace", nullptr}, KeyKind::kNamed,
     NamedKey::kBackspace},
    {"Delete", {"delete", "del", nullptr}, KeyKind::kNamed, NamedKey::kDelete},
    {"Insert", {"insert", "ins", nullptr}, KeyKind::kNamed, NamedKey::kInsert},
    {"Enter", {"enter", "return", nullptr}, KeyKind::kNamed, NamedKey::kEnter},
    {"Escape", {"escape", "esc", nullptr}, KeyKind::kNamed, NamedKey::kEscape},
    {"Up", {"up", "arrowup", nullptr}, KeyKind::kNamed, NamedKey::kUp},
    {"Down", {"down", "arrowdown", nullptr}, KeyKind::kNamed, NamedKey::kDown},
    {"Left", {"left", "arrowleft", nullptr}, KeyKind::kNamed, NamedKey::kLeft},
    {"Right", {"right", "arrowright", nullptr}, KeyKind::kNamed,
     NamedKey::kRight},
    {"Home", {"home", nullptr}, KeyKind::kNamed, NamedKey::kHome},
    {"End", {"end", nullptr}, KeyKind::kNamed, NamedKey::kEnd},
    {"PageUp", {"pageup", nullptr}, KeyKind::kNamed, NamedKey::kPageUp},
    {"PageDown", {"pagedown", nullptr}, KeyKind::kNamed, NamedKey::kPageDown},
    {"PrintScreen", {"printscreen", nullptr}, KeyKind::kNamed,
     NamedKey::kPrintScreen},
    {"MediaPlayPause", {"mediaplaypause", nullptr}, KeyKind::kNamed,
     NamedKey::kMediaPlayPause},
    {"MediaNextTrack", {"medianexttrack", nullptr}, KeyKind::kNamed,
     NamedKey::kMediaNextTrack},
    {"MediaPreviousTrack", {"mediaprevioustrack", nullptr}, KeyKind::kNamed,
     NamedKey::kMediaPreviousTrack},
    {"MediaStop", {"mediastop", nullptr}, KeyKind::kNamed,
     NamedKey::kMediaStop},
    {"VolumeUp", {"volumeup", nullptr}, KeyKind::kNamed, NamedKey::kVolumeUp},
    {"VolumeDown", {"volumedown", nullptr}, KeyKind::kNamed,
     NamedKey::kVolumeDown},
    {"VolumeMute", {"volumemute", nullptr}, KeyKind::kNamed,
     NamedKey::kVolumeMute},
    {"NumDecimal", {"numdecimal", "numdec", nullptr}, KeyKind::kNumpad,
     NamedKey::kNumDecimal},
    {"NumAdd", {"numadd", nullptr}, KeyKind::kNumpad, NamedKey::kNumAdd},
    {"NumSubtract", {"numsubtract", "numsub", nullptr}, KeyKind::kNumpad,
     NamedKey::kNumSubtract},
    {"NumMultiply", {"nummultiply", "nummult", nullptr}, KeyKind::kNumpad,
     NamedKey::kNumMultiply},
    {"NumDivide", {"numdivide", "numdiv", nullptr}, KeyKind::kNumpad,
     NamedKey::kNumDivide},
};

const char kPunctuation[] = "-=[]\\;',./`";

bool ParseKey(const std::string& token, Accelerator* out) {
  const std::string t = Lower(token);
  if (t.size() == 1) {
    unsigned char c = static_cast<unsigned char>(t[0]);
    if (c >= 'a' && c <= 'z') {
      out->kind = KeyKind::kLetter;
      out->ch = static_cast<char>(toupper(c));
      return true;
    }
    if (c >= '0' && c <= '9') {
      out->kind = KeyKind::kDigit;
      out->ch = static_cast<char>(c);
      return true;
    }
    if (c != 0 && strchr(kPunctuation, c)) {
      out->kind = KeyKind::kPunct;
      out->ch = static_cast<char>(c);
      return true;
    }
    return false;
  }
  // F1..F24
  if (t[0] == 'f' && t.size() <= 3 && isdigit(static_cast<unsigned char>(t[1]))) {
    if (t.size() == 3 && !isdigit(static_cast<unsigned char>(t[2])))
      return false;
    if (t[1] == '0')
      return false;
    int n = atoi(t.c_str() + 1);
    if (n < 1 || n > 24)
      return false;
    out->kind = KeyKind::kFunction;
    out->number = n;
    return true;
  }
  // Num0..Num9 (also "numpad0")
  for (const char* prefix : {"numpad", "num"}) {
    size_t len = strlen(prefix);
    if (t.size() == len + 1 && t.compare(0, len, prefix) == 0 &&
        isdigit(static_cast<unsigned char>(t[len]))) {
      out->kind = KeyKind::kNumpad;
      out->number = t[len] - '0';
      out->named = NamedKey::kNone;
      return true;
    }
  }
  for (const auto& e : kNamedKeys) {
    for (const char* const* a = e.aliases; *a; ++a) {
      if (t == *a) {
        out->kind = e.kind;
        out->named = e.key;
        return true;
      }
    }
  }
  return false;
}

std::string KeyName(const Accelerator& a) {
  switch (a.kind) {
    case KeyKind::kLetter:
    case KeyKind::kDigit:
    case KeyKind::kPunct:
      return std::string(1, a.ch);
    case KeyKind::kFunction:
      return "F" + std::to_string(a.number);
    case KeyKind::kNumpad:
      if (a.named == NamedKey::kNone)
        return "Num" + std::to_string(a.number);
      break;
    case KeyKind::kNamed:
      break;
  }
  for (const auto& e : kNamedKeys) {
    if (e.key == a.named)
      return e.canonical;
  }
  return "";
}

}  // namespace

bool ParseAccelerator(const std::string& text, bool for_mac, Accelerator* out,
                      std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error)
      *error = why;
    return false;
  };
  if (text.empty())
    return fail("empty accelerator");
  if (text.size() > 128)
    return fail("accelerator too long");
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t plus = text.find('+', start);
    std::string part = text.substr(
        start, plus == std::string::npos ? std::string::npos : plus - start);
    // Trim spaces around each token.
    size_t b = part.find_first_not_of(' ');
    size_t e = part.find_last_not_of(' ');
    part = b == std::string::npos ? "" : part.substr(b, e - b + 1);
    if (part.empty())
      return fail("empty token in \"" + text + "\"");
    parts.push_back(part);
    if (plus == std::string::npos)
      break;
    start = plus + 1;
  }
  Accelerator a;
  bool have_key = false;
  for (size_t i = 0; i < parts.size(); i++) {
    const std::string t = Lower(parts[i]);
    uint32_t mod = 0;
    if (t == "command" || t == "cmd") {
      if (!for_mac)
        return fail("\"" + parts[i] +
                    "\" is macOS-only; use CommandOrControl or Super");
      mod = kModSuper;
    } else if (t == "control" || t == "ctrl") {
      mod = kModCtrl;
    } else if (t == "commandorcontrol" || t == "cmdorctrl") {
      mod = for_mac ? kModSuper : kModCtrl;
    } else if (t == "alt" || t == "option") {
      mod = kModAlt;
    } else if (t == "shift") {
      mod = kModShift;
    } else if (t == "super" || t == "meta") {
      mod = kModSuper;
    }
    if (mod) {
      if (have_key)
        return fail("a modifier after the key in \"" + text + "\"");
      if (a.mods & mod)
        return fail("a repeated modifier in \"" + text + "\"");
      a.mods |= mod;
      continue;
    }
    if (have_key)
      return fail("two keys in \"" + text + "\"");
    if (!ParseKey(parts[i], &a))
      return fail("unknown key \"" + parts[i] + "\"");
    have_key = true;
  }
  if (!have_key)
    return fail("no key in \"" + text + "\"");
  *out = a;
  return true;
}

std::string CanonicalAccelerator(const Accelerator& a) {
  std::string s;
  if (a.mods & kModCtrl)
    s += "Ctrl+";
  if (a.mods & kModAlt)
    s += "Alt+";
  if (a.mods & kModShift)
    s += "Shift+";
  if (a.mods & kModSuper)
    s += "Super+";
  return s + KeyName(a);
}

bool IsAllowedGlobalShortcut(const Accelerator& a) {
  // Keys used while typing: printable ones, the whole numpad, the editing
  // keys (Enter, Tab, Backspace, Delete, Insert) and the navigation keys
  // (the arrows, Home, End, PageUp, PageDown, Escape). A global shortcut on
  // one of them alone (or with Shift, which selects text) would see (and
  // swallow) what the user types and how they move in other apps. There is no
  // way to ask for one anyway: the C ABI has no such option.
  bool typing = a.kind == KeyKind::kLetter || a.kind == KeyKind::kDigit ||
                a.kind == KeyKind::kPunct || a.kind == KeyKind::kNumpad;
  if (a.kind == KeyKind::kNamed) {
    switch (a.named) {
      case NamedKey::kSpace:
      case NamedKey::kEnter:
      case NamedKey::kTab:
      case NamedKey::kBackspace:
      case NamedKey::kDelete:
      case NamedKey::kInsert:
      case NamedKey::kEscape:
      case NamedKey::kUp:
      case NamedKey::kDown:
      case NamedKey::kLeft:
      case NamedKey::kRight:
      case NamedKey::kHome:
      case NamedKey::kEnd:
      case NamedKey::kPageUp:
      case NamedKey::kPageDown:
        typing = true;
        break;
      default:
        break;
    }
  }
  if (!typing)
    return true;
  return (a.mods & ~kModShift) != 0;
}

// --- Registry ------------------------------------------------------------------

namespace {

struct Entry {
  std::string canonical;
  bool bound = false;     // the OS answered OK
  bool released = false;  // unregistered while the OS hadn't answered yet
  uint64_t seq = 0;       // registration order, for ListShortcuts
};

std::mutex g_mutex;
std::unique_ptr<ShortcutPlatform> g_platform;
std::map<uint32_t, Entry> g_entries;  // by id
uint64_t g_next_seq = 1;
laufey_shortcut_fn g_handler = nullptr;
void* g_handler_data = nullptr;

#ifdef __APPLE__
constexpr bool kForMac = true;
#else
constexpr bool kForMac = false;
#endif

// The smallest id not in use (Windows hotkey ids must stay below 0xC000).
uint32_t AllocateId() {
  uint32_t id = 1;
  for (const auto& [k, v] : g_entries) {
    if (k != id)
      break;
    id++;
  }
  return id;
}

// The id of a live (not released) entry with this canonical form, or 0.
uint32_t FindLive(const std::string& canonical) {
  for (const auto& [id, e] : g_entries) {
    if (!e.released && e.canonical == canonical)
      return id;
  }
  return 0;
}

bool Canonicalize(const char* accelerator, std::string* canonical,
                  Accelerator* parsed) {
  if (!accelerator)
    return false;
  Accelerator a;
  if (!ParseAccelerator(accelerator, kForMac, &a, nullptr))
    return false;
  if (parsed)
    *parsed = a;
  *canonical = CanonicalAccelerator(a);
  return true;
}

}  // namespace

void InstallShortcutPlatform(std::unique_ptr<ShortcutPlatform> platform) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_platform = std::move(platform);
}

uint32_t ShortcutCapabilities() {
  ShortcutPlatform* p;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    p = g_platform.get();
  }
  return p ? p->Capabilities() : 0;
}

void SetShortcutHandler(laufey_shortcut_fn handler, void* user_data) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handler = handler;
  g_handler_data = handler ? user_data : nullptr;
}

void RegisterShortcut(const char* accelerator,
                      laufey_shortcut_result_fn callback, void* user_data) {
  auto answer = [&](int status, const std::string* canonical) {
    if (callback)
      callback(user_data, status, canonical ? canonical->c_str() : nullptr);
  };
  Accelerator parsed;
  std::string canonical;
  if (!Canonicalize(accelerator, &canonical, &parsed) ||
      !IsAllowedGlobalShortcut(parsed)) {
    answer(LAUFEY_SHORTCUT_INVALID, nullptr);
    return;
  }
  // Ask for the capabilities outside the lock: the Linux probe may block.
  uint32_t caps = ShortcutCapabilities();
  uint32_t id;
  ShortcutPlatform* platform;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    platform = g_platform.get();
    if (!platform || !(caps & LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS)) {
      id = 0;
    } else if (FindLive(canonical)) {
      id = UINT32_MAX;
    } else if (g_entries.size() >= LAUFEY_MAX_SHORTCUTS) {
      id = UINT32_MAX - 1;
    } else {
      id = AllocateId();
      g_entries[id] = Entry{canonical, false, false, g_next_seq++};
    }
  }
  if (id == 0) {
    answer(LAUFEY_SHORTCUT_NOT_SUPPORTED, nullptr);
    return;
  }
  if (id == UINT32_MAX) {
    answer(LAUFEY_SHORTCUT_ALREADY_REGISTERED, &canonical);
    return;
  }
  if (id == UINT32_MAX - 1) {
    answer(LAUFEY_SHORTCUT_FAILED, nullptr);
    return;
  }
  platform->Bind(id, parsed, [id, canonical, callback, user_data](int status) {
    bool unbind = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto it = g_entries.find(id);
      if (it != g_entries.end()) {
        if (status == LAUFEY_SHORTCUT_OK && !it->second.released) {
          it->second.bound = true;
        } else {
          // Refused, or released before the OS answered: drop it (and give a
          // binding that went through back to the OS).
          unbind = status == LAUFEY_SHORTCUT_OK;
          g_entries.erase(it);
        }
      }
      if (unbind && g_platform)
        g_platform->Unbind(id);
    }
    if (callback) {
      callback(user_data, status,
               status == LAUFEY_SHORTCUT_OK ? canonical.c_str() : nullptr);
    }
  });
}

bool UnregisterShortcut(const char* accelerator) {
  std::string canonical;
  if (!Canonicalize(accelerator, &canonical, nullptr))
    return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t id = FindLive(canonical);
  if (!id)
    return false;
  Entry& e = g_entries[id];
  if (e.bound) {
    g_entries.erase(id);
    if (g_platform)
      g_platform->Unbind(id);
  } else {
    // Still waiting for the OS: the Bind completion releases it.
    e.released = true;
  }
  return true;
}

void UnregisterAllShortcuts() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    if (it->second.bound) {
      if (g_platform)
        g_platform->Unbind(it->first);
      it = g_entries.erase(it);
    } else {
      it->second.released = true;
      ++it;
    }
  }
}

char* ListShortcuts() {
  std::string out;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // In registration order (ids are reused, so they aren't).
    std::map<uint64_t, const std::string*> ordered;
    for (const auto& [id, e] : g_entries) {
      if (e.bound && !e.released)
        ordered[e.seq] = &e.canonical;
    }
    for (const auto& [seq, canonical] : ordered) {
      if (!out.empty())
        out += '\n';
      out += *canonical;
    }
  }
  char* s = static_cast<char*>(malloc(out.size() + 1));
  if (s)
    memcpy(s, out.c_str(), out.size() + 1);
  return s;
}

char* CanonicalizeAccelerator(const char* accelerator) {
  std::string canonical;
  if (!Canonicalize(accelerator, &canonical, nullptr))
    return nullptr;
  char* s = static_cast<char*>(malloc(canonical.size() + 1));
  if (s)
    memcpy(s, canonical.c_str(), canonical.size() + 1);
  return s;
}

void DispatchShortcut(uint32_t id) {
  laufey_shortcut_fn handler;
  void* data;
  std::string canonical;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_entries.find(id);
    if (it == g_entries.end() || !it->second.bound || it->second.released)
      return;
    canonical = it->second.canonical;
    handler = g_handler;
    data = g_handler_data;
  }
  if (handler)
    handler(data, canonical.c_str());
}

bool TestTriggerShortcut(const char* accelerator) {
  std::string canonical;
  if (!Canonicalize(accelerator, &canonical, nullptr))
    return false;
  uint32_t id;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    id = FindLive(canonical);
    if (!id || !g_entries[id].bound || !g_handler)
      return false;
  }
  DispatchShortcut(id);
  return true;
}

}  // namespace laufey_common
