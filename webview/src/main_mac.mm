// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>

#include "runtime_loader.h"
#include "laufey_backend_common.h"
#include "laufey_auth_session.h"
#include "laufey_launch_args.h"
#include "laufey_notifications.h"
#include "laufey_single_instance.h"
#include "laufey_window.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// Cmd+C/V/X/A on macOS dispatch through the main menu's performKeyEquivalent:
// — Cocoa matches the keystroke against menu items, then sends their action
// (cut:/copy:/paste:/selectAll:) up the responder chain to the WKWebView,
// which forwards it to the page. Without these items in the main menu, the
// standard editing shortcuts have nowhere to land.
NSMenu* BuildDefaultEditSubmenu() {
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                           action:@selector(undo:)
                                    keyEquivalent:@"z"]];
  NSMenuItem* redo = [[NSMenuItem alloc] initWithTitle:@"Redo"
                                                action:@selector(redo:)
                                         keyEquivalent:@"Z"];
  [redo setKeyEquivalentModifierMask:(NSEventModifierFlagCommand |
                                      NSEventModifierFlagShift)];
  [edit addItem:redo];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Cut"
                                           action:@selector(cut:)
                                    keyEquivalent:@"x"]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                           action:@selector(copy:)
                                    keyEquivalent:@"c"]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                           action:@selector(paste:)
                                    keyEquivalent:@"v"]];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                           action:@selector(selectAll:)
                                    keyEquivalent:@"a"]];
  return edit;
}

static bool MenuTreeHasCopyAction(NSMenu* menu) {
  for (NSMenuItem* item in [menu itemArray]) {
    if ([item action] == @selector(copy:))
      return true;
    if ([item submenu] && MenuTreeHasCopyAction([item submenu]))
      return true;
  }
  return false;
}

// Force an Edit submenu into the menubar if the embedder didn't include one.
void EnsureEditMenu(NSMenu* menubar) {
  if (MenuTreeHasCopyAction(menubar))
    return;
  NSMenuItem* editItem = [[NSMenuItem alloc] init];
  [editItem setSubmenu:BuildDefaultEditSubmenu()];
  [menubar addItem:editItem];
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) LaufeyBackend* backend;
@property(nonatomic, copy) NSString* runtimePath;
- (void)shutDownRuntime;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  self.backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(self.backend);

  // Resolved in main() (ResolveMacRuntimePath).
  std::string runtimePath;
  if (self.runtimePath) {
    runtimePath = [self.runtimePath UTF8String];
  }

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found. Set LAUFEY_RUNTIME_PATH or place "
                 "libruntime.dylib in the app bundle."
              << std::endl;
    [NSApp terminate:nil];
    return;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime from: " << runtimePath << std::endl;
    [NSApp terminate:nil];
    return;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start runtime" << std::endl;
    [NSApp terminate:nil];
    return;
  }
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  [self shutDownRuntime];
}

// Tells the runtime the app is ending and waits for its thread. Safe to call
// twice: the loop can end through -terminate: (this delegate's
// applicationWillTerminate:) or through quit()'s -stop: (after [NSApp run]
// returns in main), and which one wins is a race.
- (void)shutDownRuntime {
  // The loop is over: UI tasks still queued are answered "not run" and an
  // auth session in progress ends cancelled, so a runtime thread waiting on
  // either is released before Shutdown waits for it.
  laufey_common::UiLoopEnded();
  RuntimeLoader::GetInstance()->Shutdown();
  // What the pages stored goes to disk before the process ends.
  if (self.backend)
    self.backend->FlushWebStorage();
  delete self.backend;
  self.backend = nullptr;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  // A menu-bar-only app uses the Accessory activation policy. Its windows can
  // be transient (for example, a tray popover), so closing the last one must
  // not terminate the process and remove its status item.
  // A tray / menu-bar app can also ask to keep running with no window
  // (set_quit_on_last_window_closed(false)); quit() ends it either way.
  return laufey_common::ShouldQuitAfterLastWindowMac() ? YES : NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                    hasVisibleWindows:(BOOL)hasVisibleWindows {
  laufey_common::FireDockReopenMac(hasVisibleWindows ? true : false);
  // Always swallow the default "show last hidden window" behavior — the
  // embedder's callback decides what to do.
  return NO;
}

// Deep links. AppKit routes the kAEGetURL Apple Event here for every scheme
// the bundle claims in CFBundleURLTypes, both at launch and while running.
// Implementing this selector is what makes AppKit install its own handler for
// that event, so there's no NSAppleEventManager registration to do.
//
// A launch URL lands before the runtime finished loading on its worker
// thread, so FireOpenUrlMac buffers until a handler registers.
- (void)application:(NSApplication*)application
           openURLs:(NSArray<NSURL*>*)urls {
  for (NSURL* url in urls) {
    NSString* absolute = [url absoluteString];
    if (absolute) {
      laufey_common::FireOpenUrlMac([absolute UTF8String]);
    }
  }
}

- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
  return laufey_common::GetDockMenuMac();
}

