// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences (API 47) on macOS: the window buttons are always on
// the left (close, minimize, zoom), a double click on a title bar does what
// System Settings > Desktop & Dock > "Double-click a window's title bar to"
// says (AppleActionOnDoubleClick: Zoom / Fill / Minimize / Do Nothing; the
// older AppleMiniaturizeOnDoubleClick where that is unset), and the colour
// scheme and accent colour are the appearance's. The change handler fires
// on the main thread when the appearance or the accent colour changes (the
// double-click setting is read on every call; macOS posts nothing for it).

#include "laufey_title_bar.h"

#import <AppKit/AppKit.h>

#include <mutex>
#include <string>

namespace laufey_common {

namespace {

NSUserDefaults* GlobalDefaults() {
  // The app's defaults search NSGlobalDomain, where System Settings writes
  // these keys.
  return [NSUserDefaults standardUserDefaults];
}

std::string DoubleClickAction() {
  NSString* action =
      [GlobalDefaults() stringForKey:@"AppleActionOnDoubleClick"];
  if (action) {
    if ([action isEqualToString:@"Minimize"])
      return "minimize";
    if ([action isEqualToString:@"None"])
      return "none";
    // "Maximize" (Zoom) and "Fill" (macOS 15) both make the window fill.
    return "maximize";
  }
  if ([GlobalDefaults() boolForKey:@"AppleMiniaturizeOnDoubleClick"])
    return "minimize";
  return "maximize";  // the system default, Zoom
}

std::string ColorScheme() {
  NSString* style = [GlobalDefaults() stringForKey:@"AppleInterfaceStyle"];
  return style && [style caseInsensitiveCompare:@"Dark"] == NSOrderedSame
             ? "dark"
             : "light";
}

std::string AccentColor() {
  @try {
    NSColor* accent = [[NSColor controlAccentColor]
        colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!accent)
      return "";
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             static_cast<unsigned>(lround(accent.redComponent * 255.0)),
             static_cast<unsigned>(lround(accent.greenComponent * 255.0)),
             static_cast<unsigned>(lround(accent.blueComponent * 255.0)));
    return buf;
  } @catch (NSException*) {
    return "";
  }
}

std::mutex g_mutex;
void (*g_handler)(void*) = nullptr;
void* g_user_data = nullptr;
std::string g_last_json;
bool g_observing = false;

void Changed() {
  std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
  void (*handler)(void*) = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (json == g_last_json)
      return;
    g_last_json = json;
    handler = g_handler;
    user_data = g_user_data;
  }
  if (handler)
    handler(user_data);
}

}  // namespace

TitleBarPreferences ProbeTitleBarPreferences() {
  TitleBarPreferences p;
  p.left = {"close", "minimize", "maximize"};
  p.double_click = DoubleClickAction();
  p.color_scheme = ColorScheme();
  p.accent_color = AccentColor();
  p.source = "os";
  return p;
}

void SetTitleBarPreferencesChangedHandler(void (*handler)(void* user_data),
                                          void* user_data) {
  bool observe = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handler = handler;
    g_user_data = user_data;
    observe = handler && !g_observing;
    if (observe)
      g_observing = true;
  }
  if (!observe)
    return;
  {
    std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_last_json = json;
  }
  // Distributed notifications the appearance and accent-colour panes post.
  NSDistributedNotificationCenter* center =
      [NSDistributedNotificationCenter defaultCenter];
  for (NSString* name in @[
         @"AppleInterfaceThemeChangedNotification",
         @"AppleColorPreferencesChangedNotification"
       ]) {
    [center addObserverForName:name
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      Changed();
                    }];
  }
}

void ResetTitleBarPreferencesForTesting() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handler = nullptr;
  g_user_data = nullptr;
}

}  // namespace laufey_common
