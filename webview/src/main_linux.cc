// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_launch_config.h"
#include "laufey_auth_session.h"
#include "laufey_notifications.h"
#include "laufey_single_instance.h"
#include "runtime_loader.h"

#include <gtk/gtk.h>

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// Brings the app to the front for a forwarded launch: presents the focused
// (else the first visible) application window. Hidden windows stay hidden.
// GTK main thread.
static void ActivateApp(void*) {
  GList* toplevels = gtk_window_list_toplevels();
  GtkWindow* target = nullptr;
  for (GList* l = toplevels; l; l = l->next) {
    GtkWindow* window = GTK_WINDOW(l->data);
    if (gtk_window_get_window_type(window) != GTK_WINDOW_TOPLEVEL ||
        !gtk_widget_get_visible(GTK_WIDGET(window)))
      continue;
    if (!target || gtk_window_is_active(window))
      target = window;
  }
  g_list_free(toplevels);
  if (target)
    gtk_window_present(target);
}

int main(int argc, char* argv[]) {
  // Single-instance mode (docs/deep-links.md): a second launch forwards its
  // arguments to the running instance and exits here, before any web
  // engine or the runtime starts.
  int single_instance_exit = 0;
  if (!laufey_common::SingleInstanceStartup(argc, argv,
                                            &single_instance_exit)) {
    return single_instance_exit;
  }
  // Notifications (API 41): the Windows toast activator / the Linux
  // scheduler start before the runtime, so a click on a toast that launched
  // the app, or a notification scheduled for while it wasn't running, is
  // delivered.
  laufey_common::InitNotificationsAtLaunch();

  gtk_init(&argc, &argv);

  laufey_common::SecondInstanceUiHooks single_instance_hooks;
  single_instance_hooks.post = [](void*, void (*task)(void*), void* data) {
    struct Task {
      void (*task)(void*);
      void* data;
    };
    g_idle_add(
        [](gpointer p) -> gboolean {
          auto* t = static_cast<Task*>(p);
          t->task(t->data);
          delete t;
          return G_SOURCE_REMOVE;
        },
        new Task{task, data});
  };
  single_instance_hooks.activate = ActivateApp;
  laufey_common::SetSecondInstanceUiHooks(single_instance_hooks);

  // Application identity for the window manager. The embedder (e.g. deno
  // desktop) passes LAUFEY_APP_ID (the reverse-DNS identifier it also uses for
  // the `.desktop` file) or ships it as "appId" in laufey-launch.json next to
  // this binary (see laufey_launch_config.h), and LAUFEY_APP_NAME (the display
  // name). GTK derives a Wayland xdg_toplevel app_id from g_get_prgname() and
  // an X11 WM_CLASS from the program class, so set both to the app id —
  // otherwise they default to this backend binary's name and the compositor
  // shows a generic placeholder icon instead of the one from the matching
  // `<app_id>.desktop`.
  std::string appId = laufey_common::LaunchAppId();
  if (appId.empty()) {
    if (const char* env = getenv("LAUFEY_APP_NAME")) {
      if (*env) {
        appId = env;
      }
    }
  }
  if (!appId.empty()) {
    g_set_prgname(appId.c_str());
    gdk_set_program_class(appId.c_str());
  }

  // Default icon for all windows. Wayland ignores client-set window icons (it
  // relies on the app_id → `.desktop` match above), but X11 honors this, so a
  // dev run under X11 shows the configured icon directly.
  if (const char* iconPath = getenv("LAUFEY_APP_ICON")) {
    if (*iconPath) {
      GError* error = nullptr;
      if (!gtk_window_set_default_icon_from_file(iconPath, &error)) {
        if (error) {
          std::cerr << "Failed to load app icon '" << iconPath
                    << "': " << error->message << std::endl;
          g_error_free(error);
        }
      }
    }
  }

  std::string runtimePath;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      runtimePath = argv[++i];
    }
  }

  if (runtimePath.empty()) {
    const char* envPath = getenv("LAUFEY_RUNTIME_PATH");
    if (envPath) {
      runtimePath = envPath;
    }
  }

  if (runtimePath.empty()) {
    runtimePath = LaufeyFindColocatedRuntime();
  }

  if (runtimePath.empty()) {
    const char* searchPaths[] = {
        "./libruntime.so", "./target/debug/libhello.so",
        "./target/release/libhello.so", "/usr/lib/laufey/libruntime.so",
        "/usr/local/lib/laufey/libruntime.so"};
    for (const char* path : searchPaths) {
      if (access(path, F_OK) == 0) {
        runtimePath = path;
        break;
      }
    }
  }

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found. Set LAUFEY_RUNTIME_PATH or use "
                 "--runtime <path>"
              << std::endl;
    return 1;
  }

  LaufeyBackend* backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(backend);

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime from: " << runtimePath << std::endl;
    delete backend;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start runtime" << std::endl;
    delete backend;
    return 1;
  }

  backend->Run();

  // The loop is over: UI tasks still queued are answered "not run" and an
  // auth session in progress ends cancelled, so a runtime thread waiting on
  // either is released before Shutdown waits for it.
  laufey_common::UiLoopEnded();
  loader->Shutdown();
  delete backend;

  return 0;
}
