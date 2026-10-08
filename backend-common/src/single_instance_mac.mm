// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS pieces of the single-instance lock shared by the CEF and WebView
// backends: bringing the app to the front for a forwarded launch, and the
// UI hooks that deliver it on the main thread. See laufey_single_instance.h.

#import <Cocoa/Cocoa.h>

#include <dispatch/dispatch.h>

#include "laufey_single_instance.h"

namespace laufey_common {

void ActivateAppMac() {
  @autoreleasepool {
    [NSApp unhide:nil];
    NSWindow* target = [NSApp mainWindow] ?: [NSApp keyWindow];
    if (!target) {
      // Front-most first; skip windows the embedder hid (orderOut:) and
      // panels/status items that can't become main.
      for (NSWindow* window in [NSApp orderedWindows]) {
        if (([window isVisible] || [window isMiniaturized]) &&
            [window canBecomeMainWindow]) {
          target = window;
          break;
        }
      }
    }
    if (!target) {
      // orderedWindows leaves out minimized windows on some releases.
      for (NSWindow* window in [NSApp windows]) {
        if ([window isMiniaturized] && [window canBecomeMainWindow]) {
          target = window;
          break;
        }
      }
    }
    if ([target isMiniaturized])
      [target deminiaturize:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [target makeKeyAndOrderFront:nil];
  }
}

void InstallSecondInstanceHooksMac() {
  SecondInstanceUiHooks hooks;
  hooks.post = [](void*, void (*task)(void*), void* data) {
    dispatch_async_f(dispatch_get_main_queue(), data, task);
  };
  hooks.activate = [](void*) { ActivateAppMac(); };
  SetSecondInstanceUiHooks(hooks);
}

void DisableArgvOpenEventsMac() {
  [[NSUserDefaults standardUserDefaults]
      registerDefaults:@{@"NSTreatUnknownArgumentsAsOpen" : @"NO"}];
}

}  // namespace laufey_common
