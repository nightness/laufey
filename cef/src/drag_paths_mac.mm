// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The file paths of an external drag over a CEF window on macOS: the drag
// pasteboard, which holds the dragged items for the length of the drag. See
// LaufeyNativeDragFilePaths in app.h.

#include "app.h"

#include "laufey_io.h"

std::vector<std::string> LaufeyNativeDragFilePaths() {
  return laufey_common::DragPasteboardFilePathsMac();
}
