# Drag and drop

laufey reports files dragged over a window and dropped on it with their native
paths, and lets the app drag its own files out to other apps or the desktop.

```rust
// Files into the window.
laufey::on_file_drop(|e| match e.phase {
  laufey::FileDragPhase::Drop => {
    println!("dropped on window {} at ({}, {}): {:?}", e.window_id, e.x, e.y, e.paths);
  }
  _ => {}
});

// Files out of the window: call it while the left button is held, e.g. from
// the page's `dragstart` after `event.preventDefault()`.
let result = laufey::start_file_drag(window_id, &["/Users/me/report.pdf"], None).await;
```

## Files into a window

C ABI (API ≥ 39):

```c
void (*set_file_drop_handler)(void* backend_data, laufey_file_drop_fn handler,
                              void* user_data);

typedef void (*laufey_file_drop_fn)(void* user_data, uint32_t window_id,
                                    int phase, double x, double y,
                                    const char* const* paths, size_t count);
```

The handler is process-wide and fires on the backend UI thread, once per phase:

| Phase               | `paths`                                                                                 | `count`             |
| ------------------- | --------------------------------------------------------------------------------------- | ------------------- |
| `LAUFEY_DRAG_ENTER` | the dragged files, or `NULL` where the engine reveals them only on the drop (see below) | the number of files |
| `LAUFEY_DRAG_OVER`  | as for ENTER                                                                            | as for ENTER        |
| `LAUFEY_DRAG_LEAVE` | `NULL` (the drag left the window, or was cancelled)                                     | 0                   |
| `LAUFEY_DRAG_DROP`  | the dropped files (never `NULL`)                                                        | the number of paths |

`x` and `y` are the pointer in window content coordinates, the space of the
mouse handlers. Only drags that carry files are reported: text, links and file
promises that are not files yet (a photo dragged out of Photos, a mail
attachment) are not. A drop is capped at `LAUFEY_MAX_DROP_PATHS` (4096) paths.

**The page keeps its own drag events.** Every backend still delivers the DOM
`dragenter` / `dragover` / `dragleave` / `drop` events to the page, whose
`DataTransfer` has `File` objects but never paths. The handler above is where
the paths are, so an app's trusted code, not the page, decides what to do with
them. A window always accepts a file drag (the cursor shows a copy), so the drop
reaches the runtime even where the page doesn't handle it; what the page does
with it is unchanged.

`window_capabilities()` reports `LAUFEY_WINDOW_CAP_FILE_DROP` where the handler
fires and `LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS` where ENTER / OVER already
carry the paths.

### Per backend

| Backend            | Where the drop is seen                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | Paths before the drop | OVER |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------- | ---- |
| WKWebView (macOS)  | The web view (a `WKWebView` subclass) is the window's drag destination; each `NSDraggingDestination` call reports the phase with the pasteboard's file URLs, then lets WebKit handle it for the page.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | yes                   | yes  |
| WebKitGTK (Linux)  | GTK's drag signals on the web view, observed before WebKit's own handlers; the paths are the `text/uri-list` WebKit itself requests, so ENTER fires when that data arrives. A page that refuses file drops (`dropEffect = "none"`) also hides them from the runtime, since GTK then never sends the drop.                                                                                                                                                                                                                                                                                                                                                                                                                            | yes                   | yes  |
| WebView2 (Windows) | An observer script reports the page's trusted drag events; on the drop it posts the `File` objects to the host with `postMessageWithAdditionalObjects`, and WebView2 hands the host their native paths (`ICoreWebView2File`). The messages carry a per-process token the page can't read, and come from the main frame only.                                                                                                                                                                                                                                                                                                                                                                                                         | no (count only)       | yes  |
| CEF (all OSes)     | The browser process reads the paths from the OS's own drag data the first time the page reports a drag with files: on X11 the XDND source's `text/uri-list` (`XdndSelection`), on Windows the OLE drag's `CF_HDROP` (laufey wraps the drop target Chromium registers on the window and forwards every call to it), on macOS the drag pasteboard. CEF's `CefDragHandler::OnDragEnter` can't be used: CEF calls it only for Alloy-style browsers, and laufey's are Chrome style. An observer the renderer injects into the main frame (with a send function no page script can reach) reports where the drag goes and the drop, from trusted events only. Under Wayland there is no drag source to ask, so drops carry no paths there. | yes                   | yes  |
| Winit              | winit's `HoveredFile` / `HoveredFileCancelled` / `DroppedFile`, gathered per window and reported once the event queue is drained. winit 0.30 has no drag position: x/y are the last pointer position seen in the window (often 0, 0).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | yes                   | no   |

On the two Chromium engines (CEF, WebView2) the observer cancels `dragover` for
file drags, which is what makes Chromium deliver `drop` at all; the page's own
handlers still run after it and decide what the drop does in the page.

### Testing

```c
bool (*test_trigger_file_drop)(void* backend_data, uint32_t window_id,
                               int phase, double x, double y,
                               const char* const* paths, size_t count);
```

Test-only: delivers a phase to the handler through the same dispatch the OS path
uses (paths copied and capped the same way), on the UI thread (on Winit, the
calling thread). Returns whether a handler got it.

## Files out of a window

```c
void (*start_file_drag)(void* backend_data, uint32_t window_id,
                        const char* const* paths, size_t count,
                        const uint8_t* icon_png, size_t icon_len,
                        laufey_drag_result_fn callback, void* user_data);

typedef void (*laufey_drag_result_fn)(void* user_data, int result);
```

Starts an OS drag of `count` existing absolute paths, offered as a copy, with
`icon_png` under the pointer (`NULL`: the platform's file icon). The OS runs the
drag from the pointer's current position, so call it while the left mouse button
is held: from the page's `dragstart` (cancel the page's own drag with
`preventDefault()` and ask the runtime to start this one), or from a `mousedown`
followed by a move. The callback fires exactly once:

- `LAUFEY_DRAG_RESULT_DROPPED`: a target took the files;
- `LAUFEY_DRAG_RESULT_CANCELLED`: the user pressed Escape, or dropped where
  nothing accepted them;
- `LAUFEY_DRAG_RESULT_FAILED`: the drag never started. It comes synchronously,
  before `start_file_drag` returns, for no paths, more than 4096, or a path that
  is not an existing absolute path; and from the UI thread (on Windows, laufey's
  I/O thread, where the drag runs) when no left button is held, another drag is
  running, or the window is gone.

| Backend        | Implementation                                                                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| macOS (both)   | `beginDraggingSessionWithItems:event:source:` from the web view (WKWebView) or the window's content view (CEF), one `NSURL` item per file. |
| Windows (both) | `DoDragDrop` with a shell data object carrying `CF_HDROP` and the preferred drop effect, plus an `IDragSourceHelper` drag image.           |
| WebKitGTK      | A GTK drag source on the window offering `text/uri-list` (X11 and Wayland).                                                                |
| CEF on Linux   | A GTK drag source on a private `GtkInvisible`, which needs an X11 display: `LAUFEY_WINDOW_CAP_FILE_DRAG_OUT` is reported only there.       |
| Winit          | Not supported (`NULL`).                                                                                                                    |

The window can be the drop target of its own drag: the drop is then reported
through the file-drop handler like any other.
