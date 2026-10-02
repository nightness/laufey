// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS window state, size constraints, screens, title bar, traffic lights
// and vibrancy on an NSWindow, shared by the WKWebView and CEF backends. See
// laufey_window.h.

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <map>
#include <mutex>

#include "laufey_window.h"

namespace laufey_common {

namespace {

// Runs `block` on the main thread and waits for it; inline when already
// there (a getter called from a handler on the main thread must not
// deadlock).
void RunOnMainSync(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

CGFloat PrimaryScreenHeight() {
  NSScreen* primary = [[NSScreen screens] firstObject];
  return primary ? primary.frame.size.height : 0;
}

// Cocoa's bottom-left global rect to the top-left space anchored to the
// primary screen that get_window_position uses.
NSRect FlipRect(NSRect r) {
  r.origin.y = PrimaryScreenHeight() - r.origin.y - r.size.height;
  return r;
}

bool IsFullscreen(NSWindow* w) {
  return (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

bool LogicalFullscreen(NSWindow* w);

uint32_t StateOf(NSWindow* w) {
  uint32_t state = 0;
  if (LogicalFullscreen(w)) {
    state |= LAUFEY_WINDOW_STATE_FULLSCREEN;
  } else if ((w.styleMask & NSWindowStyleMaskTitled) &&
             (w.styleMask & NSWindowStyleMaskResizable)) {
    if ([w isZoomed])
      state |= LAUFEY_WINDOW_STATE_MAXIMIZED;
  } else if (NSScreen* screen = w.screen) {
    // A borderless window has no zoom button, and -isZoomed is meaningless
    // there; it is maximized when it fills the visible frame.
    if (NSEqualRects(w.frame, screen.visibleFrame))
      state |= LAUFEY_WINDOW_STATE_MAXIMIZED;
  }
  if ([w isMiniaturized])
    state |= LAUFEY_WINDOW_STATE_MINIMIZED;
  return state;
}

// --- Per-window watch state ---

struct Watch {
  uint32_t window_id = 0;
  NSMutableArray* observers = nil;
  bool has_traffic_light = false;
  int traffic_x = 0;
  int traffic_y = 0;
  // Frame + style before a borderless "maximize" (no zoom button to undo
  // it), so unmaximize can put it back.
  bool has_borderless_restore = false;
  NSRect borderless_restore;
  // A fullscreen Space transition in progress: AppKit sets the fullscreen
  // style bit when it starts, refuses a toggle until it ends ("not in
  // fullscreen state"), and only then is the change done. +1 entering, -1
  // leaving, 0 none.
  int fs_transition = 0;
  // An enter / leave requested during a transition, applied when it ends.
  int pending_fs_action = 0;
};

std::mutex g_watch_mutex;
std::map<void*, Watch> g_watches;

Watch* FindWatch(NSWindow* w) {
  auto it = g_watches.find((__bridge void*)w);
  return it == g_watches.end() ? nullptr : &it->second;
}

// Fullscreen as the state API reports it: the style bit, except that a
// transition counts as done only when AppKit says it ended.
bool LogicalFullscreen(NSWindow* w) {
  int transition = 0;
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    if (Watch* watch = FindWatch(w))
      transition = watch->fs_transition;
  }
  if (transition > 0)
    return false;
  if (transition < 0)
    return true;
  return IsFullscreen(w);
}

// Records a transition starting (+1 / -1) or ending (0). Returns the action
// queued during it when it ends (0 if none).
int SetFullscreenTransition(NSWindow* w, int transition) {
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    Watch* watch = FindWatch(w);
    if (!watch)
      return 0;
    watch->fs_transition = transition;
    if (transition == 0) {
      int pending = watch->pending_fs_action;
      watch->pending_fs_action = 0;
      return pending;
    }
  }
  // A transition that never ends (AppKit only tells the window's delegate
  // about a failed one) must not freeze the state: give up after 5 s.
  __weak NSWindow* weak = w;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                 dispatch_get_main_queue(), ^{
                   NSWindow* strong = weak;
                   if (!strong)
                     return;
                   std::lock_guard<std::mutex> lock(g_watch_mutex);
                   if (Watch* watch = FindWatch(strong)) {
                     if (watch->fs_transition == transition) {
                       watch->fs_transition = 0;
                       watch->pending_fs_action = 0;
                     }
                   }
                 });
  return 0;
}

// Whether a fullscreen Space transition is running.
bool InFullscreenTransition(NSWindow* w) {
  std::lock_guard<std::mutex> lock(g_watch_mutex);
  Watch* watch = FindWatch(w);
  return watch && watch->fs_transition != 0;
}

void ReportAndTrack(NSWindow* w, uint32_t window_id) {
  uint32_t state = StateOf(w);
  // The frames of a fullscreen transition (the window growing into its
  // Space, or shrinking out of it) are not normal bounds, though the state
  // still reads normal until the transition ends: noting them made the
  // fullscreen frame the "normal bounds" once it had rested long enough.
  bool normal = state == 0 && !IsFullscreen(w) && !InFullscreenTransition(w);
  int64_t now = MonotonicMs();
  if (!normal && LastReportedWindowState(window_id) == 0)
    NoteWindowLeftNormal(window_id, now);
  NoteWindowGeometry(window_id, MacWindowBounds((__bridge void*)w), normal,
                     now);
  ReportWindowState(window_id, state);
}

void ApplyTrafficLights(NSWindow* w, int x, int y) {
  NSButton* close = [w standardWindowButton:NSWindowCloseButton];
  NSButton* mini = [w standardWindowButton:NSWindowMiniaturizeButton];
  NSButton* zoom = [w standardWindowButton:NSWindowZoomButton];
  if (!close || !mini || !zoom)
    return;
  // The buttons live in the title bar container view; growing it by `y`
  // moves them down, and their x is set directly. Mirrors Electron's
  // trafficLightPosition.
  NSView* container = close.superview.superview;
  if (!container)
    return;
  CGFloat button_height = close.frame.size.height;
  NSRect container_frame = container.frame;
  container_frame.size.height = button_height + y;
  container_frame.origin.y = w.frame.size.height - container_frame.size.height;
  [container setFrame:container_frame];
  CGFloat space = mini.frame.origin.x - close.frame.origin.x;
  NSArray<NSButton*>* buttons = @[ close, mini, zoom ];
  for (NSUInteger i = 0; i < buttons.count; ++i) {
    NSPoint origin = buttons[i].frame.origin;
    origin.x = x + i * space;
    [buttons[i] setFrameOrigin:origin];
  }
}

const char kEffectViewKey = 0;
const char kDrewBackgroundKey = 0;

bool IsKnownMaterial(int m) {
  switch (m) {
    case LAUFEY_VIBRANCY_TITLEBAR:
    case LAUFEY_VIBRANCY_SELECTION:
    case LAUFEY_VIBRANCY_MENU:
    case LAUFEY_VIBRANCY_POPOVER:
    case LAUFEY_VIBRANCY_SIDEBAR:
    case LAUFEY_VIBRANCY_HEADER_VIEW:
    case LAUFEY_VIBRANCY_SHEET:
    case LAUFEY_VIBRANCY_WINDOW_BACKGROUND:
    case LAUFEY_VIBRANCY_HUD:
    case LAUFEY_VIBRANCY_FULLSCREEN_UI:
    case LAUFEY_VIBRANCY_TOOLTIP:
    case LAUFEY_VIBRANCY_CONTENT_BACKGROUND:
    case LAUFEY_VIBRANCY_UNDER_WINDOW_BACKGROUND:
    case LAUFEY_VIBRANCY_UNDER_PAGE_BACKGROUND:
      return true;
    default:
      return false;
  }
}

bool g_display_watcher_installed = false;

}  // namespace

void MacSetWindowState(void* nswindow, int action) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w)
    return;
  switch (action) {
    case LAUFEY_WINDOW_ACTION_MAXIMIZE:
      if ([w isMiniaturized])
        [w deminiaturize:nil];
      if (IsFullscreen(w))
        break;
      if ((w.styleMask & NSWindowStyleMaskTitled) &&
          (w.styleMask & NSWindowStyleMaskResizable)) {
        if (![w isZoomed])
          [w zoom:nil];
      } else if (NSScreen* screen = w.screen ?: [NSScreen mainScreen]) {
        {
          std::lock_guard<std::mutex> lock(g_watch_mutex);
          if (Watch* watch = FindWatch(w)) {
            if (!NSEqualRects(w.frame, screen.visibleFrame)) {
              watch->has_borderless_restore = true;
              watch->borderless_restore = w.frame;
            }
          }
        }
        // Outside the lock: -setFrame: posts the resize notification
        // synchronously, and its observer takes the lock.
        [w setFrame:screen.visibleFrame display:YES];
      }
      break;
    case LAUFEY_WINDOW_ACTION_UNMAXIMIZE:
      if (IsFullscreen(w))
        break;
      if ((w.styleMask & NSWindowStyleMaskTitled) &&
          (w.styleMask & NSWindowStyleMaskResizable)) {
        if ([w isZoomed])
          [w zoom:nil];
      } else {
        NSRect restore = NSZeroRect;
        {
          std::lock_guard<std::mutex> lock(g_watch_mutex);
          if (Watch* watch = FindWatch(w)) {
            if (watch->has_borderless_restore) {
              restore = watch->borderless_restore;
              watch->has_borderless_restore = false;
            }
          }
        }
        if (!NSIsEmptyRect(restore))
          [w setFrame:restore display:YES];
      }
      break;
    case LAUFEY_WINDOW_ACTION_MINIMIZE:
      if (![w isMiniaturized]) {
        // A borderless window has no miniaturize button and -miniaturize:
        // refuses without the style bit; it doesn't change how it looks.
        if (!(w.styleMask & NSWindowStyleMaskMiniaturizable))
          w.styleMask |= NSWindowStyleMaskMiniaturizable;
        [w miniaturize:nil];
      }
      break;
    case LAUFEY_WINDOW_ACTION_RESTORE:
      if ([w isMiniaturized])
        [w deminiaturize:nil];
      break;
    case LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN:
    case LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN: {
      bool enter = action == LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN;
      {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        Watch* watch = FindWatch(w);
        if (watch && watch->fs_transition != 0) {
          // AppKit ignores a toggle mid-transition; do it when it ends.
          watch->pending_fs_action = action;
          break;
        }
      }
      if (enter == IsFullscreen(w))
        break;
      if (enter)
        w.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
      [w toggleFullScreen:nil];
      break;
    }
    default:
      break;
  }
}

