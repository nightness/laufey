// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GtkMenu / GtkMenuBar construction from a menu template. Each non-separator
// item carries its own click handler+data in a per-item GtkMenuCallbackData
// attached via g_signal_connect_data so the same builder serves application
// menus, context menus, and tray menus.
//
// API 41: accelerators (a GtkAccelGroup on the window for the app menu, shown
// without a binding in context and tray menus), item icons (a
// GtkImageMenuItem, which the AppIndicator tray exports too) and tooltips,
// context menus placed at the requested point with a close callback, and the
// accelerator test hook.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_menu.h"

#include <memory>
#include <string>

namespace laufey_common {

namespace {

struct GtkMenuCallbackData {
  laufey_menu_click_fn on_click;
  void* on_click_data;
  uint32_t window_id;
  std::string item_id;
  bool checked;
};

void OnGtkMenuItemActivate(GtkMenuItem* item, gpointer user_data) {
  auto* data = static_cast<GtkMenuCallbackData*>(user_data);
  // laufey models `checked` as a static property, but a GtkCheckMenuItem flips
  // its own active state on click. The default toggle has already run by the
  // time this RUN_FIRST handler fires, so restore the intended state to keep
  // the checkmark in sync with the app model (matches macOS/Windows).
  if (GTK_IS_CHECK_MENU_ITEM(item)) {
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), data->checked);
  }
  if (data->on_click) {
    data->on_click(data->on_click_data, data->window_id, data->item_id.c_str());
  }
}

void DestroyGtkMenuCallbackData(gpointer user_data, GClosure* /*closure*/) {
  delete static_cast<GtkMenuCallbackData*>(user_data);
}

GdkModifierType GdkModsFor(const Accelerator& a) {
  int mods = 0;
  if (a.mods & kModCtrl)
    mods |= GDK_CONTROL_MASK;
  if (a.mods & kModAlt)
    mods |= GDK_MOD1_MASK;
  if (a.mods & kModShift)
    mods |= GDK_SHIFT_MASK;
  if (a.mods & kModSuper)
    mods |= GDK_SUPER_MASK;
  return static_cast<GdkModifierType>(mods);
}

const char* RoleLabel(const std::string& role) {
  if (RoleIs(role, "quit"))
    return "Quit";
  if (RoleIs(role, "copy"))
    return "Copy";
  if (RoleIs(role, "paste"))
    return "Paste";
  if (RoleIs(role, "cut"))
    return "Cut";
  if (RoleIs(role, "selectall"))
    return "Select All";
  if (RoleIs(role, "undo"))
    return "Undo";
  if (RoleIs(role, "redo"))
    return "Redo";
  if (RoleIs(role, "minimize"))
    return "Minimize";
  if (RoleIs(role, "close"))
    return "Close";
  if (RoleIs(role, "about"))
    return "About";
  if (RoleIs(role, "togglefullscreen"))
    return "Toggle Full Screen";
  return nullptr;
}

// A 16x16 (logical) pixbuf from PNG bytes, or nullptr.
GdkPixbuf* IconPixbuf(const std::vector<uint8_t>& png) {
  if (png.empty())
    return nullptr;
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
  if (!loader)
    return nullptr;
  GdkPixbuf* scaled = nullptr;
  if (gdk_pixbuf_loader_write(loader, png.data(), png.size(), nullptr) &&
      gdk_pixbuf_loader_close(loader, nullptr)) {
    GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf) {
      scaled = gdk_pixbuf_scale_simple(pixbuf, 16, 16, GDK_INTERP_BILINEAR);
    }
  } else {
    gdk_pixbuf_loader_close(loader, nullptr);
  }
  g_object_unref(loader);
  return scaled;
}

GtkWidget* NewItem(const MenuEntry& e) {
  if (e.checked) {
    GtkWidget* item = gtk_check_menu_item_new_with_label(e.label.c_str());
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
    return item;
  }
  if (GdkPixbuf* pixbuf = IconPixbuf(e.icon_png)) {
    // GtkImageMenuItem is deprecated in GTK 3 but is the only menu item with
    // an image slot that libdbusmenu (the AppIndicator tray) also exports.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget* item = gtk_image_menu_item_new_with_label(e.label.c_str());
    GtkWidget* image = gtk_image_new_from_pixbuf(pixbuf);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), image);
    gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM(item), TRUE);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(pixbuf);
    return item;
  }
  return gtk_menu_item_new_with_label(e.label.c_str());
}

