// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Linux drag-out (a GTK drag source offering text/uri-list) and file dialogs,
// shared by the WebKitGTK and CEF backends. A file dialog is the portal's
// FileChooser (org.freedesktop.portal.FileChooser: the desktop's own dialog,
// GNOME's or Plasma's, called directly, not only inside Flatpak / Snap as
// GtkFileChooserNative does) whenever xdg-desktop-portal offers it, and
// GtkFileChooserNative otherwise (a portal backend without a FileChooser, as
// xdg-desktop-portal-wlr alone; no portal at all). GTK is not thread-safe, so
// every entry point hops to the default GLib main context, which both backends
// run (WebKitGTK's gtk_main, and Chromium's glib message pump on CEF's UI
// thread).

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_ui_tasks.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

std::function<void(std::function<void()>)>& GtkPoster() {
  static std::function<void(std::function<void()>)> post;
  return post;
}
std::function<bool()>& GtkOnThread() {
  static std::function<bool()> on;
  return on;
}

bool OnGtkThread() {
  if (GtkOnThread())
    return GtkOnThread()();
  return g_main_context_is_owner(g_main_context_default()) != FALSE;
}

}  // namespace

namespace {

// What the parked UI thread and the exiting thread share. Leaked on purpose:
// nothing may be destroyed while exit runs.
struct ExitPark {
  std::mutex mutex;
  std::condition_variable cv;
  bool answered = false;
};

// How long exit may take, once it has begun, before the process ends anyway
// (a destructor or another exit handler that hangs: a lock the parked UI
// thread holds, a library waiting for a thread that is gone):
// LAUFEY_EXIT_WATCHDOG_SECS, 5 by default; 0 turns the watchdog off.
constexpr long kDefaultExitWatchdogSeconds = 5;

// Read once, when the guard is installed (not while exit runs). A value that
// isn't a whole number of seconds keeps the default.
long ExitWatchdogSecondsFromEnv() {
  const char* env = getenv("LAUFEY_EXIT_WATCHDOG_SECS");
  if (!env || !*env)
    return kDefaultExitWatchdogSeconds;
  char* end = nullptr;
  errno = 0;
  long secs = strtol(env, &end, 10);
  if (errno != 0 || *end != '\0' || secs < 0)
    return kDefaultExitWatchdogSeconds;
  return secs;
}

long g_exit_watchdog_seconds = kDefaultExitWatchdogSeconds;

struct WatchdogArgs {
  int status;
  long seconds;
};

void* ExitWatchdog(void* arg) {
  auto* args = static_cast<WatchdogArgs*>(arg);
  struct timespec left = {static_cast<time_t>(args->seconds), 0};
  while (nanosleep(&left, &left) != 0 && errno == EINTR) {
  }
  _exit(args->status);
}

// A raw detached thread (nothing exit tears down) that ends the process with
// `status` after LAUFEY_EXIT_WATCHDOG_SECS; none when that is 0.
void ArmExitWatchdog(int status) {
  if (g_exit_watchdog_seconds <= 0)
    return;
  // Leaked on purpose, like ExitPark.
  auto* args = new WatchdogArgs{status, g_exit_watchdog_seconds};
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0)
    return;
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  pthread_create(&thread, &attr, ExitWatchdog, args);
  pthread_attr_destroy(&attr);
}

// Set by InstallUiExitGuard: until then exit() parks nothing.
std::atomic<bool> g_exit_guard_installed{false};
// The process that installed the guard, whose UI thread it parks. A child
// forked without exec inherits the guard (and the on_exit handler) but not
// the UI thread: its exit parks nothing.
std::atomic<pid_t> g_exit_guard_pid{0};
// The first exit to get here parks the UI thread (exit() below, before any
// exit handler; else the guard's own handler); a later one has nothing to do.
std::atomic<bool> g_ui_parked_for_exit{false};

void ParkUiThreadForExit(int status) {
  if (getpid() != g_exit_guard_pid.load())
    return;
  if (g_ui_parked_for_exit.exchange(true))
    return;
  ArmExitWatchdog(status);
  UiTaskDispatcher& dispatcher = UiTaskDispatcher::Get();
  // The UI thread itself exiting (main returning) has nothing to park; its
  // loop is over, so later dispatches are answered at once.
  if (dispatcher.IsUiThread()) {
    dispatcher.Close();
    return;
  }
  auto* park = new ExitPark();
  dispatcher.Dispatch(
      [](void* data, bool ran) {
        auto* p = static_cast<ExitPark*>(data);
        {
          std::lock_guard<std::mutex> lock(p->mutex);
          p->answered = true;
        }
        p->cv.notify_all();
        if (!ran)
          return;
        // The UI thread stays here until the process is gone.
        for (;;)
          pause();
      },
      park);
  {
    std::unique_lock<std::mutex> lock(park->mutex);
    park->cv.wait_for(lock, std::chrono::seconds(1),
                      [park] { return park->answered; });
  }
  // Nothing runs on the UI thread any more: every task still waiting for it
  // (a runtime thread in a synchronous UI call, say) is answered with `ran`
  // false, and so is every later dispatch, instead of queueing forever.
  dispatcher.Close();
}

