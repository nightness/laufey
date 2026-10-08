// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The WebAuthn RP ID rule the passkey options parser (laufey_passkey.h) and
// the launch file's "passkeyRpIds" (laufey_launch_config.h) share. Header-only
// so the launch file reader doesn't link the passkey code.

#ifndef LAUFEY_PASSKEY_RP_ID_H_
#define LAUFEY_PASSKEY_RP_ID_H_

#include <cstddef>
#include <string>

namespace laufey_common {

// A WebAuthn RP ID this layer passes to the OS: a lowercase ASCII domain
// name (LDH labels of 1..63 characters, at most 253 characters, no leading /
// trailing dot or hyphen) that is not an IPv4 address. Internationalized
// names must be given in their A-label (punycode) form. The OS still decides
// whether the app may use it (macOS: the associated domain).
inline bool IsValidPasskeyRpId(const std::string& rp_id) {
  if (rp_id.empty() || rp_id.size() > 253)
    return false;
  size_t label_start = 0;
  bool last_label_numeric = true;
  for (size_t i = 0; i <= rp_id.size(); ++i) {
    if (i == rp_id.size() || rp_id[i] == '.') {
      size_t len = i - label_start;
      if (len == 0 || len > 63)
        return false;
      if (rp_id[label_start] == '-' || rp_id[i - 1] == '-')
        return false;
      last_label_numeric = true;
      for (size_t k = label_start; k < i; ++k) {
        if (rp_id[k] < '0' || rp_id[k] > '9') {
          last_label_numeric = false;
          break;
        }
      }
      label_start = i + 1;
      continue;
    }
    char c = rp_id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok)
      return false;
  }
  // An all-numeric top label means an IPv4 address (or a bare number), which
  // is not a domain.
  return !last_label_numeric;
}

}  // namespace laufey_common

#endif  // LAUFEY_PASSKEY_RP_ID_H_
