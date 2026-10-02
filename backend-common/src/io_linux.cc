// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Linux drag-out (a GTK drag source offering text/uri-list) and file dialogs
// (GtkFileChooserNative, which goes through xdg-desktop-portal when GTK
// decides to: inside Flatpak / Snap, or with GTK_USE_PORTAL=1), shared by the
// WebKitGTK and CEF backends. GTK is not thread-safe, so every entry point
// hops to the default GLib main context, which both backends run (WebKitGTK's
// gtk_main, and Chromium's glib message pump on CEF's UI thread).

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_sync_call.h"

#include <cstring>
#include <memory>
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

void GtkRunSync(const std::function<void()>& fn) {
  if (OnGtkThread()) {
    fn();
    return;
  }
  // `call` lives in this frame; Done() is the task's last access to it
  // (laufey_sync_call.h).
  SyncCall call;
  GtkRunAsync([&] {
    fn();
    call.Done();
  });
  call.Wait();
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

}  // namespace

uint32_t ShowFileDialogLinux(ParentResolver parent,
                             const laufey_file_dialog_options_t* options,
                             laufey_file_dialog_result_fn callback,
                             void* user_data) {
  return ShowFileDialogCommon(
      options, callback, user_data, GtkRunner(),
      [parent](uint32_t id, const FileDialogRequest& request) {
        if (!gdk_display_get_default()) {
          FileDialogFinish(id, LAUFEY_FILE_DIALOG_FAILED, {});
          return;
        }
        GtkWindow* window =
            parent ? static_cast<GtkWindow*>(parent()) : nullptr;
        auto* dialog = new LinuxFileDialog(id, window, request);
        FileDialogAttach(id, dialog);
        if (!dialog->Show())
          FileDialogFinish(id, LAUFEY_FILE_DIALOG_FAILED, {});
      });
}

bool CancelFileDialogLinux(uint32_t dialog_id) {
  return FileDialogCancel(dialog_id, GtkRunner());
}

bool TestFileDialogRespondLinux(int action, const char* path) {
  return FileDialogTestRespond(action, path, GtkRunner());
}

}  // namespace laufey_common