uint32_t MacGetWindowState(void* nswindow) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  return w ? StateOf(w) : 0;
}

void MacApplySizeConstraints(void* nswindow, const SizeConstraints& c) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w)
    return;
  w.contentMinSize = NSMakeSize(c.min_width, c.min_height);
  w.contentMaxSize = NSMakeSize(c.max_width > 0 ? c.max_width : FLT_MAX,
                                c.max_height > 0 ? c.max_height : FLT_MAX);
  // -setContentSize: and -setFrame: ignore the constraints; bring a window
  // that is now outside them back in.
  NSRect content = [w contentRectForFrameRect:w.frame];
  int width = static_cast<int>(content.size.width);
  int height = static_cast<int>(content.size.height);
  if (!IsFullscreen(w) && ClampSize(c, &width, &height)) {
    // Keep the top-left corner where it is.
    NSRect frame = w.frame;
    CGFloat top = NSMaxY(frame);
    [w setContentSize:NSMakeSize(width, height)];
    NSRect after = w.frame;
    [w setFrameOrigin:NSMakePoint(after.origin.x, top - after.size.height)];
  }
}

Bounds MacWindowBounds(void* nswindow) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  Bounds b;
  if (!w)
    return b;
  NSRect frame = w.frame;
  NSRect content = [w contentRectForFrameRect:frame];
  b.x = static_cast<int>(frame.origin.x);
  b.y = static_cast<int>(PrimaryScreenHeight() - frame.origin.y -
                         frame.size.height);
  b.width = static_cast<int>(content.size.width);
  b.height = static_cast<int>(content.size.height);
  return b;
}

