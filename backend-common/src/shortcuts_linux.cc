// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Global shortcuts on Linux (API 40), on laufey's own thread with its own
// GLib main context, so neither the WebKitGTK nor the CEF UI thread is
// involved until a press is delivered (through GtkRunAsync).
//
// X11: passive key grabs on the root window over a private XCB connection
// (XGrabKey's protocol request; XCB reports a refused grab as a BadAccess
// error on that request, where Xlib would need a process-wide error handler).
// Each combination is grabbed with and without Caps Lock and Num Lock so the
// lock state doesn't matter. A grab another client holds -> CONFLICT.
//
// Wayland: no client may grab keys, so the XDG GlobalShortcuts portal
// (org.freedesktop.portal.GlobalShortcuts) is used: one portal session per
// shortcut, BindShortcuts with the accelerator as the preferred trigger. The
// desktop shows its own dialog, where the user may approve, decline (DENIED)
// or pick another trigger. Without the portal (or a portal backend that
// implements it) shortcuts are NOT_SUPPORTED. LAUFEY_GLOBAL_SHORTCUTS=x11 |
// portal | off overrides the choice.

#include <gio/gio.h>
#include <glib-unix.h>
#include <xcb/xcb.h>
#define XK_MISCELLANY
#define XK_LATIN1
#define XK_XKB_KEYS
#include <X11/XF86keysym.h>
#include <X11/keysymdef.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_launch_config.h"
#include "laufey_menu.h"
#include "laufey_system.h"

