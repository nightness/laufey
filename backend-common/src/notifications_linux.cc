// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications on Linux (API 41): the freedesktop.org Desktop
// Notifications protocol, org.freedesktop.Notifications on the session bus.
//
// - Notify with the actions (plus "default", the body click), the hints
//   desktop-entry, urgency, suppress-sound and image-data, and the server's
//   id of a notification with the same tag as replaces_id.
// - ActionInvoked / NotificationClosed signals become clicks, actions and
//   closes. The server sends them to the connection that posted the
//   notification, so a click after this process exited can't be delivered
//   (no cold start); the desktop-entry hint lets the shell launch the app.
// - The protocol has no scheduler: laufey's own timer delivers a scheduled
//   notification while the app runs. The schedule is persisted in the app
//   data directory (laufey-notifications.json, when there is one) and
//   re-armed at the next launch, where a notification whose time passed
//   while the app wasn't running is delivered at once. Several instances
//   share the file under a lock, and a due notification is claimed from it
//   before it is posted, so it fires once.
// - Permission is "granted" when a notification server owns (or can be
//   activated for) the name, else "unsupported"; there is no prompt.
//
// Everything runs on one thread of laufey's own, with its own GLib main
// context (the D-Bus connection, the signal subscriptions and the timers),
// so it works the same under the WebKitGTK and CEF backends.

#include "laufey_launch_config.h"
#include "laufey_notifications.h"
#include "laufey_system.h"

#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace laufey_common {
namespace {

const char kName[] = "org.freedesktop.Notifications";
const char kPath[] = "/org/freedesktop/Notifications";
const char kIface[] = "org.freedesktop.Notifications";
constexpr gint kCallTimeoutMs = 5000;
// The longest a single timer waits (GLib timeouts are guint milliseconds);
// a later time re-arms when it fires.
constexpr int64_t kMaxTimerMs = 60LL * 60 * 1000;

std::string ScheduleFilePath() {
  const std::string& dir = AppDataDir();
  if (dir.empty())
    return std::string();
  EnsureDirectory(dir);
  return JoinPath(dir, "laufey-notifications.json");
}

// The schedule file under an exclusive lock (on a sibling ".lock" file).
class ScheduleFile {
 public:
  explicit ScheduleFile(const std::string& path) : path_(path) {
    if (path_.empty())
      return;
    fd_ = open((path_ + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ >= 0)
      flock(fd_, LOCK_EX);
  }
  ~ScheduleFile() {
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      close(fd_);
    }
  }
  bool ok() const {
    return !path_.empty() && fd_ >= 0;
  }

  std::vector<ScheduledNotification> Read() {
    std::vector<ScheduledNotification> list;
    if (!ok())
      return list;
    std::ifstream in(path_, std::ios::binary);
    if (!in)
      return list;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string error;
    if (!ParseSchedule(ss.str(), &list, &error)) {
      std::cerr << "laufey: ignoring the notification schedule " << path_
                << ": " << error << std::endl;
      list.clear();
    }
    return list;
  }

  void Write(const std::vector<ScheduledNotification>& list) {
    if (!ok())
      return;
    std::string tmp = path_ + ".tmp";
    {
      int fd =
          open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd < 0)
        return;
      std::string text = SerializeSchedule(list);
      bool ok = write(fd, text.data(), text.size()) ==
                static_cast<ssize_t>(text.size());
      ok = fsync(fd) == 0 && ok;
      close(fd);
      if (!ok) {
        unlink(tmp.c_str());
        return;
      }
    }
    rename(tmp.c_str(), path_.c_str());
  }

 private:
  std::string path_;
  int fd_ = -1;
};

ScheduledNotification FromOptions(const NotificationOptions& o) {
  ScheduledNotification n;
  n.tag = o.tag;
  n.title = o.title;
  n.body = o.body;
  n.at_ms = o.schedule_at_ms;
  n.has_data = o.has_data;
  n.data = o.data;
  n.actions = o.actions;
  n.silent = o.silent;
  n.require_interaction = o.require_interaction;
  n.icon_png = o.icon_png;
  return n;
}

