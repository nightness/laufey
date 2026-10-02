// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the portable part of laufey_window.h (window_state.cc).
// Plain asserts, no framework: run via `ctest --test-dir webview/build` (or
// cef/build). Exits non-zero on the first failure.

#include "laufey_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

struct StateCall {
  uint32_t window_id;
  uint32_t state;
  uint32_t previous;
};
std::vector<StateCall> g_state_calls;

void OnState(void* user_data, uint32_t window_id, uint32_t state,
             uint32_t previous) {
  EXPECT(user_data == &g_state_calls);
  g_state_calls.push_back({window_id, state, previous});
}

int g_display_calls = 0;
void OnDisplay(void* user_data) {
  EXPECT(user_data == &g_display_calls);
  ++g_display_calls;
}

bool SameBounds(const Bounds& a, const Bounds& b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

laufey_screen_t Screen(int64_t id, int x, int y, int w, int h) {
  laufey_screen_t s = {};
  s.id = id;
  s.x = x;
  s.y = y;
  s.width = w;
  s.height = h;
  s.work_x = x;
  s.work_y = y;
  s.work_width = w;
  s.work_height = h;
  s.scale_factor = 1.0;
  return s;
}

}  // namespace

static void TestStateDiffing() {
  SetWindowStateHandler(OnState, &g_state_calls);
  // A first normal report only records.
  EXPECT(!ReportWindowState(1, 0));
  EXPECT(g_state_calls.empty());
  // A change fires once, with the previous state.
  EXPECT(ReportWindowState(1, LAUFEY_WINDOW_STATE_MAXIMIZED));
  EXPECT(g_state_calls.size() == 1);
  EXPECT(g_state_calls[0].window_id == 1);
  EXPECT(g_state_calls[0].state == LAUFEY_WINDOW_STATE_MAXIMIZED);
  EXPECT(g_state_calls[0].previous == 0);
  // Duplicates from several OS signals are dropped.
  EXPECT(!ReportWindowState(1, LAUFEY_WINDOW_STATE_MAXIMIZED));
  EXPECT(g_state_calls.size() == 1);
  EXPECT(LastReportedWindowState(1) == LAUFEY_WINDOW_STATE_MAXIMIZED);
  // Combined bits are one state.
  EXPECT(ReportWindowState(
      1, LAUFEY_WINDOW_STATE_MAXIMIZED | LAUFEY_WINDOW_STATE_MINIMIZED));
  EXPECT(g_state_calls.back().previous == LAUFEY_WINDOW_STATE_MAXIMIZED);
  // A first report that is not normal is a change from normal.
  EXPECT(ReportWindowState(2, LAUFEY_WINDOW_STATE_FULLSCREEN));
  EXPECT(g_state_calls.back().window_id == 2);
  EXPECT(g_state_calls.back().previous == 0);
  // Forgetting a window resets it.
  ForgetWindow(2);
  EXPECT(LastReportedWindowState(2) == 0);
  // No handler: still recorded, nothing called.
  SetWindowStateHandler(nullptr, nullptr);
  size_t before = g_state_calls.size();
  EXPECT(ReportWindowState(1, 0));
  EXPECT(g_state_calls.size() == before);
  EXPECT(LastReportedWindowState(1) == 0);
  ForgetWindow(1);
}

static void TestSizeConstraints() {
  SizeConstraints c = SetSizeConstraints(10, 400, 300, 200, 0);
  // A maximum below the minimum is raised to it; 0 stays unbounded.
  EXPECT(c.min_width == 400 && c.min_height == 300);
  EXPECT(c.max_width == 400 && c.max_height == 0);
  c = SetSizeConstraints(10, -5, 100, 800, 600);
  EXPECT(c.min_width == 0 && c.min_height == 100);
  EXPECT(c.max_width == 800 && c.max_height == 600);
  SizeConstraints read = GetSizeConstraints(10);
  EXPECT(read.min_height == 100 && read.max_width == 800);
  EXPECT(GetSizeConstraints(11).min_width == 0);

  int w = 50, h = 50;
  EXPECT(ClampSizeForWindow(10, &w, &h));
  EXPECT(w == 50 && h == 100);
  w = 1000;
  h = 1000;
  EXPECT(ClampSizeForWindow(10, &w, &h));
  EXPECT(w == 800 && h == 600);
  w = 640;
  h = 480;
  EXPECT(!ClampSizeForWindow(10, &w, &h));
  EXPECT(w == 640 && h == 480);
  // Unconstrained windows pass through; NULL pointers are fine.
  w = 1;
  h = 1;
  EXPECT(!ClampSizeForWindow(11, &w, &h));
  EXPECT(!ClampSize(SizeConstraints{}, nullptr, nullptr));
  ForgetWindow(10);
  EXPECT(GetSizeConstraints(10).max_width == 0);
}