namespace laufey_common {

namespace {

constexpr const char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr const char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr const char kShortcutsIface[] =
    "org.freedesktop.portal.GlobalShortcuts";

enum class Mode { kNone, kX11, kPortal };

Mode ChooseMode() {
  std::string forced = GetEnvUtf8("LAUFEY_GLOBAL_SHORTCUTS");
  if (forced == "x11")
    return Mode::kX11;
  if (forced == "portal")
    return Mode::kPortal;
  if (forced == "off")
    return Mode::kNone;
  if (!GetEnvUtf8("WAYLAND_DISPLAY").empty() ||
      GetEnvUtf8("XDG_SESSION_TYPE") == "wayland")
    return Mode::kPortal;
  if (!GetEnvUtf8("DISPLAY").empty())
    return Mode::kX11;
  return Mode::kNone;
}

}  // namespace

// The keysym for a key (X11 keysymdef names), or 0. Shared with the menu
// accelerators (laufey_menu.h).
uint32_t AcceleratorKeysym(const Accelerator& a) {
  switch (a.kind) {
    case KeyKind::kLetter:
      return XK_a + (a.ch - 'A');
    case KeyKind::kDigit:
      return XK_0 + (a.ch - '0');
    case KeyKind::kPunct:
      switch (a.ch) {
        case '-': return XK_minus;
        case '=': return XK_equal;
        case '[': return XK_bracketleft;
        case ']': return XK_bracketright;
        case '\\': return XK_backslash;
        case ';': return XK_semicolon;
        case '\'': return XK_apostrophe;
        case ',': return XK_comma;
        case '.': return XK_period;
        case '/': return XK_slash;
        case '`': return XK_grave;
      }
      return 0;
    case KeyKind::kFunction:
      return XK_F1 + (a.number - 1);
    case KeyKind::kNumpad:
      switch (a.named) {
        case NamedKey::kNone: return XK_KP_0 + a.number;
        case NamedKey::kNumDecimal: return XK_KP_Decimal;
        case NamedKey::kNumAdd: return XK_KP_Add;
        case NamedKey::kNumSubtract: return XK_KP_Subtract;
        case NamedKey::kNumMultiply: return XK_KP_Multiply;
        case NamedKey::kNumDivide: return XK_KP_Divide;
        default: return 0;
      }
    case KeyKind::kNamed:
      switch (a.named) {
        case NamedKey::kSpace: return XK_space;
        case NamedKey::kTab: return XK_Tab;
        case NamedKey::kBackspace: return XK_BackSpace;
        case NamedKey::kDelete: return XK_Delete;
        case NamedKey::kInsert: return XK_Insert;
        case NamedKey::kEnter: return XK_Return;
        case NamedKey::kEscape: return XK_Escape;
        case NamedKey::kUp: return XK_Up;
        case NamedKey::kDown: return XK_Down;
        case NamedKey::kLeft: return XK_Left;
        case NamedKey::kRight: return XK_Right;
        case NamedKey::kHome: return XK_Home;
        case NamedKey::kEnd: return XK_End;
        case NamedKey::kPageUp: return XK_Page_Up;
        case NamedKey::kPageDown: return XK_Page_Down;
        case NamedKey::kPrintScreen: return XK_Print;
        case NamedKey::kMediaPlayPause: return XF86XK_AudioPlay;
        case NamedKey::kMediaNextTrack: return XF86XK_AudioNext;
        case NamedKey::kMediaPreviousTrack: return XF86XK_AudioPrev;
        case NamedKey::kMediaStop: return XF86XK_AudioStop;
        case NamedKey::kVolumeUp: return XF86XK_AudioRaiseVolume;
        case NamedKey::kVolumeDown: return XF86XK_AudioLowerVolume;
        case NamedKey::kVolumeMute: return XF86XK_AudioMute;
        default: return 0;
      }
  }
  return 0;
}

namespace {

// The XDG shortcuts specification's name for a key: the keysym name
// (lower-case letters, "F5", "Page_Up", "KP_5", ...).
std::string PortalKeyName(const Accelerator& a) {
  switch (a.kind) {
    case KeyKind::kLetter:
      return std::string(1, static_cast<char>(a.ch - 'A' + 'a'));
    case KeyKind::kDigit:
      return std::string(1, a.ch);
    case KeyKind::kFunction:
      return "F" + std::to_string(a.number);
    default:
      break;
  }
  uint32_t sym = AcceleratorKeysym(a);
  // Not every keysym name is in a header we can call into without Xlib;
  // spell out the ones a shortcut can use.
  switch (sym) {
    case XK_minus: return "minus";
    case XK_equal: return "equal";
    case XK_bracketleft: return "bracketleft";
    case XK_bracketright: return "bracketright";
    case XK_backslash: return "backslash";
    case XK_semicolon: return "semicolon";
    case XK_apostrophe: return "apostrophe";
    case XK_comma: return "comma";
    case XK_period: return "period";
    case XK_slash: return "slash";
    case XK_grave: return "grave";
    case XK_KP_Decimal: return "KP_Decimal";
    case XK_KP_Add: return "KP_Add";
    case XK_KP_Subtract: return "KP_Subtract";
    case XK_KP_Multiply: return "KP_Multiply";
    case XK_KP_Divide: return "KP_Divide";
    case XK_space: return "space";
    case XK_Tab: return "Tab";
    case XK_BackSpace: return "BackSpace";
    case XK_Delete: return "Delete";
    case XK_Insert: return "Insert";
    case XK_Return: return "Return";
    case XK_Escape: return "Escape";
    case XK_Up: return "Up";
    case XK_Down: return "Down";
    case XK_Left: return "Left";
    case XK_Right: return "Right";
    case XK_Home: return "Home";
    case XK_End: return "End";
    case XK_Page_Up: return "Page_Up";
    case XK_Page_Down: return "Page_Down";
    case XK_Print: return "Print";
    case XF86XK_AudioPlay: return "XF86AudioPlay";
    case XF86XK_AudioNext: return "XF86AudioNext";
    case XF86XK_AudioPrev: return "XF86AudioPrev";
    case XF86XK_AudioStop: return "XF86AudioStop";
    case XF86XK_AudioRaiseVolume: return "XF86AudioRaiseVolume";
    case XF86XK_AudioLowerVolume: return "XF86AudioLowerVolume";
    case XF86XK_AudioMute: return "XF86AudioMute";
  }
  if (sym >= XK_KP_0 && sym <= XK_KP_9)
    return "KP_" + std::to_string(sym - XK_KP_0);
  return "";
}

constexpr uint16_t kRelevantMods = XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL |
                                   XCB_MOD_MASK_1 | XCB_MOD_MASK_4;

uint16_t X11Modifiers(uint32_t mods) {
  uint16_t m = 0;
  if (mods & kModCtrl)
    m |= XCB_MOD_MASK_CONTROL;
  if (mods & kModAlt)
    m |= XCB_MOD_MASK_1;
  if (mods & kModShift)
    m |= XCB_MOD_MASK_SHIFT;
  if (mods & kModSuper)
    m |= XCB_MOD_MASK_4;
  return m;
}

class LinuxShortcuts : public ShortcutPlatform {
 public:
  LinuxShortcuts() : mode_(ChooseMode()) {}