void MacWatchWindowState(void* nswindow, uint32_t window_id) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w)
    return;
  NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
  NSMutableArray* observers = [NSMutableArray array];
  __weak NSWindow* weak = w;
  void (^report)(NSNotification*) = ^(NSNotification*) {
    NSWindow* strong = weak;
    if (strong)
      ReportAndTrack(strong, window_id);
  };
  void (^report_and_relayout)(NSNotification*) = ^(NSNotification*) {
    NSWindow* strong = weak;
    if (!strong)
      return;
    ReportAndTrack(strong, window_id);
    MacReapplyTrafficLightPosition((__bridge void*)strong);
  };
  for (NSNotificationName name in @[
         NSWindowDidMiniaturizeNotification,
         NSWindowDidDeminiaturizeNotification, NSWindowDidMoveNotification
       ]) {
    [observers addObject:[nc addObserverForName:name
                                         object:w
                                          queue:nil
                                     usingBlock:report]];
  }
  for (NSNotificationName name in @[
         NSWindowDidResizeNotification, NSWindowDidEndLiveResizeNotification,
         NSWindowDidBecomeKeyNotification
       ]) {
    [observers addObject:[nc addObserverForName:name
                                         object:w
                                          queue:nil
                                     usingBlock:report_and_relayout]];
  }
  // Fullscreen is announced before the frame changes: the frame now is the
  // one to come back to.
  [observers
      addObject:[nc addObserverForName:NSWindowWillEnterFullScreenNotification
                                object:w
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                              NSWindow* strong = weak;
                              if (!strong)
                                return;
                              SetFullscreenTransition(strong, 1);
                              if ([strong isZoomed])
                                return;
                              CommitNormalBounds(
                                  window_id,
                                  MacWindowBounds((__bridge void*)strong));
                            }]];
  [observers
      addObject:[nc addObserverForName:NSWindowWillExitFullScreenNotification
                                object:w
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                              NSWindow* strong = weak;
                              if (strong)
                                SetFullscreenTransition(strong, -1);
                            }]];
  // A transition ends (or fails): report the state it reached and apply an
  // enter / leave requested meanwhile.
  void (^transition_ended)(NSNotification*) = ^(NSNotification*) {
    NSWindow* strong = weak;
    if (!strong)
      return;
    int pending = SetFullscreenTransition(strong, 0);
    ReportAndTrack(strong, window_id);
    MacReapplyTrafficLightPosition((__bridge void*)strong);
    if (pending != 0) {
      dispatch_async(dispatch_get_main_queue(), ^{
        NSWindow* again = weak;
        if (again)
          MacSetWindowState((__bridge void*)again, pending);
      });
    }
  };
  for (NSNotificationName name in @[
         NSWindowDidEnterFullScreenNotification,
         NSWindowDidExitFullScreenNotification
       ]) {
    [observers addObject:[nc addObserverForName:name
                                         object:w
                                          queue:nil
                                     usingBlock:transition_ended]];
  }
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    Watch& watch = g_watches[nswindow];
    for (id obs in watch.observers)
      [nc removeObserver:obs];
    watch.window_id = window_id;
    watch.observers = observers;
  }
  // Seed the state and the normal bounds.
  NoteWindowGeometry(window_id, MacWindowBounds(nswindow), StateOf(w) == 0,
                     MonotonicMs());
  ReportWindowState(window_id, StateOf(w));
}