void ConnectClick(GtkWidget* item, laufey_menu_click_fn on_click,
                  void* on_click_data, uint32_t window_id,
                  const std::string& key, bool checked) {
  auto* cb_data =
      new GtkMenuCallbackData{on_click, on_click_data, window_id, key, checked};
  g_signal_connect_data(item, "activate", G_CALLBACK(OnGtkMenuItemActivate),
                        cb_data, DestroyGtkMenuCallbackData, (GConnectFlags)0);
}

void ShowAccelerator(GtkWidget* item, const MenuEntry& e,
                     GtkAccelGroup* accel_group) {
  if (!e.has_accel)
    return;
  guint key = AcceleratorKeysym(e.accel);
  if (!key)
    return;
  GdkModifierType mods = GdkModsFor(e.accel);
  if (accel_group && e.enabled && gtk_accelerator_valid(key, mods)) {
    // Bound on the window: GtkWindow activates accelerators before the
    // focused widget (the web view) sees the key.
    gtk_widget_add_accelerator(item, "activate", accel_group, key, mods,
                               GTK_ACCEL_VISIBLE);
    return;
  }
  // Shown only (context and tray menus, a disabled item).
  GtkWidget* child = gtk_bin_get_child(GTK_BIN(item));
  if (child && GTK_IS_ACCEL_LABEL(child))
    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key, mods);
}

GtkWidget* Build(const std::vector<MenuEntry>& entries, uint32_t window_id,
                 laufey_menu_click_fn on_click, void* on_click_data,
                 bool is_menu_bar, GtkAccelGroup* accel_group) {
  GtkWidget* menu = is_menu_bar ? gtk_menu_bar_new() : gtk_menu_new();
  if (!is_menu_bar && accel_group)
    gtk_menu_set_accel_group(GTK_MENU(menu), accel_group);
  for (const MenuEntry& e : entries) {
    switch (e.kind) {
      case MenuEntry::Kind::kSeparator:
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        break;
      case MenuEntry::Kind::kRole: {
        // GTK has no built-in role concept: map roles to labels and forward
        // the role as the item id.
        const char* label =
            e.label.empty() ? RoleLabel(e.role) : e.label.c_str();
        if (!label)
          break;
        GtkWidget* item = gtk_menu_item_new_with_label(label);
        ConnectClick(item, on_click, on_click_data, window_id, e.role, false);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        break;
      }
      case MenuEntry::Kind::kSubmenu: {
        GtkWidget* parent = gtk_menu_item_new_with_label(e.label.c_str());
        GtkWidget* submenu = Build(e.children, window_id, on_click,
                                   on_click_data, false, accel_group);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), submenu);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
        break;
      }
      case MenuEntry::Kind::kItem: {
        GtkWidget* item = NewItem(e);
        const std::string& key = e.ClickKey();
        ConnectClick(item, on_click, on_click_data, window_id, key, e.checked);
        RegisterMenuClick(key, on_click, on_click_data, window_id);
        if (!e.enabled)
          gtk_widget_set_sensitive(item, FALSE);
        if (!e.tooltip.empty())
          gtk_widget_set_tooltip_text(item, e.tooltip.c_str());
        ShowAccelerator(item, e, accel_group);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        break;
      }
    }
  }
  return menu;
}

struct ContextMenuState {
  uint64_t session = 0;
  GtkWidget* menu = nullptr;  // weak: nulled when the menu is finalized
  bool ended = false;
  ~ContextMenuState() {
    if (menu)
      g_object_remove_weak_pointer(G_OBJECT(menu),
                                   reinterpret_cast<gpointer*>(&menu));
  }
};
using ContextMenuStatePtr = std::shared_ptr<ContextMenuState>;

// GTK thread only.
void FinishContextMenu(const ContextMenuStatePtr& state) {
  if (state->ended)
    return;
  state->ended = true;
  // After the item's "activate": GTK deactivates the menu shell first, then
  // activates the item, so the close callback waits for an idle.
  g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE,
      [](gpointer data) -> gboolean {
        auto* s = static_cast<ContextMenuStatePtr*>(data);
        EndContextMenu((*s)->session);
        if (GtkWidget* menu = (*s)->menu)
          gtk_widget_destroy(menu);
        return G_SOURCE_REMOVE;
      },
      new ContextMenuStatePtr(state),
      [](gpointer data) { delete static_cast<ContextMenuStatePtr*>(data); });
}

}  // namespace

