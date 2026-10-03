// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The file paths of an external drag, read from X11's drag-and-drop
// selection. See LaufeyNativeDragFilePaths in app.h.

#include "app.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <poll.h>

#include <chrono>
#include <string>
#include <vector>

#include "laufey_io.h"

std::vector<std::string> LaufeyNativeDragFilePaths() {
  // A connection of our own: Xlib state shared with Chromium's or GTK's
  // connection must not be touched from here. Under Wayland there is no X
  // drag source to ask (XWayland, if any, has no owner for it).
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy)
    return {};
  std::vector<std::string> paths;
  Atom selection = XInternAtom(dpy, "XdndSelection", False);
  Atom target = XInternAtom(dpy, "text/uri-list", False);
  Atom property = XInternAtom(dpy, "LAUFEY_DRAG_PATHS", False);
  if (XGetSelectionOwner(dpy, selection) != None) {
    // The XDND source owns XdndSelection for the length of the drag and
    // answers any client's conversion request (XDND: the target asks the
    // same way once the drag enters it).
    Window requestor =
        XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    XConvertSelection(dpy, selection, target, property, requestor, CurrentTime);
    XFlush(dpy);
    bool answered = false;
    Atom answer = None;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (!answered && std::chrono::steady_clock::now() < deadline) {
      while (XPending(dpy) > 0) {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type == SelectionNotify &&
            event.xselection.requestor == requestor) {
          answered = true;
          answer = event.xselection.property;
          break;
        }
      }
      if (!answered) {
        struct pollfd fd = {ConnectionNumber(dpy), POLLIN, 0};
        poll(&fd, 1, 20);
      }
    }
    if (answered && answer != None) {
      Atom type = None;
      int format = 0;
      unsigned long count = 0, after = 0;
      unsigned char* data = nullptr;
      // Up to 4 MiB (the length is in 32-bit units); a uri-list for
      // LAUFEY_MAX_DROP_PATHS files fits.
      if (XGetWindowProperty(dpy, requestor, property, 0, 1 << 20, True,
                             AnyPropertyType, &type, &format, &count, &after,
                             &data) == Success &&
          data && format == 8) {
        paths = laufey_common::UriListToPaths(
            std::string(reinterpret_cast<char*>(data), count));
      }
      if (data)
        XFree(data);
    }
    XDestroyWindow(dpy, requestor);
  }
  XCloseDisplay(dpy);
  return paths;
}