  uint32_t Capabilities() override {
    if (mode_ == Mode::kNone)
      return 0;
    Start();
    std::unique_lock<std::mutex> lock(mutex_);
    // The worker answers at once for X11; the portal probe is one D-Bus
    // round trip (bounded by its own timeout).
    cv_.wait_for(lock, std::chrono::seconds(5), [this] { return probed_; });
    return caps_;
  }

  void Bind(uint32_t sid, const Accelerator& accel,
            std::function<void(int)> done) override {
    if (!(Capabilities() & LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS)) {
      done(LAUFEY_SHORTCUT_NOT_SUPPORTED);
      return;
    }
    Post([this, sid, accel, done = std::move(done)]() mutable {
      if (mode_ == Mode::kX11)
        X11Bind(sid, accel, std::move(done));
      else
        PortalBind(sid, accel, std::move(done));
    });
  }

  void Unbind(uint32_t sid) override {
    Post([this, sid] {
      if (mode_ == Mode::kX11)
        X11Unbind(sid);
      else
        PortalUnbind(sid);
    });
  }

 private:
  // --- The worker thread ----------------------------------------------------

  void Start() {
    std::call_once(started_, [this] {
      ctx_ = g_main_context_new();
      loop_ = g_main_loop_new(ctx_, FALSE);
      std::thread([this] {
        g_main_context_push_thread_default(ctx_);
        Probe();
        g_main_loop_run(loop_);
      }).detach();
    });
  }

