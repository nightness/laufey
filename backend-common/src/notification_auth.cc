// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Authenticated notification click arguments (see "Click arguments" in
// laufey_notifications.h and docs/notifications.md).
//
// A click on a Linux portal notification arrives as
// org.freedesktop.Application.ActivateAction on the app's D-Bus name, which
// any process on the session bus can call; a Windows toast click arrives as
// INotificationActivationCallback::Activate on a COM class any process of
// the user can create. Both carry the tag, action and data in their
// arguments, so this process can't tell a click from a forgery by where it
// came from. Every argument string laufey posts ends in an HMAC-SHA256 of
// the rest under a key of this install's own (kept in the app's data
// directory, owner-only), and an activation whose MAC doesn't verify is
// dropped. A MAC only proves the arguments came from this install: the data
// is still the page's own untrusted input.

#include "laufey_backend_common.h"
#include "laufey_notifications.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

// --- SHA-256 (FIPS 180-4) ---

const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

class Sha256 {
 public:
  Sha256() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                      0xa54ff53a, 0x510e527f, 0x9b05688c,
                                      0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInit, sizeof(h_));
  }

  void Update(const uint8_t* data, size_t len) {
    total_ += len;
    while (len > 0) {
      size_t take = 64 - used_;
      if (take > len)
        take = len;
      std::memcpy(block_ + used_, data, take);
      used_ += take;
      data += take;
      len -= take;
      if (used_ == 64) {
        Compress();
        used_ = 0;
      }
    }
  }

  void Update(const std::string& s) {
    Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

  std::string Final() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56)
      Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i)
      len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Update(len, 8);
    std::string out(32, '\0');
    for (int i = 0; i < 8; ++i) {
      out[4 * i] = static_cast<char>(h_[i] >> 24);
      out[4 * i + 1] = static_cast<char>(h_[i] >> 16);
      out[4 * i + 2] = static_cast<char>(h_[i] >> 8);
      out[4 * i + 3] = static_cast<char>(h_[i]);
    }
    return out;
  }

 private:
  void Compress() {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block_[4 * i]) << 24) |
             (static_cast<uint32_t>(block_[4 * i + 1]) << 16) |
             (static_cast<uint32_t>(block_[4 * i + 2]) << 8) |
             static_cast<uint32_t>(block_[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5],
             g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
      uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t block_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};

std::string Hex(const std::string& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += kHex[c >> 4];
    out += kHex[c & 15];
  }
  return out;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// `text` (surrounding whitespace ignored) as kNotificationClickKeyBytes
// bytes of hex, else false.
bool ParseKeyFile(const std::string& text, std::string* key) {
  size_t b = text.find_first_not_of(" \t\r\n");
  size_t e = text.find_last_not_of(" \t\r\n");
  if (b == std::string::npos)
    return false;
  std::string hex = text.substr(b, e - b + 1);
  if (hex.size() != 2 * kNotificationClickKeyBytes)
    return false;
  std::string out(kNotificationClickKeyBytes, '\0');
  for (size_t i = 0; i < kNotificationClickKeyBytes; ++i) {
    int hi = HexDigit(hex[2 * i]), lo = HexDigit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = static_cast<char>(hi * 16 + lo);
  }
  *key = out;
  return true;
}

bool RandomBytes(std::string* out, size_t n) {
  out->assign(n, '\0');
#ifdef _WIN32
  return BCRYPT_SUCCESS(
      BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&(*out)[0]),
                      static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
  // getentropy: at most 256 bytes per call (glibc 2.25+, macOS 10.12+).
  return n <= 256 && getentropy(&(*out)[0], n) == 0;
#endif
}

// The MAC'd part of an argument string: domain-separated, so a MAC can't be
// replayed as one for another use of the same key.
std::string MacOf(const std::string& key, const std::string& prefix) {
  return Hex(
      HmacSha256(key, std::string("laufey-notification-click/1\n") + prefix));
}

const char kMacField[] = "&mac=";

enum class ReadResult { kRead, kMissing, kUnusable };

bool NotificationDebug() {
  static const bool on = !GetEnvUtf8("LAUFEY_NOTIFICATION_DEBUG").empty();
  return on;
}

#ifdef _WIN32

ReadResult ReadKeyFile(const std::string& path, std::string* out) {
  HANDLE h = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND ? ReadResult::kMissing
                                       : ReadResult::kUnusable;
  }
  char buf[256];
  DWORD n = 0;
  bool ok = ReadFile(h, buf, sizeof(buf), &n, nullptr) != 0;
  CloseHandle(h);
  if (!ok)
    return ReadResult::kUnusable;
  out->assign(buf, n);
  return ReadResult::kRead;
}

