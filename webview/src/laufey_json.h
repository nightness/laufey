// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_JSON_H_
#define LAUFEY_JSON_H_

#include "laufey_utf8.h"
#include "webview_value.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <clocale>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace json {

inline std::string Escape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

inline std::string Serialize(const laufey::ValuePtr& value);

inline std::string SerializeList(const laufey::ValueList& list) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0)
      ss << ",";
    ss << Serialize(list[i]);
  }
  ss << "]";
  return ss.str();
}

inline std::string SerializeDict(const laufey::ValueDict& dict) {
  std::ostringstream ss;
  ss << "{";
  bool first = true;
  for (const auto& pair : dict) {
    if (!first)
      ss << ",";
    first = false;
    ss << "\"" << Escape(pair.first) << "\":" << Serialize(pair.second);
  }
  ss << "}";
  return ss.str();
}

inline std::string Serialize(const laufey::ValuePtr& value) {
  if (!value)
    return "null";
  switch (value->type) {
    case laufey::ValueType::Null:
      return "null";
    case laufey::ValueType::Bool:
      return value->GetBool() ? "true" : "false";
    case laufey::ValueType::Int:
      return std::to_string(value->GetInt());
    case laufey::ValueType::Double: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", value->GetDouble());
      return buf;
    }
    case laufey::ValueType::String:
      return "\"" + Escape(value->GetString()) + "\"";
    case laufey::ValueType::Binary: {
      const auto& binary = value->GetBinary();
      std::string base64;
      static const char* chars =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      size_t i = 0;
      const uint8_t* data = binary.data.data();
      size_t len = binary.data.size();
      while (i < len) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len)
          n |= (data[i + 1] << 8);
        if (i + 2 < len)
          n |= data[i + 2];
        base64 += chars[(n >> 18) & 0x3F];
        base64 += chars[(n >> 12) & 0x3F];
        base64 += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
        base64 += (i + 2 < len) ? chars[n & 0x3F] : '=';
        i += 3;
      }
      return "{\"__binary__\":\"" + base64 + "\"}";
    }
    case laufey::ValueType::List:
      return SerializeList(value->GetList());
    case laufey::ValueType::Dict:
      return SerializeDict(value->GetDict());
    case laufey::ValueType::Callback:
      return "{\"__callback__\":\"" + std::to_string(value->GetCallbackId()) +
             "\"}";
  }
  return "null";
}

// --- Parser ------------------------------------------------------------------
//
// Reads the JSON a page hands the bridge (and the engines' script results).
// The input is page-controlled, so the parser is strict RFC 8259: every step
// consumes input or fails, nesting stops at kMaxBridgeJsonDepth (the parser
// recurses, and so do the Value walkers after it), and a document larger than
// kMaxBridgeJsonBytes is refused before parsing. Any error, including
// trailing garbage, makes ParseJson return nullptr.

// Nesting limit. A bridge call's arguments are a list (1) of plain values; 64
// leaves room for any structured argument while keeping the recursion (here
// and in every Value walker) far from the stack's end.
constexpr int kMaxBridgeJsonDepth = 64;
// Size limit for one document (one bridge message or script result).
constexpr size_t kMaxBridgeJsonBytes = 128u * 1024u * 1024u;