static void TestNormalBounds() {
  const uint32_t id = 20;
  Bounds out;
  EXPECT(!GetCommittedNormalBounds(id, &out));

  Bounds a{10, 20, 800, 600};
  // The first geometry is the normal bounds right away.
  NoteWindowGeometry(id, a, true, 1000);
  EXPECT(GetCommittedNormalBounds(id, &out) && SameBounds(out, a));

  // A move that settles becomes the normal bounds.
  Bounds b{50, 60, 800, 600};
  NoteWindowGeometry(id, b, true, 2000);
  // Zoom animation frames arrive while the window still looks normal...
  NoteWindowGeometry(id, Bounds{40, 50, 900, 700}, true, 5000);
  NoteWindowGeometry(id, Bounds{20, 30, 1100, 800}, true, 5100);
  // ...then the window reports maximized: the young frames are dropped and
  // the settled pre-zoom frame stays.
  NoteWindowLeftNormal(id, 5150);
  ReportWindowState(id, LAUFEY_WINDOW_STATE_MAXIMIZED);
  NoteWindowGeometry(id, Bounds{0, 25, 1440, 875}, false, 5200);
  EXPECT(GetCommittedNormalBounds(id, &out) && SameBounds(out, b));

  // Fullscreen commits the frame it announces right away.
  Bounds c{70, 80, 640, 480};
  CommitNormalBounds(id, c);
  EXPECT(GetCommittedNormalBounds(id, &out) && SameBounds(out, c));

  // Back to normal: a new candidate settles on read once it is old enough.
  ReportWindowState(id, 0);
  Bounds d{90, 100, 500, 400};
  NoteWindowGeometry(id, d, true, MonotonicMs() - kNormalBoundsSettleMs - 1);
  EXPECT(GetCommittedNormalBounds(id, &out) && SameBounds(out, d));
  // A fresh candidate does not replace it yet.
  NoteWindowGeometry(id, Bounds{1, 2, 3, 4}, true, MonotonicMs());
  EXPECT(GetCommittedNormalBounds(id, &out) && SameBounds(out, d));

  // The same geometry reported again (a periodic recheck) keeps resting:
  // it is not mistaken for a fresh animation frame when the window then
  // maximizes.
  ForgetWindow(id);
  Bounds e{80, 90, 520, 420};
  NoteWindowGeometry(id, Bounds{5, 29, 800, 600}, true, 0);
  NoteWindowGeometry(id, e, true, 1000);
  NoteWindowGeometry(id, e, true, 1350);
  NoteWindowGeometry(id, e, true, 1700);
  NoteWindowLeftNormal(id, 1750);
  ReportWindowState(id, LAUFEY_WINDOW_STATE_MAXIMIZED);
  EXPECT(GetCommittedNormalBounds(id, &out) && SameBounds(out, e));
  ForgetWindow(id);
  EXPECT(!GetCommittedNormalBounds(id, &out));
}

static void TestScreens() {
  std::vector<laufey_screen_t> screens = {Screen(7, 0, 0, 1920, 1080),
                                          Screen(9, 1920, 0, 2560, 1440)};
  laufey_screen_t out[1];
  // Reports the total even when the buffer is short.
  EXPECT(CopyScreens(screens, out, 1) == 2);
  EXPECT(out[0].id == 7);
  EXPECT(CopyScreens(screens, nullptr, 0) == 2);

  // Most overlap wins.
  EXPECT(ScreenForBounds(screens, Bounds{1800, 100, 400, 300}) == 9);
  EXPECT(ScreenForBounds(screens, Bounds{1700, 100, 300, 300}) == 7);
  // Off every screen: the nearest.
  EXPECT(ScreenForBounds(screens, Bounds{5000, 100, 100, 100}) == 9);
  EXPECT(ScreenForBounds(screens, Bounds{-3000, -3000, 100, 100}) == 7);
  EXPECT(ScreenForBounds({}, Bounds{}) == 0);

  int64_t h1 = HashDisplayName("\\\\.\\DISPLAY1", 12);
  int64_t h2 = HashDisplayName("\\\\.\\DISPLAY2", 12);
  EXPECT(h1 > 0 && h2 > 0 && h1 != h2);
  EXPECT(h1 == HashDisplayName("\\\\.\\DISPLAY1", 12));
  // Safe integer range, so a JS number holds it exactly.
  EXPECT(h1 < (int64_t{1} << 53));

  SetDisplayChangedHandler(OnDisplay, &g_display_calls);
  NotifyDisplayChanged();
  EXPECT(g_display_calls == 1);
  SetDisplayChangedHandler(nullptr, nullptr);
  NotifyDisplayChanged();
  EXPECT(g_display_calls == 1);
}

static void TestLifetime() {
  EXPECT(QuitOnLastWindowClosed());
  EXPECT(ShouldEndLoopAfterLastWindow());
  SetQuitOnLastWindowClosed(false);
  EXPECT(!ShouldEndLoopAfterLastWindow());
  // quit() ends the loop whatever keep-alive says.
  MarkQuitting();
  EXPECT(IsQuitting());
  EXPECT(ShouldEndLoopAfterLastWindow());
  SetQuitOnLastWindowClosed(true);
}

int main() {
  TestStateDiffing();
  TestSizeConstraints();
  TestNormalBounds();
  TestScreens();
  TestLifetime();
  std::printf("window_state_test: OK\n");
  return 0;
}
