// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications on Linux (API 41). Two transports, chosen per session:
//
// - The xdg-desktop-portal Notification interface (v1), when the portal has
//   one, it can register this host app's id (org.freedesktop.host.portal.
//   Registry, xdg-desktop-portal 1.19+), and `<app id>.desktop` is
//   installed. Every click is sent as the GApplication action
//   "app.laufey-notification" with the tag, action and data in its target:
//   the desktop (gnome-shell through xdg-desktop-portal-gnome, KDE's portal)
//   calls org.freedesktop.Application.ActivateAction on the app's D-Bus
//   name, which this process owns while it runs (InitNotificationsAtLaunch).
//   When it doesn't run, D-Bus starts the app from `<app id>.service` (the
//   .deb and .rpm install it; its Exec line carries kDBusActivationArg) and
//   the click is delivered as the launch: the cold start, as on macOS and
//   Windows. The portal reports no closes.
// - Otherwise org.freedesktop.Notifications itself: Notify with the actions
//   (plus "default", the body click), the hints desktop-entry, urgency,
//   suppress-sound and image-data, and replaces_id. ActionInvoked /
//   NotificationClosed go to the connection that posted, so a click after
//   this process exited can't be delivered.
//
// The server's GetCapabilities decides what is sent and reported: without
// "actions" no button or click is offered; the body is escaped (it is text,
// not markup) unless the server is known not to read markup.
//
// Scheduling: laufey's own timer delivers a scheduled notification while
// the app runs. The schedule is persisted in the app data directory
// (laufey-notifications.json) and re-armed at the next launch, where one
// whose time passed while the app wasn't running is delivered at once.
// Several instances share the file under a lock, and a due notification is
// claimed from it before it is posted, so it fires once. Where a systemd
// user manager answers on the session bus, each scheduled notification also
// gets a transient timer (laufey-<app id>-<tag id>.timer) that runs
// `<exe> --laufey-notify <tag id>` at its time: that launch (RunNotifyLaunch)
// asks a running app to post it ("laufey-schedule-due", SweepSchedule),
// posts it itself when the app doesn't run or leaves it pending, and exits. Transient timers live
// until the user manager stops (logout without linger, a reboot); every
// launch re-creates them.
//
// Permission is "granted" when a notification server owns the name, or one
// can be started for it (D-Bus activation is tried once), else
// "unsupported"; there is no prompt. Neither the permission query nor
// capabilities waits for a server to start.
//
// Everything runs on one thread of laufey's own, with its own GLib main
// context and its own session-bus connection (the portal's host registry
// must be the first portal call on a connection), so it works the same
// under the WebKitGTK and CEF backends.

#include "laufey_launch_config.h"
#include "laufey_notifications.h"
#include "laufey_single_instance.h"
#include "laufey_system.h"

#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
const char kPortalName[] = "org.freedesktop.portal.Desktop";
const char kPortalPath[] = "/org/freedesktop/portal/desktop";
const char kPortalIface[] = "org.freedesktop.portal.Notification";
const char kRegistryIface[] = "org.freedesktop.host.portal.Registry";
const char kSystemdName[] = "org.freedesktop.systemd1";
const char kSystemdPath[] = "/org/freedesktop/systemd1";
const char kSystemdIface[] = "org.freedesktop.systemd1.Manager";
constexpr gint kCallTimeoutMs = 5000;
// The longest a single timer waits (GLib timeouts are guint milliseconds);
// a later time re-arms when it fires.
constexpr int64_t kMaxTimerMs = 60LL * 60 * 1000;
// A systemd timer is made only for a notification at least this far ahead:
// a nearer one is the running app's own timer's.
constexpr int64_t kMinTimerAheadMs = 2000;
// A scheduled launch posts an entry due within this much of now.
constexpr int64_t kNotifyLaunchSlackMs = 5000;
// The GApplication action a scheduled launch nudges the running app with:
// re-read the schedule and post what is due (SweepSchedule).
const char kScheduleDueAction[] = "laufey-schedule-due";
// How long a scheduled launch waits for the running app to answer that
// before it posts the notification itself.
constexpr gint kNudgeTimeoutMs = 2000;

const char kAppXml[] =
    "<node>"
    " <interface name='org.freedesktop.Application'>"
    "  <method name='Activate'>"
    "   <arg type='a{sv}' name='platform_data' direction='in'/>"
    "  </method>"
    "  <method name='Open'>"
    "   <arg type='as' name='uris' direction='in'/>"
    "   <arg type='a{sv}' name='platform_data' direction='in'/>"
    "  </method>"
    "  <method name='ActivateAction'>"
    "   <arg type='s' name='action_name' direction='in'/>"
    "   <arg type='av' name='parameter' direction='in'/>"
    "   <arg type='a{sv}' name='platform_data' direction='in'/>"
    "  </method>"
    " </interface>"
    "</node>";

std::atomic<bool> g_activation_launch{false};

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

  // Replaces the file through a temporary one and rename(2), then syncs
  // the directory so the rename survives a crash. On any failure the old
  // file stays as it was, the failure is reported on stderr, and false is
  // returned.
  bool Write(const std::vector<ScheduledNotification>& list) {
    if (!ok())
      return false;
    std::string tmp = path_ + ".tmp";
    {
      int fd =
          open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd < 0) {
        Report("can't create " + tmp);
        return false;
      }
      std::string text = SerializeSchedule(list);
      bool ok = write(fd, text.data(), text.size()) ==
                static_cast<ssize_t>(text.size());
      ok = fsync(fd) == 0 && ok;
      ok = close(fd) == 0 && ok;
      if (!ok) {
        Report("can't write " + tmp);
        unlink(tmp.c_str());
        return false;
      }
    }
    if (rename(tmp.c_str(), path_.c_str()) != 0) {
      Report("can't replace it with " + tmp);
      unlink(tmp.c_str());
      return false;
    }
    std::string dir = path_.substr(0, path_.find_last_of('/') + 1);
    int dir_fd = open(dir.empty() ? "." : dir.c_str(),
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
      fsync(dir_fd);
      close(dir_fd);
    }
    return true;
  }

 private:
  void Report(const std::string& what) {
    int err = errno;
    std::cerr << "laufey: the notification schedule " << path_
              << " is unchanged: " << what << " (" << std::strerror(err)
              << ")" << std::endl;
  }

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

