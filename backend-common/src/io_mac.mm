// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS drag-out (NSDraggingSource / beginDraggingSessionWithItems) and file
// dialogs (NSOpenPanel / NSSavePanel), shared by the WKWebView and CEF
// backends. Everything AppKit runs on the main thread; the entry points hop
// there themselves.

#include "laufey_backend_common.h"
#include "laufey_io.h"

#import <Cocoa/Cocoa.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using laufey_common::DragOutRequest;
using laufey_common::FileDialogRequest;

void RunOnMain(std::function<void()> fn) {
  if ([NSThread isMainThread]) {
    fn();
    return;
  }
  auto* heap = new std::function<void()>(std::move(fn));
  dispatch_async(dispatch_get_main_queue(), ^{
    (*heap)();
    delete heap;
  });
}

bool g_dragging = false;

NSString* ToNS(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

}  // namespace

// ---------------------------------------------------------------------------
// Drag out
// ---------------------------------------------------------------------------

@interface LaufeyFileDragSource : NSObject <NSDraggingSource> {
 @public
  DragOutRequest* request_;
}
@end

// Sources stay alive until their session ends (AppKit holds the source weakly
// on some versions).
static NSMutableSet<LaufeyFileDragSource*>* g_drag_sources;

@implementation LaufeyFileDragSource
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
  return NSDragOperationCopy;
}
- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
  g_dragging = false;
  DragOutRequest* req = request_;
  request_ = nullptr;
  if (req) {
    req->Finish(operation == NSDragOperationNone ? LAUFEY_DRAG_RESULT_CANCELLED
                                                 : LAUFEY_DRAG_RESULT_DROPPED);
    delete req;
  }
  [g_drag_sources removeObject:self];
}
@end

namespace laufey_common {

void StartFileDragMac(void* nsview, DragOutRequest* req) {
  NSView* view = (__bridge NSView*)nsview;
  RunOnMain([view, req] {
    std::unique_ptr<DragOutRequest> owned(req);
    NSWindow* window = view.window;
    if (!view || !window || g_dragging ||
        ([NSEvent pressedMouseButtons] & 1) == 0) {
      owned->Finish(LAUFEY_DRAG_RESULT_FAILED);
      return;
    }
    // The session starts from the event that began the gesture. The page's
    // dragstart / mousemove arrives here through the runtime, after AppKit
    // moved on, so use the current event only if it is still the left-button
    // gesture; otherwise synthesize the drag event at the pointer.
    NSEvent* event = [NSApp currentEvent];
    if (!event || event.window != window ||
        (event.type != NSEventTypeLeftMouseDown &&
         event.type != NSEventTypeLeftMouseDragged)) {
      event =
          [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                             location:[window mouseLocationOutsideOfEventStream]
                        modifierFlags:0
                            timestamp:[[NSProcessInfo processInfo] systemUptime]
                         windowNumber:window.windowNumber
                              context:nil
                          eventNumber:0
                           clickCount:1
                             pressure:1.0];
    }
    NSPoint at = [view convertPoint:event.locationInWindow fromView:nil];
    NSImage* custom = nil;
    if (!owned->icon_png.empty()) {
      NSData* data = [NSData dataWithBytes:owned->icon_png.data()
                                    length:owned->icon_png.size()];
      custom = [[NSImage alloc] initWithData:data];
    }
    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
    CGFloat offset = 0;
    for (const auto& path : owned->paths) {
      NSURL* url = [NSURL fileURLWithPath:ToNS(path)];
      NSDraggingItem* item =
          [[NSDraggingItem alloc] initWithPasteboardWriter:url];
      NSImage* image =
          custom ?: [[NSWorkspace sharedWorkspace] iconForFile:ToNS(path)];
      NSSize size = custom ? custom.size : NSMakeSize(64, 64);
      if (size.width <= 0 || size.height <= 0)
        size = NSMakeSize(64, 64);
      if (size.width > 256 || size.height > 256) {
        CGFloat k = 256 / MAX(size.width, size.height);
        size = NSMakeSize(size.width * k, size.height * k);
      }
      [item setDraggingFrame:NSMakeRect(at.x - size.width / 2 + offset,
                                        at.y - size.height / 2 - offset,
                                        size.width, size.height)
                    contents:image];
      [items addObject:item];
      offset += 4;
    }
    LaufeyFileDragSource* source = [[LaufeyFileDragSource alloc] init];
    source->request_ = owned.release();
    if (!g_drag_sources)
      g_drag_sources = [NSMutableSet set];
    [g_drag_sources addObject:source];
    g_dragging = true;
    NSDraggingSession* session = [view beginDraggingSessionWithItems:items
                                                               event:event
                                                              source:source];
    if (!session) {
      g_dragging = false;
      DragOutRequest* r = source->request_;
      source->request_ = nullptr;
      [g_drag_sources removeObject:source];
      r->Finish(LAUFEY_DRAG_RESULT_FAILED);
      delete r;
      return;
    }
    session.animatesToStartingPositionsOnCancelOrFail = YES;
    session.draggingFormation = NSDraggingFormationPile;
  });
}

// ---------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------

namespace {

// Posts a Return key press to `panel`'s window through the event queue, the
// way a real key press arrives.
void PressReturn(NSSavePanel* panel) {
  NSWindow* window = panel;
  [window makeKeyAndOrderFront:nil];
  for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
    NSEvent* ev = [NSEvent
                   keyEventWithType:type
                           location:NSZeroPoint
                      modifierFlags:0
                          timestamp:[[NSProcessInfo processInfo] systemUptime]
                       windowNumber:window.windowNumber
                            context:nil
                         characters:@"\r"
        charactersIgnoringModifiers:@"\r"
                          isARepeat:NO
                            keyCode:36];
    [NSApp postEvent:ev atStart:NO];
  }
}

class MacFileDialog : public FileDialogPlatform {
 public:
  MacFileDialog(uint32_t id, NSWindow* parent, FileDialogRequest req)
      : id_(id), parent_(parent), req_(std::move(req)) {}