  void Post(std::function<void()> fn) {
    Start();
    auto* task = new std::function<void()>(std::move(fn));
    g_main_context_invoke_full(
        ctx_, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          (*static_cast<std::function<void()>*>(data))();
          return G_SOURCE_REMOVE;
        },
        task,
        [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
  }

  void Probe() {
    uint32_t caps = 0;
    if (mode_ == Mode::kX11) {
      if (X11Connect())
        caps = LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS;
    } else if (mode_ == Mode::kPortal) {
      if (PortalConnect())
        caps = LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS |
               LAUFEY_SYSTEM_CAP_SHORTCUTS_USER_BINDS;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    caps_ = caps;
    probed_ = true;
    cv_.notify_all();
  }

  static void Deliver(uint32_t sid) {
    GtkRunAsync([sid] { DispatchShortcut(sid); });
  }

  // --- X11 ------------------------------------------------------------------

  bool X11Connect() {
    int screen_num = 0;
    conn_ = xcb_connect(nullptr, &screen_num);
    if (!conn_ || xcb_connection_has_error(conn_)) {
      if (conn_)
        xcb_disconnect(conn_);
      conn_ = nullptr;
      return false;
    }
    const xcb_setup_t* setup = xcb_get_setup(conn_);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num && it.rem; i++)
      xcb_screen_next(&it);
    if (!it.rem) {
      xcb_disconnect(conn_);
      conn_ = nullptr;
      return false;
    }
    root_ = it.data->root;
    LoadKeyboard();
    GSource* src = g_unix_fd_source_new(xcb_get_file_descriptor(conn_), G_IO_IN);
    g_source_set_callback(
        src,
        reinterpret_cast<GSourceFunc>(+[](gint, GIOCondition, gpointer self) {
          static_cast<LinuxShortcuts*>(self)->X11Drain();
          return G_SOURCE_CONTINUE;
        }),
        this, nullptr);
    g_source_attach(src, ctx_);
    g_source_unref(src);
    return true;
  }

  // Reads the keycode -> keysym table and which modifier Num Lock is.
  void LoadKeyboard() {
    const xcb_setup_t* setup = xcb_get_setup(conn_);
    min_keycode_ = setup->min_keycode;
    uint8_t count = static_cast<uint8_t>(setup->max_keycode - min_keycode_ + 1);
    keysyms_.clear();
    per_keycode_ = 0;
    xcb_get_keyboard_mapping_reply_t* km = xcb_get_keyboard_mapping_reply(
        conn_, xcb_get_keyboard_mapping(conn_, min_keycode_, count), nullptr);
    if (km) {
      const xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(km);
      int n = xcb_get_keyboard_mapping_keysyms_length(km);
      keysyms_.assign(syms, syms + n);
      per_keycode_ = km->keysyms_per_keycode;
      free(km);
    }
    num_lock_ = 0;
    xcb_get_modifier_mapping_reply_t* mm = xcb_get_modifier_mapping_reply(
        conn_, xcb_get_modifier_mapping(conn_), nullptr);
    if (mm) {
      const xcb_keycode_t* codes = xcb_get_modifier_mapping_keycodes(mm);
      int per = mm->keycodes_per_modifier;
      for (int mod = 0; mod < 8; mod++) {
        for (int k = 0; k < per; k++) {
          xcb_keycode_t kc = codes[mod * per + k];
          if (kc && KeysymAt(kc, 0) == XK_Num_Lock)
            num_lock_ = static_cast<uint16_t>(1u << mod);
        }
      }
      free(mm);
    }
  }

  uint32_t KeysymAt(xcb_keycode_t kc, int col) const {
    if (kc < min_keycode_ || col >= per_keycode_)
      return 0;
    size_t i = static_cast<size_t>(kc - min_keycode_) * per_keycode_ + col;
    return i < keysyms_.size() ? keysyms_[i] : 0;
  }

  // The first keycode that produces `sym` unshifted or shifted (group 1).
  xcb_keycode_t KeycodeFor(uint32_t sym) const {
    if (!per_keycode_)
      return 0;
    size_t count = keysyms_.size() / per_keycode_;
    for (size_t i = 0; i < count; i++) {
      xcb_keycode_t kc = static_cast<xcb_keycode_t>(min_keycode_ + i);
      for (int col = 0; col < 2 && col < per_keycode_; col++) {
        uint32_t s = KeysymAt(kc, col);
        // Letters map to the lower-case keysym in column 0 and the upper-case
        // one in column 1; compare case-insensitively for Latin letters.
        if (s == sym || (sym >= XK_a && sym <= XK_z && s == sym - 0x20))
          return kc;
      }
    }
    return 0;
  }

  std::vector<uint16_t> LockVariants() const {
    std::vector<uint16_t> v;
    v.push_back(0);
    v.push_back(XCB_MOD_MASK_LOCK);
    if (num_lock_) {
      v.push_back(num_lock_);
      v.push_back(static_cast<uint16_t>(num_lock_ | XCB_MOD_MASK_LOCK));
    }
    return v;
  }

  void X11Bind(uint32_t sid, const Accelerator& a,
               std::function<void(int)> done) {
    uint32_t sym = AcceleratorKeysym(a);
    xcb_keycode_t kc = sym ? KeycodeFor(sym) : 0;
    if (!kc) {
      done(LAUFEY_SHORTCUT_INVALID);
      return;
    }
    uint16_t mods = X11Modifiers(a.mods);
    std::vector<xcb_void_cookie_t> cookies;
    for (uint16_t lock : LockVariants()) {
      cookies.push_back(xcb_grab_key_checked(
          conn_, 1, root_, static_cast<uint16_t>(mods | lock), kc,
          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
    }
    int status = LAUFEY_SHORTCUT_OK;
    for (auto cookie : cookies) {
      xcb_generic_error_t* err = xcb_request_check(conn_, cookie);
      if (err) {
        // BadAccess (10): another client holds the grab.
        if (status == LAUFEY_SHORTCUT_OK)
          status = err->error_code == 10 ? LAUFEY_SHORTCUT_CONFLICT
                                         : LAUFEY_SHORTCUT_FAILED;
        free(err);
      }
    }
    if (status != LAUFEY_SHORTCUT_OK) {
      // Release the variants that did go through.
      for (uint16_t lock : LockVariants())
        xcb_ungrab_key(conn_, kc, root_, static_cast<uint16_t>(mods | lock));
      xcb_flush(conn_);
    } else {
      grabs_[sid] = Grab{kc, mods, 0};
    }
    X11Drain();
    done(status);
  }

  void X11Unbind(uint32_t sid) {
    auto it = grabs_.find(sid);
    if (it == grabs_.end())
      return;
    for (uint16_t lock : LockVariants()) {
      xcb_ungrab_key(conn_, it->second.keycode, root_,
                     static_cast<uint16_t>(it->second.mods | lock));
    }
    xcb_flush(conn_);
    grabs_.erase(it);
  }

  void X11Drain() {
    if (!conn_)
      return;
    while (xcb_generic_event_t* ev = xcb_poll_for_event(conn_)) {
      uint8_t type = ev->response_type & 0x7f;
      if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
        auto* k = reinterpret_cast<xcb_key_press_event_t*>(ev);
        uint16_t mods = k->state & kRelevantMods;
        for (auto& [sid, g] : grabs_) {
          if (g.keycode != k->detail || g.mods != mods)
            continue;
          if (type == XCB_KEY_RELEASE) {
            g.last_release = k->time;
          } else if (k->time != g.last_release) {
            // An autorepeat press shares its timestamp with the synthetic
            // release just before it; only a real press gets through.
            Deliver(sid);
          }
          break;
        }
      } else if (type == XCB_MAPPING_NOTIFY) {
        LoadKeyboard();
      }
      free(ev);
    }
  }

  // --- The XDG GlobalShortcuts portal -------------------------------------------

  bool PortalConnect() {
    GError* error = nullptr;
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus_) {
      g_clear_error(&error);
      return false;
    }
    // A host (unsandboxed) app tells the portal who it is, where the portal
    // supports that (xdg-desktop-portal 1.19+); older portals take the app id
    // from the process's systemd scope or .desktop file.
    std::string app_id = LaunchAppId();
    if (!app_id.empty()) {
      GVariant* r = g_dbus_connection_call_sync(
          bus_, kPortalName, kPortalPath,
          "org.freedesktop.host.portal.Registry", "Register",
          g_variant_new("(s@a{sv})", app_id.c_str(),
                        g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
          nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
      if (r)
        g_variant_unref(r);
    }
    GVariant* v = g_dbus_connection_call_sync(
        bus_, kPortalName, kPortalPath, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", kShortcutsIface, "version"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    if (!v) {
      g_clear_error(&error);
      return false;
    }
    g_variant_unref(v);
    std::string sender = g_dbus_connection_get_unique_name(bus_);
    if (!sender.empty() && sender[0] == ':')
      sender = sender.substr(1);
    for (auto& c : sender) {
      if (c == '.')
        c = '_';
    }
    sender_path_ = sender;
    g_dbus_connection_signal_subscribe(
        bus_, kPortalName, kShortcutsIface, "Activated", kPortalPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, const gchar*,
            const gchar*, GVariant* params, gpointer self) {
          static_cast<LinuxShortcuts*>(self)->OnActivated(params);
        },
        this, nullptr);
    return true;
  }

  // A pending portal request: Response arrives on its own object path.
  struct Request {
    LinuxShortcuts* self;
    guint subscription;
    std::function<void(guint32, GVariant*)> on_response;
  };

  // Calls `method` with `args_before_options` (a tuple, or null) followed by
  // `options`, to which a fresh handle_token is added. Subscribes to the
  // request's Response first, so it can't be missed. `on_response` gets the
  // response code (0 success, 1 cancelled by the user, 2 other) and results.
  void PortalRequest(const char* method, GVariant* args_before_options,
                     GVariantBuilder* options,
                     std::function<void(guint32, GVariant*)> on_response) {
    std::string token = "laufey" + std::to_string(++token_counter_);
    std::string path = std::string(kPortalPath) + "/request/" + sender_path_ +
                       "/" + token;
    g_variant_builder_add(options, "{sv}", "handle_token",
                          g_variant_new_string(token.c_str()));
    auto* req = new Request{this, 0, std::move(on_response)};
    req->subscription = g_dbus_connection_signal_subscribe(
        bus_, kPortalName, "org.freedesktop.portal.Request", "Response",
        path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection* conn, const gchar*, const gchar*, const gchar*,
            const gchar*, GVariant* params, gpointer data) {
          auto* r = static_cast<Request*>(data);
          if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(ua{sv})")))
            return;
          guint32 code = 2;
          GVariant* results = nullptr;
          g_variant_get(params, "(u@a{sv})", &code, &results);
          g_dbus_connection_signal_unsubscribe(conn, r->subscription);
          r->on_response(code, results);
          if (results)
            g_variant_unref(results);
          delete r;
        },
        req, nullptr);
    // Assemble (args..., options).
    GVariantBuilder tuple;
    g_variant_builder_init(&tuple, G_VARIANT_TYPE_TUPLE);
    if (args_before_options) {
      GVariant* args = g_variant_ref_sink(args_before_options);
      GVariantIter iter;
      g_variant_iter_init(&iter, args);
      while (GVariant* child = g_variant_iter_next_value(&iter)) {
        g_variant_builder_add_value(&tuple, child);
        g_variant_unref(child);
      }
      g_variant_unref(args);
    }
    g_variant_builder_add_value(&tuple, g_variant_builder_end(options));
    struct CallCtx {
      Request* req;
      LinuxShortcuts* self;
    };
    auto* call = new CallCtx{req, this};
    g_dbus_connection_call(
        bus_, kPortalName, kPortalPath, kShortcutsIface, method,
        g_variant_builder_end(&tuple), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
          auto* c = static_cast<CallCtx*>(data);
          GError* error = nullptr;
          GVariant* reply = g_dbus_connection_call_finish(
              G_DBUS_CONNECTION(source), res, &error);
          if (reply) {
            g_variant_unref(reply);
          } else {
            // The call itself failed: no Response will come.
            g_clear_error(&error);
            g_dbus_connection_signal_unsubscribe(G_DBUS_CONNECTION(source),
                                                 c->req->subscription);
            c->req->on_response(2, nullptr);
            delete c->req;
          }
          delete c;
        },
        call);
  }

