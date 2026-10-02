// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable half of menus (API 41): template parsing with accelerators,
// the context-menu session and accelerator lookup. See laufey_menu.h.

#include "laufey_menu.h"
#include "laufey_ui_tasks.h"

#include <atomic>
#include <cctype>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace laufey_common {

namespace {

// Frees a value fetched from a template on scope exit.
class ValueRef {
 public:
  ValueRef(const laufey_backend_api_t* api, laufey_value_t* v)
      : api_(api), v_(v) {}
  ~ValueRef() {
    if (v_)
      api_->value_free(v_);
  }
  ValueRef(const ValueRef&) = delete;
  ValueRef& operator=(const ValueRef&) = delete;
  laufey_value_t* get() const {
    return v_;
  }
  explicit operator bool() const {
    return v_ != nullptr;
  }

 private:
  const laufey_backend_api_t* api_;
  laufey_value_t* v_;
};

std::string ReadString(const laufey_backend_api_t* api, laufey_value_t* dict,
                       const char* key) {
  ValueRef v(api, api->value_dict_get(dict, key));
  if (!v || !api->value_is_string(v.get()))
    return std::string();
  size_t len = 0;
  char* s = api->value_get_string(v.get(), &len);
  if (!s)
    return std::string();
  std::string out(s, len);
  api->value_free_string(s);
  return out;
}

bool ReadBool(const laufey_backend_api_t* api, laufey_value_t* dict,
              const char* key, bool dfl) {
  ValueRef v(api, api->value_dict_get(dict, key));
  if (!v || !api->value_is_bool(v.get()))
    return dfl;
  return api->value_get_bool(v.get());
}

void WarnBadAccelerator(const std::string& text, const std::string& error) {
  static std::mutex mutex;
  static std::set<std::string> warned;
  std::lock_guard<std::mutex> lock(mutex);
  if (warned.size() < 256 && warned.insert(text).second) {
    std::cerr << "laufey: menu accelerator \"" << text
              << "\" ignored: " << error << std::endl;
  }
}

void ParseList(laufey_value_t* list, const laufey_backend_api_t* api,
               bool for_mac, int depth, std::vector<MenuEntry>* out) {
  if (!list || !api->value_is_list(list) || depth > 16)
    return;
  size_t count = api->value_list_size(list);
  for (size_t i = 0; i < count; ++i) {
    ValueRef item(api, api->value_list_get(list, i));
    if (!item || !api->value_is_dict(item.get()))
      continue;
    MenuEntry e;
    if (ReadString(api, item.get(), "type") == "separator") {
      e.kind = MenuEntry::Kind::kSeparator;
      out->push_back(std::move(e));
      continue;
    }
    std::string role = ReadString(api, item.get(), "role");
    if (!role.empty()) {
      e.kind = MenuEntry::Kind::kRole;
      e.role = role;
      e.label = ReadString(api, item.get(), "label");
      out->push_back(std::move(e));
      continue;
    }
    e.label = ReadString(api, item.get(), "label");
    if (e.label.empty())
      continue;
    {
      ValueRef sub(api, api->value_dict_get(item.get(), "submenu"));
      if (sub && api->value_is_list(sub.get())) {
        e.kind = MenuEntry::Kind::kSubmenu;
        ParseList(sub.get(), api, for_mac, depth + 1, &e.children);
        out->push_back(std::move(e));
        continue;
      }
    }
    e.kind = MenuEntry::Kind::kItem;
    e.id = ReadString(api, item.get(), "id");
    e.tooltip = ReadString(api, item.get(), "tooltip");
    e.enabled = ReadBool(api, item.get(), "enabled", true);
    e.checked = ReadBool(api, item.get(), "checked", false);
    e.accelerator_text = ReadString(api, item.get(), "accelerator");
    if (!e.accelerator_text.empty()) {
      std::string error;
      e.has_accel =
          ParseAccelerator(e.accelerator_text, for_mac, &e.accel, &error);
      if (!e.has_accel)
        WarnBadAccelerator(e.accelerator_text, error);
    }
    {
      ValueRef icon(api, api->value_dict_get(item.get(), "icon"));
      if (icon && api->value_is_binary(icon.get())) {
        size_t len = 0;
        const void* bytes = api->value_get_binary(icon.get(), &len);
        if (bytes && len > 0) {
          const uint8_t* p = static_cast<const uint8_t*>(bytes);
          e.icon_png.assign(p, p + len);
        }
      }
    }
    out->push_back(std::move(e));
  }
}

void Collect(const std::vector<MenuEntry>& entries,
             std::vector<MenuAccelBinding>* out) {
  for (const MenuEntry& e : entries) {
    if (e.kind == MenuEntry::Kind::kSubmenu) {
      Collect(e.children, out);
    } else if (e.kind == MenuEntry::Kind::kItem && e.enabled && e.has_accel) {
      if (!FindMenuAccelerator(*out, e.accel))
        out->push_back(MenuAccelBinding{e.accel, e.ClickKey()});
    }
  }
}

bool SameAccelerator(const Accelerator& a, const Accelerator& b) {
  return a.mods == b.mods && a.kind == b.kind && a.ch == b.ch &&
         a.number == b.number && a.named == b.named;
}

// --- Context-menu sessions ---

struct Session {
  uint32_t window_id = 0;
  laufey_menu_closed_fn on_closed = nullptr;
  void* on_closed_data = nullptr;
  std::function<void()> dismiss;
};

std::mutex g_session_mutex;
std::map<uint64_t, Session> g_sessions;
uint64_t g_next_session = 1;

}  // namespace

