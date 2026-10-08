// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "app.h"
#include "laufey_launch_args.h"
#include "laufey_launch_config.h"
#include "custom_schemes.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_auth_session.h"
#include "laufey_notifications.h"
#include "laufey_single_instance.h"
#include "laufey_window.h"
#include "renderer_app.h"
#include "runtime_loader.h"

void LaufeyOpenExternalURL(const std::string& url) {
  std::wstring wurl = laufey_common::Utf8ToWide(url);
  if (wurl.empty())
    return;
  ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

// Windows mouse/input monitor for CEF Views windows.
// CEF Views creates its own HWND; we hook into it after window creation.

static HHOOK g_mouse_hook = nullptr;

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode >= 0) {
    MOUSEHOOKSTRUCT* mhs = reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);
    RuntimeLoader* loader = RuntimeLoader::GetInstance();

    // Find the laufey window_id from the top-level HWND
    HWND topLevel = mhs->hwnd ? GetAncestor(mhs->hwnd, GA_ROOT) : nullptr;
    uint32_t window_id =
        topLevel ? loader->GetLaufeyIdForNativeHandle((void*)topLevel) : 0;

    POINT pt = mhs->pt;
    if (mhs->hwnd) {
      ScreenToClient(mhs->hwnd, &pt);
    }
    double x = static_cast<double>(pt.x);
    double y = static_cast<double>(pt.y);

    uint32_t modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
      modifiers |= LAUFEY_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000)
      modifiers |= LAUFEY_MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)
      modifiers |= LAUFEY_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
      modifiers |= LAUFEY_MOD_META;

    switch (wParam) {
      case WM_LBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_LEFT, x, y,
                                        modifiers, 1);
        break;
      case WM_LBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_LEFT, x, y,
                                        modifiers, 1);
        break;
      case WM_RBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_RIGHT, x, y,
                                        modifiers, 1);
        break;
      case WM_RBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_RIGHT, x, y,
                                        modifiers, 1);
        break;
      case WM_MBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_MIDDLE, x, y,
                                        modifiers, 1);
        break;
      case WM_MBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_MIDDLE, x, y,
                                        modifiers, 1);
        break;
      case WM_MOUSEMOVE:
        loader->DispatchMouseMoveEvent(window_id, x, y, modifiers);
        break;
      case WM_MOUSEWHEEL: {
        // In WH_MOUSE hook, wheel data is in MOUSEHOOKSTRUCTEX::mouseData
        MOUSEHOOKSTRUCTEX* mhsx = reinterpret_cast<MOUSEHOOKSTRUCTEX*>(lParam);
        short delta = HIWORD(mhsx->mouseData);
        double delta_y = static_cast<double>(delta) / WHEEL_DELTA;
        loader->DispatchWheelEvent(window_id, 0.0, delta_y, x, y, modifiers,
                                   LAUFEY_WHEEL_DELTA_LINE);
        break;
      }
    }
  }
  return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
}

void InstallNativeMouseMonitor() {
  if (g_mouse_hook)
    return;
  g_mouse_hook =
      SetWindowsHookExW(WH_MOUSE, MouseProc, nullptr, GetCurrentThreadId());
}

