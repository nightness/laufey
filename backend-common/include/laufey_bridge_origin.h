// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Which documents may call the app through the JS bridge (API 44).
//
// The bridge namespace is installed in a window's top-level document, which
// can navigate anywhere: a link, a redirect, or script. A packaged app pins the
// origins its bridge serves in laufey-launch.json ("bridgeOrigins", or by
// default the origins of its "customSchemes"), and the backends then neither
// install the namespace in another origin's document nor accept a call from
// one. Every call also hands the runtime the calling document's origin
// (set_js_call_handler_ex), so the runtime can make its own decision.
//
// Origins are compared in their HTML serialization: lowercase
// "scheme://host", plus ":port" when the port isn't the scheme's default, and
// "null" for an opaque origin (about:, data:, file:, javascript:, a URL with
// no host).

#ifndef LAUFEY_BRIDGE_ORIGIN_H_
#define LAUFEY_BRIDGE_ORIGIN_H_

#include <cstddef>
#include <string>
#include <vector>

#include "laufey_launch_config.h"

namespace laufey_common {

inline constexpr char kOpaqueOrigin[] = "null";

namespace bridge_origin_internal {

inline char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

// The port a scheme has when a URL names none (WHATWG "special" schemes).
inline int DefaultPort(const std::string& scheme) {
  if (scheme == "http" || scheme == "ws")
    return 80;
  if (scheme == "https" || scheme == "wss")
    return 443;
  if (scheme == "ftp")
    return 21;
  return -1;
}

// Characters an origin can contain once serialized (a lowercase scheme, an
// ASCII or bracketed IPv6 host, a port).
inline bool IsOriginChar(char c) {
  return (c >= 'a' && c <= 'z') || IsDigit(c) || c == '+' || c == '-' ||
         c == '.' || c == ':' || c == '/' || c == '[' || c == ']' || c == '_';
}

}  // namespace bridge_origin_internal

// The serialized origin of a document with `scheme`, `host` and `port` (a
// negative port, or the scheme's default, is left out). "null" for a scheme
// whose documents have opaque origins or an empty host.
inline std::string SerializeOrigin(const std::string& scheme,
                                   const std::string& host, int port) {
  using namespace bridge_origin_internal;
  std::string s, h;
  for (char c : scheme)
    s.push_back(Lower(c));
  for (char c : host)
    h.push_back(Lower(c));
  if (s.empty() || h.empty() || s == "file" || s == "data" || s == "about" ||
      s == "javascript" || s == "blob")
    return kOpaqueOrigin;
  for (char c : s + h) {
    if (!IsOriginChar(c))
      return kOpaqueOrigin;
  }
  std::string origin = s + "://" + h;
  if (port >= 0 && port <= 65535 && port != DefaultPort(s))
    origin += ":" + std::to_string(port);
  return origin;
}

// The serialized origin of the document at `url` (an absolute URL, as an
// engine reports a frame's URL). A blob: URL has its inner URL's origin.
inline std::string OriginOfUrl(const std::string& url) {
  using namespace bridge_origin_internal;
  size_t colon = url.find(':');
  if (colon == std::string::npos || colon == 0 || !IsAlpha(url[0]))
    return kOpaqueOrigin;
  std::string scheme;
  for (size_t i = 0; i < colon; ++i) {
    char c = url[i];
    if (!IsAlpha(c) && !IsDigit(c) && c != '+' && c != '-' && c != '.')
      return kOpaqueOrigin;
    scheme.push_back(Lower(c));
  }
  if (scheme == "blob")
    return OriginOfUrl(url.substr(colon + 1));
  if (url.compare(colon + 1, 2, "//") != 0)
    return kOpaqueOrigin;
  size_t start = colon + 3;
  size_t end = url.find_first_of("/?#\\", start);
  std::string authority = url.substr(
      start, end == std::string::npos ? std::string::npos : end - start);
  size_t at = authority.rfind('@');
  if (at != std::string::npos)
    authority.erase(0, at + 1);
  std::string host, port;
  if (!authority.empty() && authority[0] == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos)
      return kOpaqueOrigin;
    host = authority.substr(0, close + 1);
    std::string rest = authority.substr(close + 1);
    if (!rest.empty()) {
      if (rest[0] != ':')
        return kOpaqueOrigin;
      port = rest.substr(1);
    }
  } else {
    size_t p = authority.find(':');
    host = authority.substr(0, p);
    if (p != std::string::npos)
      port = authority.substr(p + 1);
  }
  int port_number = -1;
  if (!port.empty()) {
    if (port.size() > 5)
      return kOpaqueOrigin;
    port_number = 0;
    for (char c : port) {
      if (!IsDigit(c))
        return kOpaqueOrigin;
      port_number = port_number * 10 + (c - '0');
    }
    if (port_number > 65535)
      return kOpaqueOrigin;
  }
  return SerializeOrigin(scheme, host, port_number);
}

