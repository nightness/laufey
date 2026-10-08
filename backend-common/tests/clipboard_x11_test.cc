// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The X11 CLIPBOARD client laufey uses on mutter (clipboard_x11_linux.h),
// against a private Xvfb, with a second X client in the test (a "peer" on
// a connection of its own) on the other end of every transfer:
//   - we own: the peer reads TARGETS, TIMESTAMP, UTF8_STRING, STRING (offered
//     only when the text is Latin-1, and then as Latin-1), and a PNG larger
//     than a chunk through INCR; we read our own PNG back the same way;
//   - the peer owns: we read its UTF-8 text, Latin-1 STRING, an HTML larger
//     than a chunk through INCR, and the formats; change events (XFixes);
//   - hostile peers: an owner that never answers (the read gives up after
//     its timeout), an owner that stalls mid-INCR (the read gives up after
//     a step's timeout), an INCR announced over max_bytes (refused at once),
//     a requestor that starts 10 INCR transfers and never takes a chunk (8
//     run at once, all dropped after their idle timeout, and the next read
//     still works), a read over max_bytes, an owner that keeps writing chunks
//     after we abandoned its transfer (never read into a later conversion),
//     a late refusal of an abandoned conversion (never taken as the next
//     one's answer);
//   - the connection killed mid-read (the read ends at once, the bridge is
//     gone, one reconnect after the backoff, and none after that);
//   - privacy: image reads take PNG, JPEG, BMP or GIF only (a TIFF is never
//     handed to a decoder), and while the session is locked (a mock logind's
//     LockedHint, on a private bus standing in for the system bus) every
//     read answers nothing, writes still work, and reads work again once it
//     is unlocked. Without dbus-daemon the lock cases are skipped and the
//     system bus is pointed nowhere (never the host's logind).
// The process environment is not touched by the code under test. Exits 77
// (skipped) without Xvfb.

#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../src/clipboard_x11_linux.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Finish(1);                                                             \
    }                                                                        \
  } while (0)

namespace {

pid_t g_xvfb = -1;

[[noreturn]] void Finish(int code) {
  if (g_xvfb > 0)
    kill(g_xvfb, SIGTERM);
  std::fflush(stdout);
  // The bridge's thread is detached; skip static teardown.
  std::_Exit(code);
}

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

template <typename F>
bool WaitFor(F cond, int ms = 5000) {
  for (int i = 0; i < ms / 10; i++) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

std::string Take(char* s) {
  std::string out = s ? s : "";
  free(s);
  return out;
}

// A noise image: its PNG is far larger than a 64 KiB chunk.
std::string NoisePng() {
  GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 300, 300);
  guchar* px = gdk_pixbuf_get_pixels(pixbuf);
  int stride = gdk_pixbuf_get_rowstride(pixbuf);
  uint32_t x = 2463534242u;
  for (int y = 0; y < 300; y++) {
    for (int i = 0; i < 300 * 3; i++) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      px[y * stride + i] = static_cast<guchar>(x);
    }
  }
  gchar* buf = nullptr;
  gsize len = 0;
  EXPECT(
      gdk_pixbuf_save_to_buffer(pixbuf, &buf, &len, "png", nullptr, nullptr));
  std::string png(buf, len);
  g_free(buf);
  g_object_unref(pixbuf);
  return png;
}

// --- The peer: a plain X client on a connection of its own ----------------

class Peer {
 public:
  Peer() {
    conn_ = xcb_connect(nullptr, nullptr);
    EXPECT(conn_ && !xcb_connection_has_error(conn_));
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data;
    window_ = xcb_generate_id(conn_);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, screen->root, 0, 0,
                      1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    clipboard_ = Atom("CLIPBOARD");
    xcb_flush(conn_);
  }
  ~Peer() {
    xcb_disconnect(conn_);
  }

