// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Wayland clipboard through ext-data-control-v1 (KWin 6.2+, wlroots
// 0.19+ compositors such as Sway, Hyprland, COSMIC...): a private Wayland
// connection that reads and sets the selection without a focused surface.
// The core wl_data_device only lets the client with keyboard focus do either,
// and laufey's own GTK connection is never that client under CEF (Chromium's
// connection owns the window's surfaces), nor while the app is in the
// background. Absent where the compositor doesn't offer the protocol (GNOME's
// mutter): every call then answers "unavailable" and the caller uses GTK.

#ifndef LAUFEY_CLIPBOARD_DATA_CONTROL_LINUX_H_
#define LAUFEY_CLIPBOARD_DATA_CONTROL_LINUX_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace laufey_common {
namespace data_control {

// Whether the compositor offers ext-data-control-v1 on a seat. Connects on
// the first call, on the connection's own thread, waiting for it a few
// seconds at most (LAUFEY_CLIPBOARD=gtk turns the protocol off). False from
// then on once the connection breaks.
bool Available();

// Whether the Wayland compositor is GNOME's mutter (it advertises
// gtk_shell1). Mutter has no data-control, but its Xwayland selection bridge
// copies the clipboard both ways whatever has focus.
bool CompositorIsMutter();

// The MIME types of the current selection (empty when there is none).
// False when the protocol is unavailable.
bool Types(std::vector<std::string>* out);

// The selection's data for the first of `mimes` it offers. False when the
// protocol is unavailable; *found is false when no such type is offered,
// the owner didn't answer in time, or the data exceeds `max_bytes`.
bool Read(const std::vector<std::string>& mimes, size_t max_bytes,
          std::string* out, std::string* mime_out, bool* found);

// Makes us the selection owner, offering each (MIME type, data) entry.
// False when the protocol is unavailable, or the compositor didn't confirm
// the new selection within a few seconds.
using Entries =
    std::vector<std::pair<std::string, std::shared_ptr<const std::string>>>;
bool Write(Entries entries);

// Selection changes call FireClipboardChange() while on.
void Watch(bool on);

// Transfers of our own selection in flight (each on a writer thread of its
// own, at most 8 at once; tests).
int ActiveWriters();

}  // namespace data_control
}  // namespace laufey_common

#endif  // LAUFEY_CLIPBOARD_DATA_CONTROL_LINUX_H_
