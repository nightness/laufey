// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"
#include "laufey_auth_session.h"
#include "laufey_launch_args.h"
#include "laufey_notifications.h"
#include "laufey_single_instance.h"
#include "laufey_window.h"
#include "runtime_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// Brings the app to the front for a forwarded launch: restores and
// foregrounds this thread's front-most visible top-level window. Hidden
// windows stay hidden. UI thread (EnumThreadWindows sees its own windows).
static void ActivateApp(void*) {
  HWND target = nullptr;
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND hwnd, LPARAM out) -> BOOL {
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
            (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
          return TRUE;
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&target));
  if (!target)
    return;
  if (IsIconic(target))
    ShowWindow(target, SW_RESTORE);
  SetForegroundWindow(target);
}

// A headless worker (laufey_common::IsHeadlessWorkerLaunch): the runtime runs
// with no backend, no WebView2 and no window, then the process exits.
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  // LAUFEY_CWD is only for the Windows CEF host behind CEF's bootstrap
  // (cef/src/main_windows.cc); never pass it on to what the app starts.
  SetEnvironmentVariableW(L"LAUFEY_CWD", nullptr);
  // laufey's own options end at "--" (a registered URL scheme runs
  // `"<exe>" -- "%1"`, so a link can only add positional arguments), and a
  // packaged app (a launch file or a runtime next to the executable) loads
  // only the runtime next to its executable: never one the command line,
  // LAUFEY_RUNTIME_PATH or the working directory names. A development host
  // takes --runtime, then LAUFEY_RUNTIME_PATH, then the working-directory
  // fallbacks. See laufey_launch_args.h.
  std::vector<std::string> args;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
      for (int i = 1; i < argc; ++i)
        args.push_back(laufey_common::WideToUtf8(argv[i]));
      LocalFree(argv);
    }
  }
  laufey_common::SetProcessArgs(args);
  laufey_common::RuntimeChoice runtimeChoice =
      laufey_common::ResolveRuntimePath(
          args, {LaufeyFindColocatedRuntime()},
          {".\\runtime.dll", ".\\target\\debug\\hello.dll",
           ".\\target\\release\\hello.dll"});
  std::string runtimePath = runtimeChoice.path;
  // A packaged app without its runtime exits at once (no dialog to wait on).
  if (laufey_common::IsMissingPackagedRuntime(runtimeChoice)) {
    laufey_common::ReportMissingPackagedRuntime();
    return laufey_common::kMissingRuntimeExitCode;
  }

  // A headless worker (`<exe> run <script>`, a forked worker) runs before the
  // single-instance check: forwarded to a running instance it would never
  // run, and it must not hold the lock the app itself takes. WinMain has no
  // argv; `args` comes from GetCommandLineW() above.
  if (laufey_common::IsHeadlessWorkerLaunch(args)) {
    return run_headless(runtimePath);
  }

  // Single-instance mode (docs/deep-links.md): a second launch forwards its
  // command line to the running instance and exits here, before WebView2 or
  // the runtime starts. The arguments are read from GetCommandLineW().
  int single_instance_exit = 0;
  if (!laufey_common::SingleInstanceStartup(0, nullptr,
                                            &single_instance_exit)) {
    return single_instance_exit;
  }
  // Notifications (API 41): the Windows toast activator / the Linux
  // scheduler start before the runtime, so a click on a toast that launched
  // the app, or a notification scheduled for while it wasn't running, is
  // delivered.
  laufey_common::InitNotificationsAtLaunch();

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (runtimePath.empty()) {
    MessageBoxW(nullptr,
                L"No runtime library found.\nSet LAUFEY_RUNTIME_PATH or use "
                L"--runtime <path>",
                L"LAUFEY Webview Error", MB_OK | MB_ICONERROR);
    CoUninitialize();
    return 1;
  }

  LaufeyBackend* backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(backend);

  laufey_common::SecondInstanceUiHooks single_instance_hooks;
  single_instance_hooks.post = [](void* ctx, void (*task)(void*), void* data) {
    static_cast<LaufeyBackend*>(ctx)->PostUiTask(task, data);
  };
  single_instance_hooks.activate = ActivateApp;
  single_instance_hooks.ctx = backend;
  laufey_common::SetSecondInstanceUiHooks(single_instance_hooks);

  if (!loader->Load(runtimePath)) {
    // The path is UTF-8; show it through the wide API so non-ASCII
    // characters render correctly in the dialog.
    MessageBoxW(nullptr,
                (L"Failed to load runtime from: " +
                 laufey_common::Utf8ToWide(runtimePath))
                    .c_str(),
                L"LAUFEY Webview Error", MB_OK | MB_ICONERROR);
    laufey_common::SetSecondInstanceUiHooks({});
    delete backend;
    CoUninitialize();
    return 1;
  }

  if (!loader->Start()) {
    MessageBoxW(nullptr, L"Failed to start runtime", L"LAUFEY Webview Error",
                MB_OK | MB_ICONERROR);
    laufey_common::SetSecondInstanceUiHooks({});
    delete backend;
    CoUninitialize();
    return 1;
  }

  backend->Run();

  // The loop is over: UI tasks still queued are answered "not run" and an
  // auth session in progress ends cancelled, so a runtime thread waiting on
  // either is released before Shutdown waits for it.
  laufey_common::UiLoopEnded();
  loader->Shutdown();
  laufey_common::SetSecondInstanceUiHooks({});
  delete backend;

  CoUninitialize();
  // The web views are released (WebView2's own browser process writes the
  // profile): end the process with the app's exit code (exit_app's, else 0)
  // without the CRT exit's ExitProcess, which runs every DLL's detach code
  // after ending the other threads and can then wait for good on a COM call
  // into an apartment whose thread is gone (see cef/src/main_windows.cc).
  laufey_common::EndProcess(laufey_common::RequestedExitCode());
}