#if defined(__GLIBC__)
void ParkUiThreadOnExit(int status, void*) {
  ParkUiThreadForExit(status);
}
#else
// atexit handlers aren't told exit's status (on_exit is glibc's). The
// watchdog only fires when exit hangs, and a hung exit is not a success:
// it ends the process with 1 rather than report 0 for an exit that may have
// been a failing one.
constexpr int kHungExitStatus = 1;
void ParkUiThreadAtExit() {
  ParkUiThreadForExit(kHungExitStatus);
}
#endif

using ExitFunction = void (*)(int);

// The C library's exit(), the one the exit() below stands in front of.
// Looked up as the process starts (before a CEF subprocess is sandboxed),
// and again if something exits before that.
ExitFunction g_libc_exit = nullptr;

ExitFunction LibcExit() {
  if (!g_libc_exit)
    g_libc_exit = reinterpret_cast<ExitFunction>(dlsym(RTLD_NEXT, "exit"));
  return g_libc_exit;
}

__attribute__((constructor)) void FindLibcExit() {
  LibcExit();
}

}  // namespace

void InstallUiExitGuard() {
  static std::once_flag once;
#if defined(__GLIBC__)
  // on_exit: the watchdog ends the process with the status exit was given.
  std::call_once(once, [] {
    g_exit_watchdog_seconds = ExitWatchdogSecondsFromEnv();
    g_exit_guard_pid.store(getpid());
    on_exit(ParkUiThreadOnExit, nullptr);
    g_exit_guard_installed.store(true);
  });
#else
  std::call_once(once, [] {
    g_exit_watchdog_seconds = ExitWatchdogSecondsFromEnv();
    g_exit_guard_pid.store(getpid());
    atexit(ParkUiThreadAtExit);
    g_exit_guard_installed.store(true);
  });
#endif
}

void SetGtkThread(std::function<void(std::function<void()>)> post,
                  std::function<bool()> on_thread) {
  GtkPoster() = std::move(post);
  GtkOnThread() = std::move(on_thread);
}