  xcb_atom_t Atom(const std::string& name) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(
        conn_,
        xcb_intern_atom(conn_, 0, static_cast<uint16_t>(name.size()),
                        name.c_str()),
        nullptr);
    EXPECT(r);
    xcb_atom_t a = r->atom;
    free(r);
    return a;
  }

  std::string Name(xcb_atom_t a) {
    xcb_get_atom_name_reply_t* r =
        xcb_get_atom_name_reply(conn_, xcb_get_atom_name(conn_, a), nullptr);
    if (!r)
      return "";
    std::string s(xcb_get_atom_name_name(r),
                  static_cast<size_t>(xcb_get_atom_name_name_length(r)));
    free(r);
    return s;
  }

  // The next event `want` accepts, within `ms`; null on timeout.
  xcb_generic_event_t* Wait(std::function<bool(xcb_generic_event_t*)> want,
                            int ms = 5000) {
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (true) {
      while (xcb_generic_event_t* ev = xcb_poll_for_event(conn_)) {
        if (want(ev))
          return ev;
        free(ev);
      }
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - Clock::now())
                      .count();
      if (left <= 0)
        return nullptr;
      pollfd p = {xcb_get_file_descriptor(conn_), POLLIN, 0};
      poll(&p, 1, static_cast<int>(left));
    }
  }

  std::string GetProperty(xcb_atom_t prop, xcb_atom_t* type) {
    std::string out;
    uint32_t offset = 0;
    while (true) {
      xcb_get_property_reply_t* r = xcb_get_property_reply(
          conn_,
          xcb_get_property(conn_, 0, window_, prop, XCB_GET_PROPERTY_TYPE_ANY,
                           offset, 1 << 20),
          nullptr);
      EXPECT(r);
      *type = r->type;
      int len = xcb_get_property_value_length(r);
      out.append(static_cast<const char*>(xcb_get_property_value(r)),
                 static_cast<size_t>(len));
      uint32_t after = r->bytes_after;
      free(r);
      if (after == 0 || len <= 0)
        break;
      offset += static_cast<uint32_t>(len) / 4;
    }
    return out;
  }

  // Converts CLIPBOARD to `target` (ICCCM requestor, INCR included).
  bool Fetch(const std::string& target, std::string* data,
             std::string* type_out, bool* incr_used = nullptr) {
    xcb_atom_t t = Atom(target), prop = Atom("PEER_PROP");
    xcb_delete_property(conn_, window_, prop);
    xcb_convert_selection(conn_, window_, clipboard_, t, prop,
                          XCB_CURRENT_TIME);
    xcb_flush(conn_);
    xcb_generic_event_t* ev = Wait([&](xcb_generic_event_t* e) {
      return (e->response_type & 0x7f) == XCB_SELECTION_NOTIFY;
    });
    if (!ev)
      return false;
    auto* n = reinterpret_cast<xcb_selection_notify_event_t*>(ev);
    bool refused = n->property == XCB_NONE;
    free(ev);
    if (refused)
      return false;
    xcb_atom_t type = XCB_NONE;
    *data = GetProperty(prop, &type);
    xcb_delete_property(conn_, window_, prop);
    xcb_flush(conn_);
    if (incr_used)
      *incr_used = type == Atom("INCR");
    if (type == Atom("INCR")) {
      data->clear();
      while (true) {
        ev = Wait([&](xcb_generic_event_t* e) {
          auto* p = reinterpret_cast<xcb_property_notify_event_t*>(e);
          return (e->response_type & 0x7f) == XCB_PROPERTY_NOTIFY &&
                 p->window == window_ && p->atom == prop &&
                 p->state == XCB_PROPERTY_NEW_VALUE;
        });
        if (!ev)
          return false;
        free(ev);
        std::string chunk = GetProperty(prop, &type);
        xcb_delete_property(conn_, window_, prop);
        xcb_flush(conn_);
        if (chunk.empty())
          break;
        data->append(chunk);
      }
    }
    if (type_out)
      *type_out = Name(type);
    return true;
  }

  std::vector<std::string> Targets() {
    std::string data, type;
    std::vector<std::string> out;
    if (!Fetch("TARGETS", &data, &type) || type != "ATOM")
      return out;
    for (size_t i = 0; i + 4 <= data.size(); i += 4) {
      xcb_atom_t a;
      memcpy(&a, data.data() + i, 4);
      out.push_back(Name(a));
    }
    return out;
  }

  // Starts `n` INCR conversions of `target` into separate properties and
  // takes nothing; true when each was answered with INCR or refused.
  int Hoard(const std::string& target, int n) {
    int incr = 0;
    xcb_atom_t t = Atom(target);
    for (int i = 0; i < n; i++) {
      xcb_atom_t prop = Atom("HOARD_" + std::to_string(i));
      xcb_convert_selection(conn_, window_, clipboard_, t, prop,
                            XCB_CURRENT_TIME);
      xcb_flush(conn_);
      xcb_generic_event_t* ev = Wait([&](xcb_generic_event_t* e) {
        return (e->response_type & 0x7f) == XCB_SELECTION_NOTIFY;
      });
      EXPECT(ev);
      auto* sn = reinterpret_cast<xcb_selection_notify_event_t*>(ev);
      if (sn->property != XCB_NONE)
        incr++;  // INCR announced; the property is never deleted
      free(ev);
    }
    return incr;
  }

  // Kills the client that created `window` (XKillClient).
  void Kill(xcb_window_t window) {
    xcb_kill_client(conn_, window);
    xcb_flush(conn_);
  }

  // The CLIPBOARD selection's owner window.
  xcb_window_t Owner() {
    xcb_get_selection_owner_reply_t* r = xcb_get_selection_owner_reply(
        conn_, xcb_get_selection_owner(conn_, clipboard_), nullptr);
    EXPECT(r);
    xcb_window_t owner = r->owner;
    free(r);
    return owner;
  }

  // The requestor window of the last request served (or ignored).
  xcb_window_t LastRequestor() const {
    return last_requestor_.load();
  }

  // Chunks a flooding owner has written.
  int Flooded() const {
    return flooded_.load();
  }

  // Owns CLIPBOARD with `entries` and serves requests on a thread of its own
  // until destroyed; `silent` answers nothing; an INCR transfer stops after
  // `stall_after` chunks (never, when negative); `flood` sends an INCR
  // transfer's chunks back to back, never waiting for the requestor to take
  // one, until stopped; `late_refusal` leaves the first request unanswered
  // and refuses it just before answering the next one for the same target.
  void Own(std::map<std::string, std::string> entries, bool silent,
           int stall_after = -1, bool flood = false,
           bool late_refusal = false) {
    entries_ = std::move(entries);
    silent_ = silent;
    stall_after_ = stall_after;
    flood_ = flood;
    late_refusal_ = late_refusal;
    for (const auto& [name, data] : entries_)
      atoms_[Atom(name)] = name;
    targets_ = Atom("TARGETS");
    incr_ = Atom("INCR");
    xcb_set_selection_owner(conn_, window_, clipboard_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
    serving_ = true;
    thread_ = std::thread([this] { Serve(); });
  }

  void Stop() {
    if (!serving_)
      return;
    serving_ = false;
    thread_.join();
  }

 private:
  void Serve() {
    while (serving_) {
      xcb_generic_event_t* ev = Wait(
          [](xcb_generic_event_t* e) {
            return (e->response_type & 0x7f) == XCB_SELECTION_REQUEST;
          },
          100);
      if (!ev)
        continue;
      auto req = *reinterpret_cast<xcb_selection_request_event_t*>(ev);
      free(ev);
      last_requestor_ = req.requestor;
      if (late_refusal_ && !held_requests_++) {
        held_ = req;  // unanswered, for now
        continue;
      }
      if (held_ && held_->target == req.target) {
        // The refusal of the request given up on, arriving while the
        // requestor waits for the answer to this one.
        Notify(*held_, XCB_NONE);
        held_.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!silent_)
        Answer(req);
    }
  }

  void Notify(const xcb_selection_request_event_t& r, xcb_atom_t property) {
    xcb_selection_notify_event_t n = {};
    n.response_type = XCB_SELECTION_NOTIFY;
    n.time = r.time;
    n.requestor = r.requestor;
    n.selection = r.selection;
    n.target = r.target;
    n.property = property;
    xcb_send_event(conn_, 0, r.requestor, 0, reinterpret_cast<char*>(&n));
    xcb_flush(conn_);
  }

  void Answer(const xcb_selection_request_event_t& r) {
    if (r.target == targets_) {
      std::vector<xcb_atom_t> list = {targets_};
      for (const auto& [a, name] : atoms_)
        list.push_back(a);
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, r.requestor, r.property,
                          XCB_ATOM_ATOM, 32, static_cast<uint32_t>(list.size()),
                          list.data());
      Notify(r, r.property);
      return;
    }
    auto it = atoms_.find(r.target);
    if (it == atoms_.end()) {
      Notify(r, XCB_NONE);
      return;
    }
    const std::string& data = entries_[it->second];
    constexpr size_t kPeerChunk = 32 * 1024;
    if (data.size() <= kPeerChunk) {
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, r.requestor, r.property,
                          r.target, 8, static_cast<uint32_t>(data.size()),
                          data.data());
      Notify(r, r.property);
      return;
    }
    // INCR, served here to the end (the test peer handles one at a time).
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn_, r.requestor, XCB_CW_EVENT_MASK, &mask);
    uint32_t size = static_cast<uint32_t>(data.size());
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, r.requestor, r.property,
                        incr_, 32, 1, &size);
    Notify(r, r.property);
    if (flood_) {
      // Chunks into the requestor's property, one per round trip, whatever
      // the requestor does with them.
      std::string chunk(kPeerChunk, 'S');
      while (serving_) {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, r.requestor,
                            r.property, r.target, 8,
                            static_cast<uint32_t>(chunk.size()), chunk.data());
        free(xcb_get_input_focus_reply(conn_, xcb_get_input_focus(conn_),
                                       nullptr));
        flooded_++;
      }
      return;
    }
    size_t off = 0;
    for (int chunks = 0;; chunks++) {
      if (stall_after_ >= 0 && chunks == stall_after_)
        return;  // stalls: the rest never comes
      xcb_generic_event_t* ev = Wait([&](xcb_generic_event_t* e) {
        auto* p = reinterpret_cast<xcb_property_notify_event_t*>(e);
        return (e->response_type & 0x7f) == XCB_PROPERTY_NOTIFY &&
               p->window == r.requestor && p->atom == r.property &&
               p->state == XCB_PROPERTY_DELETE;
      });
      if (!ev)
        return;
      free(ev);
      size_t n = std::min(kPeerChunk, data.size() - off);
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, r.requestor, r.property,
                          r.target, 8, static_cast<uint32_t>(n),
                          data.data() + off);
      xcb_flush(conn_);
      off += n;
      if (n == 0)
        return;
    }
  }

  xcb_connection_t* conn_ = nullptr;
  xcb_window_t window_ = 0;
  xcb_atom_t clipboard_ = 0, targets_ = 0, incr_ = 0;
  std::map<std::string, std::string> entries_;
  std::map<xcb_atom_t, std::string> atoms_;
  bool silent_ = false;
  int stall_after_ = -1;
  bool flood_ = false;
  std::atomic<int> flooded_{0};
  bool late_refusal_ = false;
  int held_requests_ = 0;
  std::optional<xcb_selection_request_event_t> held_;
  std::atomic<xcb_window_t> last_requestor_{0};
  std::atomic<bool> serving_{false};
  std::thread thread_;
};

