// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_cef_network_quiet.h"

#include <algorithm>
#include <cctype>

namespace laufey_common {

const std::vector<std::string>& CefQuietDisabledFeatures() {
  static const std::vector<std::string> features = {
      // clients2.google.com/time/1/current (network_time_component).
      "NetworkTimeServiceQuerying",
      // www.google.com/async/folae (aim_eligibility_fetch), which also asks
      // for the Google accounts in the cookie jar first.
      "AimEnabled",
      // Preconnects to the default search engine (www.google.com).
      "PreconnectToSearch",
  };
  return features;
}

std::vector<std::string> FeatureListNames(const std::string& value) {
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    if (end == std::string::npos)
      end = value.size();
    std::string entry = value.substr(start, end - start);
    size_t first = entry.find_first_not_of(" \t");
    size_t last = entry.find_last_not_of(" \t");
    entry = first == std::string::npos ? std::string()
                                       : entry.substr(first, last - first + 1);
    if (!entry.empty() && entry[0] == '*')
      entry.erase(0, 1);
    entry = entry.substr(0, entry.find_first_of("<:"));
    if (!entry.empty())
      names.push_back(entry);
    start = end + 1;
  }
  return names;
}

std::string MergeQuietDisabledFeatures(const std::string& disable,
                                       const std::string& enable) {
  const std::vector<std::string> disabled = FeatureListNames(disable);
  const std::vector<std::string> enabled = FeatureListNames(enable);
  auto contains = [](const std::vector<std::string>& list,
                     const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  std::string merged = disable;
  for (const std::string& feature : CefQuietDisabledFeatures()) {
    if (contains(disabled, feature) || contains(enabled, feature))
      continue;
    if (!merged.empty() && merged.back() != ',')
      merged += ',';
    merged += feature;
  }
  return merged;
}

namespace {

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool AllDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  });
}

// "<language>-<major>-<minor>.bdic" for `language` (both lowercased).
bool IsDictionaryFileFor(const std::string& file, const std::string& language) {
  static const std::string kExt = ".bdic";
  if (language.empty() || file.size() <= language.size() + kExt.size() + 1)
    return false;
  if (file.compare(0, language.size(), language) != 0 ||
      file[language.size()] != '-' ||
      file.compare(file.size() - kExt.size(), kExt.size(), kExt) != 0) {
    return false;
  }
  std::string version = file.substr(
      language.size() + 1, file.size() - language.size() - 1 - kExt.size());
  size_t dash = version.find('-');
  return dash != std::string::npos && AllDigits(version.substr(0, dash)) &&
         AllDigits(version.substr(dash + 1));
}

}  // namespace

std::vector<std::string> LocalHunspellDictionaries(
    const std::vector<std::string>& languages,
    const std::vector<std::string>& file_names) {
  std::vector<std::string> kept;
  for (const std::string& language : languages) {
    const std::string lang = Lower(language);
    bool found = std::any_of(file_names.begin(), file_names.end(),
                             [&](const std::string& file) {
                               return IsDictionaryFileFor(Lower(file), lang);
                             });
    if (found)
      kept.push_back(language);
  }
  return kept;
}

}  // namespace laufey_common
