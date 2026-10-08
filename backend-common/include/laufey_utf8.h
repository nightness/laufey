// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// UTF-8 encoding for text that arrives from a JavaScript engine as UTF-16
// code units. JavaScript strings may hold unpaired surrogates, which have no
// UTF-8 form: each becomes U+FFFD (as the Encoding Standard and Windows'
// WideCharToMultiByte do), while a surrogate pair becomes its one code point,
// never two three-byte sequences (CESU-8). Header-only, so the iOS build,
// which doesn't link backend-common, can use it too.

#ifndef LAUFEY_UTF8_H_
#define LAUFEY_UTF8_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace laufey_common {

inline constexpr uint32_t kReplacementCharacter = 0xFFFD;

// Append `cp` to `out` as UTF-8. A surrogate (U+D800..U+DFFF) or a value past
// U+10FFFF is appended as U+FFFD.
inline void AppendUtf8(std::string& out, uint32_t cp) {
  if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
    cp = kReplacementCharacter;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// The value of the four hex digits at `s` (any case), or -1 when they aren't
// four hex digits (a NUL ends the string early).
inline int32_t ReadHex4(const char* s) {
  int32_t value = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    int digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return -1;
    value = value * 16 + digit;
  }
  return value;
}

// Decode a JSON `\uXXXX` escape. `s` points just past the backslash (at the
// 'u'). On success stores the code point in `*cp` and returns how many
// characters it used from `s`: 5 ("uXXXX"), or 11 when a high surrogate is
// followed by a `\uXXXX` low surrogate ("uD83D\uDE00"), which combine into
// one code point. An unpaired surrogate decodes to U+FFFD (and a high one
// leaves whatever follows it alone). Returns 0 if `s` is not 'u' and four hex
// digits.
inline size_t DecodeJsonUnicodeEscape(const char* s, uint32_t* cp) {
  if (s[0] != 'u')
    return 0;
  int32_t unit = ReadHex4(s + 1);
  if (unit < 0)
    return 0;
  if (unit >= 0xD800 && unit <= 0xDBFF) {
    if (s[5] == '\\' && s[6] == 'u') {
      int32_t low = ReadHex4(s + 7);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        *cp = 0x10000 + ((static_cast<uint32_t>(unit) - 0xD800) << 10) +
              (static_cast<uint32_t>(low) - 0xDC00);
        return 11;
      }
    }
    *cp = kReplacementCharacter;
    return 5;
  }
  if (unit >= 0xDC00 && unit <= 0xDFFF) {
    *cp = kReplacementCharacter;
    return 5;
  }
  *cp = static_cast<uint32_t>(unit);
  return 5;
}

}  // namespace laufey_common

#endif  // LAUFEY_UTF8_H_
