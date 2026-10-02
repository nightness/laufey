// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Custom URL schemes the embedder registered through
// laufey_backend_api.register_scheme_handler.
//
// Every web-engine backend serves the built-in "app" scheme in-process. Since
// API 26 an embedder can register further schemes (e.g. "myapp"), and each of
// them must become a real origin: `<scheme>://<host>` as `location.origin`, a
// secure context, CORS-capable, with per-origin storage. The engine-specific
// registration differs per backend (WKURLSchemeHandler, WebKitGTK's security
// manager, WebView2's CoreWebView2CustomSchemeRegistration, CEF's
// CefSchemeRegistrar), but the bookkeeping is shared here:
//
//   * name validation — RFC 3986 `scheme = ALPHA *( ALPHA / DIGIT / "+" / "-"
//     / "." )`, compared case-insensitively and stored lowercase;
//   * the thread-safe set of registered names (the runtime registers from its
//     own thread while the UI thread creates windows and reads the set).
//
// Contract (documented on register_scheme_handler in laufey.h): a scheme must
// be registered before the first window that uses it is created. The engines
// read their scheme tables when a web view / environment is created, so a
// later registration is not retroactive.

#ifndef LAUFEY_SCHEME_REGISTRY_H_
#define LAUFEY_SCHEME_REGISTRY_H_

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace laufey_common {

// The built-in scheme every web-engine backend serves in-process. Kept in
// sync with LAUFEY_APP_SCHEME in webview/src/scheme_exchange.h and
// cef/src/scheme_handler.h.
inline constexpr char kDefaultAppScheme[] = "app";

// Lowercase ASCII copy of `scheme` (URL schemes are case-insensitive).
inline std::string NormalizeSchemeName(const std::string& scheme) {
  std::string out = scheme;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// RFC 3986 scheme grammar: a letter followed by letters, digits, "+", "-" or
// ".". Case-insensitive. Rejects the empty string, anything with "://", and
// characters outside the grammar.
inline bool IsValidSchemeName(const std::string& scheme) {
  if (scheme.empty()) {
    return false;
  }
  auto is_alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };
  auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  if (!is_alpha(scheme[0])) {
    return false;
  }
  for (char c : scheme) {
    if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

// Split a comma-separated list ("myapp, other") into normalized, valid,
// deduplicated scheme names, preserving first-seen order. Whitespace around
// entries is ignored; invalid entries are appended to `rejected` (if given)
// and skipped.
inline std::vector<std::string> ParseSchemeList(
    const std::string& list, std::vector<std::string>* rejected = nullptr) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    std::string item = list.substr(start, end - start);
    size_t first = item.find_first_not_of(" \t\r\n");
    size_t last = item.find_last_not_of(" \t\r\n");
    item = first == std::string::npos ? std::string()
                                      : item.substr(first, last - first + 1);
    if (!item.empty()) {
      if (IsValidSchemeName(item)) {
        std::string normalized = NormalizeSchemeName(item);
        if (seen.insert(normalized).second) {
          out.push_back(normalized);
        }
      } else if (rejected) {
        rejected->push_back(item);
      }
    }
    start = end + 1;
  }
  return out;
}

// The custom schemes a CEF process declares at startup: "app" first, then the
// valid names from each of `lists` (the --laufey-custom-schemes switch value,
// then LAUFEY_CUSTOM_SCHEMES) in order, normalized and deduplicated. Invalid
// entries are appended to `rejected` (if given) and skipped.
inline std::vector<std::string> MergeSchemeLists(
    const std::vector<std::string>& lists,
    std::vector<std::string>* rejected = nullptr) {
  std::vector<std::string> out = {kDefaultAppScheme};
  std::set<std::string> seen = {kDefaultAppScheme};
  for (const std::string& list : lists) {
    for (const std::string& scheme : ParseSchemeList(list, rejected)) {
      if (seen.insert(scheme).second) {
        out.push_back(scheme);
      }
    }
  }
  return out;
}

// `schemes` joined with commas, without the built-in "app" (every process
// declares it anyway): the --laufey-custom-schemes value the CEF browser
// process forwards to its children. Empty when there is nothing to forward.
inline std::string JoinForwardedSchemes(
    const std::vector<std::string>& schemes) {
  std::string joined;
  for (const std::string& scheme : schemes) {
    if (scheme == kDefaultAppScheme) {
      continue;
    }
    if (!joined.empty()) {
      joined += ',';
    }
    joined += scheme;
  }
  return joined;
}

// The --laufey-custom-schemes value for a child process whose command line
// already carries `child_value` ("" when it has none): the schemes already
// there, then `declared`, as one list without duplicates (and without "app").
// A child whose command line already names some schemes still gets the rest
// of the browser process's declaration (environment, launch file).
inline std::string MergeForwardedSchemes(
    const std::string& child_value, const std::vector<std::string>& declared) {
  return JoinForwardedSchemes(
      MergeSchemeLists({child_value, JoinForwardedSchemes(declared)}));
}

// The scheme of a serialized origin or URL ("myapp://app", "myapp://app/x"),
// normalized, or "" when it has none (an opaque origin's "null", a malformed
// string, a scheme outside the RFC 3986 grammar).
inline std::string OriginScheme(const std::string& origin) {
  size_t colon = origin.find("://");
  if (colon == std::string::npos) {
    return "";
  }
  std::string scheme = origin.substr(0, colon);
  return IsValidSchemeName(scheme) ? NormalizeSchemeName(scheme) : "";
}

// Local Network Access (Chromium's private network protection; CEF only).
// A page on a "public" origin may reach loopback or private addresses only
// after a permission prompt, and a custom scheme's origin always counts as
// public. So an app page on `myapp://app` talking to its own loopback server
// (the runtime's WebSocket relay, a dev server) would wait forever on a
// prompt the CEF host never shows.
//
// laufey answers that prompt itself. Local network access is granted only to
// origins on one of `declared`: the embedder's own custom schemes and "app".
// A page there is produced in-process by the embedder's scheme handler and
// can't come from the network. Every other origin is denied, so remote
// content keeps Chromium's protection: http(s), file, data and any scheme
// that isn't declared. Denying the prompt, rather than leaving it
// unanswered, makes such a request fail at once instead of hanging.
//
// `requested` is the prompt's permission bit set. `local_network_mask` holds
// the bits that mean local network access (CEF's LOCAL_NETWORK_ACCESS,
// LOCAL_NETWORK and LOOPBACK_NETWORK). A prompt that asks for none of them
// is left to the engine's default handling. A prompt that also asks for
// anything else is denied, even for a declared origin, so the grant never
// widens to another permission.
enum class LocalNetworkPromptDecision {
  kDefault,  // not a local network prompt: default handling
  kAccept,
  kDeny,
};

// Whether a page on `origin` is one of the embedder's own (see above).
inline bool IsLocalNetworkTrustedOrigin(
    const std::string& origin, const std::vector<std::string>& declared) {
  std::string scheme = OriginScheme(origin);
  if (scheme.empty()) {
    return false;
  }
  for (const std::string& d : declared) {
    if (NormalizeSchemeName(d) == scheme) {
      return true;
    }
  }
  return false;
}

inline LocalNetworkPromptDecision DecideLocalNetworkPrompt(
    const std::string& origin, uint32_t requested, uint32_t local_network_mask,
    const std::vector<std::string>& declared) {
  if ((requested & local_network_mask) == 0) {
    return LocalNetworkPromptDecision::kDefault;
  }
  if ((requested & ~local_network_mask) == 0 &&
      IsLocalNetworkTrustedOrigin(origin, declared)) {
    return LocalNetworkPromptDecision::kAccept;
  }
  return LocalNetworkPromptDecision::kDeny;
}

// Thread-safe set of scheme names an embedder registered. "app" is always a
// member so backends can iterate one list when they install their handlers.
class SchemeRegistry {
 public:
  SchemeRegistry();

  // Process-wide registry shared by the platform backend and the runtime
  // loader.
  static SchemeRegistry* GetInstance();

  // Validate, normalize and add `scheme`. Returns true if it is a valid name
  // that was not already registered (so the caller should install the native
  // handler for it), false if it was already present or invalid. Safe to call
  // from any thread.
  bool Add(const std::string& scheme);

  // Whether `scheme` (any case) is registered. Safe to call from any thread.
  bool Contains(const std::string& scheme) const;

  // Snapshot of the registered names, "app" first, the rest in the order
  // they were registered. Safe to call from any thread.
  std::vector<std::string> Snapshot() const;

  // Number of registered schemes, including "app".
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> ordered_;
  std::set<std::string> names_;
};

}  // namespace laufey_common

#endif  // LAUFEY_SCHEME_REGISTRY_H_