// Writes `text` to a new file `tmp` and moves it to `path`: over an existing
// one only when `replace`. False if `path` exists (and !replace) or on error.
bool PublishFile(const std::string& tmp, const std::string& path,
                 const std::string& text, bool replace) {
  std::wstring wtmp = Utf8ToWide(tmp);
  HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written,
                      nullptr) != 0 &&
            written == text.size();
  ok = FlushFileBuffers(h) != 0 && ok;
  CloseHandle(h);
  if (ok) {
    ok = MoveFileExW(wtmp.c_str(), Utf8ToWide(path).c_str(),
                     MOVEFILE_WRITE_THROUGH |
                         (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
  }
  if (!ok)
    DeleteFileW(wtmp.c_str());
  return ok;
}

#else

// The key file's contents, if it is a regular file this user owns that no
// one else can read or write (a link is not followed).
ReadResult ReadKeyFile(const std::string& path, std::string* out) {
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return errno == ENOENT ? ReadResult::kMissing : ReadResult::kUnusable;
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_uid == geteuid() && (st.st_mode & 077) == 0;
  char buf[256];
  ssize_t n = ok ? read(fd, buf, sizeof(buf)) : -1;
  close(fd);
  if (n < 0)
    return ReadResult::kUnusable;
  out->assign(buf, static_cast<size_t>(n));
  return ReadResult::kRead;
}

bool PublishFile(const std::string& tmp, const std::string& path,
                 const std::string& text, bool replace) {
  int fd = open(tmp.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  bool ok =
      write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  ok = fsync(fd) == 0 && ok;
  close(fd);
  if (ok) {
    // link() never replaces: of two processes creating the key at once,
    // one wins and the other reads the winner's.
    ok = replace ? rename(tmp.c_str(), path.c_str()) == 0
                 : link(tmp.c_str(), path.c_str()) == 0;
  }
  unlink(tmp.c_str());
  return ok;
}

#endif

std::mutex g_key_mutex;
std::string* g_key = nullptr;

}  // namespace

std::string HmacSha256(const std::string& key, const std::string& message) {
  std::string k = key;
  if (k.size() > 64) {
    Sha256 h;
    h.Update(k);
    k = h.Final();
  }
  k.resize(64, '\0');
  std::string ipad(64, '\0'), opad(64, '\0');
  for (size_t i = 0; i < 64; ++i) {
    ipad[i] = static_cast<char>(k[i] ^ 0x36);
    opad[i] = static_cast<char>(k[i] ^ 0x5c);
  }
  Sha256 inner;
  inner.Update(ipad);
  inner.Update(message);
  std::string inner_hash = inner.Final();
  Sha256 outer;
  outer.Update(opad);
  outer.Update(inner_hash);
  return outer.Final();
}

bool LoadOrCreateNotificationClickKey(const std::string& dir,
                                      std::string* key) {
  if (dir.empty() || !EnsureDirectory(dir))
    return false;
  std::string path = JoinPath(dir, kNotificationClickKeyFile);
  std::string text;
  ReadResult read = ReadKeyFile(path, &text);
  if (read == ReadResult::kRead && ParseKeyFile(text, key))
    return true;
#ifndef _WIN32
  struct stat st;
  if (read != ReadResult::kMissing &&
      (lstat(path.c_str(), &st) != 0 ||
       (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))))
    return false;  // a directory or device there: leave it alone
#endif
  // A missing key is created without replacing anything, so of two
  // processes starting together one wins and the other reads its key. One
  // that is damaged, a link, or (Unix) readable by others is replaced.
  std::string fresh, suffix;
  if (!RandomBytes(&fresh, kNotificationClickKeyBytes) ||
      !RandomBytes(&suffix, 6))
    return false;
  std::string tmp = path + ".tmp-" + Hex(suffix);
  PublishFile(tmp, path, Hex(fresh) + "\n",
              /*replace=*/read != ReadResult::kMissing);
  // Whatever is there now (ours, or the winner's of a race) is the key.
  text.clear();
  if (ReadKeyFile(path, &text) == ReadResult::kRead && ParseKeyFile(text, key))
    return true;
  return false;
}

const std::string& NotificationClickKey() {
  std::lock_guard<std::mutex> lock(g_key_mutex);
  if (!g_key) {
    std::string key;
    if (!LoadOrCreateNotificationClickKey(AppDataDir(), &key)) {
      // No app data directory (no app id): a key for this process only. A
      // click that reaches another process (a cold start, a scheduled
      // notification clicked after a restart) can't be verified there and
      // is dropped.
      if (!RandomBytes(&key, kNotificationClickKeyBytes)) {
        // No entropy at all: nothing can be verified, so nothing is.
        key.clear();
      }
      if (NotificationDebug()) {
        std::cerr << "laufey: notification clicks: no app data directory; "
                     "using a key for this process only"
                  << std::endl;
      }
    }
    g_key = new std::string(key);
  }
  return *g_key;
}

void SetNotificationClickKeyForTesting(const std::string& key) {
  std::lock_guard<std::mutex> lock(g_key_mutex);
  delete g_key;
  g_key = new std::string(key);
}

std::string SignToastArguments(const std::string& key,
                               const std::string& args) {
  return args + kMacField + MacOf(key, args);
}

bool VerifyToastArguments(const std::string& key, const std::string& args) {
  if (key.empty() || args.size() > kMaxNotificationClickArgumentsBytes)
    return false;
  size_t at = args.rfind(kMacField);
  if (at == std::string::npos)
    return false;
  std::string mac = args.substr(at + sizeof(kMacField) - 1);
  std::string expected = MacOf(key, args.substr(0, at));
  if (mac.size() != expected.size())
    return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < mac.size(); ++i)
    diff |= static_cast<unsigned char>(mac[i] ^ expected[i]);
  return diff == 0;
}

bool DecodeClickArguments(const std::string& args, std::string* tag,
                          std::string* action, bool* has_action,
                          std::string* data, bool* has_data) {
  const char* why = nullptr;
  if (args.size() > kMaxNotificationClickArgumentsBytes)
    why = "too long";
  else if (!VerifyToastArguments(NotificationClickKey(), args))
    why = "no valid MAC (not posted by this install)";
  else if (!DecodeToastArguments(args, tag, action, has_action, data, has_data))
    why = "malformed";
  else if (tag->size() > LAUFEY_NOTIFICATION_MAX_TAG_BYTES ||
           (*has_data && data->size() > LAUFEY_NOTIFICATION_MAX_DATA_BYTES) ||
           (*has_action && action->size() > kMaxNotificationActionBytes))
    why = "a field is over its limit";
  if (!why)
    return true;
  if (NotificationDebug()) {
    std::cerr << "laufey: dropped a notification click: " << why << " ("
              << args.size() << " bytes)" << std::endl;
  }
  return false;
}

}  // namespace laufey_common