namespace internal {

// Decodes a "__binary__" base64 payload (padding and stray characters are
// skipped, as before).
inline laufey::ValuePtr DecodeBinary(const std::string& base64) {
  std::vector<uint8_t> data;
  data.reserve(base64.size() / 4 * 3);
  int val = 0, bits = -8;
  for (char c : base64) {
    if (c == '=')
      break;
    int d;
    if (c >= 'A' && c <= 'Z')
      d = c - 'A';
    else if (c >= 'a' && c <= 'z')
      d = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
      d = c - '0' + 52;
    else if (c == '+')
      d = 62;
    else if (c == '/')
      d = 63;
    else
      continue;
    val = ((val << 6) | d) & 0xFFFFFF;
    bits += 6;
    if (bits >= 0) {
      data.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return laufey::Value::Binary(data.data(), data.size());
}

class Parser {
 public:
  Parser(const char* begin, const char* end) : p_(begin), end_(end) {}

  // The whole input as one value, or nullptr.
  laufey::ValuePtr ParseDocument() {
    laufey::ValuePtr value = ParseValue(0);
    if (!value)
      return nullptr;
    SkipWhitespace();
    if (p_ != end_)
      return nullptr;  // trailing garbage
    return value;
  }

 private:
  void SkipWhitespace() {
    while (p_ < end_ &&
           (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
      ++p_;
  }

  bool Literal(const char* word) {
    size_t n = std::strlen(word);
    if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, word, n) != 0)
      return false;
    p_ += n;
    return true;
  }

  laufey::ValuePtr ParseValue(int depth) {
    if (depth > kMaxBridgeJsonDepth)
      return nullptr;
    SkipWhitespace();
    if (p_ >= end_)
      return nullptr;
    switch (*p_) {
      case 'n':
        return Literal("null") ? laufey::Value::Null() : nullptr;
      case 't':
        return Literal("true") ? laufey::Value::Bool(true) : nullptr;
      case 'f':
        return Literal("false") ? laufey::Value::Bool(false) : nullptr;
      case '"': {
        std::string s;
        if (!ParseString(&s))
          return nullptr;
        return laufey::Value::String(s);
      }
      case '[':
        return ParseArray(depth);
      case '{':
        return ParseObject(depth);
      default:
        if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9'))
          return ParseNumber();
        return nullptr;
    }
  }

  static int HexDigit(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }

  bool ReadHex4(uint32_t* out) {
    if (end_ - p_ < 4)
      return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      int d = HexDigit(p_[i]);
      if (d < 0)
        return false;
      v = (v << 4) | static_cast<uint32_t>(d);
    }
    p_ += 4;
    *out = v;
    return true;
  }

  static void AppendUtf8(std::string* out, uint32_t cp) {
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

  // At the opening quote. A lone surrogate (JSON.stringify writes one as an
  // escape) decodes to U+FFFD.
  bool ParseString(std::string* out) {
    ++p_;  // '"'
    while (p_ < end_) {
      unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20)
        return false;  // raw control character
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        ++p_;
        continue;
      }
      ++p_;
      if (p_ >= end_)
        return false;
      char e = *p_++;
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
          uint32_t cp;
          if (!ReadHex4(&cp))
            return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo;
            const char* save = p_;
            if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
              p_ += 2;
              if (ReadHex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                p_ = save;
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;  // unterminated
  }

  laufey::ValuePtr ParseNumber() {
    const char* start = p_;
    if (*p_ == '-')
      ++p_;
    if (p_ >= end_)
      return nullptr;
    if (*p_ == '0') {
      ++p_;
    } else if (*p_ >= '1' && *p_ <= '9') {
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
        ++p_;
    } else {
      return nullptr;  // "-" alone, "-x"
    }
    bool integral = true;
    if (p_ < end_ && *p_ == '.') {
      integral = false;
      ++p_;
      if (p_ >= end_ || *p_ < '0' || *p_ > '9')
        return nullptr;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      integral = false;
      ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-'))
        ++p_;
      if (p_ >= end_ || *p_ < '0' || *p_ > '9')
        return nullptr;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
    // The token is validated. strtod follows the process locale's decimal
    // separator (GTK sets the locale), so hand it that separator.
    std::string token(start, p_);
    const char* point = std::localeconv()->decimal_point;
    if (point && point[0] && point[0] != '.' && !point[1]) {
      for (char& c : token) {
        if (c == '.')
          c = point[0];
      }
    }
    double d = std::strtod(token.c_str(), nullptr);
    if (!std::isfinite(d))
      return nullptr;  // out of range ("1e999"); JSON.stringify never writes it
    if (integral && d >= INT_MIN && d <= INT_MAX)
      return laufey::Value::Int(static_cast<int>(d));
    return laufey::Value::Double(d);
  }

  laufey::ValuePtr ParseArray(int depth) {
    ++p_;  // '['
    auto list = laufey::Value::List();
    SkipWhitespace();
    if (p_ < end_ && *p_ == ']') {
      ++p_;
      return list;
    }
    for (;;) {
      laufey::ValuePtr item = ParseValue(depth + 1);
      if (!item)
        return nullptr;
      list->GetList().push_back(item);
      SkipWhitespace();
      if (p_ >= end_)
        return nullptr;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == ']') {
        ++p_;
        return list;
      }
      return nullptr;
    }
  }

  laufey::ValuePtr ParseObject(int depth) {
    ++p_;  // '{'
    auto dict = laufey::Value::Dict();
    SkipWhitespace();
    if (p_ < end_ && *p_ == '}') {
      ++p_;
      return dict;
    }
    for (;;) {
      SkipWhitespace();
      if (p_ >= end_ || *p_ != '"')
        return nullptr;
      std::string key;
      if (!ParseString(&key))
        return nullptr;
      SkipWhitespace();
      if (p_ >= end_ || *p_ != ':')
        return nullptr;
      ++p_;
      laufey::ValuePtr value = ParseValue(depth + 1);
      if (!value)
        return nullptr;
      dict->GetDict()[key] = value;
      SkipWhitespace();
      if (p_ >= end_)
        return nullptr;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ == '}') {
        ++p_;
        break;
      }
      return nullptr;
    }
    // The bridge's tagged objects: a JS callback and binary data.
    const auto& d = dict->GetDict();
    auto it = d.find("__callback__");
    if (it != d.end() && it->second->IsString()) {
      const std::string& id = it->second->GetString();
      if (id.empty() || id.size() > 20 ||
          id.find_first_not_of("0123456789") != std::string::npos)
        return laufey::Value::Null();
      uint64_t n = 0;
      for (char c : id) {
        uint64_t next = n * 10 + static_cast<uint64_t>(c - '0');
        if (next / 10 != n)
          return laufey::Value::Null();  // overflow
        n = next;
      }
      return laufey::Value::Callback(n);
    }
    it = d.find("__binary__");
    if (it != d.end() && it->second->IsString())
      return DecodeBinary(it->second->GetString());
    return dict;
  }

  const char* p_;
  const char* end_;
};

}  // namespace internal

// Parses `json` (a page's bridge message, an engine's script result). Returns
// nullptr if it is not exactly one valid JSON value, nests deeper than
// kMaxBridgeJsonDepth, or is larger than kMaxBridgeJsonBytes.
inline laufey::ValuePtr ParseJson(const std::string& json) {
  if (json.size() > kMaxBridgeJsonBytes)
    return nullptr;
  return internal::Parser(json.data(), json.data() + json.size())
      .ParseDocument();
}

inline laufey::ValuePtr ParseJson(const char* json) {
  if (!json)
    return nullptr;
  return ParseJson(std::string(json));
}

}  // namespace json

#endif  // LAUFEY_JSON_H_
