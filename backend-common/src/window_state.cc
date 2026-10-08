// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Portable window bookkeeping shared by the CEF and WebView backends. See
// laufey_window.h.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#endif

#include "laufey_single_instance.h"
#include "laufey_window.h"

namespace laufey_common {

namespace {

struct NormalBoundsState {
  bool has_committed = false;
  Bounds committed;
  bool has_candidate = false;
  Bounds candidate;
  int64_t candidate_ms = 0;
};

struct WindowRecord {
  bool has_state = false;
  uint32_t state = 0;
  SizeConstraints constraints;
  NormalBoundsState normal;
};

std::mutex g_mutex;
std::map<uint32_t, WindowRecord> g_windows;

laufey_window_state_fn g_state_handler = nullptr;
void* g_state_user_data = nullptr;

laufey_display_changed_fn g_display_handler = nullptr;
void* g_display_user_data = nullptr;

std::atomic<bool> g_quit_on_last_window{true};
std::atomic<bool> g_quitting{false};
std::atomic<bool> g_exit_requested{false};
std::atomic<int> g_exit_code{0};

// Commits a candidate that has been stable long enough. Caller holds
// g_mutex.
void SettleCandidate(NormalBoundsState* n, int64_t now_ms) {
  if (n->has_candidate && now_ms - n->candidate_ms >= kNormalBoundsSettleMs) {
    n->committed = n->candidate;
    n->has_committed = true;
    n->has_candidate = false;
  }
}

bool SameBounds(const Bounds& a, const Bounds& b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

int64_t Overlap(const laufey_screen_t& s, const Bounds& b) {
  int64_t left = std::max<int64_t>(s.x, b.x);
  int64_t top = std::max<int64_t>(s.y, b.y);
  int64_t right = std::min<int64_t>(int64_t{s.x} + s.width,
                                    int64_t{b.x} + std::max(b.width, 1));
  int64_t bottom = std::min<int64_t>(int64_t{s.y} + s.height,
                                     int64_t{b.y} + std::max(b.height, 1));
  if (right <= left || bottom <= top)
    return 0;
  return (right - left) * (bottom - top);
}

int64_t DistanceSquared(const laufey_screen_t& s, const Bounds& b) {
  // From the window's center to the nearest point of the screen.
  int64_t cx = int64_t{b.x} + b.width / 2;
  int64_t cy = int64_t{b.y} + b.height / 2;
  int64_t nx = std::clamp<int64_t>(cx, s.x, int64_t{s.x} + s.width);
  int64_t ny = std::clamp<int64_t>(cy, s.y, int64_t{s.y} + s.height);
  return (cx - nx) * (cx - nx) + (cy - ny) * (cy - ny);
}

}  // namespace

// --- Window state -----------------------------------------------------------

void SetWindowStateHandler(laufey_window_state_fn handler, void* user_data) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state_handler = handler;
  g_state_user_data = user_data;
}

bool ReportWindowState(uint32_t window_id, uint32_t state) {
  laufey_window_state_fn handler = nullptr;
  void* user_data = nullptr;
  uint32_t previous = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    WindowRecord& w = g_windows[window_id];
    previous = w.has_state ? w.state : 0;
    w.has_state = true;
    w.state = state;
    if (previous == state)
      return false;
    handler = g_state_handler;
    user_data = g_state_user_data;
  }
  // Outside the lock: the handler may call back into the backend.
  if (handler)
    handler(user_data, window_id, state, previous);
  return true;
}

uint32_t LastReportedWindowState(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_windows.find(window_id);
  return it == g_windows.end() ? 0 : it->second.state;
}

void ForgetWindow(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_windows.erase(window_id);
}

// --- Size constraints -------------------------------------------------------

SizeConstraints SetSizeConstraints(uint32_t window_id, int min_width,
                                   int min_height, int max_width,
                                   int max_height) {
  SizeConstraints c;
  c.min_width = std::max(min_width, 0);
  c.min_height = std::max(min_height, 0);
  c.max_width = std::max(max_width, 0);
  c.max_height = std::max(max_height, 0);
  if (c.max_width > 0 && c.max_width < c.min_width)
    c.max_width = c.min_width;
  if (c.max_height > 0 && c.max_height < c.min_height)
    c.max_height = c.min_height;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_windows[window_id].constraints = c;
  return c;
}

SizeConstraints GetSizeConstraints(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_windows.find(window_id);
  return it == g_windows.end() ? SizeConstraints{} : it->second.constraints;
}

bool ClampSize(const SizeConstraints& c, int* width, int* height) {
  bool changed = false;
  if (width) {
    int w = *width;
    if (c.min_width > 0 && w < c.min_width)
      w = c.min_width;
    if (c.max_width > 0 && w > c.max_width)
      w = c.max_width;
    changed |= w != *width;
    *width = w;
  }
  if (height) {
    int h = *height;
    if (c.min_height > 0 && h < c.min_height)
      h = c.min_height;
    if (c.max_height > 0 && h > c.max_height)
      h = c.max_height;
    changed |= h != *height;
    *height = h;
  }
  return changed;
}

bool ClampSizeForWindow(uint32_t window_id, int* width, int* height) {
  return ClampSize(GetSizeConstraints(window_id), width, height);
}

// --- Normal bounds ----------------------------------------------------------