  void PortalBind(uint32_t sid, const Accelerator& a,
                  std::function<void(int)> done) {
    std::string canonical = CanonicalAccelerator(a);
    std::string trigger = PortalTriggerFor(a);
    auto shared_done =
        std::make_shared<std::function<void(int)>>(std::move(done));
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    std::string session_token =
        "laufey_s" + std::to_string(++token_counter_);
    g_variant_builder_add(&options, "{sv}", "session_handle_token",
                          g_variant_new_string(session_token.c_str()));
    PortalRequest(
        "CreateSession", nullptr, &options,
        [this, sid, canonical, trigger, shared_done](guint32 code,
                                                     GVariant* results) {
          const gchar* handle = nullptr;
          if (code != 0 || !results ||
              !g_variant_lookup(results, "session_handle", "&s", &handle) ||
              !handle) {
            (*shared_done)(code == 1 ? LAUFEY_SHORTCUT_DENIED
                                     : LAUFEY_SHORTCUT_FAILED);
            return;
          }
          std::string session = handle;
          GVariantBuilder props;
          g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
          g_variant_builder_add(&props, "{sv}", "description",
                                g_variant_new_string(canonical.c_str()));
          if (!trigger.empty()) {
            g_variant_builder_add(&props, "{sv}", "preferred_trigger",
                                  g_variant_new_string(trigger.c_str()));
          }
          GVariantBuilder list;
          g_variant_builder_init(&list, G_VARIANT_TYPE("a(sa{sv})"));
          g_variant_builder_add(&list, "(s@a{sv})", canonical.c_str(),
                                g_variant_builder_end(&props));
          GVariant* args =
              g_variant_new("(o@a(sa{sv})s)", session.c_str(),
                            g_variant_builder_end(&list), "");
          GVariantBuilder bind_options;
          g_variant_builder_init(&bind_options, G_VARIANT_TYPE_VARDICT);
          PortalRequest(
              "BindShortcuts", args, &bind_options,
              [this, sid, canonical, session, shared_done](guint32 code,
                                                           GVariant* results) {
                bool bound = false;
                GVariant* list = nullptr;
                if (code == 0 && results &&
                    g_variant_lookup(results, "shortcuts", "@a(sa{sv})",
                                     &list)) {
                  GVariantIter iter;
                  g_variant_iter_init(&iter, list);
                  const gchar* id = nullptr;
                  GVariant* props = nullptr;
                  while (g_variant_iter_next(&iter, "(&s@a{sv})", &id,
                                             &props)) {
                    if (id && canonical == id)
                      bound = true;
                    g_variant_unref(props);
                  }
                  g_variant_unref(list);
                }
                if (bound) {
                  sessions_[sid] = session;
                  (*shared_done)(LAUFEY_SHORTCUT_OK);
                  return;
                }
                CloseSession(session);
                (*shared_done)(code == 2 ? LAUFEY_SHORTCUT_FAILED
                                         : LAUFEY_SHORTCUT_DENIED);
              });
        });
  }