void RemoveNativeMouseMonitor() {
  if (g_mouse_hook) {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
}

// --- Headless / forked worker support ---

static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

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

// Combined app that handles both browser and renderer processes (single-exe
// model)
class LaufeyCombinedApp : public CefApp, public CefBrowserProcessHandler {
 public:
  LaufeyCombinedApp() : renderer_app_(new LaufeyRendererApp()) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return renderer_app_->GetRenderProcessHandler();
  }

  // "app" plus the schemes declared with --laufey-custom-schemes /
  // LAUFEY_CUSTOM_SCHEMES become standard, secure, fetch/CORS-enabled schemes
  // in every process (single-exe model: this runs in the browser and in each
  // subprocess). See custom_schemes.h.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override {
    laufey_schemes::RegisterAll(registrar);
  }

  void OnBeforeChildProcessLaunch(
      CefRefPtr<CefCommandLine> command_line) override {
    laufey_schemes::ForwardToChild(command_line);
  }

  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override {
    return LaufeyHandleAlreadyRunningAppRelaunch();
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    // Silence Chromium's background networking. The GCM (Google Cloud
    // Messaging) client tries to register on startup and logs noisy
    // `registration_request.cc ... PHONE_REGISTRATION_ERROR` /
    // `DEPRECATED_ENDPOINT` errors that have nothing to do with the app. A
    // webview-embedding desktop app doesn't use GCM, the component updater,
    // safebrowsing auto-update, etc., so disable the lot (matches what
    // Electron/Puppeteer do). Only the browser process needs the switch; CEF
    // propagates it to subprocesses.
    if (process_type.empty()) {
      LaufeyStripDeepLinkSwitches(command_line);
      command_line->AppendSwitch("disable-background-networking");
      // What --disable-background-networking leaves: the Chrome services
      // that still contact Google at startup (laufey_cef_network_quiet.h).
      LaufeyApplyNetworkQuietDefaults(command_line);
      LaufeyApplyInspectableToCommandLine(command_line);
    }
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    // backend-common's I/O thread (file dialogs, drag-out, clipboard change
    // events; its own STA thread, see laufey_io.h).
    laufey_common::WinIoInit();

    // Keep the handler alive for the lifetime of the app.
    // Backend_CreateWindow uses LaufeyHandler::GetInstance() from the runtime
    // thread, so the handler must outlive this function scope.
    static CefRefPtr<LaufeyHandler> handler(new LaufeyHandler());

    if (!g_runtime_path.empty()) {
      if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
        // WIN32 subsystem: stderr is usually detached, so also show a
        // dialog (matches the webview binary's load-failure behavior).
        std::cerr << "Failed to load runtime from: " << g_runtime_path
                  << std::endl;
        MessageBoxW(nullptr,
                    (L"Failed to load runtime from: " +
                     laufey_common::Utf8ToWide(g_runtime_path))
                        .c_str(),
                    L"LAUFEY Error", MB_OK | MB_ICONERROR);
        CefQuitMessageLoop();
        return;
      }
      // Defer Start() to the next message loop iteration.
      // OnContextInitialized runs during CefInitialize(), before
      // CefRunMessageLoop() has started.
      CefPostTask(TID_UI, base::BindOnce(
                              []() { RuntimeLoader::GetInstance()->Start(); }));
    } else {
      // No runtime: create a default window for demo
      uint32_t laufey_id = RuntimeLoader::GetInstance()->AllocateWindowId();
      g_pending_laufey_ids.push(laufey_id);
      CefBrowserSettings browser_settings;
      CefRefPtr<CefBrowserView> browser_view =
          CefBrowserView::CreateBrowserView(handler, "https://example.com",
                                            browser_settings, nullptr, nullptr,
                                            nullptr);
      CefWindow::CreateTopLevelWindow(
          new LaufeyWindowDelegate(browser_view, laufey_id));
    }
  }

 private:
  CefRefPtr<LaufeyRendererApp> renderer_app_;
  IMPLEMENT_REFCOUNTING(LaufeyCombinedApp);
};