void MacUnwatchWindowState(void* nswindow) {
  NSMutableArray* observers = nil;
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    auto it = g_watches.find(nswindow);
    if (it == g_watches.end())
      return;
    observers = it->second.observers;
    g_watches.erase(it);
  }
  for (id obs in observers)
    [[NSNotificationCenter defaultCenter] removeObserver:obs];
}

void MacObserveWindowStateChanges(void* nswindow, uint32_t window_id,
                                  void (*on_change)(uint32_t window_id)) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w || !on_change)
    return;
  NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
  NSMutableArray* observers = [NSMutableArray array];
  __weak NSWindow* weak = w;
  void (^changed)(NSNotification*) = ^(NSNotification*) {
    NSWindow* strong = weak;
    if (!strong)
      return;
    on_change(window_id);
    MacReapplyTrafficLightPosition((__bridge void*)strong);
  };
  for (NSNotificationName name in @[
         NSWindowDidMiniaturizeNotification,
         NSWindowDidDeminiaturizeNotification, NSWindowDidMoveNotification,
         NSWindowDidResizeNotification, NSWindowDidEndLiveResizeNotification,
         NSWindowDidEnterFullScreenNotification,
         NSWindowDidExitFullScreenNotification, NSWindowDidBecomeKeyNotification
       ]) {
    [observers addObject:[nc addObserverForName:name
                                         object:w
                                          queue:nil
                                     usingBlock:changed]];
  }
  std::lock_guard<std::mutex> lock(g_watch_mutex);
  Watch& watch = g_watches[nswindow];
  for (id obs in watch.observers)
    [nc removeObserver:obs];
  watch.window_id = window_id;
  watch.observers = observers;
}

