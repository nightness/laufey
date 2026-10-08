# Native dialogs

laufey can show the operating system's standard alert, confirmation, and prompt
dialogs. Each call is modal and blocks until the user dismisses the dialog, then
returns the user's response. A dialog can be attached to a specific window or
shown at the application level.

```rust
win.alert("Heads up", "File saved.");

if win.confirm("Delete", "Are you sure?") {
  // The user clicked OK or Yes.
}

if let Some(name) = win.prompt("Name", "What's your name?", "World") {
  println!("hello {name}");
}
```

Although the call blocks the calling thread, the underlying platform routine —
`runModal` on macOS, `MessageBoxW` on Windows, and `gtk_dialog_run` on Linux —
keeps pumping operating-system events while the dialog is open, so your other
windows continue to render and respond. A prompt returns the text the user
entered, or `None` if the user cancelled. The same three operations are also
available as the application-scoped free functions `laufey::alert`,
`laufey::confirm`, and `laufey::prompt`.

On the CEF and WebView backends, the page's own `alert()`, `confirm()`, and
`prompt()` calls are routed to these native dialogs. The Winit backend has no
web engine, so it has no page dialogs to route. On Linux, CEF makes its GTK
dialog modal to the page's window: the dialog is transient for it on X11, and
the browser view takes no input while it is open (Chromium's window is not a GTK
window, so GTK's own modality would not reach it).

On Linux, quitting while a GTK dialog of the CEF or WebView backend is open
(`laufey::quit()`, `laufey::exit()`, a termination signal) ends the dialog as a
cancel: `alert` returns, `confirm` returns `false` and `prompt` returns `None`
(a page's own dialog answers the same). The dialog is a nested loop on the UI
thread, and the app could not end until someone dismissed it. A dialog asked for
once the app is quitting is not shown, and returns as cancelled.

On Linux the Winit backend shows all three dialogs from the first provider the
session has, in the same order: `kdialog` (Plasma), then `zenity` (GNOME), then
an in-process GTK dialog. The GTK dialog is the main path where neither tool is
installed (Fedora ships neither); it runs on a GTK thread of its own, started on
first use, whatever thread asks. xdg-desktop-portal has no text-input portal, so
there is no portal step. The tools run as programs with the strings as
arguments, never through a shell, and show the page's text as plain text
(`zenity --no-markup`; kdialog's label escaped). A tool that finds no display
(zenity exits 1 with "Failed to open display", as for a cancel) is not a cancel:
the next provider runs. When none can show it (no display, no tool and no GTK
display), `show_dialog` returns `-1` (API 45): `laufey::try_prompt`,
`try_confirm` and `try_alert` (and the `Window` methods) return
`Err(DialogUnsupported)`, while `prompt` returns `None` and `confirm` `false` as
for a cancel.