// The host's entry point, in every process (browser, renderer, GPU, ...;
// single-exe model). `sandbox_info` is bootstrap.exe's sandbox information,
// or null for an unsandboxed build (-DUSE_SANDBOX=OFF), which then runs every
// child with no_sandbox.
static int LaufeyWinMain(HINSTANCE hInstance, void* sandbox_info) {
  // LAUFEY_CWD is for the host behind CEF's bootstrap (RunWinMain, which has
  // already consumed it): never pass it on to what the app starts.
  SetEnvironmentVariableW(L"LAUFEY_CWD", nullptr);
  CefMainArgs main_args(hInstance);

  std::vector<std::string> args;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i)
      args.push_back(laufey_common::WideToUtf8(argv[i]));
    if (argv)
      LocalFree(argv);
  }
  // A CEF subprocess (renderer, GPU, utility, ...) carries --type=; anything
  // else is the browser process or a headless worker.
  bool cef_child = false;
  for (const std::string& a : args) {
    if (a == "--")
      break;
    if (a.rfind("--type=", 0) == 0) {
      cef_child = true;
      break;
    }
  }

  if (!cef_child) {
    // laufey's own options end at "--" (a registered URL scheme runs
    // `"<exe>" -- "%1"`, so a link can only add positional arguments), and a
    // packaged app (a launch file or a runtime next to the executable) loads
    // only the runtime next to its executable: never one the command line or
    // LAUFEY_RUNTIME_PATH names. The arguments are also kept for the browser
    // process's command line hook, which drops a deep-link launch's Chromium
    // switches (LaufeyStripDeepLinkSwitches). See laufey_launch_args.h.
    laufey_common::SetProcessArgs(args);
    laufey_common::RuntimeChoice choice = laufey_common::ResolveRuntimePath(
        args, {LaufeyFindColocatedRuntime()}, {});
    // A packaged app without its runtime exits at once, before CEF starts.
    if (laufey_common::IsMissingPackagedRuntime(choice)) {
      laufey_common::ReportMissingPackagedRuntime();
      return laufey_common::kMissingRuntimeExitCode;
    }
    g_runtime_path = choice.path;

    // A headless / forked worker never loads CEF (libcef.dll is delay-loaded
    // and first called by CefExecuteProcess below): no Chromium threads or
    // files held open in a process that, like the full-app updater's helper,
    // may outlive or move the app's install directory.
    if (laufey_common::IsHeadlessWorkerLaunch(args)) {
      return run_headless(g_runtime_path);
    }
  }

  // Single-exe model: CEF subprocesses run in here. A sandboxed child (a
  // renderer, the GPU process) enters the sandbox in CefExecuteProcess.
  CefRefPtr<LaufeyCombinedApp> app(new LaufeyCombinedApp());
  int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
  if (exit_code >= 0) {
    return exit_code;
  }

  // Single-instance mode (docs/deep-links.md): a second launch forwards its
  // command line (GetCommandLineW) to the running instance and exits here,
  // before CefInitialize (so CEF's own profile singleton is never reached)
  // and before the runtime loads.
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

  CefSettings settings;
  // `--no-sandbox` on the command line still turns it off for a debugging
  // session (Chromium reads the switch itself); a deep-link launch drops it
  // with every other Chromium switch (LaufeyStripDeepLinkSwitches).
  settings.no_sandbox = sandbox_info == nullptr;
  settings.log_severity = LaufeyCefLogSeverity();

  // Set cache path. With a per-app data dir (LAUFEY_DATA_DIR / LAUFEY_APP_ID)
  // the profile persists there; cache_path must be set too (equal to the root)
  // or CEF runs the browser "incognito" and keeps localStorage/cookies in
  // memory. Without one, keep the throwaway per-process temp root.
  //
  // CefString decodes std::string as UTF-8, so read the temp dir wide and
  // convert; GetTempPathA would hand over active-codepage bytes that garble
  // non-ASCII profile names. On failure leave the cache path unset (CEF then
  // runs with its in-memory default) rather than pointing it at garbage.
  std::string cache_path = laufey_common::AppDataSubdir("CEF");
  if (!cache_path.empty()) {
    CefString(&settings.root_cache_path) = cache_path;
    CefString(&settings.cache_path) = cache_path;
  } else {
    wchar_t tempPath[MAX_PATH + 2];
    DWORD tempLen = GetTempPathW(MAX_PATH + 2, tempPath);
    if (tempLen > 0 && tempLen < MAX_PATH + 2) {
      cache_path = laufey_common::WideToUtf8(tempPath) + "laufey_cef_" +
                   std::to_string(GetCurrentProcessId());
      CefString(&settings.root_cache_path) = cache_path;
    }
  }

  wchar_t port_buf[16];
  DWORD port_len =
      GetEnvironmentVariableW(L"LAUFEY_REMOTE_DEBUGGING_PORT", port_buf, 16);
  // On a value of 16+ chars the API returns the required size and leaves the
  // buffer untouched, so the upper bound is load-bearing.
  // No remote debugging while DevTools are off (API 40, inspectable).
  if (port_len > 0 && port_len < 16 && laufey_common::LaunchInspectable()) {
    int port = _wtoi(port_buf);
    if (port > 0 && port < 65536) {
      settings.remote_debugging_port = port;
    }
  }

  if (!CefInitialize(main_args, settings, app.get(), sandbox_info)) {
    LaufeyReportCefInitializeFailure(cache_path);
    return 1;
  }
  LaufeyInstallSecondInstanceHooks();

  CefRunMessageLoop();

  // The loop is over: UI tasks still queued are answered "not run" and an
  // auth session in progress ends cancelled, so a runtime thread waiting on
  // either is released before Shutdown waits for it.
  laufey_common::UiLoopEnded();

  LaufeyClearSecondInstanceHooks();
  RuntimeLoader::GetInstance()->Shutdown();

  CefShutdown();

  // How a Windows app ends (docs/backends.md): CEF has shut down, so the
  // profile, cookies and web storage are on disk; end the process here with
  // the app's exit code (exit_app's, else 0) instead of returning to the
  // bootstrap's CRT exit. That exit is ExitProcess, which ends every other
  // thread and then runs each DLL's DLL_PROCESS_DETACH, static destructors
  // and atexit callbacks, and one of them can wait there for good: on
  // windows-ci 2 of about 160 CEF e2e processes that ended through
  // ExitProcess never ended, the one thread left waiting in combase's
  // MTAThreadWaitForCall (a cross-apartment COM call) under
  // Windows.Media.dll's exit-time cleanup in ucrtbase. Chromium loads that
  // DLL on the browser's main thread in every app. Such a process couldn't
  // be killed and kept its profile locked, so the app's next launch failed.
  laufey_common::EndProcess(laufey_common::RequestedExitCode());
}

#if defined(CEF_USE_BOOTSTRAP)