  bool Show() override {
    bool open = req_.kind == LAUFEY_FILE_DIALOG_OPEN;
    if (open) {
      NSOpenPanel* p = [NSOpenPanel openPanel];
      bool dirs = req_.ChoosesDirectories();
      bool files = req_.ChoosesFiles();
      p.canChooseDirectories = dirs;
      p.canChooseFiles = files || !dirs;
      p.allowsMultipleSelection = req_.Multiple();
      p.resolvesAliases = YES;
      panel_ = p;
    } else {
      panel_ = [NSSavePanel savePanel];
    }
    panel_.canCreateDirectories = YES;
    panel_.showsHiddenFiles =
        (req_.flags & LAUFEY_FILE_DIALOG_SHOW_HIDDEN) != 0;
    if (!req_.title.empty()) {
      // Panels on macOS 11+ show no title bar; the message line is where the
      // user sees it.
      panel_.title = ToNS(req_.title);
      panel_.message = ToNS(req_.title);
    }
    if (!req_.button_label.empty())
      panel_.prompt = ToNS(req_.button_label);
    NSMutableArray<NSString*>* types = [NSMutableArray array];
    bool any = false;
    for (const auto& f : req_.filters) {
      for (const auto& ext : f.extensions) {
        if (ext == "*")
          any = true;
        else
          [types addObject:ToNS(ext)];
      }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // allowedContentTypes (UTType) needs macOS 11; the deployment target is
    // 10.15, and the extension list is what callers pass anyway.
    if (!any && types.count > 0) {
      panel_.allowedFileTypes = types;
      panel_.allowsOtherFileTypes = NO;
    }
#pragma clang diagnostic pop
    std::string dir, name;
    laufey_common::SplitDefaultPath(req_.default_path, &dir, &name);
    if (!dir.empty())
      panel_.directoryURL = [NSURL fileURLWithPath:ToNS(dir) isDirectory:YES];
    if (!name.empty() && !open)
      panel_.nameFieldStringValue = ToNS(name);

    uint32_t id = id_;
    NSSavePanel* panel = panel_;
    void (^done)(NSModalResponse) = ^(NSModalResponse result) {
      std::vector<std::string> paths;
      if (result == NSModalResponseOK) {
        if ([panel isKindOfClass:[NSOpenPanel class]]) {
          for (NSURL* url in ((NSOpenPanel*)panel).URLs) {
            if (url.isFileURL && url.path)
              paths.emplace_back(url.path.UTF8String);
          }
        } else if (panel.URL.isFileURL && panel.URL.path) {
          paths.emplace_back(panel.URL.path.UTF8String);
        }
      }
      laufey_common::FileDialogFinish(id,
                                      result == NSModalResponseOK
                                          ? LAUFEY_FILE_DIALOG_ACCEPTED
                                          : LAUFEY_FILE_DIALOG_CANCELLED,
                                      paths);
    };
    if (parent_ && parent_.isVisible && !parent_.attachedSheet) {
      [panel_ beginSheetModalForWindow:parent_ completionHandler:done];
    } else {
      [panel_ beginWithCompletionHandler:done];
      [panel_ makeKeyAndOrderFront:nil];
    }
    return true;
  }

  void Cancel() override {
    [panel_ cancel:nil];
  }

  bool TestAccept(const std::string& path) override {
    if (!path.empty()) {
      NSURL* url = [NSURL fileURLWithPath:ToNS(path)];
      if (req_.kind == LAUFEY_FILE_DIALOG_SAVE) {
        panel_.directoryURL = url.URLByDeletingLastPathComponent;
        panel_.nameFieldStringValue = url.lastPathComponent;
      } else {
        // A file URL as the directory selects that file in the enclosing
        // directory; a directory URL opens it, and accepting an open panel
        // with no selection that can choose directories returns it.
        panel_.directoryURL = url;
      }
    }
    // The panel applies the new location asynchronously; accept a moment
    // later, and cancel if accepting didn't close it (nothing selectable).
    uint32_t id = id_;
    NSSavePanel* panel = panel_;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
                     if (!laufey_common::FileDialogIsOpen(id))
                       return;
                     // An in-process panel implements ok:. An out-of-process
                     // one (the panel service) throws "not implemented":
                     // press Return in it instead, as a user would.
                     @try {
                       [panel ok:nil];
                     } @catch (NSException* e) {
                       PressReturn(panel);
                     }
                   });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                     if (laufey_common::FileDialogIsOpen(id))
                       [panel cancel:nil];
                   });
    return true;
  }

 private:
  uint32_t id_;
  NSWindow* parent_;
  FileDialogRequest req_;
  NSSavePanel* panel_ = nil;
};