bool Has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v) {
    if (x == s)
      return true;
  }
  return false;
}

std::atomic<int> g_changes{0};
void OnChange(void*) {
  g_changes++;
}

// --- A mock logind on a private bus -----------------------------------------

const char kLogindXml[] =
    "<node>"
    " <interface name='org.freedesktop.login1.Manager'>"
    "  <method name='GetSession'>"
    "   <arg type='s' name='id' direction='in'/>"
    "   <arg type='o' name='path' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.login1.Session'>"
    "  <property name='LockedHint' type='b' access='read'/>"
    " </interface>"
    "</node>";
constexpr char kSessionPath[] = "/org/freedesktop/login1/session/_3test";

std::atomic<bool> g_locked{false};
std::atomic<int> g_lock_queries{0};

void LogindMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                  const gchar* method, GVariant*,
                  GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(method, "GetSession") == 0) {
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(o)", kSessionPath));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

GVariant* LogindProperty(GDBusConnection*, const gchar*, const gchar*,
                         const gchar*, const gchar* property, GError**,
                         gpointer) {
  if (strcmp(property, "LockedHint") == 0) {
    g_lock_queries++;
    return g_variant_new_boolean(g_locked.load());
  }
  return nullptr;
}

const GDBusInterfaceVTable kLogindVTable = {
    LogindMethod, LogindProperty, nullptr, {}};

// Starts the mock and points the system bus at it; false without
// dbus-daemon (the system bus then points nowhere).
bool StartMockLogind() {
  unsetenv("XDG_SESSION_ID");
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/laufey", 1);
    return false;
  }
  g_free(daemon);
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), 1);
  std::atomic<bool> ready{false};
  std::thread([bus, &ready] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GError* error = nullptr;
    GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(bus),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    EXPECT(conn);
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kLogindXml, &error);
    EXPECT(info);
    EXPECT(g_dbus_connection_register_object(
               conn, "/org/freedesktop/login1",
               g_dbus_node_info_lookup_interface(
                   info, "org.freedesktop.login1.Manager"),
               &kLogindVTable, nullptr, nullptr, &error) != 0);
    EXPECT(g_dbus_connection_register_object(
               conn, kSessionPath,
               g_dbus_node_info_lookup_interface(
                   info, "org.freedesktop.login1.Session"),
               &kLogindVTable, nullptr, nullptr, &error) != 0);
    GVariant* r = g_dbus_connection_call_sync(
        conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.login1", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    EXPECT(r);
    g_variant_unref(r);
    ready = true;
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  EXPECT(WaitFor([&] { return ready.load(); }));
  return true;
}

