// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Drag and drop, native file dialogs and the rich clipboard (API 39). See
// docs/drag-and-drop.md, docs/file-dialogs.md and docs/clipboard.md.
//
// The portable part (io.cc) is what every CEF and WebView backend shares:
// the file-drop handler and its dispatch (path copying, the path cap, and the
// test hook), the validation of drag-out paths, the one-dialog-at-a-time file
// dialog slot (ids, exactly-once completion, cancel and the test hook), the
// copy of the dialog options, and the DOM observer script the Chromium-based
// engines (CEF, WebView2) use to report where a drag is. The per-OS parts work
// on native handles so the WebView and CEF backends of one OS share them:
// io_mac.mm, io_win.cc and io_linux.cc (plus the clipboard_*.cc files).

#ifndef LAUFEY_IO_H_
#define LAUFEY_IO_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_file_drop_observer.h"

namespace laufey_common {

// --- File drop (into a window) ---------------------------------------------

// Registers the embedder's file-drop handler (NULL clears it).
void SetFileDropHandler(laufey_file_drop_fn handler, void* user_data);

// True when a file-drop handler is registered.
bool HasFileDropHandler();

// Delivers one drag phase to the handler. `paths` is capped at
// LAUFEY_MAX_DROP_PATHS; for LEAVE it is ignored. `count` is the number of
// files dragged (used for ENTER / OVER without paths); when paths are given
// it is their number. Returns true if a handler received it. Call it on the
// backend UI thread.
bool DispatchFileDrop(uint32_t window_id, int phase, double x, double y,
                      const std::vector<std::string>& paths, size_t count);

// Backs test_trigger_file_drop: the same dispatch, from C arguments.
bool TestTriggerFileDrop(uint32_t window_id, int phase, double x, double y,
                         const char* const* paths, size_t count);

// Parses a `file://` URI (as in text/uri-list) into a native path. Returns
// false for anything that is not a local file URI.
bool FileUriToPath(const std::string& uri, std::string* path);

// The local file paths a text/uri-list (RFC 2483: CRLF or LF separated,
// "#" comment lines) names, in order, at most LAUFEY_MAX_DROP_PATHS. URIs
// that aren't local files are skipped.
std::vector<std::string> UriListToPaths(const std::string& list);

// --- Drag out ---------------------------------------------------------------

// Checks start_file_drag's arguments: 1..LAUFEY_MAX_DROP_PATHS existing
// absolute paths. Fills `out` and returns true when they are fine.
bool ValidateDragPaths(const char* const* paths, size_t count,
                       std::vector<std::string>* out);

// Calls `callback` (if any) with `result`. A drag-out completion helper so
// every backend reports exactly once.
struct DragOutRequest {
  std::vector<std::string> paths;
  std::vector<uint8_t> icon_png;
  laufey_drag_result_fn callback = nullptr;
  void* user_data = nullptr;
  void Finish(int result);
  bool finished = false;
};

#ifdef __APPLE__
// Starts the drag from `view` (an NSView*: the WKWebView, or the window's
// content view) on the main thread; hops there if needed. Takes ownership of
// `req` and finishes it exactly once.
void StartFileDragMac(void* nsview, DragOutRequest* req);

// The local files on the drag pasteboard (NSPasteboardNameDrag): the items
// of the drag in progress, or of the last one. At most
// LAUFEY_MAX_DROP_PATHS. Any thread.
std::vector<std::string> DragPasteboardFilePathsMac();
#endif

#ifdef _WIN32
// Starts the drag (DoDragDrop, a CF_HDROP data object) on the I/O window's
// thread (see WinIoInit); `hwnd` is the window the drag comes from. Takes
// ownership of `req`.
void StartFileDragWin(void* hwnd, DragOutRequest* req);
#endif

#ifdef __linux__
// Starts a GTK drag source (text/uri-list) on the GTK main thread, from the
// GtkWidget* `source_widget` returns there (the window the drag comes from;
// called on the GTK thread so a closed window is never used) or, when it is
// null, from a private GtkInvisible (X11 only: a Wayland drag needs a real
// surface). Takes ownership of `req`. False from CanStartFileDragLinux means
// GTK has no display to drag on.
bool CanStartFileDragLinux();
void StartFileDragLinux(std::function<void*()> source_widget,
                        DragOutRequest* req);
#endif

// --- File dialogs -----------------------------------------------------------

struct FileFilter {
  std::string name;
  std::vector<std::string> extensions;  // without the dot; "*" = any
};

struct FileDialogRequest {
  int kind = LAUFEY_FILE_DIALOG_OPEN;
  uint32_t flags = 0;
  std::string title;
  std::string default_path;
  std::string button_label;
  std::vector<FileFilter> filters;

