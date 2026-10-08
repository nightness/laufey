// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The CEF backend makes no network request of its own: every request a
// laufey app sends is one its pages (or its runtime) asked for. See
// docs/backends.md, "No network requests of its own".
//
// laufey's browsers are Chrome-style, so Chrome's profile services run in
// them, and several of those contact Google at startup even with
// --disable-background-networking. Found from net logs of a fresh launch
// (each request's traffic annotation names the feature):
//
//   clients2.google.com/time/1/current   network_time_component
//                                        (NetworkTimeServiceQuerying)
//   www.google.com/async/folae           aim_eligibility_fetch
//                                        (AimEnabled: AI Mode eligibility)
//   www.google.com/ (preconnects)        the default search engine
//                                        preconnector (PreconnectToSearch)
//   accounts.google.com/ListAccounts     gaia_auth_list_accounts: the
//                                        Google accounts in the cookie jar,
//                                        asked for by signin metrics
//                                        services at profile start
//   redirector.gvt1.com/edgedl/chrome/   spellcheck_hunspell_dictionary
//     dict/*.bdic                        (Linux: Hunspell dictionaries are
//                                        downloaded on first use)
//   update.googleapis.com,               update_client: the component
//     edgedl.me.gvt1.com (after 60 s)    updater's checks and downloads
//
// The host turns the first three features off (MergeQuietDisabledFeatures),
// points Chrome's Google-accounts origin at a port Chromium refuses to
// connect to (kCefQuietGaiaUrl: the requests fail before any DNS lookup or
// socket), passes --disable-component-update, and on Linux keeps only the
// spellcheck dictionaries whose file is already on disk
// (LocalHunspellDictionaries).

#ifndef LAUFEY_CEF_NETWORK_QUIET_H_
#define LAUFEY_CEF_NETWORK_QUIET_H_

#include <string>
#include <vector>

namespace laufey_common {

// The Chromium features the CEF host disables by default, in the order it
// appends them to --disable-features.
const std::vector<std::string>& CefQuietDisabledFeatures();

// The feature names in a --enable-features / --disable-features value: each
// comma-separated entry without a leading "*" and without a "<Trial" or
// ":param/value" suffix. Empty entries are skipped.
std::vector<std::string> FeatureListNames(const std::string& value);

// The --disable-features value with the quiet defaults added: `disable` (the
// value already on the command line, possibly empty) followed by each
// default that is neither already in it nor named in `enable`. A feature
// the app's own command line enables stays enabled. Returns `disable`
// unchanged when there is nothing to add.
std::string MergeQuietDisabledFeatures(const std::string& disable,
                                       const std::string& enable);

// The --gaia-url value the host sets when the command line has none: the
// origin of Chrome's Google-accounts requests (ListAccounts and the rest of
// the GAIA cookie checks). Port 9 is on Chromium's restricted-port list, so
// each such request ends with ERR_UNSAFE_PORT before a DNS lookup or a
// socket; "localhost" never leaves the machine even if that changed.
inline constexpr char kCefQuietGaiaUrl[] = "https://localhost:9";

// Of the spellcheck languages in `languages` (the profile's
// "spellcheck.dictionaries"), the ones whose Hunspell dictionary is among
// `file_names` (the names in the profile's Dictionaries folder): a file
// named "<language>-<major>-<minor>.bdic", compared without regard to case,
// as Chromium names them (en-US-10-1.bdic). Chromium downloads a missing
// one from Google, so the Linux host keeps only these.
std::vector<std::string> LocalHunspellDictionaries(
    const std::vector<std::string>& languages,
    const std::vector<std::string>& file_names);

}  // namespace laufey_common

#endif  // LAUFEY_CEF_NETWORK_QUIET_H_