const laufey_common::UiRunner& MainRunner() {
  static const laufey_common::UiRunner runner = [](std::function<void()> fn) {
    RunOnMain(std::move(fn));
  };
  return runner;
}

}  // namespace

uint32_t ShowFileDialogMac(ParentResolver parent,
                           const laufey_file_dialog_options_t* options,
                           laufey_file_dialog_result_fn callback,
                           void* user_data) {
  return ShowFileDialogCommon(
      options, callback, user_data, MainRunner(),
      [parent](uint32_t id, const FileDialogRequest& request) {
        NSWindow* window = parent ? (__bridge NSWindow*)parent() : nil;
        auto* dialog = new MacFileDialog(id, window, request);
        FileDialogAttach(id, dialog);
        if (!dialog->Show())
          FileDialogFinish(id, LAUFEY_FILE_DIALOG_FAILED, {});
      });
}

bool CancelFileDialogMac(uint32_t dialog_id) {
  return FileDialogCancel(dialog_id, MainRunner());
}

bool TestFileDialogRespondMac(int action, const char* path) {
  return FileDialogTestRespond(action, path, MainRunner());
}

std::vector<std::string> DragPasteboardFilePathsMac() {
  std::vector<std::string> paths;
  @autoreleasepool {
    NSPasteboard* pasteboard =
        [NSPasteboard pasteboardWithName:NSPasteboardNameDrag];
    NSArray<NSURL*>* urls =
        [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                  options:@{
                                    NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                  }];
    for (NSURL* url in urls) {
      if (paths.size() >= LAUFEY_MAX_DROP_PATHS)
        break;
      // As the WKWebView backend reports a drop: the path in UTF-8, as the
      // URL has it (not fileSystemRepresentation's decomposed form).
      if (url.isFileURL && url.path.length > 0)
        paths.emplace_back(url.path.UTF8String);
    }
  }
  return paths;
}

}  // namespace laufey_common