std::vector<MenuEntry> ParseMenuTemplate(laufey_value_t* menu_template,
                                         const laufey_backend_api_t* api,
                                         bool for_mac) {
  std::vector<MenuEntry> out;
  if (api)
    ParseList(menu_template, api, for_mac, 0, &out);
  return out;
}

bool RoleIs(const std::string& role, const char* name) {
  size_t n = std::char_traits<char>::length(name);
  if (role.size() != n)
    return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(role[i])) !=
        std::tolower(static_cast<unsigned char>(name[i])))
      return false;
  }
  return true;
}

std::string AcceleratorDisplayText(const Accelerator& accel) {
  std::string s = CanonicalAccelerator(accel);
#ifdef _WIN32
  const std::string super = "Super+";
  size_t at = s.find(super);
  if (at != std::string::npos)
    s.replace(at, super.size(), "Win+");
#endif
  return s;
}

int AcceleratorUsVirtualKey(const Accelerator& a) {
  switch (a.kind) {
    case KeyKind::kLetter:
    case KeyKind::kDigit:
      return a.ch;
    case KeyKind::kPunct:
      switch (a.ch) {
        case '-':
          return 0xBD;  // VK_OEM_MINUS
        case '=':
          return 0xBB;  // VK_OEM_PLUS
        case '[':
          return 0xDB;  // VK_OEM_4
        case ']':
          return 0xDD;  // VK_OEM_6
        case '\\':
          return 0xDC;  // VK_OEM_5
        case ';':
          return 0xBA;  // VK_OEM_1
        case '\'':
          return 0xDE;  // VK_OEM_7
        case ',':
          return 0xBC;  // VK_OEM_COMMA
        case '.':
          return 0xBE;  // VK_OEM_PERIOD
        case '/':
          return 0xBF;  // VK_OEM_2
        case '`':
          return 0xC0;  // VK_OEM_3
      }
      return 0;
    case KeyKind::kFunction:
      return 0x70 + (a.number - 1);  // VK_F1..VK_F24
    case KeyKind::kNumpad:
      switch (a.named) {
        case NamedKey::kNone:
          return 0x60 + a.number;  // VK_NUMPAD0..9
        case NamedKey::kNumMultiply:
          return 0x6A;
        case NamedKey::kNumAdd:
          return 0x6B;
        case NamedKey::kNumSubtract:
          return 0x6D;
        case NamedKey::kNumDecimal:
          return 0x6E;
        case NamedKey::kNumDivide:
          return 0x6F;
        default:
          return 0;
      }
    case KeyKind::kNamed:
      switch (a.named) {
        case NamedKey::kBackspace:
          return 0x08;
        case NamedKey::kTab:
          return 0x09;
        case NamedKey::kEnter:
          return 0x0D;
        case NamedKey::kEscape:
          return 0x1B;
        case NamedKey::kSpace:
          return 0x20;
        case NamedKey::kPageUp:
          return 0x21;
        case NamedKey::kPageDown:
          return 0x22;
        case NamedKey::kEnd:
          return 0x23;
        case NamedKey::kHome:
          return 0x24;
        case NamedKey::kLeft:
          return 0x25;
        case NamedKey::kUp:
          return 0x26;
        case NamedKey::kRight:
          return 0x27;
        case NamedKey::kDown:
          return 0x28;
        case NamedKey::kPrintScreen:
          return 0x2C;
        case NamedKey::kInsert:
          return 0x2D;
        case NamedKey::kDelete:
          return 0x2E;
        case NamedKey::kVolumeMute:
          return 0xAD;
        case NamedKey::kVolumeDown:
          return 0xAE;
        case NamedKey::kVolumeUp:
          return 0xAF;
        case NamedKey::kMediaNextTrack:
          return 0xB0;
        case NamedKey::kMediaPreviousTrack:
          return 0xB1;
        case NamedKey::kMediaStop:
          return 0xB2;
        case NamedKey::kMediaPlayPause:
          return 0xB3;
        default:
          return 0;
      }
  }
  return 0;
}

