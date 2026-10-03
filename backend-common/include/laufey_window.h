// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Window state, size constraints, screens, title bar / backdrop and app
// lifetime (API 38). See docs/window-management.md.
//
// The portable part (window_state.cc) is the bookkeeping every CEF and
// WebView backend shares: the last reported state of each window (so the
// state handler fires once per real change, whatever OS signal noticed it),
// the size constraints the embedder set (so set_window_size can clamp to them
// on every OS), the bounds a window returns to when it leaves the maximized /
// minimized / fullscreen state, the display-changed handler, and whether the
// event loop ends with the last window. The per-OS parts work on native
// handles so the WebView and CEF backends of one OS share them:
// window_mac.mm (NSWindow) and window_win.cc (HWND).

#ifndef LAUFEY_WINDOW_H_
#define LAUFEY_WINDOW_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "laufey.h"

namespace laufey_common {

// --- Window state -----------------------------------------------------------

// Registers the embedder's window-state handler (NULL clears it).
void SetWindowStateHandler(laufey_window_state_fn handler, void* user_data);

// Records `state` (LAUFEY_WINDOW_STATE_* bits) as the window's current state
// and, if it differs from the last one recorded, calls the handler with the
// old and new state. The first report of a window only records it (a window
// is born normal; a first report that is not 0 is a change from 0). Returns
// true if the handler was due. Call it from the UI thread, from whichever OS
// signals can reveal a change; duplicates are dropped here.
bool ReportWindowState(uint32_t window_id, uint32_t state);

// The state last recorded for the window (0 if none).
uint32_t LastReportedWindowState(uint32_t window_id);

// Drops everything recorded for a window (state, constraints, bounds). Call
// it when the window is destroyed.
void ForgetWindow(uint32_t window_id);

// --- Size constraints -------------------------------------------------------

struct SizeConstraints {
  int min_width = 0;  // 0: no constraint on that axis
  int min_height = 0;
  int max_width = 0;
  int max_height = 0;
};

// Stores the constraints for a window after normalizing them: negative values
// become 0, and a maximum below the minimum on an axis is raised to it.
// Returns the normalized constraints.
SizeConstraints SetSizeConstraints(uint32_t window_id, int min_width,
                                   int min_height, int max_width,
                                   int max_height);
SizeConstraints GetSizeConstraints(uint32_t window_id);

// Clamps a requested size to the window's constraints. Returns true if the
// size was changed.
bool ClampSize(const SizeConstraints& c, int* width, int* height);
bool ClampSizeForWindow(uint32_t window_id, int* width, int* height);

// --- Normal bounds ----------------------------------------------------------
//
// The bounds a window returns to when it leaves the maximized, minimized or
// fullscreen state. Backends that can't ask the OS (macOS, GTK, CEF) feed
// every observed geometry here; geometry seen while the window is normal is a
// candidate that becomes the normal bounds once it has been stable for
// kNormalBoundsSettleMs, so the intermediate frames of a zoom animation (which
// macOS reports while the window still looks normal) never replace the frame
// the window had before.

constexpr int64_t kNormalBoundsSettleMs = 300;

struct Bounds {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Records geometry observed at `now_ms` (any monotonic clock, in ms).
// `is_normal` is whether the window is neither maximized, minimized nor
// fullscreen right now.
void NoteWindowGeometry(uint32_t window_id, const Bounds& bounds,
                        bool is_normal, int64_t now_ms);

// Commits geometry that must be taken as normal right away, such as the
// frame of a window that is about to enter fullscreen (macOS announces it
// before the frame changes).
void CommitNormalBounds(uint32_t window_id, const Bounds& bounds);

// Called when the window leaves the normal state at `now_ms`: a candidate
// older than kNormalBoundsSettleMs is committed, a younger one (an animation
// frame) is dropped.
void NoteWindowLeftNormal(uint32_t window_id, int64_t now_ms);

// The committed normal bounds. Returns false if none was recorded yet.
bool GetCommittedNormalBounds(uint32_t window_id, Bounds* out);

// Milliseconds on a monotonic clock, for the functions above.
int64_t MonotonicMs();

// --- Screens ----------------------------------------------------------------

// Copies `screens` into `out` (up to `capacity`) and returns the count, the
// get_screens contract.
size_t CopyScreens(const std::vector<laufey_screen_t>& screens,
                   laufey_screen_t* out, size_t capacity);

// The id of the screen that overlaps `bounds` the most (the nearest one when
// none overlaps); 0 for an empty list.
int64_t ScreenForBounds(const std::vector<laufey_screen_t>& screens,
                        const Bounds& bounds);

// Stable 64-bit FNV-1a hash of a byte string, for display ids built from a
// device name. Never 0.
int64_t HashDisplayName(const char* data, size_t len);

// Registers the display-changed handler (NULL clears it).
void SetDisplayChangedHandler(laufey_display_changed_fn handler,
                              void* user_data);
// Calls the handler, if any.
void NotifyDisplayChanged();

// --- App lifetime -----------------------------------------------------------

void SetQuitOnLastWindowClosed(bool quit);
bool QuitOnLastWindowClosed();
// Set by quit(): the loop must end even with keep-alive on.
void MarkQuitting();
bool IsQuitting();
// Whether closing the last window should end the event loop now: quitting,
// or keep-alive off. (macOS backends also keep an Accessory-policy app
// alive; see ShouldQuitAfterLastWindowMac.)
bool ShouldEndLoopAfterLastWindow();

#if defined(__APPLE__)
// --- macOS (window_mac.mm). Main thread unless noted. -----------------------

// `nswindow` is an NSWindow* (bridged, unretained).
void MacSetWindowState(void* nswindow, int action);
uint32_t MacGetWindowState(void* nswindow);
// Applies the constraints as contentMinSize / contentMaxSize.
void MacApplySizeConstraints(void* nswindow, const SizeConstraints& c);
// Top-left-origin bounds (anchored to the primary screen) of the window frame
// and content size, the get_window_position / get_window_size convention.
Bounds MacWindowBounds(void* nswindow);
// Observes the window's miniaturize / zoom / fullscreen / resize / move
// notifications and feeds ReportWindowState and the normal-bounds tracker.
void MacWatchWindowState(void* nswindow, uint32_t window_id);
void MacUnwatchWindowState(void* nswindow);
// For a backend that keeps its own geometry bookkeeping (CEF): calls
// `on_change(window_id)` for the miniaturize / fullscreen / resize / move
// notifications instead. Removed by MacUnwatchWindowState.
void MacObserveWindowStateChanges(void* nswindow, uint32_t window_id,
                                  void (*on_change)(uint32_t window_id));
// Size constraints on the window's content (contentMinSize /
// contentMaxSize), for a backend that does not set them itself (CEF).
void MacApplyContentSizeConstraints(void* nswindow, const SizeConstraints& c);
// Screens (any thread; hops to the main thread when needed).
std::vector<laufey_screen_t> MacGetScreens();
int64_t MacScreenForWindow(void* nswindow);
// Starts forwarding NSApplicationDidChangeScreenParametersNotification to
// NotifyDisplayChanged (idempotent).
void MacInstallDisplayWatcher();
bool MacSetTitlebarStyle(void* nswindow, int style);
bool MacSetTrafficLightPosition(void* nswindow, int x, int y);
// Re-applies a custom traffic-light position (after a resize / fullscreen /
// title change re-laid out the title bar). No-op without one.
void MacReapplyTrafficLightPosition(void* nswindow);
// Vibrancy behind `webview` (an NSView* that must be the window's content
// view or its only subview). `backdrop` is LAUFEY_BACKDROP_VIBRANCY or
// LAUFEY_BACKDROP_NONE. Returns false for any other backdrop.
bool MacSetVibrancy(void* nswindow, void* webview, int backdrop, int material);
// Brings the app's first shown window to the front, once per process: the
// app is activated and `nswindow` made key, and the window is ordered in
// front of other apps' windows even when the system declines the activation
// (macOS 14+ only lets an app take activation cooperatively, so one started
// from a terminal, an IDE or a background agent can stay inactive). Without
// that the window opens behind the active app's, and WebKit / Chromium read
// a covered window as occluded: the page reports visibilityState "hidden"
// and requestAnimationFrame stops. Later calls only make the window key and
// order it front within the app, as before. Only for a window that is being
// shown and may activate the app: never call it for a hidden or a
// non-activating (panel) window.
void MacRevealWindowAtLaunch(void* nswindow);
// Whether closing the last window should end the loop on macOS:
// ShouldEndLoopAfterLastWindow(), except that an Accessory-policy app
// (set_dock_visible(false)) keeps running unless it is quitting.
bool ShouldQuitAfterLastWindowMac();
#endif

#if defined(_WIN32)
// --- Windows (window_win.cc). UI thread unless noted. -----------------------

// `hwnd` is the top-level HWND.
void WinSetWindowState(void* hwnd, uint32_t window_id, int action);
uint32_t WinGetWindowState(void* hwnd, uint32_t window_id);
bool WinIsFullscreen(uint32_t window_id);
// Fills a MINMAXINFO from the constraints. `units_to_pixels` converts the
// constraint units to the window's pixels (1 when the constraints are in
// outer window pixels, as in the WebView2 backend), and `chrome_w/h` is added
// when they are content sizes. Returns false when there is nothing to apply.
bool WinApplyMinMaxInfo(void* minmaxinfo, const SizeConstraints& c,
                        double units_to_pixels, int chrome_width,
                        int chrome_height);
// GetWindowPlacement's restored rectangle in screen coordinates (the
// workspace offset applied), or the rectangle saved when the window entered
// fullscreen. Returns false on failure.
bool WinGetNormalRect(void* hwnd, uint32_t window_id, int* x, int* y,
                      int* width, int* height);
// Screens in physical pixels (the coordinates GetWindowRect uses in a
// per-monitor-DPI-aware process), primary first. Any thread.
std::vector<laufey_screen_t> WinGetScreens();
// The id (as WinGetScreens) of the monitor the window is on, 0 if unknown.
int64_t WinScreenForWindow(void* hwnd);
// Creates (once) a hidden top-level window on the calling (UI) thread that
// turns WM_DISPLAYCHANGE / WM_DPICHANGED / work-area changes into
// NotifyDisplayChanged.
void WinInstallDisplayWatcher();
// The Windows build number (e.g. 22631), 0 if unknown.
uint32_t WinBuildNumber();
// Backdrop capability bits (LAUFEY_WINDOW_CAP_BACKDROP_*) for this build.
uint32_t WinBackdropCapabilities();
// Applies a LAUFEY_BACKDROP_* through DWM: DWMWA_SYSTEMBACKDROP_TYPE on
// 22621+, the older DWMWA_MICA_EFFECT for Mica on 22000-22620, and extends
// the frame into the client area so the backdrop can show. Returns false for
// a backdrop this build can't show (and changes nothing).
bool WinSetBackdrop(void* hwnd, int backdrop);
// Subclasses a window this code does not own (CEF's) to call
// `on_change(window_id)` on WM_SIZE (minimize / maximize / restore).
// Removed by WinUnsubclassForStateChanges (call it before the window is
// destroyed, e.g. from the destroy notification).
void WinSubclassForStateChanges(void* hwnd, uint32_t window_id,
                                void (*on_change)(uint32_t window_id));
void WinUnsubclassForStateChanges(void* hwnd);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_WINDOW_H_