void GtkRunAsync(std::function<void()> fn) {
  if (GtkPoster()) {
    GtkPoster()(std::move(fn));
    return;
  }
  auto* heap = new std::function<void()>(std::move(fn));
  g_main_context_invoke_full(
      nullptr, G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
      },
      heap,
      [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

bool GtkRunSync(const std::function<void()>& fn) {
  if (OnGtkThread()) {
    fn();
    return true;
  }
  // Over GtkRunAsync's transport, through the UI task dispatcher (the GTK
  // thread is the backend's UI thread: WebKitGTK's gtk_main, CEF's TID_UI),
  // so the wait ends with the loop instead of outliving it.
  return RunOnUiThreadAndWait(fn, [](void (*task)(void*), void* data) {
    GtkRunAsync([task, data] { task(data); });
    return true;
  });
}

namespace {

bool IsWayland(GdkDisplay* display) {
  return display && strstr(G_OBJECT_TYPE_NAME(display), "Wayland") != nullptr;
}

// ===========================================================================
// Drag out
// ===========================================================================

struct LinuxDrag {
  std::unique_ptr<DragOutRequest> req;
  GtkWidget* source = nullptr;
  GdkDragContext* context = nullptr;
  gulong get_id = 0, failed_id = 0, end_id = 0;
  bool data_sent = false;
  bool failed = false;
};

LinuxDrag* g_drag = nullptr;

GtkWidget* DragSourceWidget() {
  // A GtkInvisible is realized on creation and has no handlers of its own,
  // so the drag signals connected below are the only ones it answers.
  static GtkWidget* invisible = nullptr;
  if (!invisible) {
    invisible = gtk_invisible_new();
    g_object_ref_sink(invisible);
  }
  return invisible;
}

void EndDrag(int result) {
  LinuxDrag* d = g_drag;
  if (!d)
    return;
  g_drag = nullptr;
  if (d->get_id)
    g_signal_handler_disconnect(d->source, d->get_id);
  if (d->failed_id)
    g_signal_handler_disconnect(d->source, d->failed_id);
  if (d->end_id)
    g_signal_handler_disconnect(d->source, d->end_id);
  g_object_unref(d->source);
  d->req->Finish(result);
  delete d;
}

void OnDragDataGet(GtkWidget*, GdkDragContext* ctx, GtkSelectionData* data,
                   guint, guint, gpointer) {
  LinuxDrag* d = g_drag;
  if (!d || ctx != d->context)
    return;
  std::vector<gchar*> uris;
  for (const auto& p : d->req->paths) {
    if (gchar* uri = g_filename_to_uri(p.c_str(), nullptr, nullptr))
      uris.push_back(uri);
  }
  uris.push_back(nullptr);
  gtk_selection_data_set_uris(data, uris.data());
  for (gchar* u : uris)
    g_free(u);
  d->data_sent = true;
}

gboolean OnDragFailed(GtkWidget*, GdkDragContext* ctx, GtkDragResult,
                      gpointer) {
  if (g_drag && ctx == g_drag->context)
    g_drag->failed = true;
  return FALSE;  // keep GTK's "snap back" animation
}

void OnDragEnd(GtkWidget*, GdkDragContext* ctx, gpointer) {
  LinuxDrag* d = g_drag;
  if (!d || ctx != d->context)
    return;
  EndDrag(!d->failed && d->data_sent ? LAUFEY_DRAG_RESULT_DROPPED
                                     : LAUFEY_DRAG_RESULT_CANCELLED);
}

bool LeftButtonDown(GdkDisplay* display) {
  GdkSeat* seat = gdk_display_get_default_seat(display);
  GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
  if (!pointer)
    return false;
  GdkWindow* root = gdk_screen_get_root_window(gdk_screen_get_default());
  GdkModifierType mask = static_cast<GdkModifierType>(0);
  gdk_device_get_state(pointer, root, nullptr, &mask);
  return (mask & GDK_BUTTON1_MASK) != 0;
}

}  // namespace

bool CanStartFileDragLinux() {
  bool ok = false;
  GtkRunSync([&] { ok = gdk_display_get_default() != nullptr; });
  return ok;
}

void StartFileDragLinux(std::function<void*()> source_widget,
                        DragOutRequest* req) {
  GtkRunAsync([source_widget, req] {
    GtkWidget* widget =
        source_widget ? static_cast<GtkWidget*>(source_widget()) : nullptr;
    std::unique_ptr<DragOutRequest> owned(req);
    GdkDisplay* display = gdk_display_get_default();
    // A window that closed meanwhile: no source.
    if (source_widget && !widget) {
      owned->Finish(LAUFEY_DRAG_RESULT_FAILED);
      return;
    }
    // Held for the drag; dropped in EndDrag (or right here on failure).
    GtkWidget* source = widget ? widget : DragSourceWidget();
    g_object_ref(source);
    // Wayland reports no pointer state outside an event; the compositor
    // refuses a drag without a held button there anyway.
    if (!display || g_drag || (widget && !gtk_widget_get_realized(widget)) ||
        (!IsWayland(display) && !LeftButtonDown(display))) {
      g_object_unref(source);
      owned->Finish(LAUFEY_DRAG_RESULT_FAILED);
      return;
    }
    auto* d = new LinuxDrag();
    d->req = std::move(owned);
    d->source = source;  // the reference taken above, dropped in EndDrag
    GtkTargetList* targets = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add_uri_targets(targets, 0);
    d->get_id = g_signal_connect(d->source, "drag-data-get",
                                 G_CALLBACK(OnDragDataGet), nullptr);
    d->failed_id = g_signal_connect(d->source, "drag-failed",
                                    G_CALLBACK(OnDragFailed), nullptr);
    d->end_id =
        g_signal_connect(d->source, "drag-end", G_CALLBACK(OnDragEnd), nullptr);
    g_drag = d;
    d->context = gtk_drag_begin_with_coordinates(
        d->source, targets, GDK_ACTION_COPY, 1, nullptr, -1, -1);
    gtk_target_list_unref(targets);
    if (!d->context) {
      EndDrag(LAUFEY_DRAG_RESULT_FAILED);
      return;
    }
    GdkPixbuf* icon = nullptr;
    if (!d->req->icon_png.empty()) {
      GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
      if (loader) {
        if (gdk_pixbuf_loader_write(loader, d->req->icon_png.data(),
                                    d->req->icon_png.size(), nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr)) {
          icon = gdk_pixbuf_loader_get_pixbuf(loader);
          if (icon)
            g_object_ref(icon);
        } else {
          gdk_pixbuf_loader_close(loader, nullptr);
        }
        g_object_unref(loader);
      }
    }
    if (icon) {
      gtk_drag_set_icon_pixbuf(d->context, icon, gdk_pixbuf_get_width(icon) / 2,
                               gdk_pixbuf_get_height(icon) / 2);
      g_object_unref(icon);
    } else {
      gtk_drag_set_icon_name(d->context, "text-x-generic", 0, 0);
    }
  });
}

// ===========================================================================
// File dialogs
// ===========================================================================

namespace {

const UiRunner& GtkRunner() {
  static const UiRunner runner = [](std::function<void()> fn) {
    GtkRunAsync(std::move(fn));
  };
  return runner;
}

// "*.png" matching either case, as GTK 3's glob patterns are case-sensitive.
std::string CaseInsensitiveGlob(const std::string& ext) {
  std::string out = "*.";
  for (unsigned char c : ext) {
    if (g_ascii_isalpha(c)) {
      out += '[';
      out += static_cast<char>(g_ascii_tolower(c));
      out += static_cast<char>(g_ascii_toupper(c));
      out += ']';
    } else if (c == '[' || c == ']' || c == '*' || c == '?' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

class LinuxFileDialog : public FileDialogPlatform {
 public:
  LinuxFileDialog(uint32_t id, GtkWindow* parent, FileDialogRequest req)
      : id_(id), parent_(parent), req_(std::move(req)) {}
  ~LinuxFileDialog() override {
    if (accept_timer_)
      g_source_remove(accept_timer_);
    if (native_)
      g_object_unref(native_);
  }

  bool Show() override {
    GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
    const char* default_title = "Open";
    if (req_.kind == LAUFEY_FILE_DIALOG_SAVE) {
      action = GTK_FILE_CHOOSER_ACTION_SAVE;
      default_title = "Save";
    } else if (req_.ChoosesDirectories()) {
      // GTK has no files-and-folders mode; a directory request picks
      // directories.
      action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
      default_title = "Select Folder";
    }
    native_ = gtk_file_chooser_native_new(
        req_.title.empty() ? default_title : req_.title.c_str(), parent_,
        action, req_.button_label.empty() ? nullptr : req_.button_label.c_str(),
        nullptr);
    if (!native_)
      return false;
    GtkFileChooser* chooser = GTK_FILE_CHOOSER(native_);
    gtk_file_chooser_set_select_multiple(chooser, req_.Multiple());
    gtk_file_chooser_set_show_hidden(
        chooser, (req_.flags & LAUFEY_FILE_DIALOG_SHOW_HIDDEN) != 0);
    gtk_file_chooser_set_local_only(chooser, TRUE);
    if (req_.kind == LAUFEY_FILE_DIALOG_SAVE) {
      gtk_file_chooser_set_do_overwrite_confirmation(
          chooser, (req_.flags & LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM) == 0);
      gtk_file_chooser_set_create_folders(chooser, TRUE);
    }
    if (action != GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER) {
      for (const auto& f : req_.filters) {
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, f.name.c_str());
        for (const auto& ext : f.extensions) {
          if (ext == "*")
            gtk_file_filter_add_pattern(filter, "*");
          else
            gtk_file_filter_add_pattern(filter,
                                        CaseInsensitiveGlob(ext).c_str());
        }
        gtk_file_chooser_add_filter(chooser, filter);  // takes the ref
      }
    }
    std::string dir, name;
    SplitDefaultPath(req_.default_path, &dir, &name);
    if (!dir.empty())
      gtk_file_chooser_set_current_folder(chooser, dir.c_str());
    if (!name.empty() && req_.kind == LAUFEY_FILE_DIALOG_SAVE)
      gtk_file_chooser_set_current_name(chooser, name.c_str());
    g_signal_connect(native_, "response", G_CALLBACK(OnResponseThunk), this);
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(native_), parent_ != nullptr);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native_));
    return true;
  }

  void Cancel() override {
    if (done_ || !native_)
      return;
    // hide() closes the dialog without a "response"; report it ourselves.
    GtkNativeDialog* native = GTK_NATIVE_DIALOG(native_);
    g_object_ref(native);
    OnResponse(GTK_RESPONSE_CANCEL);
    gtk_native_dialog_hide(native);
    g_object_unref(native);
  }

  bool TestAccept(const std::string& path) override {
    if (done_ || !native_)
      return false;
    GtkFileChooser* chooser = GTK_FILE_CHOOSER(native_);
    if (!path.empty()) {
      if (req_.kind == LAUFEY_FILE_DIALOG_SAVE) {
        std::string dir, name;
        SplitDefaultPath(path, &dir, &name);
        if (!dir.empty())
          gtk_file_chooser_set_current_folder(chooser, dir.c_str());
        if (!name.empty())
          gtk_file_chooser_set_current_name(chooser, name.c_str());
      } else {
        gtk_file_chooser_set_filename(chooser, path.c_str());
      }
    }
    // The chooser loads the folder asynchronously before the selection is
    // readable; accept a moment later (OnResponse and the destructor remove
    // the timer, so it never outlives the dialog).
    if (!accept_timer_)
      accept_timer_ = g_timeout_add(500, AcceptNowThunk, this);
    return true;
  }

 private:
  static gboolean AcceptNowThunk(gpointer data) {
    auto* self = static_cast<LinuxFileDialog*>(data);
    self->accept_timer_ = 0;
    if (!self->done_ && self->native_) {
      // Read the selection before hiding: a hidden portal dialog has none.
      GtkNativeDialog* native = GTK_NATIVE_DIALOG(self->native_);
      g_object_ref(native);
      self->OnResponse(GTK_RESPONSE_ACCEPT);
      gtk_native_dialog_hide(native);
      g_object_unref(native);
    }
    return G_SOURCE_REMOVE;
  }

  static void OnResponseThunk(GtkNativeDialog*, gint response, gpointer data) {
    static_cast<LinuxFileDialog*>(data)->OnResponse(response);
  }

  void OnResponse(gint response) {
    if (done_)
      return;
    done_ = true;
    if (accept_timer_) {
      g_source_remove(accept_timer_);
      accept_timer_ = 0;
    }
    std::vector<std::string> paths;
    if (response == GTK_RESPONSE_ACCEPT) {
      GSList* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(native_));
      for (GSList* l = files; l; l = l->next) {
        if (l->data)
          paths.emplace_back(static_cast<char*>(l->data));
        g_free(l->data);
      }
      g_slist_free(files);
    }
    FileDialogFinish(id_,
                     response == GTK_RESPONSE_ACCEPT
                         ? LAUFEY_FILE_DIALOG_ACCEPTED
                         : LAUFEY_FILE_DIALOG_CANCELLED,
                     paths);
  }

  uint32_t id_;
  GtkWindow* parent_;
  FileDialogRequest req_;
  GtkFileChooserNative* native_ = nullptr;
  bool done_ = false;
  guint accept_timer_ = 0;
};

// --- xdg-desktop-portal's FileChooser ------------------------------------

constexpr char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kFileChooserInterface[] = "org.freedesktop.portal.FileChooser";
constexpr char kRequestInterface[] = "org.freedesktop.portal.Request";
// The first call may start xdg-desktop-portal.
constexpr int kPortalTimeoutMs = 3000;

// The portal's FileChooser version, asked once per process (-1: not yet).
std::atomic<int> g_portal_chooser_version{-1};

GDBusConnection* PortalBus() {
  const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  bool have = address && *address;
  if (!have && runtime && *runtime) {
    std::string bus = std::string(runtime) + "/bus";
    have = g_file_test(bus.c_str(), G_FILE_TEST_EXISTS);
  }
  if (!have)
    return nullptr;  // never autolaunch a bus
  return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

// Calls `then(version)` on the GTK thread with the portal's FileChooser
// version (0: none), asking the portal once, asynchronously (it may start).
void WithPortalFileChooserVersion(std::function<void(uint32_t)> then) {
  int known = g_portal_chooser_version.load();
  if (known >= 0) {
    then(static_cast<uint32_t>(known));
    return;
  }
  GDBusConnection* bus = PortalBus();
  if (!bus) {
    g_portal_chooser_version = 0;
    then(0);
    return;
  }
  auto* pending = new std::function<void(uint32_t)>(std::move(then));
  g_dbus_connection_call(
      bus, kPortalName, kPortalPath, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", kFileChooserInterface, "version"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, kPortalTimeoutMs, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::function<void(uint32_t)>> then(
            static_cast<std::function<void(uint32_t)>*>(data));
        GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                    result, nullptr);
        uint32_t version = 0;
        if (r) {
          GVariant* inner = nullptr;
          g_variant_get(r, "(v)", &inner);
          if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32))
            version = g_variant_get_uint32(inner);
          if (inner)
            g_variant_unref(inner);
          g_variant_unref(r);
        }
        g_portal_chooser_version = static_cast<int>(version);
        (*then)(version);
      },
      pending);
  g_object_unref(bus);
}

