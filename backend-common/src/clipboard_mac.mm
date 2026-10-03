// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSPasteboard-backed clipboard: text, HTML, PNG images, the formats present
// and a change watcher. Pasteboard access is forwarded to the main thread
// (RunOnMainSync when called elsewhere), matching the convention used by the
// AppKit dialog/menu code in this directory, so every call is safe from any
// thread.
//
// macOS has no clipboard-change notification; the watcher polls the general
// pasteboard's changeCount (an integer read, no payload) twice a second, and
// only while a change handler is registered.

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_ui_tasks.h"

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

void OnMain(void (^body)(void)) {
  RunOnMainSync(body);
}

char* StrdupCapped(NSString* str) {
  if (!str)
    return nullptr;
  const char* utf8 = [str UTF8String];
  if (!utf8)
    return nullptr;
  size_t len = strlen(utf8);
  if (len > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return nullptr;
  return strdup(utf8);
}

NSData* PngFromImageData(NSData* data) {
  if (!data)
    return nil;
  NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:data];
  if (!rep)
    return nil;
  return [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
}

}  // namespace

char* ClipboardReadTextMac() {
  __block char* result = nullptr;
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    result = StrdupCapped([pb stringForType:NSPasteboardTypeString]);
  });
  return result;
}

void ClipboardWriteTextMac(const std::string& text) {
  NSString* str = [NSString stringWithUTF8String:text.c_str()];
  if (!str)
    str = @"";
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str forType:NSPasteboardTypeString];
  });
}

uint32_t ClipboardCapabilitiesMac() {
  return LAUFEY_CLIPBOARD_CAP_TEXT | LAUFEY_CLIPBOARD_CAP_HTML |
         LAUFEY_CLIPBOARD_CAP_IMAGE | LAUFEY_CLIPBOARD_CAP_FORMATS |
         LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS;
}

char* ClipboardReadHtmlMac() {
  __block char* result = nullptr;
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    result = StrdupCapped([pb stringForType:NSPasteboardTypeHTML]);
  });
  return result;
}

bool ClipboardWriteHtmlMac(const std::string& html, const char* text_or_null) {
  NSString* nsHtml = [NSString stringWithUTF8String:html.c_str()];
  if (!nsHtml)
    return false;
  NSString* nsText =
      text_or_null ? [NSString stringWithUTF8String:text_or_null] : nil;
  if (text_or_null && !nsText)
    return false;
  __block bool ok = false;
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    ok = [pb setString:nsHtml forType:NSPasteboardTypeHTML];
    if (ok && nsText)
      ok = [pb setString:nsText forType:NSPasteboardTypeString];
  });
  return ok;
}

uint8_t* ClipboardReadImageMac(size_t* len_out) {
  if (len_out)
    *len_out = 0;
  __block NSData* png = nil;
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    NSData* data = [pb dataForType:NSPasteboardTypePNG];
    if (data && data.length > 0 &&
        LooksLikePng(static_cast<const uint8_t*>(data.bytes), data.length)) {
      png = data;
      return;
    }
    NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF];
    if (tiff) {
      if (tiff.length <= LAUFEY_CLIPBOARD_MAX_READ_BYTES)
        png = PngFromImageData(tiff);
      return;
    }
    // Any other image type AppKit can read (a PDF, a JPEG some app put there),
    // through NSImage. Not for text: NSImage can render some text types.
    NSArray* imageTypes = [NSImage imageTypes];
    NSString* found = [pb availableTypeFromArray:imageTypes];
    if (found) {
      NSData* raw = [pb dataForType:found];
      if (raw && raw.length <= LAUFEY_CLIPBOARD_MAX_READ_BYTES) {
        NSImage* image = [[NSImage alloc] initWithData:raw];
        png = PngFromImageData([image TIFFRepresentation]);
      }
    }
  });
  if (!png || png.length == 0 || png.length > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return nullptr;
  uint8_t* out = MallocCopy(png.bytes, png.length);
  if (out && len_out)
    *len_out = png.length;
  return out;
}

bool ClipboardWriteImageMac(const uint8_t* png, size_t len) {
  if (!LooksLikePng(png, len))
    return false;
  NSData* data = [NSData dataWithBytes:png length:len];
  NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:data];
  if (!rep)
    return false;
  // TIFF too: older apps and some image editors only paste TIFF.
  NSData* tiff = [rep TIFFRepresentation];
  __block bool ok = false;
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    ok = [pb setData:data forType:NSPasteboardTypePNG];
    if (ok && tiff)
      [pb setData:tiff forType:NSPasteboardTypeTIFF];
  });
  return ok;
}

char* ClipboardReadFormatsMac() {
  __block std::vector<std::string> formats;
  OnMain(^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    for (NSPasteboardType type in [pb types]) {
      if ([type isEqualToString:NSPasteboardTypeString]) {
        formats.push_back("text/plain");
      } else if ([type isEqualToString:NSPasteboardTypeHTML]) {
        formats.push_back("text/html");
      } else if ([type isEqualToString:NSPasteboardTypePNG] ||
                 [type isEqualToString:NSPasteboardTypeTIFF]) {
        formats.push_back("image/png");
      } else if ([type isEqualToString:NSPasteboardTypeFileURL]) {
        formats.push_back("text/uri-list");
      } else if ([type isEqualToString:NSPasteboardTypeRTF]) {
        formats.push_back("text/rtf");
      }
    }
  });
  return JoinClipboardFormats(formats);
}

}  // namespace laufey_common

// The change-count poll. Lives on the main run loop in the common modes, so
// it keeps running while a menu tracks or a modal panel is up.
@interface LaufeyClipboardWatcher : NSObject
@property(nonatomic, strong) NSTimer* timer;
@property(nonatomic) NSInteger lastCount;
@end

@implementation LaufeyClipboardWatcher
- (void)start {
  if (self.timer)
    return;
  self.lastCount = [[NSPasteboard generalPasteboard] changeCount];
  self.timer = [NSTimer timerWithTimeInterval:0.5
                                       target:self
                                     selector:@selector(tick:)
                                     userInfo:nil
                                      repeats:YES];
  self.timer.tolerance = 0.1;
  [[NSRunLoop mainRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)stop {
  [self.timer invalidate];
  self.timer = nil;
}
- (void)tick:(NSTimer*)timer {
  NSInteger count = [[NSPasteboard generalPasteboard] changeCount];
  if (count == self.lastCount)
    return;
  self.lastCount = count;
  laufey_common::FireClipboardChange();
}
@end

namespace laufey_common {

void ClipboardWatchMac(bool on) {
  static LaufeyClipboardWatcher* watcher = nil;
  // Async: the caller may hold a lock the main thread needs, and the watcher
  // only has to live on the main thread.
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!watcher)
      watcher = [[LaufeyClipboardWatcher alloc] init];
    if (on) {
      [watcher start];
    } else {
      [watcher stop];
    }
  });
}

}  // namespace laufey_common