NotificationOptions ToOptions(const ScheduledNotification& n) {
  NotificationOptions o;
  o.tag = n.tag;
  o.title = n.title;
  o.body = n.body;
  o.has_data = n.has_data;
  o.data = n.data;
  o.actions = n.actions;
  o.silent = n.silent;
  o.require_interaction = n.require_interaction;
  o.icon_png = n.icon_png;
  return o;
}

GVariant* ImageData(const std::vector<uint8_t>& png) {
  if (png.empty())
    return nullptr;
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
  if (!loader)
    return nullptr;
  GVariant* result = nullptr;
  if (gdk_pixbuf_loader_write(loader, png.data(), png.size(), nullptr) &&
      gdk_pixbuf_loader_close(loader, nullptr)) {
    GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pb) {
      int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
      int stride = gdk_pixbuf_get_rowstride(pb);
      int channels = gdk_pixbuf_get_n_channels(pb);
      int bits = gdk_pixbuf_get_bits_per_sample(pb);
      gboolean alpha = gdk_pixbuf_get_has_alpha(pb);
      gsize len = static_cast<gsize>(stride) * (h - 1) +
                  static_cast<gsize>(w) * channels * ((bits + 7) / 8);
      GVariant* bytes = g_variant_new_fixed_array(
          G_VARIANT_TYPE_BYTE, gdk_pixbuf_read_pixels(pb), len, 1);
      result = g_variant_new("(iiibii@ay)", w, h, stride, alpha, bits, channels,
                             bytes);
    }
  } else {
    gdk_pixbuf_loader_close(loader, nullptr);
  }
  g_object_unref(loader);
  return result;
}