// "x11:<xid>" for a realized GtkWindow on X11; on Wayland, `done` gets
// "wayland:<handle>" once GDK has exported the toplevel (xdg-foreign); "" for
// none. Calls `done` exactly once, on the GTK thread.
void PortalParentFor(GtkWindow* window, std::function<void(std::string)> done) {
  GdkWindow* gdk = window ? gtk_widget_get_window(GTK_WIDGET(window)) : nullptr;
  if (!gdk) {
    done("");
    return;
  }
#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_WINDOW(gdk)) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "x11:%lx",
                  static_cast<unsigned long>(gdk_x11_window_get_xid(gdk)));
    done(buf);
    return;
  }
#endif
#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_WINDOW(gdk)) {
    auto* pending = new std::function<void(std::string)>(std::move(done));
    if (gdk_wayland_window_export_handle(
            gdk,
            [](GdkWindow*, const char* handle, gpointer data) {
              std::unique_ptr<std::function<void(std::string)>> d(
                  static_cast<std::function<void(std::string)>*>(data));
              (*d)(handle ? std::string("wayland:") + handle : std::string());
            },
            pending, nullptr)) {
      return;
    }
    // No xdg-foreign on this compositor: an app-level dialog.
    std::unique_ptr<std::function<void(std::string)>> d(pending);
    (*d)("");
    return;
  }