GtkWidget* BuildGtkMenuFromEntries(const std::vector<MenuEntry>& entries,
                                   uint32_t window_id,
                                   laufey_menu_click_fn on_click,
                                   void* on_click_data, bool is_menu_bar,
                                   GtkAccelGroup* accel_group) {
  return Build(entries, window_id, on_click, on_click_data, is_menu_bar,
               accel_group);
}

GtkWidget* BuildGtkMenuFromValue(laufey_value_t* val,
                                 const laufey_backend_api_t* api,
                                 uint32_t window_id,
                                 laufey_menu_click_fn on_click,
                                 void* on_click_data, bool is_menu_bar) {
  if (!val || !api->value_is_list(val))
    return nullptr;
  return Build(ParseMenuTemplate(val, api, false), window_id, on_click,
               on_click_data, is_menu_bar, nullptr);
}

void ShowGtkContextMenu(GtkWidget* relative_to, int x, int y,
                        const std::vector<MenuEntry>& entries,
                        uint32_t window_id, laufey_menu_click_fn on_click,
                        void* on_click_data, laufey_menu_closed_fn on_closed,
                        void* on_closed_data) {
  GtkWidget* toplevel =
      relative_to ? gtk_widget_get_toplevel(relative_to) : nullptr;
  GdkWindow* gdk_window = toplevel && gtk_widget_get_realized(toplevel)
                              ? gtk_widget_get_window(toplevel)
                              : nullptr;
  if (!gdk_window || entries.empty()) {
    FireContextMenuClosedNow(window_id, on_closed, on_closed_data);
    return;
  }
  GtkWidget* menu =
      Build(entries, window_id, on_click, on_click_data, false, nullptr);
  gtk_menu_attach_to_widget(GTK_MENU(menu), toplevel, nullptr);
  gtk_widget_show_all(menu);

  auto state = std::make_shared<ContextMenuState>();
  state->menu = menu;
  g_object_add_weak_pointer(G_OBJECT(menu),
                            reinterpret_cast<gpointer*>(&state->menu));
  state->session =
      BeginContextMenu(window_id, on_closed, on_closed_data, [state] {
        GtkRunAsync([state] {
          if (!state->ended && state->menu)
            gtk_menu_shell_cancel(GTK_MENU_SHELL(state->menu));
        });
      });
  g_signal_connect_data(
      menu, "deactivate", G_CALLBACK(+[](GtkMenuShell*, gpointer data) {
        FinishContextMenu(*static_cast<ContextMenuStatePtr*>(data));
      }),
      new ContextMenuStatePtr(state),
      [](gpointer data, GClosure*) {
        delete static_cast<ContextMenuStatePtr*>(data);
      },
      (GConnectFlags)0);

  // The point is in the coordinates of `relative_to` (the web view); place
  // the menu there rather than at the pointer (show_context_menu may come
  // from a keyboard shortcut or a timer, with no pointer event to anchor to).
  int tx = x, ty = y;
  if (relative_to != toplevel)
    gtk_widget_translate_coordinates(relative_to, toplevel, x, y, &tx, &ty);
  GdkRectangle rect = {tx, ty, 1, 1};
  gtk_menu_popup_at_rect(GTK_MENU(menu), gdk_window, &rect,
                         GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST,
                         nullptr);
  // A popup GTK refused (no grab could be taken) never deactivates.
  if (!gtk_widget_get_visible(menu))
    FinishContextMenu(state);
}

bool TestTriggerMenuAcceleratorGtk(GtkWidget* window, const char* accelerator) {
  if (!window || !accelerator)
    return false;
  Accelerator accel;
  if (!ParseAccelerator(accelerator, false, &accel, nullptr))
    return false;
  guint key = AcceleratorKeysym(accel);
  if (!key)
    return false;
  // What GtkWindow's key-press handler runs first for a key press.
  return gtk_accel_groups_activate(G_OBJECT(window), key, GdkModsFor(accel));
}

}  // namespace laufey_common