  bool ChoosesDirectories() const {
    return kind == LAUFEY_FILE_DIALOG_OPEN &&
           (flags & LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES) != 0;
  }
  bool ChoosesFiles() const {
    return kind != LAUFEY_FILE_DIALOG_OPEN ||
           (flags & LAUFEY_FILE_DIALOG_CHOOSE_FILES) != 0 ||
           (flags & LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES) == 0;
  }
  bool Multiple() const {
    return kind == LAUFEY_FILE_DIALOG_OPEN &&
           (flags & LAUFEY_FILE_DIALOG_MULTIPLE) != 0;
  }
};

// Copies and checks the C options. Returns false (and leaves `out` unspecified)
// for an unknown kind or invalid UTF-8; extra filters / extensions beyond the
// LAUFEY_FILE_DIALOG_MAX_* caps and empty extensions are dropped, and a
// leading "." on an extension is removed.
bool CopyFileDialogOptions(const laufey_file_dialog_options_t* options,
                           FileDialogRequest* out);

// Splits a default path into the directory to start in and the file name to
// propose: an existing directory is all directory; anything else is a
// directory (its parent, if that exists) plus a name. Either may come back
// empty.
void SplitDefaultPath(const std::string& default_path, std::string* directory,
                      std::string* name);

// The platform part of a dialog, implemented per OS. Each is called once per
// dialog, on the UI thread, by the slot below.
class FileDialogPlatform {
 public:
  virtual ~FileDialogPlatform() = default;
  // Show the dialog. Returns false if it could not be shown (the slot then
  // finishes it with FAILED).
  virtual bool Show() = 0;
  // Close it as cancelled (cancel_file_dialog). The dialog's own completion
  // path then finishes it.
  virtual void Cancel() = 0;
  // Fill in `path` (may be empty) and accept, as the test hook asks. Returns
  // false if the platform can't.
  virtual bool TestAccept(const std::string& path) = 0;
};

// The one-dialog-at-a-time slot.
//   Begin  reserves the slot and returns the new id, or 0 when a dialog is
//          already open (the caller answers BUSY).
//   Attach hands the slot the platform dialog once it exists.
//   Finish delivers the result exactly once and frees the slot; later calls
//          for the same id are ignored.
// Cancel / TestRespond act on the open dialog (any thread) through
// `run_on_ui` and report whether one was open.
using UiRunner = std::function<void(std::function<void()>)>;

uint32_t FileDialogBegin(laufey_file_dialog_result_fn callback,
                         void* user_data);
void FileDialogAttach(uint32_t dialog_id, FileDialogPlatform* platform);
void FileDialogFinish(uint32_t dialog_id, int status,
                      const std::vector<std::string>& paths);
bool FileDialogIsOpen(uint32_t dialog_id);
bool FileDialogCancel(uint32_t dialog_id, const UiRunner& run_on_ui);
bool FileDialogTestRespond(int action, const char* path,
                           const UiRunner& run_on_ui);

// Validates the request and either answers at once (returns 0) or reserves
// the slot and runs `show(id, request)` on the UI thread through `run_on_ui`
// (returns the id). `show` must create the platform dialog, FileDialogAttach
// it and Show() it, or FileDialogFinish(id, FAILED, {}).
uint32_t ShowFileDialogCommon(
    const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data,
    const UiRunner& run_on_ui,
    std::function<void(uint32_t, const FileDialogRequest&)> show);

// Each OS's ShowFileDialog* resolves its parent window on the UI thread,
// right before showing, through `parent` (null, or returning null: an
// app-level dialog), so a window closed in between is never used.
using ParentResolver = std::function<void*()>;

#ifdef __APPLE__
// NSOpenPanel / NSSavePanel; a sheet on the NSWindow* `parent` returns.
uint32_t ShowFileDialogMac(ParentResolver parent,
                           const laufey_file_dialog_options_t* options,
                           laufey_file_dialog_result_fn callback,
                           void* user_data);
bool CancelFileDialogMac(uint32_t dialog_id);
bool TestFileDialogRespondMac(int action, const char* path);
#endif

#ifdef _WIN32
// IFileOpenDialog / IFileSaveDialog, owned by the HWND `parent` returns, shown
// on the I/O window's thread (see WinIoInit).
uint32_t ShowFileDialogWin(ParentResolver parent,
                           const laufey_file_dialog_options_t* options,
                           laufey_file_dialog_result_fn callback,
                           void* user_data);
bool CancelFileDialogWin(uint32_t dialog_id);
bool TestFileDialogRespondWin(int action, const char* path);
#endif

#ifdef __linux__
// GtkFileChooserNative (portal-aware), transient for the GtkWindow* `parent`
// returns.
uint32_t ShowFileDialogLinux(ParentResolver parent,
                             const laufey_file_dialog_options_t* options,
                             laufey_file_dialog_result_fn callback,
                             void* user_data);
bool CancelFileDialogLinux(uint32_t dialog_id);
bool TestFileDialogRespondLinux(int action, const char* path);
#endif

// --- Clipboard ---------------------------------------------------------------
//
// Any thread: each OS implementation runs where it must (macOS: the main
// queue; Linux: the GTK main context; Windows: the calling thread, retrying
// OpenClipboard while another app holds the clipboard).

// Registers the clipboard-change handler (NULL clears it) and starts / stops
// the OS watcher accordingly.
void SetClipboardChangeHandler(laufey_clipboard_change_fn handler,
                               void* user_data);
// Calls the handler, if any (from the OS watcher, on the UI thread).
void FireClipboardChange();

// Joins MIME types with '\n' into a malloc'd C string (the
// read_clipboard_formats format), dropping duplicates in order.
char* JoinClipboardFormats(const std::vector<std::string>& formats);

// CF_HTML ("HTML Format", Windows) wrapping and unwrapping; portable so the
// unit test runs everywhere.
std::string BuildCfHtml(const std::string& fragment);
bool ExtractCfHtmlFragment(const std::string& cf_html, std::string* html);

// True if `data` starts with the PNG signature.
bool LooksLikePng(const uint8_t* data, size_t len);

// malloc'd copy of `len` bytes (NULL for 0 or on failure).
uint8_t* MallocCopy(const void* data, size_t len);

#ifdef __APPLE__
uint32_t ClipboardCapabilitiesMac();
char* ClipboardReadHtmlMac();
bool ClipboardWriteHtmlMac(const std::string& html, const char* text_or_null);
uint8_t* ClipboardReadImageMac(size_t* len_out);
bool ClipboardWriteImageMac(const uint8_t* png, size_t len);
char* ClipboardReadFormatsMac();
// Starts / stops the change-count poll.
void ClipboardWatchMac(bool on);
#endif

#ifdef _WIN32
uint32_t ClipboardCapabilitiesWin();
char* ClipboardReadHtmlWin();
bool ClipboardWriteHtmlWin(const std::string& html, const char* text_or_null);
uint8_t* ClipboardReadImageWin(size_t* len_out);
bool ClipboardWriteImageWin(const uint8_t* png, size_t len);
char* ClipboardReadFormatsWin();
// Adds / removes the I/O window as a clipboard format listener.
void ClipboardWatchWin(bool on);

// The I/O thread: laufey's own STA thread (OLE initialized) with a
// message-only window and a message loop. File dialogs and drags out run
// there, so their native modal loops never stall the engine's UI thread, and
// its window is the clipboard format listener (so change events fire on it).
// WinIoInit starts it (idempotent, any thread). WinRunOnIoThread runs `task`
// there (inline when already there); it returns false when WinIoInit was
// never called.
void WinIoInit();
bool WinRunOnIoThread(std::function<void()> task);
// Runs `task` on the I/O thread after `ms` milliseconds (a window timer, so
// it fires inside native modal loops too).
void WinRunOnIoThreadAfter(unsigned ms, std::function<void()> task);
#endif

#ifdef __linux__
uint32_t ClipboardCapabilitiesLinux();
char* ClipboardReadHtmlLinux();
bool ClipboardWriteHtmlLinux(const std::string& html, const char* text_or_null);
uint8_t* ClipboardReadImageLinux(size_t* len_out);
bool ClipboardWriteImageLinux(const uint8_t* png, size_t len);
char* ClipboardReadFormatsLinux();
void ClipboardWatchLinux(bool on);

// Runs `fn` on the GTK thread and waits (inline when already there). False
// when `fn` did not run: the backend's loop had ended (the wait never
// outlives it; see RunOnUiThreadAndWait in laufey_ui_tasks.h).
bool GtkRunSync(const std::function<void()>& fn);
// Queues `fn` on the GTK thread.
void GtkRunAsync(std::function<void()> fn);
// By default the GTK thread is whichever iterates the GLib default main
// context (gtk_main on WebKitGTK). A host whose UI thread runs its own loop
// (CEF) installs its own poster and "am I on it" check instead.
void SetGtkThread(std::function<void(std::function<void()>)> post,
                  std::function<bool()> on_thread);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_IO_H_