namespace {

// Whether bootstrap.exe (`theirs`) comes from the CEF distribution this DLL
// was built against: the sandbox information it created is passed to this
// DLL's libcef, so a bootstrap from another CEF release must not run it.
bool LaufeyBootstrapMatches(const cef_version_info_t* theirs) {
  if (!theirs ||
      theirs->size < offsetof(cef_version_info_t, chrome_version_patch) +
                         sizeof(theirs->chrome_version_patch)) {
    return false;
  }
  cef_version_info_t ours = {};
  CEF_POPULATE_VERSION_INFO(&ours);
  if (theirs->cef_version_major != ours.cef_version_major ||
      theirs->cef_version_minor != ours.cef_version_minor ||
      theirs->cef_version_patch != ours.cef_version_patch ||
      theirs->cef_commit_number != ours.cef_commit_number ||
      theirs->chrome_version_major != ours.chrome_version_major ||
      theirs->chrome_version_minor != ours.chrome_version_minor ||
      theirs->chrome_version_build != ours.chrome_version_build ||
      theirs->chrome_version_patch != ours.chrome_version_patch) {
    return false;
  }
#if CEF_API_ADDED(14600)
  // The sandbox compatibility hash, when the bootstrap passes one.
  if (theirs->size >= CEF_VERSION_INFO_SIZE_WITH_SANDBOX_HASH &&
      strncmp(theirs->sandbox_compat_hash, ours.sandbox_compat_hash,
              sizeof(ours.sandbox_compat_hash)) != 0) {
    return false;
  }
#endif
  return true;
}

// CEF's bootstrap moves a browser-type process (the app, a headless worker)
// to the executable's directory before calling RunWinMain
// (SetCwdForBrowserProcess in CEF's bootstrap_win.cc), so a process loses
// the working directory it was started in. A launcher that knows it, such as
// the runtime forking a worker of its own executable, passes it in
// LAUFEY_CWD: change back to it. The variable is removed either way, so the
// app's own child processes don't inherit it.
void LaufeyRestoreLaunchWorkingDirectory() {
  const wchar_t kName[] = L"LAUFEY_CWD";
  DWORD len = GetEnvironmentVariableW(kName, nullptr, 0);
  if (len == 0)
    return;
  std::wstring dir(len, L'\0');
  DWORD got = GetEnvironmentVariableW(kName, dir.data(), len);
  SetEnvironmentVariableW(kName, nullptr);
  if (got == 0 || got >= len)
    return;
  dir.resize(got);
  if (!SetCurrentDirectoryW(dir.c_str())) {
    std::cerr << "laufey: LAUFEY_CWD: can't change to "
              << laufey_common::WideToUtf8(dir) << " (error " << GetLastError()
              << ")" << std::endl;
  }
}

}  // namespace

// Called by bootstrap.exe, the app's executable (`<app>.exe` loads this DLL
// as `<app>.dll`), in every process, with the sandbox information it created.
// See docs/backends.md, "The Chromium sandbox".
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance, LPTSTR lpCmdLine,
                                    int nCmdShow, void* sandbox_info,
                                    cef_version_info_t* version_info) {
  // DLL planting: the bootstrap starts a browser-type process in the
  // executable's directory, but LAUFEY_CWD (below) moves it to wherever the
  // app was launched from, a directory anyone may have written a DLL into.
  // Take the working directory out of the DLL search order before any
  // library is loaded by name (libcef.dll is delay-loaded; Windows itself
  // loads more on demand). The executable's directory, System32 and PATH
  // stay. SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) would
  // also drop PATH, which breaks what the runtime loads for the app: a
  // Node-API addon whose dependency DLL is found on PATH (an SDK's bin
  // directory) and a Deno.dlopen() of a bare DLL name on PATH both fail
  // with ERROR_MOD_NOT_FOUND under it (checked on Windows 11), so it is not
  // called.
  SetDllDirectoryW(L"");
  (void)lpCmdLine;
  (void)nCmdShow;
  if (!LaufeyBootstrapMatches(version_info)) {
    // WIN32 subsystem: stderr is usually detached, so also show a dialog.
    const wchar_t* msg =
        L"This app's executable (CEF's bootstrap) comes from a different CEF "
        L"release than its host library.";
    std::cerr << laufey_common::WideToUtf8(msg) << std::endl;
    MessageBoxW(nullptr, msg, L"LAUFEY Error", MB_OK | MB_ICONERROR);
    return 1;
  }
  LaufeyRestoreLaunchWorkingDirectory();
  return LaufeyWinMain(hInstance, sandbox_info);
}

#else

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  (void)hPrevInstance;
  (void)lpCmdLine;
  (void)nCmdShow;
  return LaufeyWinMain(hInstance, nullptr);
}

#endif  // defined(CEF_USE_BOOTSTRAP)