// The origins a packaged app's bridge serves.
struct BridgeOriginPolicy {
  // False: every origin may call (no launch file, or one that pins neither
  // "bridgeOrigins" nor "customSchemes", or "bridgeOrigins": ["*"]).
  bool restricted = false;
  // Serialized origins allowed exactly.
  std::vector<std::string> origins;
  // Schemes any of whose origins is allowed ("<scheme>://*").
  std::vector<std::string> schemes;
};

// Normalizes one "bridgeOrigins" entry: "*", "<scheme>://*", or an origin
// ("scheme://host[:port]", nothing after it but an optional "/"). Returns
// false for anything else.
inline bool ParseBridgeOriginEntry(const std::string& entry,
                                   std::string* origin, std::string* scheme,
                                   bool* any) {
  using namespace bridge_origin_internal;
  *any = false;
  origin->clear();
  scheme->clear();
  if (entry == "*") {
    *any = true;
    return true;
  }
  size_t sep = entry.find("://");
  if (sep == std::string::npos || sep == 0)
    return false;
  std::string s;
  for (size_t i = 0; i < sep; ++i) {
    char c = entry[i];
    if (!(IsAlpha(c) ||
          (i > 0 && (IsDigit(c) || c == '+' || c == '-' || c == '.'))))
      return false;
    s.push_back(Lower(c));
  }
  std::string rest = entry.substr(sep + 3);
  if (rest == "*") {
    *scheme = s;
    return true;
  }
  if (!rest.empty() && rest.back() == '/')
    rest.pop_back();
  if (rest.empty() || rest.find_first_of("/?#@\\*") != std::string::npos)
    return false;
  std::string o = OriginOfUrl(s + "://" + rest);
  if (o == kOpaqueOrigin)
    return false;
  *origin = o;
  return true;
}

// The policy a launch file sets: its "bridgeOrigins" if it has the key (the
// entries ParseLaunchConfig kept), else one "<scheme>://*" per entry of its
// "customSchemes", else none.
inline BridgeOriginPolicy BridgeOriginPolicyFrom(const LaunchConfig& config) {
  BridgeOriginPolicy policy;
  if (config.has_bridge_origins) {
    policy.restricted = true;
    for (const std::string& entry : config.bridge_origins) {
      std::string origin, scheme;
      bool any = false;
      if (!ParseBridgeOriginEntry(entry, &origin, &scheme, &any))
        continue;
      if (any)
        return BridgeOriginPolicy();
      if (!scheme.empty())
        policy.schemes.push_back(scheme);
      else
        policy.origins.push_back(origin);
    }
    return policy;
  }
  if (config.has_custom_schemes && !config.custom_schemes.empty()) {
    policy.restricted = true;
    for (const std::string& scheme : config.custom_schemes) {
      std::string lower;
      for (char c : scheme)
        lower.push_back(bridge_origin_internal::Lower(c));
      policy.schemes.push_back(lower);
    }
  }
  return policy;
}

// Whether a document with serialized origin `origin` may use the bridge.
inline bool BridgeOriginAllowed(const BridgeOriginPolicy& policy,
                                const std::string& origin) {
  if (!policy.restricted)
    return true;
  if (origin == kOpaqueOrigin)
    return false;
  for (const std::string& o : policy.origins) {
    if (o == origin)
      return true;
  }
  for (const std::string& s : policy.schemes) {
    if (origin.size() > s.size() + 3 && origin.compare(0, s.size(), s) == 0 &&
        origin.compare(s.size(), 3, "://") == 0)
      return true;
  }
  return false;
}

// A JavaScript expression that is true when `location.origin` may use the
// bridge, for the bridge script to test before it installs anything. "true"
// when the policy doesn't restrict. The policy's strings hold origin
// characters only (see SerializeOrigin), so they quote safely.
inline std::string BridgeOriginGuardJs(const BridgeOriginPolicy& policy) {
  if (!policy.restricted)
    return "true";
  std::string js = "(function(o){return false";
  for (const std::string& o : policy.origins)
    js += "||o==='" + o + "'";
  for (const std::string& s : policy.schemes)
    js += "||(o.length>" + std::to_string(s.size() + 3) + "&&o.slice(0," +
          std::to_string(s.size() + 3) + ")==='" + s + "://')";
  js += ";})(String(location.origin))";
  return js;
}

// This process's policy, from its launch file (read once).
inline const BridgeOriginPolicy& ProcessBridgeOriginPolicy() {
  static const BridgeOriginPolicy policy =
      BridgeOriginPolicyFrom(ProcessLaunchConfig());
  return policy;
}

// The error a call from a document the policy refuses is rejected with.
inline constexpr char kBridgeOriginRefused[] =
    "laufey: this page's origin may not call the app";

}  // namespace laufey_common

#endif  // LAUFEY_BRIDGE_ORIGIN_H_
