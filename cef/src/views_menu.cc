// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF Views menus (Windows and Linux). See views_menu.h.

#include "views_menu.h"

#if defined(_WIN32) || defined(__linux__)

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_menu_model.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/cef_menu_model_delegate.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_label_button.h"
#include "include/views/cef_menu_button.h"
#include "include/views/cef_menu_button_delegate.h"
#include "include/views/cef_panel.h"
#include "laufey_backend_common.h"

namespace laufey_cef_menu {

namespace {

using laufey_common::MenuEntry;

// Window accelerator command ids live above Chrome's IDC_* range.
constexpr int kFirstAcceleratorId = 0x7A000;
// Menu model command ids (per model; Chrome's context-menu ids end far
// below this).
constexpr int kFirstMenuCommandId = 0x7B000;

// The clicks a menu model's commands run.
class MenuDelegate : public CefMenuModelDelegate {
 public:
  MenuDelegate(uint32_t window_id, laufey_menu_click_fn on_click,
               void* on_click_data)
      : window_id_(window_id),
        on_click_(on_click),
        on_click_data_(on_click_data) {}

  int Add(const std::string& key) {
    int id = next_id_++;
    commands_[id] = key;
    return id;
  }

  void ExecuteCommand(CefRefPtr<CefMenuModel> /*menu_model*/, int command_id,
                      cef_event_flags_t /*event_flags*/) override {
    auto it = commands_.find(command_id);
    if (it != commands_.end() && on_click_)
      on_click_(on_click_data_, window_id_, it->second.c_str());
  }

  // A context menu: CEF calls MenuClosed after ExecuteCommand (it posts the
  // close notification so the command runs first).
  void SetSession(uint64_t session) {
    session_ = session;
  }
  void MenuClosed(CefRefPtr<CefMenuModel> /*menu_model*/) override {
    if (session_) {
      uint64_t s = session_;
      session_ = 0;
      laufey_common::EndContextMenu(s);
    }
  }

 private:
  uint32_t window_id_;
  laufey_menu_click_fn on_click_;
  void* on_click_data_;
  std::map<int, std::string> commands_;
  int next_id_ = kFirstMenuCommandId;
  uint64_t session_ = 0;
  IMPLEMENT_REFCOUNTING(MenuDelegate);
};

const char* RoleLabel(const std::string& role) {
  if (laufey_common::RoleIs(role, "quit"))
    return "Quit";
  if (laufey_common::RoleIs(role, "copy"))
    return "Copy";
  if (laufey_common::RoleIs(role, "paste"))
    return "Paste";
  if (laufey_common::RoleIs(role, "cut"))
    return "Cut";
  if (laufey_common::RoleIs(role, "selectall"))
    return "Select All";
  if (laufey_common::RoleIs(role, "undo"))
    return "Undo";
  if (laufey_common::RoleIs(role, "redo"))
    return "Redo";
  if (laufey_common::RoleIs(role, "minimize"))
    return "Minimize";
  if (laufey_common::RoleIs(role, "close"))
    return "Close";
  if (laufey_common::RoleIs(role, "about"))
    return "About";
  if (laufey_common::RoleIs(role, "togglefullscreen"))
    return "Toggle Full Screen";
  return nullptr;
}

// The key code CEF accelerators take: a Windows virtual-key code (CEF uses
// them on every platform). Layout-aware on Windows.
int KeyCodeFor(const laufey_common::Accelerator& a) {
#if defined(_WIN32)
  return static_cast<int>(laufey_common::AcceleratorVirtualKey(a));
#else
  return laufey_common::AcceleratorUsVirtualKey(a);
#endif
}

void Fill(CefRefPtr<CefMenuModel> model, const std::vector<MenuEntry>& entries,
          MenuDelegate* delegate) {
  for (const MenuEntry& e : entries) {
    switch (e.kind) {
      case MenuEntry::Kind::kSeparator:
        model->AddSeparator();
        break;
      case MenuEntry::Kind::kRole: {
        const char* label =
            e.label.empty() ? RoleLabel(e.role) : e.label.c_str();
        if (label)
          model->AddItem(delegate->Add(e.role), label);
        break;
      }
      case MenuEntry::Kind::kSubmenu: {
        CefRefPtr<CefMenuModel> sub =
            model->AddSubMenu(delegate->Add(std::string()), e.label);
        if (sub)
          Fill(sub, e.children, delegate);
        break;
      }
      case MenuEntry::Kind::kItem: {
        int id = delegate->Add(e.ClickKey());
        if (e.checked) {
          model->AddCheckItem(id, e.label);
          model->SetChecked(id, true);
        } else {
          model->AddItem(id, e.label);
        }
        model->SetEnabled(id, e.enabled);
        if (e.has_accel && !(e.accel.mods & laufey_common::kModSuper)) {
          if (int key = KeyCodeFor(e.accel)) {
            model->SetAccelerator(
                id, key, (e.accel.mods & laufey_common::kModShift) != 0,
                (e.accel.mods & laufey_common::kModCtrl) != 0,
                (e.accel.mods & laufey_common::kModAlt) != 0);
          }
        }
        break;
      }
    }
  }
}

// Register the items for test_click_menu_item.
void RegisterClicks(const std::vector<MenuEntry>& entries, uint32_t window_id,
                    laufey_menu_click_fn on_click, void* on_click_data) {
  for (const MenuEntry& e : entries) {
    if (e.kind == MenuEntry::Kind::kSubmenu)
      RegisterClicks(e.children, window_id, on_click, on_click_data);
    else if (e.kind == MenuEntry::Kind::kItem)
      laufey_common::RegisterMenuClick(e.ClickKey(), on_click, on_click_data,
                                       window_id);
  }
}

// A top-level menu of the bar.
class BarMenuButtonDelegate : public CefMenuButtonDelegate {
 public:
  explicit BarMenuButtonDelegate(CefRefPtr<CefMenuModel> model)
      : model_(model) {}
  void OnMenuButtonPressed(
      CefRefPtr<CefMenuButton> menu_button, const CefPoint& /*screen_point*/,
      CefRefPtr<CefMenuButtonPressedLock> /*lock*/) override {
    CefRect bounds = menu_button->GetBoundsInScreen();
    menu_button->ShowMenu(model_, CefPoint(bounds.x, bounds.y + bounds.height),
                          CEF_MENU_ANCHOR_TOPLEFT);
  }
  void OnButtonPressed(CefRefPtr<CefButton> /*button*/) override {}

