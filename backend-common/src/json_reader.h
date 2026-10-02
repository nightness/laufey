// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Internal to backend-common: a small strict JSON reader shared by the
// launch file (launch_config.cc) and the passkey options parser
// (passkey.cc). Not part of the public headers.

#ifndef LAUFEY_BACKEND_COMMON_JSON_READER_H_
#define LAUFEY_BACKEND_COMMON_JSON_READER_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace laufey_common {
namespace json {

// Nesting limit for the JSON reader (the launch file needs 2, passkey
// options 4).
constexpr int kMaxJsonDepth = 32;

// --- Minimal JSON reader ----------------------------------------------------
//
// RFC 8259 syntax: objects, arrays, strings (all escapes, \u surrogate pairs
// decoded to UTF-8), numbers, true/false/null. Values the schema doesn't use
// are validated for syntax and dropped.

struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  // For kNumber: true when the literal has no fraction or exponent and fits
  // in an int64_t, with the value in `integer`. Other numbers are only
  // syntax-checked.
  bool is_integer = false;
  int64_t integer = 0;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;
};

inline const char* TypeName(JsonValue::Type type) {
  switch (type) {
    case JsonValue::Type::kNull:
      return "null";
    case JsonValue::Type::kBool:
      return "a boolean";
    case JsonValue::Type::kNumber:
      return "a number";
    case JsonValue::Type::kString:
      return "a string";
    case JsonValue::Type::kArray:
      return "an array";
    case JsonValue::Type::kObject:
      return "an object";
  }
  return "?";
}

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  // Parses the whole text as one value. On failure returns false and sets
  // `error`.
  bool ParseDocument(JsonValue* out, std::string* error) {
    SkipSpace();
    if (!ParseValue(out, 0)) {
      *error = error_;
      return false;
    }
    SkipSpace();
    if (pos_ != text_.size()) {
      *error = At("unexpected trailing characters");
      return false;
    }
    return true;
  }

 private:
  std::string At(const std::string& what) const {
    return what + " at byte " + std::to_string(pos_);
  }

  bool Fail(const std::string& what) {
    if (error_.empty())
      error_ = At(what);
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r'))
      ++pos_;
  }

  bool Consume(const char* literal) {
    size_t n = std::strlen(literal);
    if (text_.compare(pos_, n, literal) != 0)
      return false;
    pos_ += n;
    return true;
  }

  bool ParseValue(JsonValue* out, int depth) {
    if (depth > kMaxJsonDepth)
      return Fail("nesting too deep");
    if (pos_ >= text_.size())
      return Fail("unexpected end of input");
    char c = text_[pos_];
    if (c == '{')
      return ParseObject(out, depth);
    if (c == '[')
      return ParseArray(out, depth);
    if (c == '"') {
      out->type = JsonValue::Type::kString;
      return ParseString(&out->string);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      out->type = JsonValue::Type::kNumber;
      return ParseNumber(out);
    }
    if (Consume("true")) {
      out->type = JsonValue::Type::kBool;
      out->boolean = true;
      return true;
    }
    if (Consume("false")) {
      out->type = JsonValue::Type::kBool;
      out->boolean = false;
      return true;
    }
    if (Consume("null")) {
      out->type = JsonValue::Type::kNull;
      return true;
    }
    return Fail("unexpected character");
  }

  bool ParseObject(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kObject;
    ++pos_;  // '{'
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"')
        return Fail("expected a string key");
      std::string key;
      if (!ParseString(&key))
        return false;
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':')
        return Fail("expected ':'");
      ++pos_;
      SkipSpace();
      JsonValue value;
      if (!ParseValue(&value, depth + 1))
        return false;
      out->object.emplace_back(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kArray;
    ++pos_;  // '['
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      SkipSpace();
      JsonValue value;
      if (!ParseValue(&value, depth + 1))
        return false;
      out->array.push_back(std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseHex4(unsigned* out) {
    if (text_.size() - pos_ < 4)
      return Fail("truncated \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<unsigned>(c - 'A' + 10);
      else
        return Fail("invalid \\u escape");
    }
    *out = v;
    return true;
  }

  static void AppendUtf8(std::string* out, unsigned cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string* out) {
    ++pos_;  // opening quote
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return Fail("control character in string");
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size())
        break;
      char e = text_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out->push_back(e);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          unsigned cp = 0;
          if (!ParseHex4(&cp))
            return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned low = 0;
            if (!Consume("\\u") || !ParseHex4(&low) || low < 0xDC00 ||
                low > 0xDFFF)
              return Fail("unpaired surrogate in \\u escape");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate in \\u escape");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("invalid escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber(JsonValue* out) {
    size_t start = pos_;
    bool negative = false;
    if (text_[pos_] == '-') {
      negative = true;
      ++pos_;
    }
    auto digit = [&] {
      return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9';
    };
    if (!digit())
      return Fail("invalid number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      while (digit())
        ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (!digit())
        return Fail("invalid number");
      while (digit())
        ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
        ++pos_;
      if (!digit())
        return Fail("invalid number");
      while (digit())
        ++pos_;
    }
    // Integer value of a plain integer literal (no fraction / exponent).
    size_t int_begin = start + (negative ? 1 : 0);
    bool plain = true;
    for (size_t i = int_begin; i < pos_; ++i) {
      if (text_[i] < '0' || text_[i] > '9') {
        plain = false;
        break;
      }
    }
    // 18 digits always fit in an int64_t.
    if (plain && pos_ - int_begin <= 18) {
      int64_t v = 0;
      for (size_t i = int_begin; i < pos_; ++i)
        v = v * 10 + (text_[i] - '0');
      out->is_integer = true;
      out->integer = negative ? -v : v;
    }
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace json
}  // namespace laufey_common

#endif  // LAUFEY_BACKEND_COMMON_JSON_READER_H_