std::vector<MenuAccelBinding> CollectMenuAccelerators(
    const std::vector<MenuEntry>& entries) {
  std::vector<MenuAccelBinding> out;
  Collect(entries, &out);
  return out;
}

const MenuAccelBinding* FindMenuAccelerator(
    const std::vector<MenuAccelBinding>& bindings, const Accelerator& accel) {
  for (const MenuAccelBinding& b : bindings) {
    if (SameAccelerator(b.accel, accel))
      return &b;
  }
  return nullptr;
}

namespace {

std::atomic<NativeModalLoopHook> g_modal_loop_hook{nullptr};
// Depth of the native modal loops on this thread (the UI thread), and the
// hook told about the outermost one, which is the one told about its end.
thread_local int g_modal_loop_depth = 0;
thread_local NativeModalLoopHook g_modal_loop_entered = nullptr;

}  // namespace

void SetNativeModalLoopHook(NativeModalLoopHook hook) {
  g_modal_loop_hook.store(hook);
}

ScopedNativeModalLoop::ScopedNativeModalLoop()
    : on_ui_thread_(UiTaskDispatcher::Get().IsUiThread()) {
  if (!on_ui_thread_)
    return;
  if (g_modal_loop_depth++ == 0) {
    g_modal_loop_entered = g_modal_loop_hook.load();
    if (g_modal_loop_entered)
      g_modal_loop_entered(true);
  }
}

ScopedNativeModalLoop::~ScopedNativeModalLoop() {
  if (!on_ui_thread_)
    return;
  if (--g_modal_loop_depth == 0 && g_modal_loop_entered) {
    NativeModalLoopHook hook = g_modal_loop_entered;
    g_modal_loop_entered = nullptr;
    hook(false);
  }
}

uint64_t BeginContextMenu(uint32_t window_id, laufey_menu_closed_fn on_closed,
                          void* on_closed_data, std::function<void()> dismiss) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  uint64_t id = g_next_session++;
  g_sessions[id] =
      Session{window_id, on_closed, on_closed_data, std::move(dismiss)};
  return id;
}

void EndContextMenu(uint64_t session) {
  Session s;
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    auto it = g_sessions.find(session);
    if (it == g_sessions.end())
      return;
    s = std::move(it->second);
    g_sessions.erase(it);
  }
  // Outside the lock: the callback may show another menu.
  if (s.on_closed)
    s.on_closed(s.on_closed_data, s.window_id);
}

bool DismissOpenContextMenu() {
  std::function<void()> dismiss;
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (g_sessions.empty())
      return false;
    dismiss = g_sessions.rbegin()->second.dismiss;
  }
  if (!dismiss)
    return false;
  dismiss();
  return true;
}

void FireContextMenuClosedNow(uint32_t window_id,
                              laufey_menu_closed_fn on_closed,
                              void* on_closed_data) {
  if (on_closed)
    on_closed(on_closed_data, window_id);
}

}  // namespace laufey_common