#endif
  done("");
}

// The request path the portal will use for `token` (the portal spec's
// /org/freedesktop/portal/desktop/request/SENDER/TOKEN, SENDER being our
// unique name without the ':' and with '.' as '_'), so the Response can be
// subscribed to before the call (a fast answer would otherwise be lost).
std::string ExpectedRequestPath(GDBusConnection* bus,
                                const std::string& token) {
  std::string sender = g_dbus_connection_get_unique_name(bus);
  if (!sender.empty() && sender[0] == ':')
    sender.erase(0, 1);
  for (char& c : sender) {
    if (c == '.')
      c = '_';
  }
  return std::string(kPortalPath) + "/request/" + sender + "/" + token;
}

// The portal's filters (a(sa(us)): a name and globs) for the request's.
GVariant* PortalFilters(const FileDialogRequest& req) {
  GVariantBuilder filters;
  g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
  for (const auto& f : req.filters) {
    GVariantBuilder globs;
    g_variant_builder_init(&globs, G_VARIANT_TYPE("a(us)"));
    for (const auto& ext : f.extensions) {
      std::string glob =
          ext == "*" ? std::string("*") : CaseInsensitiveGlob(ext);
      g_variant_builder_add(&globs, "(us)", 0u, glob.c_str());
    }
    g_variant_builder_add(&filters, "(sa(us))", f.name.c_str(), &globs);
  }
  return g_variant_builder_end(&filters);
}