// The first `<data dir>/<sub>/<file>` that is a file, searching
// $XDG_DATA_HOME (else ~/.local/share), then $XDG_DATA_DIRS (else
// /usr/local/share:/usr/share). "" when none is.
std::string FindDataFile(const std::string& sub, const std::string& file) {
  std::vector<std::string> dirs;
  dirs.push_back(g_get_user_data_dir());
  for (const gchar* const* d = g_get_system_data_dirs(); d && *d; ++d)
    dirs.push_back(*d);
  for (const std::string& dir : dirs) {
    std::string path = dir + "/" + sub + "/" + file;
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      return path;
  }
  return std::string();
}

// The executable a scheduled launch runs: the AppImage itself when this is
// one (the running file is inside its temporary mount, $APPDIR; $APPIMAGE
// alone is not trusted), else this file.
std::string LaunchExecutable() {
  return RelaunchExecutablePath();
}

class LinuxNotificationPlatform : public NotificationPlatform {
 public:
  LinuxNotificationPlatform() {
    app_id_ = LaunchAppId();
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

  // Never waits for a server to start: one D-Bus can start is started in
  // the background (Activate), and until it answers it counts (show,
  // schedule and clicks; actions are known once it runs). A start that
  // failed counts for nothing from then on.
  uint32_t Capabilities() override {
    uint32_t caps = 0;
    RunSync([&] {
      Server server = ServerState();
      if (server == Server::kNone)
        return;
      caps = LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE;
      if (TimersAvailable(nullptr))
        caps |= LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS;
      if (server == Server::kActivatable) {
        caps |= LAUFEY_NOTIFICATION_CAP_CLICKS;
        Activate(nullptr);
        return;
      }
      if (!ServerCaps().count("actions"))
        return;  // no action: no button and no click (the body is one)
      caps |= LAUFEY_NOTIFICATION_CAP_CLICKS | LAUFEY_NOTIFICATION_CAP_ACTIONS;
      if (ColdStart(nullptr))
        caps |= LAUFEY_NOTIFICATION_CAP_COLD_START;
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
      if (portal_tags_.erase(tag) && Connection()) {
        GVariant* r = g_dbus_connection_call_sync(
            conn_, kPortalName, kPortalPath, kPortalIface, "RemoveNotification",
            g_variant_new("(s)", tag.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE,
            kCallTimeoutMs, nullptr, nullptr);
        if (r)
          g_variant_unref(r);
      }
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

  // Answered from the notification thread without blocking the caller: a
  // server D-Bus can start is started asynchronously, and the answer waits
  // for that (granted when it started, unsupported when it failed).
  void QueryPermission(int /*kind*/, std::function<void(int)> done) override {
    RunAsync([this, done] {
      switch (ServerState()) {
        case Server::kRunning:
          done(LAUFEY_PERMISSION_STATUS_GRANTED);
          return;
        case Server::kNone:
          done(LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
          return;
        case Server::kActivatable:
          Activate([done](bool started) {
            done(started ? LAUFEY_PERMISSION_STATUS_GRANTED
                         : LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
          });
          return;
      }
    });
  }

  // No prompt on Linux: the server shows what it is sent.
  void RequestPermission(int kind, std::function<void(int)> done) override {
    QueryPermission(kind, std::move(done));
  }

  // At launch: claim the app's D-Bus name (clicks on portal notifications
  // arrive there), then re-arm the persisted schedule and its timers.
  void Init(bool activation_launch) {
    launch_click_pending_ = activation_launch;
    RunSync([&] {
      ClaimAppName();
      ScheduleFile file(ScheduleFilePath());
      if (!file.ok())
        return;
      for (const ScheduledNotification& n : file.Read()) {
        Arm(n);
        StartTimer(n);
      }
    });
  }

  // A scheduled launch (RunNotifyLaunch): posts the entry `id` names. When
  // the app runs (another process owns its name) it is nudged to post it
  // itself (its clicks then reach it, whatever the transport), and the entry
  // is posted here only if it is still in the file after that: the owner
  // didn't answer in time, or answered without claiming it (a process that
  // took the name but isn't the app, an instance that never armed it). The
  // claim under the file lock keeps it to one post either way.
  void NotifyLaunch(const std::string& id) {
    RunSync([&] {
      if (!Connection())
        return;
      if (AppRunning()) {
        if (NudgeRunningApp(id)) {
          std::cerr << "laufey: " << app_id_
                    << " is running; it was asked to post the notification"
                    << std::endl;
        } else {
          std::cerr << "laufey: " << app_id_
                    << " is running but didn't answer; the notification is "
                       "posted from here if it is still pending"
                    << std::endl;
        }
      }
      ScheduledNotification n;
      {
        ScheduleFile file(ScheduleFilePath());
        if (!file.ok())
          return;
        std::vector<ScheduledNotification> list = file.Read();
        int64_t now = UnixTimeMs();
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const ScheduledNotification& e) {
                                 return NotificationTagId(e.tag) == id &&
                                        e.at_ms <= now + kNotifyLaunchSlackMs;
                               });
        if (it == list.end())
          return;  // cancelled, or already posted
        if (!ServerPresent()) {
          // Left in the file: the next launch delivers it ("missed").
          std::cerr << "laufey: no notification server; the notification "
                       "waits for the app's next launch"
                    << std::endl;
          return;
        }
        n = *it;
        list.erase(it);
        file.Write(list);
      }
      NotifyOnThread(ToOptions(n));
    });
  }

  NotificationFacts Facts() {
    NotificationFacts f;
    RunSync([&] {
      Server server = ServerState();
      if (server != Server::kNone)
        f.transport = UsePortal(nullptr) ? "portal" : "freedesktop";
      if (server == Server::kNone && activation_failed_)
        f.activation_error = activation_error_;
      f.schedule_while_closed = TimersAvailable(&f.schedule_reason);
      if (server == Server::kRunning) {
        f.server_caps_known = true;
        const std::set<std::string>& caps = ServerCaps();
        f.server_caps.assign(caps.begin(), caps.end());
        f.cold_start = ColdStart(&f.cold_start_reason);
      } else {
        f.cold_start_reason =
            server == Server::kNone
                ? (f.activation_error.empty() ? "no notification server"
                                              : f.activation_error)
                : "the notification server hasn't started yet";
      }
    });
    return f;
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

  // Runs `fn` on the notification thread and returns at once (inline when
  // already there).
  void RunAsync(std::function<void()> fn) {
    g_main_context_invoke_full(
        ctx_, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          (*static_cast<std::function<void()>*>(data))();
          return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)),
        [](gpointer data) {
          delete static_cast<std::function<void()>*>(data);
        });
  }

  // A session-bus connection of its own (the portal's host registry must be
  // the first portal call on its connection, and the shared one may have
  // made others), connected and subscribed on first use. Thread only.
  GDBusConnection* Connection() {
    if (conn_ || conn_failed_)
      return conn_;
    GError* error = nullptr;
    gchar* address =
        g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (address) {
      conn_ = g_dbus_connection_new_for_address_sync(
          address,
          static_cast<GDBusConnectionFlags>(
              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr, nullptr, &error);
      g_free(address);
    }
    if (!conn_) {
      conn_failed_ = true;
      if (error)
        g_error_free(error);
      return nullptr;
    }
    g_dbus_connection_set_exit_on_close(conn_, FALSE);
    g_dbus_connection_signal_subscribe(conn_, kName, kIface, "ActionInvoked",
                                       kPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                       OnActionInvoked, this, nullptr);
    g_dbus_connection_signal_subscribe(
        conn_, kName, kIface, "NotificationClosed", kPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, OnNotificationClosed, this, nullptr);
    g_dbus_connection_signal_subscribe(
        conn_, kPortalName, kPortalIface, "ActionInvoked", kPortalPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, OnPortalActionInvoked, this, nullptr);
    return conn_;
  }

  // Bus-daemon helpers. Thread only, after Connection().
  bool NameHasOwner(const char* name) {
    GVariant* r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (!r)
      return false;
    gboolean owned = FALSE;
    g_variant_get(r, "(b)", &owned);
    g_variant_unref(r);
    return owned;
  }

  std::string NameOwner(const char* name) {
    GVariant* r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (!r)
      return std::string();
    const char* owner = nullptr;
    g_variant_get(r, "(&s)", &owner);
    std::string out = owner ? owner : "";
    g_variant_unref(r);
    return out;
  }

  // --- The app's D-Bus name: org.freedesktop.Application ---

  // Exports org.freedesktop.Application and takes the app id's name (never
  // queued: another instance that holds it keeps it, and gets the clicks).
  // Thread only.
  void ClaimAppName() {
    if (name_claimed_ || !IsValidApplicationId(app_id_) || !Connection())
      return;
    name_claimed_ = true;
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kAppXml, &error);
    if (!info) {
      if (error)
        g_error_free(error);
      return;
    }
    static const GDBusInterfaceVTable kVTable = {
        OnAppMethod, nullptr, nullptr, {}};
    guint reg = g_dbus_connection_register_object(
        conn_, ApplicationObjectPath(app_id_).c_str(), info->interfaces[0],
        &kVTable, this, nullptr, &error);
    g_dbus_node_info_unref(info);
    if (!reg) {
      std::cerr << "laufey: could not export org.freedesktop.Application: "
                << (error ? error->message : "") << std::endl;
      if (error)
        g_error_free(error);
      return;
    }
    // DBUS_NAME_FLAG_DO_NOT_QUEUE (4).
    GVariant* r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", app_id_.c_str(), 4u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr);
    if (!r)
      return;
    guint32 reply = 0;
    g_variant_get(r, "(u)", &reply);
    g_variant_unref(r);
    // 1: the primary owner; 4: already it.
    name_owned_ = reply == 1 || reply == 4;
  }

  // Whether another process owns the app's name: the app runs. Thread only.
  bool AppRunning() {
    return IsValidApplicationId(app_id_) && NameHasOwner(app_id_.c_str());
  }

  // ActivateAction(kScheduleDueAction, [id]) on the app's name: whether its
  // owner answered within kNudgeTimeoutMs. Thread only.
  bool NudgeRunningApp(const std::string& id) {
    GVariantBuilder param;
    g_variant_builder_init(&param, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&param, "v", g_variant_new_string(id.c_str()));
    GVariantBuilder platform_data;
    g_variant_builder_init(&platform_data, G_VARIANT_TYPE("a{sv}"));
    GVariant* r = g_dbus_connection_call_sync(
        conn_, app_id_.c_str(), ApplicationObjectPath(app_id_).c_str(),
        "org.freedesktop.Application", "ActivateAction",
        g_variant_new("(sava{sv})", kScheduleDueAction, &param,
                      &platform_data),
        nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, kNudgeTimeoutMs, nullptr,
        nullptr);
    if (!r)
      return false;
    g_variant_unref(r);
    return true;
  }

  static void OnAppMethod(GDBusConnection*, const gchar* /*sender*/,
                          const gchar*, const gchar*, const gchar* method,
                          GVariant* params, GDBusMethodInvocation* invocation,
                          gpointer self_ptr) {
    auto* self = static_cast<LinuxNotificationPlatform*>(self_ptr);
    // A scheduled launch's nudge (NotifyLaunch): post what is due, then
    // answer, so the launch finds it claimed. Not a click: it leaves the
    // pending launch click alone.
    if (g_strcmp0(method, "ActivateAction") == 0) {
      const char* name = nullptr;
      g_variant_get(params, "(&sav@a{sv})", &name, nullptr, nullptr);
      if (g_strcmp0(name, kScheduleDueAction) == 0) {
        self->SweepSchedule();
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
    }
    if (g_strcmp0(method, "ActivateAction") == 0) {
      // Any process on the session bus can call this: a click counts only
      // with arguments laufey posted (DecodeClickArguments), and a forgery
      // neither arrives nor uses up the launch.
      const char* name = nullptr;
      GVariantIter* param = nullptr;
      GVariant* platform_data = nullptr;
      g_variant_get(params, "(&sav@a{sv})", &name, &param, &platform_data);
      GVariant* target = nullptr;
      if (g_strcmp0(name, kNotificationActionName) == 0 &&
          g_variant_iter_next(param, "v", &target)) {
        if (g_variant_is_of_type(target, G_VARIANT_TYPE_STRING))
          self->HandleClickTarget(g_variant_get_string(target, nullptr),
                                  /*from_activation=*/true);
        g_variant_unref(target);
      }
      g_variant_iter_free(param);
      g_variant_unref(platform_data);
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    // The first call to a process D-Bus started is what it was started for.
    bool launch = self->launch_click_pending_.exchange(false);
    if (g_strcmp0(method, "Activate") == 0) {
      // The app was started (or brought up) from its launcher: a started
      // process just runs; a running one comes to the front, as a second
      // launch would.
      if (!launch)
        QueueSecondInstance(SecondInstanceMessage());
    } else if (g_strcmp0(method, "Open") == 0) {
      // Links and files, as a second launch with them as its arguments.
      SecondInstanceMessage message;
      GVariantIter* uris = nullptr;
      GVariant* platform_data = nullptr;
      g_variant_get(params, "(as@a{sv})", &uris, &platform_data);
      const char* uri = nullptr;
      message.args.push_back("--");
      while (g_variant_iter_loop(uris, "&s", &uri)) {
        gchar* path = g_str_has_prefix(uri, "file:")
                          ? g_filename_from_uri(uri, nullptr, nullptr)
                          : nullptr;
        message.args.push_back(path ? path : uri);
        g_free(path);
      }
      g_variant_iter_free(uris);
      g_variant_unref(platform_data);
      if (message.args.size() > 1)
        QueueSecondInstance(std::move(message));
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
  }

  // A click on a portal notification: ActivateAction (and, where a portal
  // backend also sends it, the portal's ActionInvoked). The first of the two
  // wins for a few seconds. A target laufey didn't post is dropped
  // (DecodeClickArguments). The first verified ActivateAction to a process
  // D-Bus started is the click it was started for (`from_activation`).
  void HandleClickTarget(const std::string& target, bool from_activation) {
    std::string tag, action, data;
    bool has_action = false, has_data = false;
    if (!DecodeClickArguments(target, &tag, &action, &has_action, &data,
                              &has_data))
      return;
    bool launch =
        from_activation && launch_click_pending_.exchange(false);
    auto now = std::chrono::steady_clock::now();
    for (auto it = recent_clicks_.begin(); it != recent_clicks_.end();) {
      if (now - it->second > std::chrono::seconds(5))
        it = recent_clicks_.erase(it);
      else
        ++it;
    }
    if (!recent_clicks_.emplace(target, now).second)
      return;
    DispatchNotificationClick(tag, has_action ? action.c_str() : nullptr,
                              has_data ? &data : nullptr, launch);
  }

  static void OnPortalActionInvoked(GDBusConnection*, const gchar*,
                                    const gchar*, const gchar*, const gchar*,
                                    GVariant* params, gpointer self_ptr) {
    auto* self = static_cast<LinuxNotificationPlatform*>(self_ptr);
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sssav)")))
      return;
    GVariantIter* param = nullptr;
    const char *app = nullptr, *id = nullptr, *action = nullptr;
    g_variant_get(params, "(&s&s&sav)", &app, &id, &action, &param);
    GVariant* target = nullptr;
    if (g_variant_iter_next(param, "v", &target)) {
      if (g_variant_is_of_type(target, G_VARIANT_TYPE_STRING))
        self->HandleClickTarget(g_variant_get_string(target, nullptr), false);
      g_variant_unref(target);
    }
    g_variant_iter_free(param);
  }

  // --- The portal ---

  // Whether notifications go through the portal, probed once: the host
  // registry accepting this app's id, the Notification interface, and
  // `<app id>.desktop` installed (the desktop names the app by it, and the
  // shell refuses an id without one). `reason` (optional): why not.
  // Thread only.
  bool UsePortal(std::string* reason) {
    if (portal_state_ == 0)
      ProbePortal();
    if (reason && portal_state_ < 0)
      *reason = portal_reason_;
    return portal_state_ > 0;
  }

  void ProbePortal() {
    portal_state_ = -1;
    if (!IsValidApplicationId(app_id_)) {
      portal_reason_ =
          app_id_.empty()
              ? "no app id (LAUFEY_APP_ID or the launch file's appId)"
              : "the app id \"" + app_id_ + "\" is not a valid D-Bus name";
      return;
    }
    if (FindDataFile("applications", app_id_ + ".desktop").empty()) {
      portal_reason_ = "no " + app_id_ +
                       ".desktop is installed (the .deb and .rpm install "
                       "it; an AppImage or a tarball doesn't)";
      return;
    }
    if (!Connection()) {
      portal_reason_ = "no session bus";
      return;
    }
    // The registry first: it must be this connection's first portal call.
    GError* error = nullptr;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kPortalName, kPortalPath, kRegistryIface, "Register",
        g_variant_new("(sa{sv})", app_id_.c_str(), &options), nullptr,
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &error);
    if (!r) {
      // Not running and not startable (NameHasNoOwner: XFCE / i3 under GDM,
      // where the unit's Requisite=graphical-session.target fails) is not
      // the same as too old (UnknownMethod). The call above already asked
      // D-Bus to start the portal once.
      std::string name;
      std::string message;
      if (error) {
        gchar* remote = g_dbus_error_get_remote_error(error);
        name = remote ? remote : "";
        g_free(remote);
        if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
          name = "org.freedesktop.DBus.Error.UnknownMethod";
        g_dbus_error_strip_remote_error(error);
        message = error->message;
        g_error_free(error);
      }
      portal_reason_ = PortalRegistryFailureReason(name, message);
      return;
    }
    g_variant_unref(r);
    r = g_dbus_connection_call_sync(
        conn_, kPortalName, kPortalPath, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", kPortalIface, "version"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    uint32_t version = 0;
    if (r) {
      GVariant* v = nullptr;
      g_variant_get(r, "(v)", &v);
      if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        version = g_variant_get_uint32(v);
      g_variant_unref(v);
      g_variant_unref(r);
    }
    if (version < 1) {
      portal_reason_ = "xdg-desktop-portal has no Notification interface";
      return;
    }
    portal_state_ = 1;
  }

  // Whether a click on a notification starts the app when it isn't running.
  // Thread only.
  bool ColdStart(std::string* reason) {
    std::string why;
    bool ok = false;
    if (!UsePortal(&why)) {
      why =
          "notifications go to org.freedesktop.Notifications, whose clicks "
          "reach only the process that posted them (" +
          why + ")";
    } else if (!ServerCaps().count("actions")) {
      why = "the notification server reports no actions";
    } else if (FindDataFile("dbus-1/services", app_id_ + ".service").empty()) {
      why = "no D-Bus service file " + app_id_ +
            ".service is installed (the .deb and .rpm install it)";
    } else if (!name_owned_) {
      why = "this process doesn't own the app's D-Bus name " + app_id_;
    } else {
      ok = true;
    }
    if (reason && !ok)
      *reason = why;
    return ok;
  }

  bool PortalNotify(const NotificationOptions& o) {
    GVariantBuilder n;
    g_variant_builder_init(&n, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&n, "{sv}", "title",
                          g_variant_new_string(o.title.c_str()));
    if (!o.body.empty())
      g_variant_builder_add(&n, "{sv}", "body",
                            g_variant_new_string(o.body.c_str()));
    g_variant_builder_add(
        &n, "{sv}", "priority",
        g_variant_new_string(o.require_interaction ? "urgent" : "normal"));
    if (!o.icon_png.empty()) {
      GBytes* bytes = g_bytes_new(o.icon_png.data(), o.icon_png.size());
      GIcon* icon = g_bytes_icon_new(bytes);
      GVariant* serialized = g_icon_serialize(icon);
      if (serialized)
        g_variant_builder_add(&n, "{sv}", "icon", serialized);
      g_object_unref(icon);
      g_bytes_unref(bytes);
    }
    std::string action = std::string("app.") + kNotificationActionName;
    const std::string* data = o.has_data ? &o.data : nullptr;
    if (ServerCaps().count("actions")) {
      std::string body_target = EncodeToastArguments(o.tag, nullptr, data);
      g_variant_builder_add(&n, "{sv}", "default-action",
                            g_variant_new_string(action.c_str()));
      g_variant_builder_add(&n, "{sv}", "default-action-target",
                            g_variant_new_string(body_target.c_str()));
      GVariantBuilder buttons;
      g_variant_builder_init(&buttons, G_VARIANT_TYPE("aa{sv}"));
      bool any = false;
      for (const NotificationAction& a : o.actions) {
        if (a.id == "default")
          continue;
        std::string target = EncodeToastArguments(o.tag, a.id.c_str(), data);
        GVariantBuilder button;
        g_variant_builder_init(&button, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&button, "{sv}", "label",
                              g_variant_new_string(a.title.c_str()));
        g_variant_builder_add(&button, "{sv}", "action",
                              g_variant_new_string(action.c_str()));
        g_variant_builder_add(&button, "{sv}", "target",
                              g_variant_new_string(target.c_str()));
        g_variant_builder_add(&buttons, "a{sv}", &button);
        any = true;
      }
      if (any)
        g_variant_builder_add(&n, "{sv}", "buttons",
                              g_variant_builder_end(&buttons));
      else
        g_variant_builder_clear(&buttons);
    }
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kPortalName, kPortalPath, kPortalIface, "AddNotification",
        g_variant_new("(sa{sv})", o.tag.c_str(), &n), nullptr,
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &error);
    if (!r) {
      std::cerr << "laufey: notification not posted through the portal: "
                << (error ? error->message : "no answer") << std::endl;
      if (error)
        g_error_free(error);
      return false;
    }
    g_variant_unref(r);
    portal_tags_.insert(o.tag);
    while (portal_tags_.size() > 512)
      portal_tags_.erase(portal_tags_.begin());
    return true;
  }

  // --- org.freedesktop.Notifications ---

  enum class Server {
    kRunning,      // something owns the name
    kActivatable,  // D-Bus can start one (not known to fail)
    kNone,         // neither, or its start failed earlier
  };

  // Thread only. Bus-daemon calls only: never waits for a server to start.
  Server ServerState() {
    if (!Connection())
      return Server::kNone;
    if (NameHasOwner(kName))
      return Server::kRunning;
    return !activation_failed_ && Activatable() ? Server::kActivatable
                                                : Server::kNone;
  }

  // Starts the activatable server in the background (once at a time) and
  // calls `done` (may be null) on this thread with whether it started. A
  // failure is remembered for the process: an installed daemon that can't
  // start in this session (Sway with no daemon of its own) would otherwise
  // make every Notify wait for the activation to time out. Thread only.
  void Activate(std::function<void(bool)> done) {
    if (activation_failed_ || !Connection()) {
      if (done)
        done(false);
      return;
    }
    if (done)
      activation_waiters_.push_back(std::move(done));
    if (activation_pending_)
      return;
    activation_pending_ = true;
    g_dbus_connection_call(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "StartServiceByName",
        g_variant_new("(su)", kName, 0u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer self_ptr) {
          auto* self = static_cast<LinuxNotificationPlatform*>(self_ptr);
          GError* error = nullptr;
          GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                      result, &error);
          bool started = r != nullptr;
          if (r)
            g_variant_unref(r);
          self->NoteActivation(started, error);
          self->activation_pending_ = false;
          std::vector<std::function<void(bool)>> waiters;
          waiters.swap(self->activation_waiters_);
          for (auto& waiter : waiters)
            waiter(started);
        },
        this);
  }

  bool Activatable() {
    bool found = false;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListActivatableNames", nullptr,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (!r)
      return false;
    GVariantIter* iter = nullptr;
    const char* name = nullptr;
    g_variant_get(r, "(as)", &iter);
    while (g_variant_iter_loop(iter, "&s", &name)) {
      if (g_strcmp0(name, kName) == 0)
        found = true;
    }
    g_variant_iter_free(iter);
    g_variant_unref(r);
    return found;
  }

  // For posting (Show, a schedule, a timer): waits for an activatable
  // server to start (Notify would wait for it anyway). Thread only.
  bool ServerPresent() {
    switch (ServerState()) {
      case Server::kRunning:
        return true;
      case Server::kNone:
        return false;
      case Server::kActivatable:
        break;
    }
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "StartServiceByName",
        g_variant_new("(su)", kName, 0u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &error);
    if (r)
      g_variant_unref(r);
    NoteActivation(r != nullptr, error);
    return r != nullptr;
  }

  // Remembers how a start of the activatable server ended (frees `error`):
  // a failure, with D-Bus's own words for it, counts for the rest of the
  // process. One seen on Fedora: dunst and xfce4-notifyd both installed,
  // each with a systemd unit for org.freedesktop.Notifications, and systemd
  // refuses to start either ("unit is invalid"). Thread only.
  void NoteActivation(bool started, GError* error) {
    activation_failed_ = !started;
    if (!started) {
      if (error)
        g_dbus_error_strip_remote_error(error);
      activation_error_ =
          std::string("D-Bus could not start the notification server for "
                      "org.freedesktop.Notifications: ") +
          (error ? error->message : "no answer");
    }
    if (error)
      g_error_free(error);
  }

  // The running server's GetCapabilities, read once per server (a new owner
  // of the name is asked again). Empty while no server runs. Thread only.
  const std::set<std::string>& ServerCaps() {
    std::string owner = Connection() ? NameOwner(kName) : std::string();
    if (owner == caps_owner_)
      return server_caps_;
    caps_owner_ = owner;
    server_caps_.clear();
    if (owner.empty())
      return server_caps_;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kName, kPath, kIface, "GetCapabilities", nullptr,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        nullptr);
    if (r) {
      GVariantIter* iter = nullptr;
      const char* cap = nullptr;
      g_variant_get(r, "(as)", &iter);
      while (g_variant_iter_loop(iter, "&s", &cap))
        server_caps_.insert(cap);
      g_variant_iter_free(iter);
      g_variant_unref(r);
    } else {
      caps_owner_.clear();  // ask again next time
    }
    return server_caps_;
  }

  // Whether ServerCaps() (called just before) holds the running server's
  // answer, not an empty set for want of one. Thread only.
  bool ServerCapsKnown() const {
    return !caps_owner_.empty();
  }

  bool NotifyOnThread(const NotificationOptions& o) {
    // An activatable server is started first (once: a start that fails, as
    // on Sway with only Plasma's activatable service, is remembered), so a
    // session with no server fails at once instead of on every Notify's
    // auto-start timeout.
    if (!Connection() || !ServerPresent())
      return false;
    bool posted = UsePortal(nullptr) ? PortalNotify(o) : FdoNotify(o);
    if (!posted)
      return false;
    if (o.has_data)
      data_[o.tag] = o.data;
    else
      data_.erase(o.tag);
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

  bool FdoNotify(const NotificationOptions& o) {
    uint32_t replaces = 0;
    auto prev = by_tag_.find(o.tag);
    if (prev != by_tag_.end()) {
      replaces = prev->second;
      Forget(replaces);
    }
    const std::set<std::string>& caps = ServerCaps();
    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    // Without "actions" the server draws none and reports no click: none
    // is sent. A server whose capabilities aren't known gets them.
    if (caps.empty() || caps.count("actions")) {
      // "default" is the body click. GNOME Shell, Plasma, dunst and mako
      // never draw it as a button; xfce4-notifyd draws every action, this
      // one too, so it gets a label instead of a blank button.
      g_variant_builder_add(&actions, "s", "default");
      g_variant_builder_add(&actions, "s", "Open");
      for (const NotificationAction& a : o.actions) {
        if (a.id == "default")
          continue;  // the protocol's own key
        g_variant_builder_add(&actions, "s", a.id.c_str());
        g_variant_builder_add(&actions, "s", a.title.c_str());
      }
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (!app_id_.empty())
      g_variant_builder_add(&hints, "{sv}", "desktop-entry",
                            g_variant_new_string(app_id_.c_str()));
    g_variant_builder_add(&hints, "{sv}", "urgency",
                          g_variant_new_byte(o.require_interaction ? 2 : 1));
    if (o.silent)
      g_variant_builder_add(&hints, "{sv}", "suppress-sound",
                            g_variant_new_boolean(TRUE));
    if (GVariant* image = ImageData(o.icon_png))
      g_variant_builder_add(&hints, "{sv}", "image-data", image);
    // The body is text: escaped unless the server is known not to read
    // markup (its capabilities were read and lack "body-markup"). One whose
    // capabilities couldn't be read may well read markup.
    bool markup = !ServerCapsKnown() || caps.count("body-markup");
    std::string body = markup ? EscapeNotificationMarkup(o.body) : o.body;
    std::string app_name = LoginItemName();
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kName, kPath, kIface, "Notify",
        g_variant_new("(susssasa{sv}i)", app_name.c_str(), replaces, "",
                      o.title.c_str(), body.c_str(), &actions, &hints,
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
    // Bounded: a notification the server never reports closed stays.
    while (by_id_.size() > 512)
      Forget(by_id_.begin()->first);
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

  // --- systemd user timers ---

  // Whether a scheduled notification can be posted while the app is
  // closed: a systemd user manager answers on the session bus, and the
  // executable it would run is known. Probed once. Thread only.
  bool TimersAvailable(std::string* reason) {
    if (timers_state_ == 0) {
      timers_state_ = -1;
      if (!IsValidApplicationId(app_id_)) {
        timers_reason_ = "no valid app id";
      } else if (!Connection()) {
        timers_reason_ = "no session bus";
      } else if (!NameHasOwner(kSystemdName)) {
        timers_reason_ =
            "no systemd user manager on the session bus: a scheduled "
            "notification is posted while the app runs, or at its next "
            "launch";
      } else if ((exe_ = LaunchExecutable()).empty()) {
        timers_reason_ = "the app's executable path is unknown";
      } else {
        timers_state_ = 1;
      }
    }
    if (reason && timers_state_ < 0)
      *reason = timers_reason_;
    return timers_state_ > 0;
  }

  // A transient timer that runs `<exe> --laufey-notify <tag id>` at the
  // notification's time (replacing one the tag had). Thread only.
  void StartTimer(const ScheduledNotification& n) {
    if (n.at_ms <= UnixTimeMs() + kMinTimerAheadMs || !TimersAvailable(nullptr))
      return;
    std::string unit = NotificationTimerUnit(app_id_, n.tag);
    StopTimer(n.tag);
    std::string description = "Scheduled notification for " + app_id_;

    GVariantBuilder calendar;
    g_variant_builder_init(&calendar, G_VARIANT_TYPE("a(ss)"));
    std::string spec = SystemdCalendarUtc(n.at_ms);
    g_variant_builder_add(&calendar, "(ss)", "OnCalendar", spec.c_str());
    GVariantBuilder timer;
    g_variant_builder_init(&timer, G_VARIANT_TYPE("a(sv)"));
    g_variant_builder_add(&timer, "(sv)", "Description",
                          g_variant_new_string(description.c_str()));
    g_variant_builder_add(&timer, "(sv)", "TimersCalendar",
                          g_variant_builder_end(&calendar));
    g_variant_builder_add(&timer, "(sv)", "AccuracyUSec",
                          g_variant_new_uint64(1000000));
    g_variant_builder_add(&timer, "(sv)", "RemainAfterElapse",
                          g_variant_new_boolean(FALSE));

    std::string id = NotificationTagId(n.tag);
    const gchar* argv[] = {exe_.c_str(), kNotifyLaunchArg, id.c_str(), nullptr};
    GVariantBuilder exec;
    g_variant_builder_init(&exec, G_VARIANT_TYPE("a(sasb)"));
    g_variant_builder_add(&exec, "(s^asb)", exe_.c_str(), argv, FALSE);
    // The launch finds the app's data directory as this process does.
    GVariantBuilder env;
    g_variant_builder_init(&env, G_VARIANT_TYPE("as"));
    std::string app_env = "LAUFEY_APP_ID=" + app_id_;
    g_variant_builder_add(&env, "s", app_env.c_str());
    std::string data_env;
    if (const char* data_dir = getenv("LAUFEY_DATA_DIR")) {
      if (*data_dir) {
        data_env = std::string("LAUFEY_DATA_DIR=") + data_dir;
        g_variant_builder_add(&env, "s", data_env.c_str());
      }
    }
    GVariantBuilder service;
    g_variant_builder_init(&service, G_VARIANT_TYPE("a(sv)"));
    g_variant_builder_add(&service, "(sv)", "Description",
                          g_variant_new_string(description.c_str()));
    g_variant_builder_add(&service, "(sv)", "ExecStart",
                          g_variant_builder_end(&exec));
    g_variant_builder_add(&service, "(sv)", "Environment",
                          g_variant_builder_end(&env));
    g_variant_builder_add(&service, "(sv)", "CollectMode",
                          g_variant_new_string("inactive-or-failed"));
    GVariantBuilder aux;
    g_variant_builder_init(&aux, G_VARIANT_TYPE("a(sa(sv))"));
    std::string service_name = unit + ".service";
    g_variant_builder_add(&aux, "(sa(sv))", service_name.c_str(), &service);

    std::string timer_name = unit + ".timer";
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        conn_, kSystemdName, kSystemdPath, kSystemdIface, "StartTransientUnit",
        g_variant_new("(ssa(sv)a(sa(sv)))", timer_name.c_str(), "replace",
                      &timer, &aux),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
        &error);
    if (!r) {
      std::cerr << "laufey: no systemd timer for the scheduled notification "
                   "(it is posted while the app runs): "
                << (error ? error->message : "no answer") << std::endl;
      if (error)
        g_error_free(error);
      return;
    }
    g_variant_unref(r);
  }

  // Stops (and so unloads) the tag's transient timer, if it has one.
  void StopTimer(const std::string& tag) {
    if (timers_state_ <= 0)
      return;
    std::string unit = NotificationTimerUnit(app_id_, tag);
    for (const char* suffix : {".timer", ".service"}) {
      std::string name = unit + suffix;
      GVariant* r = g_dbus_connection_call_sync(
          conn_, kSystemdName, kSystemdPath, kSystemdIface, "StopUnit",
          g_variant_new("(ss)", name.c_str(), "replace"), G_VARIANT_TYPE("(o)"),
          G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr);
      if (r)
        g_variant_unref(r);
    }
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
        // A timer only for a persisted entry: the launch it runs reads it
        // from the file.
        StartTimer(n);
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
    if (list.size() != before) {
      file.Write(list);
      StopTimer(tag);
    }
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
    Deliver(n);
    // Another instance (or a scheduled launch that found this one running)
    // may have added entries this one never armed.
    SweepSchedule();
  }

  // Re-reads the schedule: posts every entry due (within the scheduled
  // launch's slack) and arms the ones this instance hasn't. The nudge from a
  // scheduled launch, and after each of this instance's own timers. Thread
  // only.
  void SweepSchedule() {
    std::vector<ScheduledNotification> list;
    {
      ScheduleFile file(ScheduleFilePath());
      if (!file.ok())
        return;
      list = file.Read();
    }
    int64_t now = UnixTimeMs();
    for (const ScheduledNotification& e : list) {
      if (e.at_ms <= now + kNotifyLaunchSlackMs) {
        Deliver(e);
        continue;
      }
      auto armed = scheduled_.find(e.tag);
      if (armed == scheduled_.end() || armed->second.n.at_ms != e.at_ms)
        Arm(e);
    }
  }

  // Posts the due entry `n` once: claimed from the file first (another
  // instance or a scheduled launch may have posted it, or the app cancelled
  // it). With no server yet it stays and is retried in 30 seconds. Thread
  // only.
  void Deliver(const ScheduledNotification& due) {
    // A copy: `due` may be the armed entry, which Arm / Disarm free.
    const ScheduledNotification n = due;
    const std::string& tag = n.tag;
    if (!ServerPresent()) {
      // No notification server (yet: a session still starting): keep it,
      // and try again in a while.
      Arm(n, 30000);
      return;
    }
    Disarm(tag);
    // Claim it from the file: another instance (or a scheduled launch) may
    // have posted it, or the app cancelled it.
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
        StopTimer(tag);
      }
    }
    if (!NotifyOnThread(ToOptions(n)))
      DispatchNotificationClosed(tag);
  }

  std::string app_id_;
  GMainContext* ctx_ = nullptr;
  GMainLoop* loop_ = nullptr;
  GDBusConnection* conn_ = nullptr;
  bool conn_failed_ = false;
  // An activatable notification server failed to start (Activate,
  // ServerPresent); remembered for the process.
  bool activation_failed_ = false;
  std::string activation_error_;  // why, when activation_failed_
  bool activation_pending_ = false;  // an Activate call is in flight
  std::vector<std::function<void(bool)>> activation_waiters_;
  std::map<uint32_t, std::string> by_id_;    // server id -> tag
  std::map<std::string, uint32_t> by_tag_;   // tag -> server id
  std::map<std::string, std::string> data_;  // tag -> data
  std::map<std::string, Scheduled> scheduled_;
  // The server's capabilities and the unique name they were read from.
  std::string caps_owner_;
  std::set<std::string> server_caps_;
  // The app's D-Bus name.
  bool name_claimed_ = false;
  bool name_owned_ = false;
  // Started by D-Bus activation, and the call it was started for hasn't
  // arrived yet.
  std::atomic<bool> launch_click_pending_{false};
  std::map<std::string, std::chrono::steady_clock::time_point> recent_clicks_;
  // The portal: 0 not probed, 1 used, -1 not (portal_reason_ says why).
  int portal_state_ = 0;
  std::string portal_reason_;
  std::set<std::string> portal_tags_;  // posted through the portal
  // systemd user timers: 0 not probed, 1 available, -1 not.
  int timers_state_ = 0;
  std::string timers_reason_;
  std::string exe_;
};

LinuxNotificationPlatform* g_linux_platform = nullptr;

// This process was started by D-Bus activation (`<app id>.service` runs the
// app with kDBusActivationArg): the host said so (SetDBusActivationLaunch),
// or, for a host that doesn't, the argument is on the command line.
bool StartedByDBusActivation() {
  if (g_activation_launch)
    return true;
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  if (!in)
    return false;
  std::vector<std::string> args;
  std::string arg;
  bool first = true;
  while (std::getline(in, arg, '\0')) {
    if (!first)
      args.push_back(arg);
    first = false;
  }
  return HasDBusActivationArg(args);
}

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
      g_linux_platform->Init(StartedByDBusActivation());
  });
}

NotificationFacts LinuxNotificationFacts() {
  NotificationPlatform* p = EnsureNotificationPlatform();
  if (!p || p != g_linux_platform)
    return NotificationFacts();
  return g_linux_platform->Facts();
}

int RunNotifyLaunch(const std::string& id) {
  NotificationPlatform* p = EnsureNotificationPlatform();
  if (p && p == g_linux_platform)
    g_linux_platform->NotifyLaunch(id);
  return 0;
}

void SetDBusActivationLaunch(bool activated) {
  g_activation_launch = activated;
}

void SetDBusActivationLaunchForTesting(bool activated) {
  g_activation_launch = activated;
}

}  // namespace laufey_common
