// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// json::ParseJson (webview/src/laufey_json.h): the parser for the JSON a page
// posts to the bridge. The input is page-controlled, so every malformed or
// hostile document must fail (nullptr) quickly: no infinite loop on a token
// that consumes nothing, no stack overflow on deep nesting, no unbounded
// input. Plain asserts, no framework.

#include "laufey_json.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using laufey::ValuePtr;

static ValuePtr P(const std::string& s) {
  return json::ParseJson(s);
}

int main() {
  // Well-formed documents keep parsing as before.
  {
    ValuePtr v =
        P(R"({"callId":7,"method":"a.b","args":[1,-2.5,"x",true,false,null,)"
          R"({"k":[]},{"__binary__":"aGk="},{"__callback__":"42"}]})");
    EXPECT(v && v->IsDict());
    const auto& d = v->GetDict();
    EXPECT(d.at("callId")->IsInt() && d.at("callId")->GetInt() == 7);
    EXPECT(d.at("method")->GetString() == "a.b");
    const auto& args = d.at("args")->GetList();
    EXPECT(args.size() == 9);
    EXPECT(args[0]->GetInt() == 1);
    EXPECT(args[1]->IsDouble() && args[1]->GetDouble() == -2.5);
    EXPECT(args[2]->GetString() == "x");
    EXPECT(args[3]->GetBool() && !args[4]->GetBool() && args[5]->IsNull());
    EXPECT(args[6]->IsDict() && args[6]->GetDict().at("k")->IsList());
    EXPECT(args[7]->IsBinary() && args[7]->GetBinary().data.size() == 2 &&
           args[7]->GetBinary().data[0] == 'h');
    EXPECT(args[8]->IsCallback() && args[8]->GetCallbackId() == 42);
  }
  EXPECT(P("  [ ]  ") && P("[]")->GetList().empty());
  EXPECT(P("{}") && P("{}")->IsDict());
  EXPECT(P("0")->GetInt() == 0);
  EXPECT(P("1e3")->IsDouble() && P("1e3")->GetDouble() == 1000);
  EXPECT(P("4294967296")->IsDouble());  // outside int: a double, as before
  EXPECT(P("5e-324") && P("5e-324")->IsDouble());

  // Strings: escapes, surrogate pairs, lone surrogates.
  EXPECT(P(R"("a\"b\\c\/d\n\t")")->GetString() == "a\"b\\c/d\n\t");
  EXPECT(P(R"("\u00e9")")->GetString() == "\xC3\xA9");
  EXPECT(P(R"("\ud83d\ude00")")->GetString() == "\xF0\x9F\x98\x80");
  EXPECT(P(R"("\ud83d")")->GetString() == "\xEF\xBF\xBD");
  EXPECT(P(R"("\ude00x")")->GetString() == "\xEF\xBF\xBDx");
  EXPECT(P(R"("\u0000")")->GetString() == std::string(1, '\0'));

  // Malformed: each fails instead of looping, guessing or reading past the
  // end. The old parser looped forever on "[x" and "[-".
  for (const char* bad : {
           "",          " ",        "[",          "[x",
           "[-",        "[-x]",     "[1,",        "[1 2]",
           "[,]",       "{",        "{\"a\"",     "{\"a\":",
           "{\"a\" 1}", "{a:1}",    "{\"a\":1,}", "[1,]",
           "\"abc",     "\"\\x\"",  "\"\\u12\"",  "\"\\u12G4\"",
           "tru",       "nul",      "falsey",     "01",
           "1.",        ".5",       "1e",         "1e+",
           "--1",       "+1",       "1e999",      "[1]x",
           "{} {}",     "\"a\nb\"", "[\"\\",      "{\"__callback__\":\"1\"",
       }) {
    if (P(bad)) {
      std::fprintf(stderr, "parsed malformed input: %s\n", bad);
      std::exit(1);
    }
  }
  EXPECT(!json::ParseJson(static_cast<const char*>(nullptr)));

  // A malformed callback id is null, not an exception or a wrapped id.
  EXPECT(P(R"({"__callback__":"12x"})")->IsNull());
  EXPECT(P(R"({"__callback__":"99999999999999999999999"})")->IsNull());

  // Depth: the limit parses, one more fails; a deep document fails fast
  // instead of overflowing the stack.
  {
    int limit = json::kMaxBridgeJsonDepth;
    std::string ok = std::string(limit, '[') + "1" + std::string(limit, ']');
    EXPECT(P(ok));
    std::string over =
        std::string(limit + 1, '[') + "1" + std::string(limit + 1, ']');
    EXPECT(!P(over));
    std::string objs;
    for (int i = 0; i <= limit; ++i)
      objs += "{\"a\":";
    objs += "1" + std::string(limit + 1, '}');
    EXPECT(!P(objs));
    auto t0 = std::chrono::steady_clock::now();
    std::string deep(2000000, '[');
    EXPECT(!P(deep));
    deep = std::string(1000000, '[') + std::string(1000000, ']');
    EXPECT(!P(deep));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    EXPECT(ms < 5000);
  }

  // Size: a document over the cap is refused before parsing.
  {
    std::string big;
    big.reserve(json::kMaxBridgeJsonBytes + 3);
    big.push_back('"');
    big.append(json::kMaxBridgeJsonBytes, 'a');
    big.push_back('"');
    EXPECT(!P(big));
  }

  // Round trip through Serialize.
  {
    ValuePtr v = P(R"({"s":"\u2713\n","n":[1,2.5,null]})");
    ValuePtr again = P(json::Serialize(v));
    EXPECT(again && again->GetDict().at("s")->GetString() == "\xE2\x9C\x93\n");
    EXPECT(again->GetDict().at("n")->GetList().size() == 3);
  }

  std::printf("laufey_json_test: ok\n");
  return 0;
}