void MacApplyContentSizeConstraints(void* nswindow, const SizeConstraints& c) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w)
    return;
  w.contentMinSize = NSMakeSize(c.min_width, c.min_height);
  w.contentMaxSize = NSMakeSize(c.max_width > 0 ? c.max_width : FLT_MAX,
                                c.max_height > 0 ? c.max_height : FLT_MAX);
}

std::vector<laufey_screen_t> MacGetScreens() {
  __block std::vector<laufey_screen_t> result;
  RunOnMainSync(^{
    NSArray<NSScreen*>* screens = [NSScreen screens];
    for (NSUInteger i = 0; i < screens.count; ++i) {
      NSScreen* s = screens[i];
      laufey_screen_t out = {};
      NSNumber* number = s.deviceDescription[@"NSScreenNumber"];
      out.id = number ? static_cast<int64_t>(number.unsignedIntValue)
                      : static_cast<int64_t>(i + 1);
      NSRect frame = FlipRect(s.frame);
      NSRect work = FlipRect(s.visibleFrame);
      out.x = static_cast<int32_t>(frame.origin.x);
      out.y = static_cast<int32_t>(frame.origin.y);
      out.width = static_cast<int32_t>(frame.size.width);
      out.height = static_cast<int32_t>(frame.size.height);
      out.work_x = static_cast<int32_t>(work.origin.x);
      out.work_y = static_cast<int32_t>(work.origin.y);
      out.work_width = static_cast<int32_t>(work.size.width);
      out.work_height = static_cast<int32_t>(work.size.height);
      out.scale_factor = s.backingScaleFactor;
      // [NSScreen screens][0] is the screen with the menu bar, whose
      // origin is the global origin: the primary display.
      out.is_primary = i == 0;
      result.push_back(out);
    }
  });
  return result;
}

int64_t MacScreenForWindow(void* nswindow) {
  __block int64_t id = 0;
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w)
    return 0;
  RunOnMainSync(^{
    NSScreen* s = w.screen;
    NSNumber* number = s.deviceDescription[@"NSScreenNumber"];
    if (number)
      id = static_cast<int64_t>(number.unsignedIntValue);
  });
  return id;
}

void MacInstallDisplayWatcher() {
  RunOnMainSync(^{
    if (g_display_watcher_installed)
      return;
    g_display_watcher_installed = true;
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  NotifyDisplayChanged();
                }];
  });
}

bool MacSetTitlebarStyle(void* nswindow, int style) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w || !(w.styleMask & NSWindowStyleMaskTitled))
    return false;
  switch (style) {
    case LAUFEY_TITLEBAR_DEFAULT:
      w.toolbar = nil;
      w.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
      w.titlebarAppearsTransparent = NO;
      w.titleVisibility = NSWindowTitleVisible;
      break;
    case LAUFEY_TITLEBAR_HIDDEN:
    case LAUFEY_TITLEBAR_HIDDEN_INSET: {
      w.styleMask |= NSWindowStyleMaskFullSizeContentView;
      w.titlebarAppearsTransparent = YES;
      w.titleVisibility = NSWindowTitleHidden;
      if (style == LAUFEY_TITLEBAR_HIDDEN_INSET) {
        // An empty toolbar makes the title bar taller and insets the
        // traffic lights (Electron's hiddenInset).
        NSToolbar* toolbar =
            [[NSToolbar alloc] initWithIdentifier:@"laufeyHiddenInset"];
        toolbar.showsBaselineSeparator = NO;
        w.toolbar = toolbar;
      } else {
        w.toolbar = nil;
      }
      break;
    }
    default:
      return false;
  }
  MacReapplyTrafficLightPosition(nswindow);
  return true;
}

