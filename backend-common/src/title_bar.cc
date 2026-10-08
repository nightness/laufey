// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences (API 47), the portable part: the GNOME button-layout
// parser, the normalisation of each source's values, the merge and the
// JSON. The sources are title_bar_linux.cc / title_bar_mac.mm /
// title_bar_win.cc. See laufey_title_bar.h.

#include "laufey_title_bar.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace laufey_common {

const char kGtkDefaultButtonLayout[] = "menu:minimize,maximize,close";

namespace {

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

std::string StringOrNull(const std::string& s) {
  return s.empty() ? "null" : Quote(s);
}

std::string Array(const std::vector<std::string>& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i)
      out += ",";
    out += Quote(items[i]);
  }
  return out + "]";
}

std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

bool KnownButton(const std::string& item) {
  return item == "close" || item == "minimize" || item == "maximize" ||
         item == "appmenu" || item == "menu" || item == "icon";
}

void ParseSide(const std::string& side, std::vector<std::string>* out,
               std::vector<std::string>* placed) {
  size_t start = 0;
  while (start <= side.size()) {
    size_t comma = side.find(',', start);
    if (comma == std::string::npos)
      comma = side.size();
    std::string item = Trim(side.substr(start, comma - start));
    bool seen = false;
    for (const auto& p : *placed)
      seen = seen || p == item;
    if (KnownButton(item) && !seen) {
      out->push_back(item);
      placed->push_back(item);
    }
    start = comma + 1;
  }
}

std::string Hex(double r, double g, double b) {
  auto byte = [](double v) {
    return static_cast<unsigned>(std::lround(v * 255.0));
  };
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(r), byte(g), byte(b));
  return buf;
}

}  // namespace

void ParseButtonLayout(const std::string& layout,
                       std::vector<std::string>* left,
                       std::vector<std::string>* right) {
  left->clear();
  right->clear();
  std::vector<std::string> placed;
  size_t colon = layout.find(':');
  if (colon == std::string::npos) {
    ParseSide(layout, left, &placed);
    return;
  }
  ParseSide(layout.substr(0, colon), left, &placed);
  // A second ':' (GTK ignores what follows it) ends the right side.
  std::string rest = layout.substr(colon + 1);
  size_t second = rest.find(':');
  if (second != std::string::npos)
    rest = rest.substr(0, second);
  ParseSide(rest, right, &placed);
}

std::string DoubleClickActionFromGnome(const std::string& value) {
  if (value == "toggle-maximize" || value == "toggle-maximize-horizontally" ||
      value == "toggle-maximize-vertically") {
    return "maximize";
  }
  if (value == "minimize")
    return "minimize";
  if (value == "toggle-shade")
    return "shade";
  if (value == "lower")
    return "lower";
  if (value == "menu")
    return "menu";
  if (value == "none")
    return "none";
  return "";
}

std::string ColorSchemeFromPortal(uint32_t value) {
  switch (value) {
    case 0:
      return "no-preference";
    case 1:
      return "dark";
    case 2:
      return "light";
    default:
      return "";
  }
}

std::string ColorSchemeFromGnome(const std::string& value) {
  if (value == "default")
    return "no-preference";
  if (value == "prefer-dark")
    return "dark";
  if (value == "prefer-light")
    return "light";
  return "";
}

std::string AccentFromPortalRgb(double r, double g, double b) {
  for (double v : {r, g, b}) {
    if (!(v >= 0.0 && v <= 1.0))  // also NaN
      return "";
  }
  return Hex(r, g, b);
}

std::string AccentFromGnomeName(const std::string& name) {
  // libadwaita's accent palette (AdwAccentColor, the light-style values).
  static const struct {
    const char* name;
    const char* hex;
  } kAccents[] = {
      {"blue", "#3584e4"},   {"teal", "#2190a4"},   {"green", "#3a944a"},
      {"yellow", "#c88800"}, {"orange", "#ed5b00"}, {"red", "#e62d42"},
      {"pink", "#d56199"},   {"purple", "#9141ac"}, {"slate", "#6f8396"},
  };
  for (const auto& a : kAccents) {
    if (name == a.name)
      return a.hex;
  }
  return "";
}

TitleBarPreferences ResolveTitleBarPreferences(
    const TitleBarSettings& portal, const TitleBarSettings& gsettings) {
  TitleBarPreferences p;
  // The button layout: the first source that answered one.
  std::string layout = kGtkDefaultButtonLayout;
  p.source = "default";
  if (portal.button_layout) {
    layout = *portal.button_layout;
    p.source = "portal";
  } else if (gsettings.button_layout) {
    layout = *gsettings.button_layout;
    p.source = "gsettings";
  }
  ParseButtonLayout(layout, &p.left, &p.right);
  // An unknown action falls through to the next source, then GTK's default.
  for (const auto* s : {&portal, &gsettings}) {
    if (p.double_click.empty() && s->double_click)
      p.double_click = DoubleClickActionFromGnome(*s->double_click);
  }
  if (p.double_click.empty())
    p.double_click = "maximize";
  for (const auto* s : {&portal, &gsettings}) {
    if (p.color_scheme.empty() && s->color_scheme)
      p.color_scheme = *s->color_scheme;
    if (p.accent_color.empty() && s->accent_color)
      p.accent_color = *s->accent_color;
    if (p.font.empty() && s->font)
      p.font = *s->font;
  }
  if (p.color_scheme.empty())
    p.color_scheme = "no-preference";
  return p;
}

std::string ButtonSide(const TitleBarPreferences& p) {
  for (const auto& b : p.left) {
    if (b == "close")
      return "left";
  }
  for (const auto& b : p.right) {
    if (b == "close")
      return "right";
  }
  auto windowButtons = [](const std::vector<std::string>& side) {
    int n = 0;
    for (const auto& b : side)
      n += b == "minimize" || b == "maximize";
    return n;
  };
  return windowButtons(p.left) > windowButtons(p.right) ? "left" : "right";
}

std::string TitleBarPreferencesToJson(const TitleBarPreferences& p) {
  std::string out = "{\"buttons\":{\"left\":" + Array(p.left) +
                    ",\"right\":" + Array(p.right) + "}";
  out += ",\"side\":" + Quote(ButtonSide(p));
  out += ",\"doubleClick\":" + Quote(p.double_click);
  out += ",\"colorScheme\":" + Quote(p.color_scheme);
  out += ",\"accentColor\":" + StringOrNull(p.accent_color);
  out += ",\"font\":" + StringOrNull(p.font);
  out += ",\"source\":" + Quote(p.source);
  out += "}";
  return out;
}

char* TitleBarPreferencesJsonForAbi() {
  std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
  char* out = static_cast<char*>(std::malloc(json.size() + 1));
  if (out)
    std::memcpy(out, json.c_str(), json.size() + 1);
  return out;
}

}  // namespace laufey_common
