// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS-only unit tests for io_mac.mm: the files read back from the drag
// pasteboard (DragPasteboardFilePathsMac, which the CEF backend uses for
// external drops). Writes the drag pasteboard as a drag source does. Run
// via `ctest --test-dir webview/build`.

#include "laufey_io.h"

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using laufey_common::DragPasteboardFilePathsMac;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static void WriteDrag(NSArray<id<NSPasteboardWriting>>* items) {
  NSPasteboard* pasteboard =
      [NSPasteboard pasteboardWithName:NSPasteboardNameDrag];
  [pasteboard clearContents];
  if (items.count)
    EXPECT([pasteboard writeObjects:items]);
}

int main() {
  @autoreleasepool {
    // Files, in order, a non-ASCII and a spaced name among them. A file
    // URL holds the path in its decomposed form, which is what a drop
    // reports (on the WKWebView backend too).
    NSURL* accented = [NSURL fileURLWithPath:@"/tmp/laufey drop é.txt"];
    WriteDrag(@[ accented, [NSURL fileURLWithPath:@"/private/var/x"] ]);
    std::vector<std::string> paths = DragPasteboardFilePathsMac();
    EXPECT(paths.size() == 2);
    EXPECT(paths[0] == accented.path.UTF8String);
    EXPECT(paths[0].rfind("/tmp/laufey drop e", 0) == 0);
    EXPECT(paths[1] == "/private/var/x");

    // Web links and plain text are not files.
    WriteDrag(@[
      [NSURL URLWithString:@"https://example.com/x"],
      @"/tmp/not-a-url",
    ]);
    EXPECT(DragPasteboardFilePathsMac().empty());

    // A file among other items still counts.
    WriteDrag(@[
      [NSURL URLWithString:@"https://example.com/y"],
      [NSURL fileURLWithPath:@"/tmp/only"],
    ]);
    paths = DragPasteboardFilePathsMac();
    EXPECT(paths.size() == 1 && paths[0] == "/tmp/only");

    // Nothing on the pasteboard.
    WriteDrag(@[]);
    EXPECT(DragPasteboardFilePathsMac().empty());
  }
  std::printf("laufey_io_mac_test: ok\n");
  return 0;
}