bool MacSetTrafficLightPosition(void* nswindow, int x, int y) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w || !(w.styleMask & NSWindowStyleMaskTitled))
    return false;
  bool reset = x < 0 || y < 0;
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    Watch& watch = g_watches[nswindow];
    watch.has_traffic_light = !reset;
    watch.traffic_x = x;
    watch.traffic_y = y;
  }
  if (reset) {
    // Let AppKit lay the title bar out again from scratch.
    NSString* title = w.title;
    w.title = @"";
    w.title = title ?: @"";
    [w.contentView setNeedsLayout:YES];
    NSButton* close = [w standardWindowButton:NSWindowCloseButton];
    [close.superview.superview setNeedsLayout:YES];
    return true;
  }
  ApplyTrafficLights(w, x, y);
  return true;
}

void MacReapplyTrafficLightPosition(void* nswindow) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  if (!w || IsFullscreen(w))
    return;
  int x = 0, y = 0;
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    auto it = g_watches.find(nswindow);
    if (it == g_watches.end() || !it->second.has_traffic_light)
      return;
    x = it->second.traffic_x;
    y = it->second.traffic_y;
  }
  ApplyTrafficLights(w, x, y);
}

bool MacSetVibrancy(void* nswindow, void* webview, int backdrop, int material) {
  NSWindow* w = (__bridge NSWindow*)nswindow;
  NSView* web = (__bridge NSView*)webview;
  if (!w || !web)
    return false;
  NSVisualEffectView* effect = objc_getAssociatedObject(w, &kEffectViewKey);
  if (backdrop == LAUFEY_BACKDROP_NONE) {
    if (!effect)
      return true;
    [web removeFromSuperview];
    [w setContentView:web];
    [w makeFirstResponder:web];
    objc_setAssociatedObject(w, &kEffectViewKey, nil,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSNumber* drew = objc_getAssociatedObject(w, &kDrewBackgroundKey);
    if (drew && drew.boolValue) {
      @try {
        [web setValue:@YES forKey:@"drawsBackground"];
      } @catch (NSException*) {
      }
    }
    return true;
  }
  if (backdrop != LAUFEY_BACKDROP_VIBRANCY || !IsKnownMaterial(material))
    return false;
  if (!effect) {
    NSNumber* drew = @YES;
    @try {
      id value = [web valueForKey:@"drawsBackground"];
      if ([value isKindOfClass:[NSNumber class]])
        drew = value;
    } @catch (NSException*) {
    }
    objc_setAssociatedObject(w, &kDrewBackgroundKey, drew,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSView* content = w.contentView;
    effect = [[NSVisualEffectView alloc] initWithFrame:content.frame];
    effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    effect.state = NSVisualEffectStateFollowsWindowActiveState;
    [web removeFromSuperview];
    [w setContentView:effect];
    web.frame = effect.bounds;
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [effect addSubview:web];
    [w makeFirstResponder:web];
    objc_setAssociatedObject(w, &kEffectViewKey, effect,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    @try {
      [web setValue:@NO forKey:@"drawsBackground"];
    } @catch (NSException*) {
    }
  }
  effect.material = static_cast<NSVisualEffectMaterial>(material);
  return true;
}

bool ShouldQuitAfterLastWindowMac() {
  if (IsQuitting())
    return true;
  if ([NSApp activationPolicy] == NSApplicationActivationPolicyAccessory)
    return false;
  return QuitOnLastWindowClosed();
}

}  // namespace laufey_common
