// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// An XDND drag source for the Linux native e2e (io_checks.rs): a small GTK
// window whose whole area drags the files named on the command line as
// text/uri-list, the way a file manager does. The battery starts it on the
// run's X server, drives a real drag out of it into a laufey window with
// xdotool (XTEST pointer events), and checks that the drop reaches
// on_file_drop through the backend's own XDND handling.
//
//   laufey_xdnd_source <x> <y> <path>...
//
// Prints "ready <x> <y> <width> <height>" (the window's area in root-window
// pixels) once it is mapped, then "pressed <button>", "drag-begin" and
// "drag-end" as the drag goes, and exits after a drag or after 60 seconds.

#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_uris;

void OnDragDataGet(GtkWidget*, GdkDragContext*, GtkSelectionData* data, guint,
                   guint, gpointer) {
  std::vector<gchar*> uris;
  for (auto& u : g_uris)
    uris.push_back(const_cast<gchar*>(u.c_str()));
  uris.push_back(nullptr);
  gtk_selection_data_set_uris(data, uris.data());
}

gboolean OnButtonPress(GtkWidget*, GdkEventButton* event, gpointer) {
  std::printf("pressed %d\n", event->button);
  std::fflush(stdout);
  return FALSE;
}

void OnDragBegin(GtkWidget*, GdkDragContext*, gpointer) {
  std::printf("drag-begin\n");
  std::fflush(stdout);
}

void OnDragEnd(GtkWidget*, GdkDragContext*, gpointer) {
  std::printf("drag-end\n");
  std::fflush(stdout);
  // Let the XDND finish handshake complete before going away.
  g_timeout_add(
      500,
      [](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
      },
      nullptr);
}

gboolean ReportReady(gpointer data) {
  GtkWidget* window = static_cast<GtkWidget*>(data);
  GdkWindow* gdk = gtk_widget_get_window(window);
  if (!gdk || !gdk_window_is_viewable(gdk))
    return G_SOURCE_CONTINUE;
  int x = 0, y = 0;
  gdk_window_get_origin(gdk, &x, &y);
  int scale = gdk_window_get_scale_factor(gdk);
  std::printf("ready %d %d %d %d\n", x * scale, y * scale,
              gdk_window_get_width(gdk) * scale,
              gdk_window_get_height(gdk) * scale);
  std::fflush(stdout);
  return G_SOURCE_REMOVE;
}

}  // namespace

int main(int argc, char** argv) {
  gtk_init(&argc, &argv);
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <x> <y> <path>...\n", argv[0]);
    return 2;
  }
  for (int i = 3; i < argc; ++i) {
    gchar* uri = g_filename_to_uri(argv[i], nullptr, nullptr);
    if (!uri) {
      std::fprintf(stderr, "not an absolute path: %s\n", argv[i]);
      return 2;
    }
    g_uris.push_back(uri);
    g_free(uri);
  }

  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "laufey-xdnd-source");
  gtk_window_set_default_size(GTK_WINDOW(window), 160, 120);
  gtk_window_move(GTK_WINDOW(window), std::atoi(argv[1]), std::atoi(argv[2]));
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

  GtkWidget* box = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(window), box);
  gtk_container_add(GTK_CONTAINER(box), gtk_label_new("drag me"));
  GtkTargetEntry target = {const_cast<gchar*>("text/uri-list"), 0, 0};
  gtk_drag_source_set(box, GDK_BUTTON1_MASK, &target, 1, GDK_ACTION_COPY);
  g_signal_connect(box, "drag-data-get", G_CALLBACK(OnDragDataGet), nullptr);
  g_signal_connect(box, "drag-end", G_CALLBACK(OnDragEnd), nullptr);
  g_signal_connect(box, "drag-begin", G_CALLBACK(OnDragBegin), nullptr);
  g_signal_connect(box, "button-press-event", G_CALLBACK(OnButtonPress),
                   nullptr);

  gtk_widget_show_all(window);
  g_timeout_add(50, ReportReady, window);
  g_timeout_add_seconds(
      60,
      [](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
      },
      nullptr);
  gtk_main();
  return 0;
}
