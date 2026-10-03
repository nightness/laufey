// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSAlert-based dialogs. `runModal` itself spins NSRunLoop, pumping AppKit
// events for other windows while the dialog is up. Runs on main directly
// when the caller is already there; from another thread (the runtime's) it
// runs from a main run-loop block and the caller waits for it. Not from a
// main-queue block (dispatch_sync): the main queue is serial, so while that
// block ran the modal, no other main-queue work would run until the dialog
// closed -- the backend's UI-thread calls and dispatch_ui_task among it.
// The wait goes through the UI task dispatcher (laufey_ui_tasks.h): a call
// still waiting when the loop ends returns as if cancelled.

#include "laufey_backend_common.h"
#include "laufey_menu.h"
#include "laufey_ui_tasks.h"

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

int ShowDialogMac(int dialog_type, const std::string& title,
                  const std::string& message,
                  const std::string& default_value, char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
  NSString* nsMessage = [NSString stringWithUTF8String:message.c_str()];
  NSString* nsDefault = [NSString stringWithUTF8String:default_value.c_str()];

  __block bool confirmed = false;
  __block char* input_strdup = nullptr;
  void (^body)(void) = ^{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:nsTitle];
    [alert setInformativeText:nsMessage];

    NSTextField* inputField = nil;
    if (dialog_type == LAUFEY_DIALOG_ALERT) {
      [alert addButtonWithTitle:@"OK"];
    } else if (dialog_type == LAUFEY_DIALOG_CONFIRM) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
    } else if (dialog_type == LAUFEY_DIALOG_PROMPT) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
      inputField =
          [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
      [inputField setStringValue:nsDefault];
      [alert setAccessoryView:inputField];
      [alert layout];
      [[alert window] makeFirstResponder:inputField];
    }

    NSModalResponse response = [alert runModal];
    confirmed = (response == NSAlertFirstButtonReturn);
    if (dialog_type == LAUFEY_DIALOG_PROMPT && confirmed && inputField &&
        out_input_value) {
      const char* text = [[inputField stringValue] UTF8String];
      if (text)
        input_strdup = strdup(text);
    }
  };

  if ([NSThread isMainThread]) {
    ScopedNativeModalLoop modal_loop;
    body();
  } else {
    auto run = [body] {
      ScopedNativeModalLoop modal_loop;
      body();
    };
    RunOnUiThreadAndWait(run, [](void (*task)(void*), void* data) {
      RunFromMainRunLoopMac([task, data] { task(data); });
      return true;
    });
  }

  if (out_input_value && input_strdup)
    *out_input_value = input_strdup;
  return confirmed ? 1 : 0;
}

}  // namespace laufey_common