void NoteWindowGeometry(uint32_t window_id, const Bounds& bounds,
                        bool is_normal, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(g_mutex);
  NormalBoundsState& n = g_windows[window_id].normal;
  if (!is_normal)
    return;
  // A candidate that has settled before this new geometry arrived was a
  // real resting place of the window: keep it.
  SettleCandidate(&n, now_ms);
  if (!n.has_committed) {
    // The very first geometry of a window is its normal bounds until
    // something better settles.
    n.committed = bounds;
    n.has_committed = true;
  }
  // The same geometry seen again (several OS signals, or a periodic
  // recheck) keeps its original timestamp: it has been resting since then.
  if (n.has_candidate && SameBounds(n.candidate, bounds))
    return;
  n.candidate = bounds;
  n.candidate_ms = now_ms;
  n.has_candidate = true;
}

void CommitNormalBounds(uint32_t window_id, const Bounds& bounds) {
  std::lock_guard<std::mutex> lock(g_mutex);
  NormalBoundsState& n = g_windows[window_id].normal;
  n.committed = bounds;
  n.has_committed = true;
  n.has_candidate = false;
}

void NoteWindowLeftNormal(uint32_t window_id, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(g_mutex);
  NormalBoundsState& n = g_windows[window_id].normal;
  SettleCandidate(&n, now_ms);
  n.has_candidate = false;
}

bool GetCommittedNormalBounds(uint32_t window_id, Bounds* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_windows.find(window_id);
  if (it == g_windows.end())
    return false;
  NormalBoundsState& n = it->second.normal;
  // A window still in the normal state settles its candidate on read.
  if ((it->second.state &
       (LAUFEY_WINDOW_STATE_MAXIMIZED | LAUFEY_WINDOW_STATE_MINIMIZED |
        LAUFEY_WINDOW_STATE_FULLSCREEN)) == 0) {
    SettleCandidate(&n, MonotonicMs());
  }
  if (!n.has_committed)
    return false;
  if (out)
    *out = n.committed;
  return true;
}

int64_t MonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- Screens ----------------------------------------------------------------

size_t CopyScreens(const std::vector<laufey_screen_t>& screens,
                   laufey_screen_t* out, size_t capacity) {
  if (out) {
    size_t n = std::min(capacity, screens.size());
    for (size_t i = 0; i < n; ++i)
      out[i] = screens[i];
  }
  return screens.size();
}

int64_t ScreenForBounds(const std::vector<laufey_screen_t>& screens,
                        const Bounds& bounds) {
  if (screens.empty())
    return 0;
  const laufey_screen_t* best = nullptr;
  int64_t best_overlap = 0;
  for (const auto& s : screens) {
    int64_t o = Overlap(s, bounds);
    if (o > best_overlap) {
      best_overlap = o;
      best = &s;
    }
  }
  if (best)
    return best->id;
  // Off every screen: the nearest one.
  best = &screens[0];
  int64_t best_d = DistanceSquared(screens[0], bounds);
  for (const auto& s : screens) {
    int64_t d = DistanceSquared(s, bounds);
    if (d < best_d) {
      best_d = d;
      best = &s;
    }
  }
  return best->id;
}

int64_t HashDisplayName(const char* data, size_t len) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<unsigned char>(data[i]);
    h *= 1099511628211ull;
  }
  // Positive and non-zero, so it survives a round trip through a JS number
  // and never reads as "unknown".
  int64_t id = static_cast<int64_t>(h & 0x1fffffffffffffull);
  return id == 0 ? 1 : id;
}

void SetDisplayChangedHandler(laufey_display_changed_fn handler,
                              void* user_data) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_display_handler = handler;
  g_display_user_data = user_data;
}

void NotifyDisplayChanged() {
  laufey_display_changed_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    handler = g_display_handler;
    user_data = g_display_user_data;
  }
  if (handler)
    handler(user_data);
}

// --- App lifetime -----------------------------------------------------------

void SetQuitOnLastWindowClosed(bool quit) {
  g_quit_on_last_window.store(quit);
}

bool QuitOnLastWindowClosed() {
  return g_quit_on_last_window.load();
}

void MarkQuitting() {
  g_quitting.store(true);
  // Quitting: a later launch must become the primary, not be forwarded here.
  MarkSingleInstanceEnding();
}

bool IsQuitting() {
  return g_quitting.load();
}

bool ShouldEndLoopAfterLastWindow() {
  return IsQuitting() || QuitOnLastWindowClosed();
}

void MarkExitRequested(int code) {
  // The first request wins: a second exit_app (or one racing a panic's exit)
  // doesn't change the code the app already asked for.
  bool expected = false;
  if (g_exit_requested.compare_exchange_strong(expected, true))
    g_exit_code.store(code);
  MarkQuitting();
}

bool ExitRequested() {
  return g_exit_requested.load();
}

int RequestedExitCode() {
  return g_exit_requested.load() ? g_exit_code.load() : 0;
}

#if defined(_WIN32)
void EndProcess(int code) {
  std::cout.flush();
  std::cerr.flush();
  fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
  // Terminating the current process doesn't return; should it ever, the
  // CRT's fast exit still runs no atexit callback or destructor.
  _exit(code);
}
#endif

}  // namespace laufey_common