  void CloseSession(const std::string& session) {
    g_dbus_connection_call(bus_, kPortalName, session.c_str(),
                           "org.freedesktop.portal.Session", "Close", nullptr,
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           nullptr, nullptr);
  }

  void PortalUnbind(uint32_t sid) {
    auto it = sessions_.find(sid);
    if (it == sessions_.end())
      return;
    CloseSession(it->second);
    sessions_.erase(it);
  }

  void OnActivated(GVariant* params) {
    const gchar* session = nullptr;
    const gchar* id = nullptr;
    guint64 timestamp = 0;
    GVariant* options = nullptr;
    g_variant_get(params, "(&o&st@a{sv})", &session, &id, &timestamp,
                  &options);
    if (options)
      g_variant_unref(options);
    if (!session)
      return;
    for (const auto& [sid, s] : sessions_) {
      if (s == session) {
        Deliver(sid);
        break;
      }
    }
  }

  struct Grab {
    xcb_keycode_t keycode;
    uint16_t mods;
    xcb_timestamp_t last_release;
  };

  const Mode mode_;
  std::once_flag started_;
  GMainContext* ctx_ = nullptr;
  GMainLoop* loop_ = nullptr;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool probed_ = false;
  uint32_t caps_ = 0;

  // Worker thread only from here on.
  xcb_connection_t* conn_ = nullptr;
  xcb_window_t root_ = 0;
  xcb_keycode_t min_keycode_ = 0;
  int per_keycode_ = 0;
  std::vector<xcb_keysym_t> keysyms_;
  uint16_t num_lock_ = 0;
  std::map<uint32_t, Grab> grabs_;

  GDBusConnection* bus_ = nullptr;
  std::string sender_path_;
  uint64_t token_counter_ = 0;
  std::map<uint32_t, std::string> sessions_;  // id -> portal session handle
};

}  // namespace

std::string PortalTriggerFor(const Accelerator& a) {
  std::string key = PortalKeyName(a);
  if (key.empty())
    return "";
  std::string s;
  if (a.mods & kModCtrl)
    s += "CTRL+";
  if (a.mods & kModAlt)
    s += "ALT+";
  if (a.mods & kModShift)
    s += "SHIFT+";
  if (a.mods & kModSuper)
    s += "LOGO+";
  return s + key;
}

std::unique_ptr<ShortcutPlatform> CreateShortcutPlatformLinux() {
  return std::make_unique<LinuxShortcuts>();
}

}  // namespace laufey_common