// What one portal request shares with its callbacks (which may arrive after
// the dialog object is gone).
struct PortalRequest {
  uint32_t id = 0;
  GDBusConnection* bus = nullptr;  // owned
  std::string handle;              // the request object, once known
  std::string subscribed;          // the path the Response is followed on
  guint subscription = 0;
  bool done = false;
  bool cancelled = false;         // closed before the portal answered
  GdkWindow* exported = nullptr;  // a Wayland export to drop at the end

  ~PortalRequest() {
    if (subscription)
      g_dbus_connection_signal_unsubscribe(bus, subscription);
#ifdef GDK_WINDOWING_WAYLAND
    if (exported) {
      gdk_wayland_window_unexport_handle(exported);
      g_object_unref(exported);
    }
#endif
    if (bus)
      g_object_unref(bus);
  }

  // Request.Close on the request object: the one the portal named, or,
  // before it answered the call, the path it will use (the handle_token's),
  // which works once the portal has exported it; OnCalled closes the named
  // one again when the answer comes. Flushed at once, so the Close is on the
  // wire before the cancel is reported.
  void Close() {
    const std::string& path = handle.empty() ? subscribed : handle;
    if (path.empty())
      return;
    g_dbus_connection_call(bus, kPortalName, path.c_str(), kRequestInterface,
                           "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE,
                           -1, nullptr, nullptr, nullptr);
    g_dbus_connection_flush_sync(bus, nullptr, nullptr);
  }

  void Finish(int status, const std::vector<std::string>& paths) {
    if (done)
      return;
    done = true;
    FileDialogFinish(id, status, paths);
  }
};

using PortalRequestRef = std::shared_ptr<PortalRequest>;

// The Response signal on a request object: (u response, a{sv} results).
void OnPortalResponse(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar*, GVariant* params,
                      gpointer data) {
  PortalRequestRef req = *static_cast<PortalRequestRef*>(data);
  if (req->done || !g_variant_is_of_type(params, G_VARIANT_TYPE("(ua{sv})")))
    return;
  guint32 response = 2;
  GVariant* results = nullptr;
  g_variant_get(params, "(u@a{sv})", &response, &results);
  std::vector<std::string> paths;
  if (response == 0 && results) {
    GVariant* uris =
        g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
    if (uris) {
      gsize n = 0;
      const gchar** list = g_variant_get_strv(uris, &n);
      for (gsize i = 0; i < n; ++i) {
        GFile* file = g_file_new_for_uri(list[i]);
        gchar* path = g_file_get_path(file);  // null for a non-local URI
        if (path)
          paths.emplace_back(path);
        g_free(path);
        g_object_unref(file);
      }
      g_free(list);
      g_variant_unref(uris);
    }
  }
  if (results)
    g_variant_unref(results);
  // 0: chosen; 1: the user cancelled; 2: ended some other way.
  req->Finish(response == 0 ? LAUFEY_FILE_DIALOG_ACCEPTED
                            : LAUFEY_FILE_DIALOG_CANCELLED,
              paths);
}

void SubscribeResponse(const PortalRequestRef& req, const std::string& path) {
  if (req->subscription)
    g_dbus_connection_signal_unsubscribe(req->bus, req->subscription);
  req->subscribed = path;
  req->subscription = g_dbus_connection_signal_subscribe(
      req->bus, kPortalName, kRequestInterface, "Response", path.c_str(),
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE, OnPortalResponse,
      new PortalRequestRef(req),
      [](gpointer data) { delete static_cast<PortalRequestRef*>(data); });
}

// A Linux file dialog: attached to the slot at once (so a cancel or a test
// response that comes while the portal is asked is never lost), it picks the
// portal's FileChooser or GTK's chooser once the portal's FileChooser version
// is known (ChooseFileChooser), and shows that.
class LinuxChooserDialog : public FileDialogPlatform {
 public:
  LinuxChooserDialog(uint32_t id, GtkWindow* parent,
                     PortalParentResolver portal_parent, FileDialogRequest req)
      : id_(id),
        parent_(parent),
        portal_parent_(std::move(portal_parent)),
        req_(std::move(req)),
        alive_(std::make_shared<bool>(true)) {
    if (parent_)
      g_object_ref(parent_);
  }
  ~LinuxChooserDialog() override {
    *alive_ = false;
    if (parent_)
      g_object_unref(parent_);
  }