// Starts a private Xvfb and points DISPLAY at it; its pid, or -1.
pid_t StartXvfb() {
  gchar* xvfb = g_find_program_in_path("Xvfb");
  if (!xvfb)
    return -1;
  int fds[2];
  EXPECT(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    std::string fd = std::to_string(fds[1]);
    execl(xvfb, xvfb, "-displayfd", fd.c_str(), "-nolisten", "tcp", "-screen",
          "0", "640x480x24", static_cast<char*>(nullptr));
    _exit(127);
  }
  g_free(xvfb);
  close(fds[1]);
  pollfd p = {fds[0], POLLIN, 0};
  char buf[32] = {};
  if (poll(&p, 1, 10000) != 1 || read(fds[0], buf, sizeof(buf) - 1) <= 0) {
    kill(pid, SIGKILL);
    return -1;
  }
  close(fds[0]);
  std::string display = std::string(":") + strtok(buf, "\n");
  setenv("DISPLAY", display.c_str(), 1);
  return pid;
}

}  // namespace

int main() {
  // First: GTestDBus unsets DISPLAY.
  bool have_logind = StartMockLogind();
  g_xvfb = StartXvfb();
  if (g_xvfb < 0) {
    std::printf("laufey_clipboard_x11_test: Xvfb missing, skipped\n");
    return 77;
  }
  // No data-control: the X11 client is the clipboard.
  unsetenv("WAYLAND_DISPLAY");
  x11_clipboard::EnableForTesting();
  // Change events are delivered "on the GTK thread"; here, inline.
  SetGtkThread([](std::function<void()> fn) { fn(); }, [] { return true; });
  auto start = Clock::now();
  EXPECT(x11_clipboard::Available());
  std::printf("connected in %.2f s\n", SecondsSince(start));
  Peer peer;

  // --- We own -------------------------------------------------------------

  // Text outside Latin-1: UTF8_STRING, no STRING.
  ClipboardWriteTextLinux("h\xc3\xa9llo \xe2\x9c\x93");
  std::vector<std::string> targets = peer.Targets();
  EXPECT(Has(targets, "TARGETS") && Has(targets, "TIMESTAMP"));
  EXPECT(Has(targets, "UTF8_STRING"));
  EXPECT(Has(targets, "text/plain;charset=utf-8"));
  EXPECT(!Has(targets, "STRING"));
  EXPECT(!Has(targets, "TEXT"));
  std::string data, type;
  EXPECT(peer.Fetch("UTF8_STRING", &data, &type));
  EXPECT(data == "h\xc3\xa9llo \xe2\x9c\x93" && type == "UTF8_STRING");
  EXPECT(!peer.Fetch("STRING", &data, &type));  // refused
  EXPECT(!peer.Fetch("MULTIPLE", &data, &type));
  EXPECT(peer.Fetch("TIMESTAMP", &data, &type));
  EXPECT(type == "INTEGER" && data.size() == 4);

  // Latin-1 text: STRING too, as Latin-1.
  ClipboardWriteTextLinux("caf\xc3\xa9");
  EXPECT(Has(peer.Targets(), "STRING"));
  EXPECT(peer.Fetch("STRING", &data, &type));
  EXPECT(data == "caf\xe9" && type == "STRING");
  EXPECT(peer.Fetch("UTF8_STRING", &data, &type));
  EXPECT(data == "caf\xc3\xa9");
  // And our own read of it.
  EXPECT(Take(ClipboardReadTextLinux()) == "caf\xc3\xa9");

  // HTML with its text alternative.
  EXPECT(ClipboardWriteHtmlLinux("<b>bold</b>", "bold"));
  EXPECT(peer.Fetch("text/html", &data, &type));
  EXPECT(data == "<b>bold</b>");
  EXPECT(Take(ClipboardReadHtmlLinux()) == "<b>bold</b>");
  EXPECT(Take(ClipboardReadTextLinux()) == "bold");

  // A PNG larger than a chunk: INCR, to the peer and to ourselves.
  std::string png = NoisePng();
  EXPECT(png.size() > 128 * 1024);
  EXPECT(ClipboardWriteImageLinux(reinterpret_cast<const uint8_t*>(png.data()),
                                  png.size()));
  bool incr = false;
  EXPECT(peer.Fetch("image/png", &data, &type, &incr));
  EXPECT(incr && data == png && type == "image/png");
  size_t len = 0;
  uint8_t* got = ClipboardReadImageLinux(&len);
  EXPECT(got && len == png.size() && memcmp(got, png.data(), len) == 0);
  free(got);
  EXPECT(Take(ClipboardReadFormatsLinux()) == "image/png");

  // Over max_bytes: not delivered.
  {
    bool found = true;
    EXPECT(x11_clipboard::Read({"image/png"}, 1000, &data, nullptr, &found));
    EXPECT(!found);
  }

  // A requestor that starts 10 INCR transfers and never takes a chunk: 8 at
  // once, the rest refused, all dropped after their idle timeout (3 s).
  // (First, the transfer the read above abandoned ends the same way.)
  EXPECT(WaitFor([] { return x11_clipboard::ActiveTransfers() == 0; }, 8000));
  {
    Peer hoarder;
    int announced = hoarder.Hoard("image/png", 10);
    std::printf("hoarder: %d INCR transfers announced, %d in flight\n",
                announced, x11_clipboard::ActiveTransfers());
    EXPECT(announced == 8);
    EXPECT(x11_clipboard::ActiveTransfers() == 8);
    start = Clock::now();
    EXPECT(WaitFor([] { return x11_clipboard::ActiveTransfers() == 0; }, 8000));
    std::printf("hoarder: transfers dropped after %.1f s\n",
                SecondsSince(start));
    // Still serving.
    EXPECT(peer.Fetch("image/png", &data, &type));
    EXPECT(data == png);
  }

  // --- The peer owns ------------------------------------------------------

  SetClipboardChangeHandler(OnChange, nullptr);
  int before = g_changes.load();
  std::string html(300 * 1024, 'h');
  html = "<p>" + html + "</p>";
  {
    Peer owner;
    owner.Own({{"UTF8_STRING", "peer \xe2\x9c\x93"},
               {"text/html", html},
               {"image/png", png}},
              false);
    EXPECT(WaitFor([&] { return g_changes.load() > before; }));
    EXPECT(Take(ClipboardReadTextLinux()) == "peer \xe2\x9c\x93");
    EXPECT(Take(ClipboardReadHtmlLinux()) == html);  // INCR
    std::string formats = Take(ClipboardReadFormatsLinux());
    EXPECT(formats.find("text/plain") != std::string::npos);
    EXPECT(formats.find("text/html") != std::string::npos);
    EXPECT(formats.find("image/png") != std::string::npos);
    got = ClipboardReadImageLinux(&len);
    EXPECT(got && len == png.size() && memcmp(got, png.data(), len) == 0);
    free(got);
    owner.Stop();
  }
  {
    Peer owner;
    owner.Own({{"STRING", "na\xefve"}}, false);  // Latin-1 only
    EXPECT(WaitFor(
        [] { return Take(ClipboardReadTextLinux()) == "na\xc3\xafve"; }));
    owner.Stop();
  }
  SetClipboardChangeHandler(nullptr, nullptr);

  // An owner that never answers: the read gives up after its timeout.
  {
    Peer owner;
    owner.Own({{"UTF8_STRING", "never"}}, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    start = Clock::now();
    EXPECT(Take(ClipboardReadTextLinux()).empty());
    double took = SecondsSince(start);
    std::printf("silent owner: read gave up after %.1f s\n", took);
    EXPECT(took >= 2.5 && took < 5.0);
    owner.Stop();
  }

  // A late refusal of a conversion we gave up on (the owner sat on it past a
  // step's timeout) arrives while the next read waits for its answer to the
  // same target: it is not taken as that answer (each conversion carries a
  // server timestamp of its own, and an earlier one's answer is told apart
  // by it), so the next read gets the data.
  {
    Peer owner;
    owner.Own({{"UTF8_STRING", "after the refusal"}}, false,
              /*stall_after=*/-1, /*flood=*/false, /*late_refusal=*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT(Take(ClipboardReadTextLinux()).empty());  // the abandoned one
    EXPECT(Take(ClipboardReadTextLinux()) == "after the refusal");
    owner.Stop();
  }

  // An owner that stalls mid-INCR: the read gives up after a step's timeout
  // (3 s), not the whole conversion's.
  {
    Peer owner;
    owner.Own({{"text/html", html}}, false, /*stall_after=*/2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    start = Clock::now();
    bool found = true;
    EXPECT(x11_clipboard::Read({"text/html"}, 1 << 20, &data, nullptr, &found));
    double took = SecondsSince(start);
    std::printf("stalled INCR: read gave up after %.1f s\n", took);
    EXPECT(!found);
    EXPECT(took >= 2.5 && took < 6.0);
    owner.Stop();
  }

  // An INCR transfer announced larger than max_bytes: refused at once,
  // before a chunk is taken (this owner never sends one, so waiting for the
  // first would take a step's timeout).
  {
    Peer owner;
    owner.Own({{"text/html", html}}, false, /*stall_after=*/0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    start = Clock::now();
    bool found = true;
    EXPECT(x11_clipboard::Read({"text/html"}, 1000, &data, nullptr, &found));
    EXPECT(!found);
    EXPECT(SecondsSince(start) < 1.0);
    owner.Stop();
  }

  // An owner that keeps writing chunks into the property of a transfer we
  // abandoned (refused here: announced over max_bytes), still at it while
  // another client owns the clipboard and we read from that one: every read
  // gets the new owner's data, never a stale chunk (each conversion has a
  // property of its own, and an abandoned one rests).
  {
    Peer stale;
    stale.Own({{"text/html", html}}, false, /*stall_after=*/-1,
              /*flood=*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool found = true;
    EXPECT(x11_clipboard::Read({"text/html"}, 1000, &data, nullptr, &found));
    EXPECT(!found);
    EXPECT(WaitFor([&] { return stale.Flooded() > 50; }));
    Peer fresh;
    fresh.Own({{"UTF8_STRING", "fresh"}, {"text/html", "<i>fresh</i>"}}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int flooded = stale.Flooded();
    for (int i = 0; i < 10; i++) {
      EXPECT(Take(ClipboardReadTextLinux()) == "fresh");
      EXPECT(Take(ClipboardReadHtmlLinux()) == "<i>fresh</i>");
    }
    std::printf(
        "stale INCR owner: %d chunks written during the reads, none read\n",
        stale.Flooded() - flooded);
    EXPECT(stale.Flooded() > flooded);  // it really was still writing
    fresh.Stop();
    stale.Stop();
  }

  // And we can take the clipboard back.
  ClipboardWriteTextLinux("back");
  EXPECT(Take(ClipboardReadTextLinux()) == "back");
  EXPECT(peer.Fetch("UTF8_STRING", &data, &type) && data == "back");

  // --- Privacy ------------------------------------------------------------

  // Image reads: PNG, JPEG, BMP or GIF only. A TIFF (the bytes never reach
  // a decoder; these aren't even a valid one) is not read at all.
  {
    Peer owner;
    owner.Own({{"image/tiff", std::string("II*\0garbage", 11)}}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    got = ClipboardReadImageLinux(&len);
    EXPECT(!got && len == 0);
    EXPECT(Take(ClipboardReadFormatsLinux()).empty());
    owner.Stop();
  }
  {
    GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 16, 16);
    gdk_pixbuf_fill(pixbuf, 0x3366ccff);
    gchar* buf = nullptr;
    gsize jpeg_len = 0;
    EXPECT(gdk_pixbuf_save_to_buffer(pixbuf, &buf, &jpeg_len, "jpeg", nullptr,
                                     nullptr));
    std::string jpeg(buf, jpeg_len);
    g_free(buf);
    g_object_unref(pixbuf);
    Peer owner;
    owner.Own(
        {{"image/tiff", std::string("II*\0garbage", 11)}, {"image/jpeg", jpeg}},
        false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    got = ClipboardReadImageLinux(&len);
    EXPECT(got && LooksLikePng(got, len));  // the JPEG, re-encoded
    free(got);
    EXPECT(Take(ClipboardReadFormatsLinux()) == "image/png");
    owner.Stop();
  }

  if (have_logind) {
    // Locked: every read answers nothing, at once; writes still work.
    ClipboardWriteTextLinux("secret");
    EXPECT(Take(ClipboardReadTextLinux()) == "secret");
    int queries = g_lock_queries.load();
    EXPECT(queries > 0);  // asked logind
    g_locked = true;
    start = Clock::now();
    EXPECT(Take(ClipboardReadTextLinux()).empty());
    EXPECT(Take(ClipboardReadFormatsLinux()).empty());
    EXPECT(ClipboardWriteHtmlLinux("<i>locked</i>", "locked"));
    EXPECT(Take(ClipboardReadHtmlLinux()).empty());
    got = ClipboardReadImageLinux(&len);
    EXPECT(!got);
    EXPECT(SecondsSince(start) < 1.0);
    // The peer still gets what we offer (only our reads are refused).
    EXPECT(peer.Fetch("text/html", &data, &type) && data == "<i>locked</i>");
    // Unlocked: reads work again.
    g_locked = false;
    EXPECT(Take(ClipboardReadTextLinux()) == "locked");
    EXPECT(Take(ClipboardReadHtmlLinux()) == "<i>locked</i>");
    std::printf("lock: reads refused while locked, back once unlocked\n");
  } else {
    std::printf("lock: dbus-daemon missing, lock cases skipped\n");
  }

  // --- The connection dies --------------------------------------------------

  // Killed mid-read (the owner never answers; the peer kills the requestor's
  // client, which is the bridge): the read ends at once, the bridge is gone,
  // and after the backoff it reconnects, once.
  x11_clipboard::SetReconnectBackoffForTesting(500);
  {
    Peer owner;
    owner.Own({{"UTF8_STRING", "never"}}, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::atomic<bool> read_done{false};
    std::atomic<bool> read_found{true};
    std::thread reader([&] {
      std::string got_data;
      bool found = true;
      x11_clipboard::Read({"UTF8_STRING"}, 1 << 20, &got_data, nullptr, &found);
      read_found = found;
      read_done = true;
    });
    EXPECT(WaitFor([&] { return owner.LastRequestor() != 0; }));
    start = Clock::now();
    owner.Kill(owner.LastRequestor());
    EXPECT(WaitFor([&] { return read_done.load(); }, 2000));
    reader.join();
    std::printf("killed mid-read: the read ended after %.2f s\n",
                SecondsSince(start));
    EXPECT(!read_found.load());
    EXPECT(!x11_clipboard::Available());  // within the backoff
    owner.Stop();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT(x11_clipboard::Available());  // the one reconnect
  EXPECT(x11_clipboard::Write(
      {{"UTF8_STRING", std::make_shared<const std::string>("again")}}));
  {
    bool found = false;
    EXPECT(
        x11_clipboard::Read({"UTF8_STRING"}, 1 << 20, &data, nullptr, &found));
    EXPECT(found && data == "again");
  }
  EXPECT(peer.Fetch("UTF8_STRING", &data, &type) && data == "again");
  // Killed again: no second reconnect.
  peer.Kill(peer.Owner());
  EXPECT(WaitFor([] { return !x11_clipboard::Available(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT(!x11_clipboard::Available());
  std::printf("connection killed: reconnected once, then gone for good\n");

  std::printf("laufey_clipboard_x11_test: ok\n");
  Finish(0);
}