@end

// Run the runtime headless (no window) for forked worker processes.
// Framework dev servers (e.g. Next.js Turbopack) fork child processes
// via child_process.fork(), which re-executes this binary. We detect
// these workers and run the Deno runtime without creating a window.
static int run_headless(const std::string& path) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  // Create a minimal backend with no visible window
  loader->SetBackend(nullptr);

  if (path.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(path)) {
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

// The runtime library to load. A packaged app (a launch file, or a runtime
// it ships: Contents/Frameworks/libruntime.dylib or
// Contents/MacOS/libruntime.dylib in the bundle, or <executable>.dylib next
// to the executable) loads only that one; a development host takes
// --runtime (before "--"), then LAUFEY_RUNTIME_PATH, then the working
// directory's fallbacks. See laufey_launch_args.h.
static std::string ResolveMacRuntimePath(int argc, char* argv[]) {
  std::vector<std::string> bundled;
  @autoreleasepool {
    NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
    if (bundlePath) {
      bundled.push_back([[bundlePath
          stringByAppendingPathComponent:
              @"Contents/Frameworks/libruntime.dylib"] UTF8String]);
      bundled.push_back([[bundlePath
          stringByAppendingPathComponent:@"Contents/MacOS/libruntime.dylib"]
          UTF8String]);
    }
  }
  bundled.push_back(LaufeyFindColocatedRuntime());
  laufey_common::RuntimeChoice choice = laufey_common::ResolveRuntimePath(
      std::vector<std::string>(argv + 1, argv + argc), bundled,
      {"./libruntime.dylib", "./target/debug/libhello.dylib",
       "./target/release/libhello.dylib"});
  // A packaged app without its runtime exits at once, before AppKit starts.
  if (laufey_common::IsMissingPackagedRuntime(choice)) {
    laufey_common::ReportMissingPackagedRuntime();
    exit(laufey_common::kMissingRuntimeExitCode);
  }
  return choice.path;
}

int main(int argc, char* argv[]) {
  // LAUFEY_CWD is only for the Windows CEF host behind CEF's bootstrap
  // (cef/src/main_windows.cc); never pass it on to what the app starts.
  unsetenv("LAUFEY_CWD");

  const std::string runtimePath = ResolveMacRuntimePath(argc, argv);

  // Forked worker processes should not create a window.
  if (laufey_common::IsHeadlessWorkerLaunch(
          std::vector<std::string>(argv + 1, argv + argc))) {
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

  @autoreleasepool {
    // Allow the host to override the user-visible app name (menu-bar app
    // menu, Dock, Cmd-Tab) at launch, e.g. the project name during
    // `deno desktop --hmr`. AppKit derives the app menu title from the
    // process name for an exec'd bundle like ours, so set it before the
    // menu is built and before +sharedApplication.
    if (const char* app_name = getenv("LAUFEY_APP_NAME")) {
      if (*app_name) {
        [[NSProcessInfo processInfo]
            setProcessName:[NSString stringWithUTF8String:app_name]];
      }
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Allow the host to override the dock icon at launch (e.g. a project's
    // favicon during `deno desktop --hmr`). Setting it programmatically
    // bypasses LaunchServices' icon cache and the bundle's CFBundleIconFile.
    if (const char* icon_path = getenv("LAUFEY_APP_ICON")) {
      NSImage* icon = [[NSImage alloc]
          initWithContentsOfFile:[NSString stringWithUTF8String:icon_path]];
      if (icon) {
        [NSApp setApplicationIconImage:icon];
      }
    }

    AppDelegate* delegate = [[AppDelegate alloc] init];
    delegate.runtimePath =
        runtimePath.empty()
            ? nil
            : [NSString stringWithUTF8String:runtimePath.c_str()];

    [NSApp setDelegate:delegate];
    // The notification-center delegate must be in place before AppKit
    // finishes launching, or the click that launched the app is lost.
    laufey_common::InitNotificationsAtLaunch();

    // Files and URLs reach the runtime through argv (direct exec) or the
    // open-url handler (LaunchServices), never both; forwarded launches are
    // delivered on the main queue once [NSApp run] starts.
    laufey_common::DisableArgvOpenEventsMac();
    laufey_common::InstallSecondInstanceHooksMac();

    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] init];
    NSMenuItem* quitItem =
        [[NSMenuItem alloc] initWithTitle:@"Quit"
                                   action:@selector(terminate:)
                            keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    EnsureEditMenu(menubar);
    [NSApp setMainMenu:menubar];

    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];

    // quit() ends the loop with -stop:, which returns here without
    // -terminate:'s applicationWillTerminate:. Unless AppKit's own
    // terminate-after-last-window check got in first, the runtime has not
    // been told yet; returning would exit the process under it, so shut it
    // down here, as the other backends do after their loop.
    [delegate shutDownRuntime];
  }

  // exit_app's code (API 46), else 0.
  return laufey_common::RequestedExitCode();
}