  bool Show() override {
    std::shared_ptr<bool> alive = alive_;
    WithPortalFileChooserVersion([this, alive](uint32_t version) {
      if (!*alive || finished_)
        return;
      FileChooserChoice choice =
          ChooseFileChooser(version, &req_, std::getenv("LAUFEY_FILE_CHOOSER"));
      if (choice.portal && !pending_accept_)
        ShowPortal();
      else
        ShowGtk();
    });
    return true;
  }

  void Cancel() override {
    if (gtk_)
      return gtk_->Cancel();
    if (finished_)
      return;
    if (state_) {
      if (state_->done)
        return;
      // The request object may not be known yet: it is closed as soon as
      // the portal names it (OnCalled).
      state_->cancelled = true;
      state_->Close();
      state_->Finish(LAUFEY_FILE_DIALOG_CANCELLED, {});
      return;
    }
    // Still asking the portal (or exporting the parent): nothing shown yet.
    finished_ = true;
    FileDialogFinish(id_, LAUFEY_FILE_DIALOG_CANCELLED, {});
  }

  bool TestAccept(const std::string& path) override {
    if (gtk_)
      return gtk_->TestAccept(path);
    if (state_)
      return false;  // the desktop's own dialog can't be driven from here
    // Not shown yet: a test that answers this early gets GTK's chooser (the
    // only one it can drive), accepted once it is up.
    pending_accept_ = path;
    return true;
  }

 private:
  void ShowGtk() {
    gtk_ = std::make_unique<LinuxFileDialog>(id_, parent_, req_);
    if (!gtk_->Show()) {
      finished_ = true;
      FileDialogFinish(id_, LAUFEY_FILE_DIALOG_FAILED, {});
      return;
    }
    if (pending_accept_)
      gtk_->TestAccept(*pending_accept_);
  }

  void ShowPortal() {
    GDBusConnection* bus = PortalBus();
    if (!bus)
      return ShowGtk();
    state_ = std::make_shared<PortalRequest>();
    state_->id = id_;
    state_->bus = bus;
    if (portal_parent_)
      return Call(portal_parent_());
    std::shared_ptr<bool> alive = alive_;
    PortalRequestRef state = state_;
    GtkWindow* parent = parent_;
    PortalParentFor(parent, [this, alive, state, parent](std::string handle) {
#ifdef GDK_WINDOWING_WAYLAND
      if (handle.rfind("wayland:", 0) == 0 && parent) {
        GdkWindow* gdk = gtk_widget_get_window(GTK_WIDGET(parent));
        if (gdk)
          state->exported = static_cast<GdkWindow*>(g_object_ref(gdk));
      }
#else
      (void)parent;
#endif
      // Cancelled while the handle was exported: nothing to show.
      if (!*alive || state->done)
        return;
      Call(handle);
    });
  }

  // The OpenFile / SaveFile call, with the parent window's identifier.
  void Call(const std::string& parent_handle) {
    static std::atomic<unsigned> counter{0};
    std::string token =
        "laufey" + std::to_string(getpid()) + "_" + std::to_string(++counter);
    SubscribeResponse(state_, ExpectedRequestPath(state_->bus, token));

    bool save = req_.kind == LAUFEY_FILE_DIALOG_SAVE;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(token.c_str()));
    if (!req_.button_label.empty()) {
      g_variant_builder_add(&options, "{sv}", "accept_label",
                            g_variant_new_string(req_.button_label.c_str()));
    }
    g_variant_builder_add(&options, "{sv}", "modal",
                          g_variant_new_boolean(!parent_handle.empty()));
    if (!save) {
      g_variant_builder_add(&options, "{sv}", "multiple",
                            g_variant_new_boolean(req_.Multiple()));
      if (req_.ChoosesDirectories()) {
        g_variant_builder_add(&options, "{sv}", "directory",
                              g_variant_new_boolean(TRUE));
      }
    }
    if (!req_.ChoosesDirectories() && !req_.filters.empty())
      g_variant_builder_add(&options, "{sv}", "filters", PortalFilters(req_));
    std::string dir, name;
    SplitDefaultPath(req_.default_path, &dir, &name);
    if (!dir.empty()) {
      g_variant_builder_add(&options, "{sv}", "current_folder",
                            g_variant_new_bytestring(dir.c_str()));
    }
    if (save && !name.empty()) {
      g_variant_builder_add(&options, "{sv}", "current_name",
                            g_variant_new_string(name.c_str()));
    }
    const char* title = !req_.title.empty()         ? req_.title.c_str()
                        : save                      ? "Save"
                        : req_.ChoosesDirectories() ? "Select Folder"
                                                    : "Open";
    auto* pending = new Pending{alive_, this, state_};
    g_dbus_connection_call(
        state_->bus, kPortalName, kPortalPath, kFileChooserInterface,
        save ? "SaveFile" : "OpenFile",
        g_variant_new("(ss@a{sv})", parent_handle.c_str(), title,
                      g_variant_builder_end(&options)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, kPortalTimeoutMs * 4,
        nullptr, OnCalled, pending);
  }

