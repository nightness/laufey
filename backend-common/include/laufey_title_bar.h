// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences (API 47): how the user set up their title bars, so
// an app that draws its own (a hidden title bar with a drag region) puts the
// window buttons where the user's other windows have them, and a double
// click on its drag region does what a double click on a title bar does
// there. Never a look of our own: everything is read from the session.
//
//   title_bar.cc        the portable part: the GNOME button-layout parser,
//                       the action / colour-scheme / accent normalisation,
//                       the merge of what each source answered, the JSON
//   title_bar_linux.cc  the sources on Linux, and the change watcher:
//                       org.freedesktop.portal.Settings first (its
//                       SettingChanged followed live), then GSettings, then
//                       GTK's defaults
//   title_bar_mac.mm    macOS: the buttons on the left, the double-click
//                       action from AppleActionOnDoubleClick, the appearance
//   title_bar_win.cc    Windows: the buttons on the right, a double click
//                       maximizes, the app colour mode and accent colour
//
// The portal's answer wins over GSettings for every key it answers: on
// Ubuntu GNOME GSettings' button-layout can differ from what the portal
// (and so every sandboxed and GTK 4 app) uses. Desktops that keep their own
// decoration settings publish them through the portal too (Plasma's portal
// answers org.gnome.desktop.wm.preferences from KWin's button order), so
// nothing branches on the desktop's name. See docs/title-bar.md.

#ifndef LAUFEY_TITLE_BAR_H_
#define LAUFEY_TITLE_BAR_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace laufey_common {

// What the user set up.
struct TitleBarPreferences {
  // The window buttons in order, on each side of the title. Items are
  // "close", "minimize", "maximize" and the app's own menu / icon
  // ("appmenu", "menu", "icon"), as GTK's decoration layout names them;
  // anything else in the layout is dropped.
  std::vector<std::string> left;
  std::vector<std::string> right;
  // What a double click on the title bar does: "maximize" (toggle; GNOME's
  // toggle-maximize and its horizontal / vertical forms, macOS Zoom / Fill),
  // "minimize", "shade" (roll up), "lower", "menu" (the window menu) or
  // "none".
  std::string double_click;
  // "light", "dark" or "no-preference".
  std::string color_scheme;
  // The accent colour as "#rrggbb", or "" when the session has none.
  std::string accent_color;
  // The title bar font ("Cantarell Bold 11"), or "" when not known.
  std::string font;
  // Where the button layout came from: "portal", "gsettings", "default"
  // (GTK's own defaults: nothing answered), or "os" (macOS / Windows).
  std::string source;
};

// One source's answers (a portal ReadAll, GSettings): every field is unset
// when that source didn't answer it.
struct TitleBarSettings {
  std::optional<std::string> button_layout;  // "appmenu:minimize,close"
  std::optional<std::string> double_click;   // action-double-click-titlebar
  std::optional<std::string> font;           // titlebar-font
  std::optional<std::string> color_scheme;   // already normalised
  std::optional<std::string> accent_color;   // already "#rrggbb"
};

// GTK's decoration-layout default ("menu:minimize,maximize,close"), the
// layout used when no source answers.
extern const char kGtkDefaultButtonLayout[];

// Splits a GNOME button-layout / GTK decoration-layout ("close:minimize",
// "appmenu:close", ":minimize,maximize,close", "close,minimize:") into the
// known items on each side, in order. Unknown items ("spacer", typos) are
// dropped, as are repeats of an item already placed. A layout without ':'
// is all on the left, as GTK reads it.
void ParseButtonLayout(const std::string& layout,
                       std::vector<std::string>* left,
                       std::vector<std::string>* right);

// GNOME's action-double-click-titlebar value as a TitleBarPreferences
// action; "" for a value we don't know.
std::string DoubleClickActionFromGnome(const std::string& value);

// org.freedesktop.appearance color-scheme (0 no preference, 1 dark, 2
// light) as a colour scheme; "" for any other number.
std::string ColorSchemeFromPortal(uint32_t value);

// org.gnome.desktop.interface color-scheme ("default", "prefer-dark",
// "prefer-light") as a colour scheme; "" for any other value.
std::string ColorSchemeFromGnome(const std::string& value);

// org.freedesktop.appearance accent-color (r, g, b in [0, 1]) as
// "#rrggbb"; "" when a channel is out of range (the portal's "unset").
std::string AccentFromPortalRgb(double r, double g, double b);

// org.gnome.desktop.interface accent-color ("blue", "teal", ...) as the
// colour GNOME shows for it (libadwaita's palette); "" for an unknown name.
std::string AccentFromGnomeName(const std::string& name);

// The preferences from the sources, highest priority first: for each key,
// the first source that answered it, else GTK's default. `source` is where
// the button layout came from ("portal" for sources[0], "gsettings" for
// sources[1], "default" when neither answered it).
TitleBarPreferences ResolveTitleBarPreferences(
    const TitleBarSettings& portal, const TitleBarSettings& gsettings);

// Which side the close button is on ("left" / "right"); with no close
// button, the side holding the other buttons, else "right".
std::string ButtonSide(const TitleBarPreferences& p);

// The JSON object title_bar_preferences hands out:
//   {"buttons":{"left":[...],"right":[...]},"side":"left"|"right",
//    "doubleClick":"maximize"|...,"colorScheme":"light"|"dark"|
//    "no-preference","accentColor":"#rrggbb"|null,"font":string|null,
//    "source":"portal"|"gsettings"|"default"|"os"}
std::string TitleBarPreferencesToJson(const TitleBarPreferences& p);

// What the session says now. On Linux one portal call (bounded: the first
// may wait a few seconds for xdg-desktop-portal to start) and a GSettings
// read; macOS and Windows read the user defaults / registry. Any thread.
TitleBarPreferences ProbeTitleBarPreferences();

// ProbeTitleBarPreferences() as JSON, malloc'd for the C ABI (freed with
// the backend's string_free). Any thread.
char* TitleBarPreferencesJsonForAbi();

// The handler fired when the preferences change (set_title_bar_
// preferences_changed_handler, API 47): on Linux when the portal reports a
// SettingChanged (or GSettings a change) that alters the answer, followed
// on a watcher thread of its own; on macOS when the appearance or accent
// colour changes; on Windows when the app colour mode or accent colour
// changes. Fires on the watcher's thread (Linux, Windows) or the main thread
// (macOS), only when the JSON answer differs from the last one reported. A
// null handler clears it (the watcher stays). Any thread.
void SetTitleBarPreferencesChangedHandler(void (*handler)(void* user_data),
                                          void* user_data);

#if defined(__linux__) && !defined(__ANDROID__)
// The Linux sources on their own (title_bar_linux.cc): what the portal's
// Settings interface answers (nothing without a portal), and what GSettings
// answers (nothing for a schema that isn't installed). Any thread.
TitleBarSettings PortalTitleBarSettings();
TitleBarSettings GSettingsTitleBarSettings();
#endif

// Test-only: stop the Linux watcher and forget what it reported.
void ResetTitleBarPreferencesForTesting();

}  // namespace laufey_common

#endif  // LAUFEY_TITLE_BAR_H_
