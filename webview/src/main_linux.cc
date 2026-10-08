// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"
#include "laufey_launch_args.h"
#include "laufey_launch_config.h"
#include "laufey_auth_session.h"
#include "laufey_notifications.h"
#include "laufey_platform_features.h"
#include "laufey_single_instance.h"
#include "laufey_window.h"
#include "runtime_loader.h"

#include <gtk/gtk.h>

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

// The backend whose Quit a termination signal calls (GTK main thread).
static LaufeyBackend* g_backend = nullptr;

// SIGTERM, SIGINT and SIGHUP end the app through quit()'s path: the loop
// ends, the runtime is shut down and what the pages stored is written to
// disk (laufey_common::InstallTerminationSignalHandlers), instead of the
// default action killing the process without either.
static void RequestQuit() {
  if (g_backend)
    g_backend->Quit();
}

// A headless worker (laufey_common::IsHeadlessWorkerLaunch): the runtime runs
// with no backend, no GTK and no window, then the process exits.
static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(nullptr);
  if (runtimePath.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }
  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }
  // No UI loop in a headless worker: UI tasks are answered "not run" at
  // once instead of waiting for a loop that never runs.
  laufey_common::UiLoopEnded();
  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }
  // It ends when the runtime returns, however long that takes.
  loader->WaitForRuntime();
  loader->Shutdown();
  return 0;
}

int main(int argc, char* argv[]) {
  // D-Bus activation for a notification click (`<app id>.service` passes
  // --laufey-dbus-activated): noted, and left out of the copy of argv that
  // GTK and the single-instance forwarding get. The process's argv is never
  // changed (gtk_init edits the copy): the runtime library's .init_array
  // functions still get its original argc and argv. The runtime leaves the
  // argument out itself (laufey::args_os).
  // Deliberately leaked: whatever keeps a pointer into the copy (the
  // toolkit, at-exit handlers) can still read it during exit, whatever the
  // order static destructors run in.
  static auto* host_argv = new std::vector<char*>;
  laufey_common::SetDBusActivationLaunch(
      laufey_common::CopyArgvWithoutDBusActivationArg(argc, argv, host_argv));
  argc = static_cast<int>(host_argv->size()) - 1;
  argv = host_argv->data();

  // LAUFEY_CWD is only for the Windows CEF host behind CEF's bootstrap
  // (cef/src/main_windows.cc); never pass it on to what the app starts.
  unsetenv("LAUFEY_CWD");

  const std::vector<std::string> args(argv + 1, argv + argc);

  // A scheduled notification's systemd timer (`<exe> --laufey-notify <id>`,
  // docs/notifications.md): post it and exit, with no GTK, no web engine and
  // no runtime.
  std::string notify_id;
  if (laufey_common::ParseNotifyLaunch(args, &notify_id))
    return laufey_common::RunNotifyLaunch(notify_id);

  // The runtime library: a packaged app (a launch file, or a runtime next to
  // the executable) loads only the one next to its executable; a development
  // host takes --runtime (before "--"), then LAUFEY_RUNTIME_PATH, then the
  // working directory and system fallbacks. See laufey_launch_args.h.
  laufey_common::RuntimeChoice runtimeChoice =
      laufey_common::ResolveRuntimePath(
          args, {LaufeyFindColocatedRuntime()},
          {"./libruntime.so", "./target/debug/libhello.so",
           "./target/release/libhello.so", "/usr/lib/laufey/libruntime.so",
           "/usr/local/lib/laufey/libruntime.so"});
  if (laufey_common::IsMissingPackagedRuntime(runtimeChoice)) {
    laufey_common::ReportMissingPackagedRuntime();
    return laufey_common::kMissingRuntimeExitCode;
  }
  std::string runtimePath = runtimeChoice.path;

  // A headless worker (`<exe> run <script>`, a forked worker) runs before the
  // single-instance check: forwarded to a running instance it would never
  // run, and it must not hold the lock the app itself takes.
  if (laufey_common::IsHeadlessWorkerLaunch(args)) {
    return run_headless(runtimePath);
  }

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

  // WebKitGTK on an X11 display: frames in shared memory
  // (WEBKIT_DMABUF_RENDERER_FORCE_SHM=1) unless the user chose a renderer
  // setting. GTK 3 can't show a GPU buffer on X11, so WebKit's UI process
  // maps each GBM buffer to the CPU to draw it; shared memory moves the same
  // read back into the web process and keeps this process off libgbm (the
  // BufferGBM::didUpdateContents crashes on Mali / Panfrost came from
  // libgbm's teardown during exit(), which the exit guard now runs after
  // parking the UI thread). Before the first web view: WebKit reads it when
  // it starts the web process. See docs/backends.md.
  {
    GdkDisplay* display = gdk_display_get_default();
    bool x11 =
        display && g_strcmp0(G_OBJECT_TYPE_NAME(display), "GdkX11Display") == 0;
    if (laufey_common::ShouldForceWebKitShm(
            x11, [](const char* name) { return getenv(name); })) {
      setenv("WEBKIT_DMABUF_RENDERER_FORCE_SHM", "1", 0);
    }
  }

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

  g_backend = backend;
  laufey_common::InstallTerminationSignalHandlers(RequestQuit);

  backend->Run();

  // A signal from here on takes its default action.
  laufey_common::RemoveTerminationSignalHandlers();
  g_backend = nullptr;

  // The loop is over: UI tasks still queued are answered "not run" and an
  // auth session in progress ends cancelled, so a runtime thread waiting on
  // either is released before Shutdown waits for it.
  laufey_common::UiLoopEnded();
  loader->Shutdown();
  // What the pages stored goes to disk before the process ends.
  backend->FlushWebStorage();
  delete backend;

  // exit_app's code (API 46), else 0.
  return laufey_common::RequestedExitCode();
}
