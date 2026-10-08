// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences (title_bar.cc), the portable part: the GNOME
// button-layout parser, the value normalisation, the merge of the sources
// (the portal wins over GSettings key by key; GTK's defaults when nothing
// answers) and the JSON. On macOS and Windows, the OS's own answer: the
// buttons' side and source.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "laufey_title_bar.h"

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

using V = std::vector<std::string>;

void Layout(const char* layout, const V& left, const V& right) {
  V l, r;
  ParseButtonLayout(layout, &l, &r);
  if (l != left || r != right) {
    std::fprintf(stderr, "layout '%s' parsed wrong\n", layout);
    std::exit(1);
  }
}

}  // namespace

int main() {
  // --- The layout parser ---------------------------------------------------
  Layout(":minimize,maximize,close", {}, {"minimize", "maximize", "close"});
  Layout("close,minimize,maximize:", {"close", "minimize", "maximize"}, {});
  Layout("appmenu:close", {"appmenu"}, {"close"});
  Layout("icon:minimize,maximize,close", {"icon"},
         {"minimize", "maximize", "close"});
  // No ':' is all on the left, as GTK reads it.
  Layout("close", {"close"}, {});
  // Spaces, unknown items (spacer), repeats; a second ':' ends the right.
  Layout(" menu , spacer:close,close:minimize", {"menu"}, {"close"});
  Layout("", {}, {});

  // --- Values --------------------------------------------------------------
  EXPECT(DoubleClickActionFromGnome("toggle-maximize") == "maximize");
  EXPECT(DoubleClickActionFromGnome("toggle-maximize-vertically") ==
         "maximize");
  EXPECT(DoubleClickActionFromGnome("toggle-shade") == "shade");
  EXPECT(DoubleClickActionFromGnome("minimize") == "minimize");
  EXPECT(DoubleClickActionFromGnome("lower") == "lower");
  EXPECT(DoubleClickActionFromGnome("menu") == "menu");
  EXPECT(DoubleClickActionFromGnome("none") == "none");
  EXPECT(DoubleClickActionFromGnome("bogus").empty());
  EXPECT(ColorSchemeFromPortal(0) == "no-preference");
  EXPECT(ColorSchemeFromPortal(1) == "dark");
  EXPECT(ColorSchemeFromPortal(2) == "light");
  EXPECT(ColorSchemeFromPortal(9).empty());
  EXPECT(ColorSchemeFromGnome("prefer-dark") == "dark");
  EXPECT(ColorSchemeFromGnome("default") == "no-preference");
  EXPECT(ColorSchemeFromGnome("bogus").empty());
  // Plasma's Breeze blue, as its portal sends it.
  EXPECT(AccentFromPortalRgb(0.23921568691730499, 0.68235296010971069,
                             0.91372549533843994) == "#3daee9");
  // Out of range is the portal's "unset".
  EXPECT(AccentFromPortalRgb(-1, -1, -1).empty());
  EXPECT(AccentFromPortalRgb(0.5, 1.5, 0.5).empty());
  EXPECT(AccentFromGnomeName("orange") == "#ed5b00");
  EXPECT(AccentFromGnomeName("bogus").empty());

  // --- The merge -----------------------------------------------------------
  // Nothing answers: GTK's defaults.
  TitleBarPreferences d =
      ResolveTitleBarPreferences(TitleBarSettings(), TitleBarSettings());
  EXPECT(d.source == "default");
  EXPECT(d.left == V({"menu"}));
  EXPECT(d.right == V({"minimize", "maximize", "close"}));
  EXPECT(d.double_click == "maximize");
  EXPECT(d.color_scheme == "no-preference");
  EXPECT(d.accent_color.empty() && d.font.empty());
  EXPECT(ButtonSide(d) == "right");
  EXPECT(TitleBarPreferencesToJson(d) ==
         "{\"buttons\":{\"left\":[\"menu\"],\"right\":[\"minimize\","
         "\"maximize\",\"close\"]},\"side\":\"right\",\"doubleClick\":"
         "\"maximize\",\"colorScheme\":\"no-preference\",\"accentColor\":"
         "null,\"font\":null,\"source\":\"default\"}");

  // Ubuntu GNOME: GSettings says appmenu:close, the portal (what sandboxed
  // and GTK 4 apps use) :minimize,maximize,close. The portal wins; a key it
  // didn't answer comes from GSettings.
  TitleBarSettings portal;
  portal.button_layout = ":minimize,maximize,close";
  portal.color_scheme = "dark";
  TitleBarSettings gsettings;
  gsettings.button_layout = "appmenu:close";
  gsettings.double_click = "minimize";
  gsettings.color_scheme = "light";
  gsettings.accent_color = "#3584e4";
  gsettings.font = "Ubuntu Bold 11";
  TitleBarPreferences p = ResolveTitleBarPreferences(portal, gsettings);
  EXPECT(p.source == "portal");
  EXPECT(p.left.empty());
  EXPECT(p.right == V({"minimize", "maximize", "close"}));
  EXPECT(p.double_click == "minimize");
  EXPECT(p.color_scheme == "dark");
  EXPECT(p.accent_color == "#3584e4");
  EXPECT(p.font == "Ubuntu Bold 11");

  // GSettings alone.
  TitleBarPreferences g =
      ResolveTitleBarPreferences(TitleBarSettings(), gsettings);
  EXPECT(g.source == "gsettings");
  EXPECT(g.left == V({"appmenu"}) && g.right == V({"close"}));

  // An action we don't know falls through to the next source.
  TitleBarSettings odd;
  odd.double_click = "toggle-something";
  EXPECT(ResolveTitleBarPreferences(odd, gsettings).double_click == "minimize");
  EXPECT(ResolveTitleBarPreferences(odd, TitleBarSettings()).double_click ==
         "maximize");

  // The close button's side; with none, where the other buttons are.
  TitleBarPreferences left;
  left.left = {"close", "minimize"};
  EXPECT(ButtonSide(left) == "left");
  TitleBarPreferences no_close;
  no_close.left = {"minimize", "maximize"};
  no_close.right = {"appmenu"};
  EXPECT(ButtonSide(no_close) == "left");
  no_close.left.clear();
  EXPECT(ButtonSide(no_close) == "right");

  // Strings in the JSON are escaped.
  TitleBarPreferences font = d;
  font.font = "Odd \"Font\"\\ 11";
  EXPECT(TitleBarPreferencesToJson(font).find(
             "\"font\":\"Odd \\\"Font\\\"\\\\ 11\"") != std::string::npos);

#if defined(__APPLE__)
  // macOS: the traffic lights are on the left.
  TitleBarPreferences mac = ProbeTitleBarPreferences();
  EXPECT(mac.source == "os");
  EXPECT(mac.left == V({"close", "minimize", "maximize"}));
  EXPECT(mac.right.empty());
  EXPECT(ButtonSide(mac) == "left");
  EXPECT(mac.color_scheme == "light" || mac.color_scheme == "dark");
#elif defined(_WIN32)
  // Windows: the caption buttons are on the right; a double click
  // maximizes.
  TitleBarPreferences win = ProbeTitleBarPreferences();
  EXPECT(win.source == "os");
  EXPECT(win.left.empty());
  EXPECT(win.right == V({"minimize", "maximize", "close"}));
  EXPECT(win.double_click == "maximize");
  EXPECT(win.color_scheme == "light" || win.color_scheme == "dark");
#endif
  // The C ABI's JSON is the probe's.
  char* json = TitleBarPreferencesJsonForAbi();
  EXPECT(json && std::strstr(json, "\"buttons\":{"));
  std::free(json);

  std::printf("laufey_title_bar_test: ok\n");
  return 0;
}