 private:
  CefRefPtr<CefMenuModel> model_;
  IMPLEMENT_REFCOUNTING(BarMenuButtonDelegate);
};

// A plain item or role on the bar itself.
class BarItemButtonDelegate : public CefButtonDelegate {
 public:
  BarItemButtonDelegate(uint32_t window_id, std::string key,
                        laufey_menu_click_fn on_click, void* on_click_data)
      : window_id_(window_id),
        key_(std::move(key)),
        on_click_(on_click),
        on_click_data_(on_click_data) {}
  void OnButtonPressed(CefRefPtr<CefButton> /*button*/) override {
    if (on_click_)
      on_click_(on_click_data_, window_id_, key_.c_str());
  }

 private:
  uint32_t window_id_;
  std::string key_;
  laufey_menu_click_fn on_click_;
  void* on_click_data_;
  IMPLEMENT_REFCOUNTING(BarItemButtonDelegate);
};

struct WindowMenu {
  CefRefPtr<CefPanel> bar;
  CefRefPtr<CefWindow> window;
  std::map<int, std::string> accelerators;  // window command id -> click key
  std::vector<laufey_common::MenuAccelBinding> bindings;
  laufey_menu_click_fn on_click = nullptr;
  void* on_click_data = nullptr;
};

// TID_UI only.
std::map<uint32_t, WindowMenu>& Menus() {
  static std::map<uint32_t, WindowMenu> menus;
  return menus;
}

}  // namespace

void SetApplicationMenu(CefRefPtr<CefWindow> window,
                        CefRefPtr<CefBrowserView> browser_view,
                        uint32_t window_id, std::vector<MenuEntry> entries,
                        laufey_menu_click_fn on_click, void* on_click_data) {
  if (!window || !browser_view)
    return;
  WindowMenu& wm = Menus()[window_id];
  wm.window = window;

  // Old accelerators and bar out.
  for (const auto& [command_id, key] : wm.accelerators)
    window->RemoveAccelerator(command_id);
  wm.accelerators.clear();
  wm.bindings.clear();
  if (wm.bar) {
    window->RemoveChildView(wm.bar);
    wm.bar = nullptr;
  }
  wm.on_click = on_click;
  wm.on_click_data = on_click_data;

  if (entries.empty()) {
    window->SetToFillLayout();
    window->Layout();
    return;
  }

  CefRefPtr<CefPanel> bar = CefPanel::CreatePanel(nullptr);
  CefBoxLayoutSettings bar_settings;
  bar_settings.horizontal = true;
  bar->SetToBoxLayout(bar_settings);
  for (const MenuEntry& e : entries) {
    if (e.kind == MenuEntry::Kind::kSubmenu) {
      CefRefPtr<MenuDelegate> delegate =
          new MenuDelegate(window_id, on_click, on_click_data);
      CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(delegate);
      Fill(model, e.children, delegate.get());
      CefRefPtr<CefMenuButton> button = CefMenuButton::CreateMenuButton(
          new BarMenuButtonDelegate(model), e.label);
      bar->AddChildView(button);
    } else if (e.kind == MenuEntry::Kind::kItem ||
               e.kind == MenuEntry::Kind::kRole) {
      std::string key =
          e.kind == MenuEntry::Kind::kRole ? e.role : e.ClickKey();
      const char* label = !e.label.empty() ? e.label.c_str()
                                           : (e.kind == MenuEntry::Kind::kRole
                                                  ? RoleLabel(e.role)
                                                  : nullptr);
      if (!label)
        continue;
      CefRefPtr<CefLabelButton> button = CefLabelButton::CreateLabelButton(
          new BarItemButtonDelegate(window_id, key, on_click, on_click_data),
          label);
      button->SetEnabled(e.enabled);
      bar->AddChildView(button);
    }
  }
  RegisterClicks(entries, window_id, on_click, on_click_data);

  CefBoxLayoutSettings settings;
  settings.horizontal = false;
  CefRefPtr<CefBoxLayout> layout = window->SetToBoxLayout(settings);
  window->AddChildViewAt(bar, 0);
  layout->SetFlexForView(browser_view, 1);
  layout->SetFlexForView(bar, 0);
  window->Layout();
  wm.bar = bar;

  // Accelerators: high priority, so they win over the page as a native menu
  // bar's would.
  wm.bindings = laufey_common::CollectMenuAccelerators(entries);
  int next = kFirstAcceleratorId;
  for (const auto& b : wm.bindings) {
    if (b.accel.mods & laufey_common::kModSuper)
      continue;  // CEF accelerators have no Super / Windows-key modifier
    int key = KeyCodeFor(b.accel);
    if (!key)
      continue;
    int command_id = next++;
    window->SetAccelerator(command_id, key,
                           (b.accel.mods & laufey_common::kModShift) != 0,
                           (b.accel.mods & laufey_common::kModCtrl) != 0,
                           (b.accel.mods & laufey_common::kModAlt) != 0,
                           /*high_priority=*/true);
    wm.accelerators[command_id] = b.click_key;
  }
}

bool OnAccelerator(uint32_t window_id, int command_id) {
  auto it = Menus().find(window_id);
  if (it == Menus().end())
    return false;
  auto acc = it->second.accelerators.find(command_id);
  if (acc == it->second.accelerators.end())
    return false;
  std::string key = acc->second;
  laufey_menu_click_fn fn = it->second.on_click;
  void* data = it->second.on_click_data;
  if (fn)
    fn(data, window_id, key.c_str());
  return true;
}

bool TestTriggerAccelerator(uint32_t window_id, const char* accelerator) {
  if (!accelerator)
    return false;
  laufey_common::Accelerator accel;
  if (!laufey_common::ParseAccelerator(accelerator, false, &accel, nullptr))
    return false;
  auto it = Menus().find(window_id);
  if (it == Menus().end())
    return false;
  const laufey_common::MenuAccelBinding* b =
      laufey_common::FindMenuAccelerator(it->second.bindings, accel);
  if (!b)
    return false;
  for (const auto& [command_id, key] : it->second.accelerators) {
    if (key == b->click_key)
      return OnAccelerator(window_id, command_id);
  }
  return false;
}

void ShowContextMenu(CefRefPtr<CefWindow> window,
                     CefRefPtr<CefBrowserView> browser_view, uint32_t window_id,
                     int x, int y, std::vector<MenuEntry> entries,
                     laufey_menu_click_fn on_click, void* on_click_data,
                     laufey_menu_closed_fn on_closed, void* on_closed_data) {
  if (!window || !browser_view || entries.empty()) {
    laufey_common::FireContextMenuClosedNow(window_id, on_closed,
                                            on_closed_data);
    return;
  }
  CefRefPtr<MenuDelegate> delegate =
      new MenuDelegate(window_id, on_click, on_click_data);
  CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(delegate);
  Fill(model, entries, delegate.get());
  RegisterClicks(entries, window_id, on_click, on_click_data);

  CefPoint point(x, y);
  browser_view->ConvertPointToScreen(point);
  CefRefPtr<CefWindow> w = window;
  uint64_t session = laufey_common::BeginContextMenu(
      window_id, on_closed, on_closed_data, [w] {
        CefPostTask(
            TID_UI,
            base::BindOnce([](CefRefPtr<CefWindow> win) { win->CancelMenu(); },
                           w));
      });
  delegate->SetSession(session);
  window->ShowMenu(model, point, CEF_MENU_ANCHOR_TOPLEFT);
}

void ForgetWindow(uint32_t window_id) {
  Menus().erase(window_id);
}

}  // namespace laufey_cef_menu

#endif  // _WIN32 || __linux__
