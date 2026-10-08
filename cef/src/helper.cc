// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "include/cef_app.h"
#include "renderer_app.h"

#if defined(OS_MAC)
#include "include/cef_sandbox_mac.h"
#include "include/wrapper/cef_library_loader.h"
#endif

int main(int argc, char* argv[]) {
#if defined(OS_MAC)
  // Chromium's sandbox (Seatbelt) for this helper, before the framework
  // loads: the browser process passes the profile for the helper's role
  // (renderer, GPU, ...) on the command line, and a role that runs
  // unsandboxed (or a browser started with --no-sandbox) gets none. It loads
  // libcef_sandbox.dylib from the framework's Libraries directory. See
  // docs/backends.md, "The Chromium sandbox".
  CefScopedSandboxContext sandbox_context;
  if (!sandbox_context.Initialize(argc, argv)) {
    return 1;
  }

  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInHelper()) {
    return 1;
  }
#endif

  CefMainArgs main_args(argc, argv);

  CefRefPtr<LaufeyRendererApp> app(new LaufeyRendererApp());
  return CefExecuteProcess(main_args, app, nullptr);
}