  struct Pending {
    std::shared_ptr<bool> alive;
    LinuxChooserDialog* dialog;
    PortalRequestRef state;
  };

  // The portal answered the call with the request object (or an error).
  static void OnCalled(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
    PortalRequestRef state = pending->state;
    GError* error = nullptr;
    GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                result, &error);
    if (!r) {
      std::fprintf(stderr,
                   "laufey: the portal's FileChooser failed (%s); using GTK's "
                   "file chooser\n",
                   error ? error->message : "no answer");
      g_clear_error(&error);
      // Not finished, so still the slot's dialog: shown by GTK instead.
      if (!state->done && *pending->alive)
        pending->dialog->FallBackToGtk();
      return;
    }
    const gchar* handle = nullptr;
    g_variant_get(r, "(&o)", &handle);
    std::string path = handle ? handle : "";
    g_variant_unref(r);
    state->handle = path;
    if (state->cancelled) {
      // Cancelled before the answer: the Close sent then may have come before
      // the portal exported the request (or went to the token's path, which
      // a portal older than 0.9 doesn't use). Close the named one now.
      state->Close();
      return;
    }
    // A portal older than 0.9 ignores handle_token and picks its own path:
    // follow that one.
    if (!state->done && !path.empty() && path != state->subscribed)
      SubscribeResponse(state, path);
  }

  void FallBackToGtk() {
    g_portal_chooser_version = 0;  // don't ask it again in this process
    if (state_ && state_->subscription) {
      g_dbus_connection_signal_unsubscribe(state_->bus, state_->subscription);
      state_->subscription = 0;
    }
    state_.reset();
    ShowGtk();
  }

  uint32_t id_;
  GtkWindow* parent_;
  PortalParentResolver portal_parent_;
  FileDialogRequest req_;
  std::shared_ptr<bool> alive_;  // false once destroyed (late callbacks)
  bool finished_ = false;        // cancelled before anything was shown
  std::optional<std::string> pending_accept_;
  PortalRequestRef state_;
  std::unique_ptr<LinuxFileDialog> gtk_;
};

}  // namespace

uint32_t ShowFileDialogLinux(ParentResolver parent,
                             const laufey_file_dialog_options_t* options,
                             laufey_file_dialog_result_fn callback,
                             void* user_data,
                             PortalParentResolver portal_parent) {
  return ShowFileDialogCommon(
      options, callback, user_data, GtkRunner(),
      [parent, portal_parent](uint32_t id, const FileDialogRequest& request) {
        if (!gdk_display_get_default()) {
          FileDialogFinish(id, LAUFEY_FILE_DIALOG_FAILED, {});
          return;
        }
        GtkWindow* window =
            parent ? static_cast<GtkWindow*>(parent()) : nullptr;
        auto* dialog =
            new LinuxChooserDialog(id, window, portal_parent, request);
        FileDialogAttach(id, dialog);
        if (!dialog->Show())
          FileDialogFinish(id, LAUFEY_FILE_DIALOG_FAILED, {});
      });
}

int PortalFileChooserVersionForTesting() {
  return g_portal_chooser_version.load();
}

void ResetPortalFileChooserForTesting() {
  g_portal_chooser_version = -1;
}

bool CancelFileDialogLinux(uint32_t dialog_id) {
  return FileDialogCancel(dialog_id, GtkRunner());
}

bool TestFileDialogRespondLinux(int action, const char* path) {
  return FileDialogTestRespond(action, path, GtkRunner());
}

}  // namespace laufey_common

// exit() for the whole process: the host executable exports it (see
// backend-common/CMakeLists.txt), so the runtime library's exit() (the
// runtime's Deno.exit() before the app started, a native addon's exit())
// and every other library's call reach this one first. With the UI exit
// guard installed it parks the UI thread before the C library's exit() runs
// a single exit handler. The guard's own handler can't do that: exit
// handlers run in reverse order, so those a library registered after the
// guard (a static built on first use, such as WebKitGTK's GBM device, and
// libraries loaded later, such as Mesa's) ran first, while the UI thread
// still painted. Destroying the GBM device unloaded libgbm's backend, and
// the next frame (AcceleratedBackingStore::BufferGBM::didUpdateContents ->
// gbm_bo_map) called into the unmapped library. An exit the C library makes
// itself (main returning, on the UI thread) still goes through the guard's
// handler.
extern "C" __attribute__((visibility("default"))) void exit(
    int status) noexcept {
  if (laufey_common::g_exit_guard_installed.load())
    laufey_common::ParkUiThreadForExit(status);
  if (laufey_common::ExitFunction libc_exit = laufey_common::LibcExit())
    libc_exit(status);
  // No C library exit() to call (not reached in practice): end without the
  // exit handlers rather than not at all.
  fflush(nullptr);
  _exit(status);
}
