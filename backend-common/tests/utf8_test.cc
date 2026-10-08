// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// UTF-8 for text a JavaScript engine hands over as UTF-16 (laufey_utf8.h:
// the JSON `\uXXXX` decoding behind webview/src/laufey_json.h) and, on
// Windows, laufey_common::WideToUtf8 (strings_win.cc), which converts
// WebView2's ExecuteScript results: a surrogate pair is one four-byte code
// point, an unpaired surrogate U+FFFD. Plain asserts, no framework.

#include "laufey_utf8.h"

#ifdef _WIN32
#include "laufey_backend_common.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using laufey_common::AppendUtf8;
using laufey_common::DecodeJsonUnicodeEscape;
using laufey_common::ReadHex4;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static std::string Utf8(uint32_t cp) {
  std::string out;
  AppendUtf8(out, cp);
  return out;
}

static void TestAppendUtf8() {
  EXPECT(Utf8('A') == "A");
  EXPECT(Utf8(0) == std::string(1, '\0'));
  EXPECT(Utf8(0xE9) == "\xC3\xA9");
  EXPECT(Utf8(0x20AC) == "\xE2\x82\xAC");
  EXPECT(Utf8(0x1F600) == "\xF0\x9F\x98\x80");
  EXPECT(Utf8(0x10FFFF) == "\xF4\x8F\xBF\xBF");
  // No UTF-8 form: U+FFFD.
  EXPECT(Utf8(0xD800) == "\xEF\xBF\xBD");
  EXPECT(Utf8(0xDFFF) == "\xEF\xBF\xBD");
  EXPECT(Utf8(0x110000) == "\xEF\xBF\xBD");
}

// Decode every escape in a JSON string body the way laufey_json.h does.
static std::string Unescape(const char* s) {
  std::string out;
  for (const char* p = s; *p; ++p) {
    if (*p == '\\' && p[1]) {
      ++p;
      uint32_t cp = 0;
      size_t used = DecodeJsonUnicodeEscape(p, &cp);
      if (used) {
        AppendUtf8(out, cp);
        p += used - 1;
      } else {
        out += *p;
      }
    } else {
      out += *p;
    }
  }
  return out;
}

static void TestJsonEscapes() {
  EXPECT(ReadHex4("00e9") == 0xE9);
  EXPECT(ReadHex4("D83D") == 0xD83D);
  EXPECT(ReadHex4("12g4") == -1);
  EXPECT(ReadHex4("12") == -1);  // the NUL ends it

  uint32_t cp = 0;
  EXPECT(DecodeJsonUnicodeEscape("u00e9", &cp) == 5 && cp == 0xE9);
  // A pair is one code point (not CESU-8).
  EXPECT(DecodeJsonUnicodeEscape("ud83d\\ude00", &cp) == 11 && cp == 0x1F600);
  EXPECT(Unescape("\\ud83d\\ude00") == "\xF0\x9F\x98\x80");
  EXPECT(Unescape("a\\u00e9\\u20ACb") == "a\xC3\xA9\xE2\x82\xAC" "b");
  // Unpaired surrogates (JSON.stringify writes them as escapes): U+FFFD, and
  // what follows a lone high surrogate is kept.
  EXPECT(DecodeJsonUnicodeEscape("ud800", &cp) == 5 && cp == 0xFFFD);
  EXPECT(DecodeJsonUnicodeEscape("udc00", &cp) == 5 && cp == 0xFFFD);
  EXPECT(Unescape("x\\ud800y") == "x\xEF\xBF\xBDy");
  EXPECT(Unescape("\\ud800\\u0041") == "\xEF\xBF\xBD" "A");
  EXPECT(Unescape("\\ude00\\ud83d") == "\xEF\xBF\xBD\xEF\xBF\xBD");
  EXPECT(Unescape("\\ud83d\\ud83d\\ude00") == "\xEF\xBF\xBD\xF0\x9F\x98\x80");
  // Malformed: not decoded.
  EXPECT(DecodeJsonUnicodeEscape("u12", &cp) == 0);
  EXPECT(DecodeJsonUnicodeEscape("uzzzz", &cp) == 0);
  EXPECT(DecodeJsonUnicodeEscape("n", &cp) == 0);
}

#ifdef _WIN32
static void TestWideToUtf8() {
  using laufey_common::WideToUtf8;
  const wchar_t cafe[] = {L'"', L'c', L'a', L'f', 0x00E9, L'"', 0};
  EXPECT(WideToUtf8(cafe) == "\"caf\xC3\xA9\"");
  const wchar_t pair[] = {0xD83D, 0xDE00, 0};
  EXPECT(WideToUtf8(pair) == "\xF0\x9F\x98\x80");
  const wchar_t lone_high[] = {L'a', 0xD800, L'b', 0};
  EXPECT(WideToUtf8(lone_high) == "a\xEF\xBF\xBD" "b");
  const wchar_t lone_low[] = {0xDC00, 0};
  EXPECT(WideToUtf8(lone_low) == "\xEF\xBF\xBD");
}
#endif

int main() {
  TestAppendUtf8();
  TestJsonEscapes();
#ifdef _WIN32
  TestWideToUtf8();
#endif
  std::printf("utf8_test: OK\n");
  return 0;
}