class LinuxNotificationPlatform : public NotificationPlatform {
 public:
  LinuxNotificationPlatform() {
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    std::thread([this, &m, &cv, &started] {
      ctx_ = g_main_context_new();
      g_main_context_push_thread_default(ctx_);
      loop_ = g_main_loop_new(ctx_, FALSE);
      {
        std::lock_guard<std::mutex> lock(m);
        started = true;
        cv.notify_one();
      }
      g_main_loop_run(loop_);
    }).detach();
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return started; });
  }

  uint32_t Capabilities() override {
    uint32_t caps = 0;
    RunSync([&] {
      if (!ServerPresent())
        return;
      caps = LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE |
             LAUFEY_NOTIFICATION_CAP_CLICKS;
      if (ServerHasActions())
        caps |= LAUFEY_NOTIFICATION_CAP_ACTIONS;
    });
    return caps;
  }

  bool Show(const NotificationOptions& opts) override {
    bool ok = false;
    RunSync([&] {
      if (opts.schedule_at_ms > 0) {
        ok = ScheduleOnThread(FromOptions(opts));
      } else {
        // A pending one with this tag is replaced by this one.
        Unschedule(opts.tag);
        ok = NotifyOnThread(opts);
      }
    });
    return ok;
  }

  void Remove(const std::string& tag) override {
    RunSync([&] {
      Unschedule(tag);
      auto it = by_tag_.find(tag);
      if (it == by_tag_.end() || !Connection())
        return;
      uint32_t id = it->second;
      Forget(id);
      GVariant* r = g_dbus_connection_call_sync(
          conn_, kName, kPath, kIface, "CloseNotification",
          g_variant_new("(u)", id), nullptr, G_DBUS_CALL_FLAGS_NONE,
          kCallTimeoutMs, nullptr, nullptr);
      if (r)
        g_variant_unref(r);
    });
  }

  void ListScheduled(
      std::function<void(std::vector<ScheduledNotification>)> done) override {
    std::vector<ScheduledNotification> list;
    RunSync([&] {
      ScheduleFile file(ScheduleFilePath());
      if (file.ok()) {
        list = file.Read();
      } else {
        for (const auto& [tag, entry] : scheduled_)
          list.push_back(entry.n);
      }
    });
    done(std::move(list));
  }

  void QueryPermission(int /*kind*/, std::function<void(int)> done) override {
    bool present = false;
    RunSync([&] { present = ServerPresent(); });
    done(present ? LAUFEY_PERMISSION_STATUS_GRANTED
                 : LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }

  // No prompt on Linux: the server shows what it is sent.
  void RequestPermission(int kind, std::function<void(int)> done) override {
    QueryPermission(kind, std::move(done));
  }

  // At launch: re-arm the persisted schedule.
  void Rearm() {
    RunSync([&] {
      ScheduleFile file(ScheduleFilePath());
      if (!file.ok())
        return;
      for (const ScheduledNotification& n : file.Read())
        Arm(n);
    });
  }

 private:
  struct Scheduled {
    ScheduledNotification n;
    GSource* source = nullptr;
  };

  void RunSync(const std::function<void()>& fn) {
    if (g_main_context_is_owner(ctx_)) {
      fn();
      return;
    }
    struct Call {
      const std::function<void()>* fn;
      std::mutex m;
      std::condition_variable cv;
      bool done = false;
    } call;
    call.fn = &fn;
    g_main_context_invoke(
        ctx_,
        [](gpointer data) -> gboolean {
          auto* c = static_cast<Call*>(data);
          (*c->fn)();
          // Notify under the lock: `call` is the waiter's stack frame.
          std::lock_guard<std::mutex> lock(c->m);
          c->done = true;
          c->cv.notify_one();
          return G_SOURCE_REMOVE;
        },
        &call);
    std::unique_lock<std::mutex> lock(call.m);
    call.cv.wait(lock, [&] { return call.done; });
  }

  // The session bus, connected (and subscribed) on first use. Thread only.
  GDBusConnection* Connection() {
    if (conn_ || conn_failed_)
      return conn_;
    GError* error = nullptr;
    conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn_) {
      conn_failed_ = true;
      if (error)
        g_error_free(error);
      return nullptr;
    }
    g_dbus_connection_signal_subscribe(conn_, kName, kIface, "ActionInvoked",
                                       kPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                       OnActionInvoked, this, nullptr);
    g_dbus_connection_signal_subscribe(
        conn_, kName, kIface, "NotificationClosed", kPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, OnNotificationClosed, this, nullptr);
    return conn_;
  }

  bool ServerPresent() {
    if (!Connection())
      return false;
    bool present = false;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", kName),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (r) {
      gboolean owned = FALSE;
      g_variant_get(r, "(b)", &owned);
      g_variant_unref(r);
      present = owned;
    }
    if (present)
      return true;
    // Not running, but D-Bus can start it.
    r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListActivatableNames", nullptr,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (r) {
      GVariantIter* iter = nullptr;
      const char* name = nullptr;
      g_variant_get(r, "(as)", &iter);
      while (g_variant_iter_loop(iter, "&s", &name)) {
        if (g_strcmp0(name, kName) == 0)
          present = true;
      }
      g_variant_iter_free(iter);
      g_variant_unref(r);
    }
    return present;
  }

  bool ServerHasActions() {
    if (!Connection())
      return false;
    bool actions = false;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kName, kPath, kIface, "GetCapabilities", nullptr,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (r) {
      GVariantIter* iter = nullptr;
      const char* cap = nullptr;
      g_variant_get(r, "(as)", &iter);
      while (g_variant_iter_loop(iter, "&s", &cap)) {
        if (g_strcmp0(cap, "actions") == 0)
          actions = true;
      }
      g_variant_iter_free(iter);
      g_variant_unref(r);
    }
    return actions;
  }

  bool NotifyOnThread(const NotificationOptions& o) {
    if (!Connection())
      return false;
    uint32_t replaces = 0;
    auto prev = by_tag_.find(o.tag);
    if (prev != by_tag_.end()) {
      replaces = prev->second;
      Forget(replaces);
    }
    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    // "default" is the body click; servers don't draw it as a button.
    g_variant_builder_add(&actions, "s", "default");
    g_variant_builder_add(&actions, "s", "");
    for (const NotificationAction& a : o.actions) {
      if (a.id == "default")
        continue;  // the protocol's own key
      g_variant_builder_add(&actions, "s", a.id.c_str());
      g_variant_builder_add(&actions, "s", a.title.c_str());
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    std::string app = LaunchAppId();
    if (!app.empty())
      g_variant_builder_add(&hints, "{sv}", "desktop-entry",
                            g_variant_new_string(app.c_str()));
    g_variant_builder_add(&hints, "{sv}", "urgency",
                          g_variant_new_byte(o.require_interaction ? 2 : 1));
    if (o.silent)
      g_variant_builder_add(&hints, "{sv}", "suppress-sound",
                            g_variant_new_boolean(TRUE));
    if (GVariant* image = ImageData(o.icon_png))
      g_variant_builder_add(&hints, "{sv}", "image-data", image);
    std::string app_name = LoginItemName();
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kName, kPath, kIface, "Notify",
        g_variant_new("(susssasa{sv}i)", app_name.c_str(), replaces, "",
                      o.title.c_str(), o.body.c_str(), &actions, &hints,
                      o.require_interaction ? 0 : -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        &error);
    if (!r) {
      std::cerr << "laufey: notification not posted: "
                << (error ? error->message : "no notification server")
                << std::endl;
      if (error)
        g_error_free(error);
      return false;
    }
    uint32_t id = 0;
    g_variant_get(r, "(u)", &id);
    g_variant_unref(r);
    by_id_[id] = o.tag;
    by_tag_[o.tag] = id;
    if (o.has_data)
      data_[o.tag] = o.data;
    else
      data_.erase(o.tag);
    // Bounded: a notification the server never reports closed stays.
    while (by_id_.size() > 512)
      Forget(by_id_.begin()->first);
    std::string tag = o.tag;
    // After Show returned (the core expects SHOWN after the id).
    g_main_context_invoke_full(
        ctx_, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          DispatchNotificationShown(*static_cast<std::string*>(data));
          return G_SOURCE_REMOVE;
        },
        new std::string(tag),
        [](gpointer data) { delete static_cast<std::string*>(data); });
    return true;
  }

  void Forget(uint32_t id) {
    auto it = by_id_.find(id);
    if (it == by_id_.end())
      return;
    auto t = by_tag_.find(it->second);
    if (t != by_tag_.end() && t->second == id)
      by_tag_.erase(t);
    by_id_.erase(it);
  }

  static void OnActionInvoked(GDBusConnection*, const gchar*, const gchar*,
                              const gchar*, const gchar*, GVariant* params,
                              gpointer self_ptr) {
    auto* self = static_cast<LinuxNotificationPlatform*>(self_ptr);
    uint32_t id = 0;
    const char* key = nullptr;
    g_variant_get(params, "(u&s)", &id, &key);
    auto it = self->by_id_.find(id);
    if (it == self->by_id_.end())
      return;  // not ours (the signal goes to every subscriber on some buses)
    std::string tag = it->second;
    auto d = self->data_.find(tag);
    std::string data;
    bool has_data = d != self->data_.end();
    if (has_data)
      data = d->second;
    bool body = g_strcmp0(key, "default") == 0;
    std::string action = key ? key : "";
    DispatchNotificationClick(tag, body ? nullptr : action.c_str(),
                              has_data ? &data : nullptr);
  }

  static void OnNotificationClosed(GDBusConnection*, const gchar*, const gchar*,
                                   const gchar*, const gchar*, GVariant* params,
                                   gpointer self_ptr) {
    auto* self = static_cast<LinuxNotificationPlatform*>(self_ptr);
    uint32_t id = 0, reason = 0;
    g_variant_get(params, "(uu)", &id, &reason);
    auto it = self->by_id_.find(id);
    if (it == self->by_id_.end())
      return;
    std::string tag = it->second;
    self->Forget(id);
    DispatchNotificationClosed(tag);
  }

  // --- The scheduler ---

  bool ScheduleOnThread(const ScheduledNotification& n) {
    if (!ServerPresent())
      return false;
    {
      ScheduleFile file(ScheduleFilePath());
      if (file.ok()) {
        std::vector<ScheduledNotification> list = file.Read();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const ScheduledNotification& e) {
                                    return e.tag == n.tag;
                                  }),
                   list.end());
        list.push_back(n);
        file.Write(list);
      }
    }
    Arm(n);
    return true;
  }

  // Arms the timer for `n`: at its time, or after `retry_ms` (>= 0).
  void Arm(const ScheduledNotification& n, int64_t retry_ms = -1) {
    Disarm(n.tag);
    Scheduled& s = scheduled_[n.tag];
    s.n = n;
    int64_t delay =
        retry_ms >= 0 ? retry_ms : std::max<int64_t>(0, n.at_ms - UnixTimeMs());
    s.source =
        g_timeout_source_new(static_cast<guint>(std::min(delay, kMaxTimerMs)));
    g_source_set_callback(
        s.source,
        [](gpointer data) -> gboolean {
          auto* fired = static_cast<FiredTimer*>(data);
          fired->self->OnTimer(fired->tag);
          return G_SOURCE_REMOVE;
        },
        new FiredTimer{this, n.tag},
        [](gpointer data) { delete static_cast<FiredTimer*>(data); });
    g_source_attach(s.source, ctx_);
  }

  void Disarm(const std::string& tag) {
    auto it = scheduled_.find(tag);
    if (it == scheduled_.end())
      return;
    if (it->second.source) {
      g_source_destroy(it->second.source);
      g_source_unref(it->second.source);
    }
    scheduled_.erase(it);
  }

  void Unschedule(const std::string& tag) {
    Disarm(tag);
    ScheduleFile file(ScheduleFilePath());
    if (!file.ok())
      return;
    std::vector<ScheduledNotification> list = file.Read();
    size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const ScheduledNotification& e) {
                                return e.tag == tag;
                              }),
               list.end());
    if (list.size() != before)
      file.Write(list);
  }

  struct FiredTimer {
    LinuxNotificationPlatform* self;
    std::string tag;
  };

  void OnTimer(const std::string& tag) {
    auto it = scheduled_.find(tag);
    if (it == scheduled_.end())
      return;
    ScheduledNotification n = it->second.n;
    if (n.at_ms > UnixTimeMs() + 50) {
      Arm(n);  // a long wait, in steps
      return;
    }
    if (!ServerPresent()) {
      // No notification server (yet: a session still starting): keep it,
      // and try again in a while.
      Arm(n, 30000);
      return;
    }
    Disarm(tag);
    // Claim it from the file: another instance may have fired (or the app
    // cancelled) it.
    {
      ScheduleFile file(ScheduleFilePath());
      if (file.ok()) {
        std::vector<ScheduledNotification> list = file.Read();
        auto found = std::find_if(list.begin(), list.end(),
                                  [&](const ScheduledNotification& e) {
                                    return e.tag == tag && e.at_ms == n.at_ms;
                                  });
        if (found == list.end())
          return;
        list.erase(found);
        file.Write(list);
      }
    }
    if (!NotifyOnThread(ToOptions(n)))
      DispatchNotificationClosed(tag);
  }

  GMainContext* ctx_ = nullptr;
  GMainLoop* loop_ = nullptr;
  GDBusConnection* conn_ = nullptr;
  bool conn_failed_ = false;
  std::map<uint32_t, std::string> by_id_;    // server id -> tag
  std::map<std::string, uint32_t> by_tag_;   // tag -> server id
  std::map<std::string, std::string> data_;  // tag -> data
  std::map<std::string, Scheduled> scheduled_;
};

LinuxNotificationPlatform* g_linux_platform = nullptr;

}  // namespace

std::unique_ptr<NotificationPlatform> CreateNotificationPlatform() {
  auto p = std::make_unique<LinuxNotificationPlatform>();
  g_linux_platform = p.get();
  return p;
}

void InitNotificationsAtLaunch() {
  static std::once_flag once;
  std::call_once(once, [] {
    NotificationPlatform* p = EnsureNotificationPlatform();
    if (p && p == g_linux_platform)
      g_linux_platform->Rearm();
  });
}

}  // namespace laufey_common
