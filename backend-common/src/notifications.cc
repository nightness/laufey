// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable notification core (API 41). See laufey_notifications.h.

#include "laufey_notifications.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <utility>

#include "json_reader.h"

namespace laufey_common {

namespace {

struct Live {
  uint32_t id = 0;
  std::string tag;
  laufey_notification_event_fn on_event = nullptr;
  void* user_data = nullptr;
  bool has_data = false;
  std::string data;
};

std::mutex g_mutex;
// Never destroyed: a static destructor would release the platform's OS
// objects (WinRT toasts, the D-Bus connection) during process exit, after
// the threads they belong to are gone, which hangs ExitProcess on Windows.
NotificationPlatform* g_platform = nullptr;
bool g_platform_created = false;
std::map<uint32_t, Live> g_live;           // by id
std::map<std::string, uint32_t> g_by_tag;  // tag -> id
// Data of notifications this process showed, by tag, for clicks that arrive
// after the live callback is gone (the OS doesn't always keep it).
std::map<std::string, std::string> g_data_by_tag;
std::atomic<uint32_t> g_next_id{1};

laufey_notification_response_fn g_response_fn = nullptr;
void* g_response_data = nullptr;
std::deque<std::string> g_pending_responses;  // JSON, oldest first

NotificationPlatform* PlatformLocked() {
  if (!g_platform && !g_platform_created) {
    g_platform_created = true;
    g_platform = CreateNotificationPlatform().release();
  }
  return g_platform;
}

NotificationPlatform* Platform() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return PlatformLocked();
}

// Drops the live entry for `id`. Caller holds g_mutex.
void ForgetLocked(uint32_t id) {
  auto it = g_live.find(id);
  if (it == g_live.end())
    return;
  auto t = g_by_tag.find(it->second.tag);
  if (t != g_by_tag.end() && t->second == id)
    g_by_tag.erase(t);
  g_live.erase(it);
}

bool IsSafeUtf8(const std::string& s) {
  // Rejects NUL bytes; the rest is the OS's business.
  return s.find('\0') == std::string::npos;
}

void AppendHex4(std::string* out, unsigned v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "\\u%04x", v);
  *out += buf;
}

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Value(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

std::string PercentEncode(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

bool PercentDecode(const std::string& s, std::string* out) {
  out->clear();
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '%') {
      *out += s[i];
      continue;
    }
    if (i + 2 >= s.size())
      return false;
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
    if (hi < 0 || lo < 0)
      return false;
    *out += static_cast<char>(hi * 16 + lo);
    i += 2;
  }
  return true;
}

const json::JsonValue* Field(const json::JsonValue& obj, const char* name) {
  for (const auto& [k, v] : obj.object) {
    if (k == name)
      return &v;
  }
  return nullptr;
}

std::string StringField(const json::JsonValue& obj, const char* name) {
  const json::JsonValue* v = Field(obj, name);
  return v && v->type == json::JsonValue::Type::kString ? v->string
                                                        : std::string();
}

}  // namespace

// --- Helpers -----------------------------------------------------------------

int64_t UnixTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string JsonQuote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20)
          AppendHex4(&out, c);
        else
          out += static_cast<char>(c);
    }
  }
  out += '"';
  return out;
}

std::string Base64Encode(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += kB64[n & 63];
  }
  if (i + 1 == bytes.size()) {
    uint32_t n = bytes[i] << 16;
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == bytes.size()) {
    uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

bool Base64Decode(const std::string& text, std::vector<uint8_t>* out) {
  out->clear();
  if (text.size() % 4 != 0)
    return false;
  for (size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    for (int k = 0; k < 4; ++k) {
      char c = text[i + k];
      if (c == '=') {
        // Padding only in the last group's last one or two places.
        if (i + 4 != text.size() || k < 2 || (k == 2 && text[i + 3] != '='))
          return false;
        v[k] = 0;
      } else {
        v[k] = B64Value(c);
        if (v[k] < 0)
          return false;
      }
    }
    uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out->push_back(static_cast<uint8_t>(n >> 16));
    if (text[i + 2] != '=')
      out->push_back(static_cast<uint8_t>(n >> 8));
    if (text[i + 3] != '=')
      out->push_back(static_cast<uint8_t>(n));
  }
  return true;
}

bool ValidateNotificationOptions(const NotificationOptions& opts,
                                 std::string* error) {
  auto fail = [&](const char* why) {
    if (error)
      *error = why;
    return false;
  };
  if (opts.title.empty())
    return fail("a notification needs a title");
  if (opts.tag.size() > LAUFEY_NOTIFICATION_MAX_TAG_BYTES)
    return fail("the tag is too long");
  if (opts.has_data && opts.data.size() > LAUFEY_NOTIFICATION_MAX_DATA_BYTES)
    return fail("the data is too long");
  if (!IsSafeUtf8(opts.tag) || !IsSafeUtf8(opts.data) ||
      !IsSafeUtf8(opts.title) || !IsSafeUtf8(opts.body))
    return fail("a string contains a NUL byte");
  if (opts.schedule_at_ms > 0 && opts.tag.empty())
    return fail("a scheduled notification needs a tag");
  std::set<std::string> ids;
  for (const NotificationAction& a : opts.actions) {
    if (!ids.insert(a.id).second)
      return fail("two actions share an id");
    if (!IsSafeUtf8(a.id) || !IsSafeUtf8(a.title))
      return fail("an action contains a NUL byte");
  }
  return true;
}

std::string GenerateNotificationTag(uint32_t notification_id) {
  static const uint64_t run = [] {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^
           static_cast<uint64_t>(UnixTimeMs());
  }();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "laufey-%016llx-%u",
                static_cast<unsigned long long>(run), notification_id);
  return buf;
}

std::string BuildNotificationResponseJson(const std::string& tag,
                                          const char* action,
                                          const std::string* data,
                                          bool launch) {
  std::string out = "{\"tag\":" + JsonQuote(tag) + ",\"action\":";
  out += action ? JsonQuote(action) : "null";
  out += ",\"data\":";
  out += data ? JsonQuote(*data) : "null";
  out += ",\"launch\":";
  out += launch ? "true" : "false";
  out += "}";
  return out;
}

std::string BuildScheduledListJson(std::vector<ScheduledNotification> list) {
  std::stable_sort(
      list.begin(), list.end(),
      [](const ScheduledNotification& a, const ScheduledNotification& b) {
        return a.at_ms < b.at_ms;
      });
  std::string out = "[";
  bool first = true;
  for (const ScheduledNotification& n : list) {
    if (!first)
      out += ",";
    first = false;
    out += "{\"tag\":" + JsonQuote(n.tag) + ",\"title\":" + JsonQuote(n.title) +
           ",\"body\":" + JsonQuote(n.body) +
           ",\"at\":" + std::to_string(n.at_ms) + ",\"data\":";
    out += n.has_data ? JsonQuote(n.data) : "null";
    out += ",\"actions\":[";
    for (size_t i = 0; i < n.actions.size(); ++i) {
      if (i)
        out += ",";
      out += "{\"id\":" + JsonQuote(n.actions[i].id) +
             ",\"title\":" + JsonQuote(n.actions[i].title) + "}";
    }
    out += "]}";
  }
  out += "]";
  return out;
}

std::string SerializeSchedule(const std::vector<ScheduledNotification>& list) {
  std::string out = "{\"version\":1,\"notifications\":[";
  for (size_t i = 0; i < list.size(); ++i) {
    const ScheduledNotification& n = list[i];
    if (i)
      out += ",";
    out +=
        "\n{\"tag\":" + JsonQuote(n.tag) + ",\"title\":" + JsonQuote(n.title) +
        ",\"body\":" + JsonQuote(n.body) + ",\"at\":" + std::to_string(n.at_ms);
    if (n.has_data)
      out += ",\"data\":" + JsonQuote(n.data);
    out += ",\"silent\":";
    out += n.silent ? "true" : "false";
    out += ",\"requireInteraction\":";
    out += n.require_interaction ? "true" : "false";
    if (!n.icon_png.empty())
      out += ",\"icon\":\"" + Base64Encode(n.icon_png) + "\"";
    out += ",\"actions\":[";
    for (size_t k = 0; k < n.actions.size(); ++k) {
      if (k)
        out += ",";
      out += "{\"id\":" + JsonQuote(n.actions[k].id) +
             ",\"title\":" + JsonQuote(n.actions[k].title) + "}";
    }
    out += "]}";
  }
  out += "\n]}\n";
  return out;
}

bool ParseSchedule(const std::string& text,
                   std::vector<ScheduledNotification>* out,
                   std::string* error) {
  out->clear();
  json::JsonValue root;
  std::string err;
  if (!json::JsonReader(text).ParseDocument(&root, &err)) {
    if (error)
      *error = err;
    return false;
  }
  using T = json::JsonValue::Type;
  if (root.type != T::kObject) {
    if (error)
      *error = "not an object";
    return false;
  }
  const json::JsonValue* version = Field(root, "version");
  if (!version || version->type != T::kNumber || !version->is_integer ||
      version->integer != 1) {
    if (error)
      *error = "unknown version";
    return false;
  }
  const json::JsonValue* list = Field(root, "notifications");
  if (!list || list->type != T::kArray) {
    if (error)
      *error = "no notifications array";
    return false;
  }
  for (const json::JsonValue& item : list->array) {
    if (item.type != T::kObject)
      continue;
    ScheduledNotification n;
    n.tag = StringField(item, "tag");
    n.title = StringField(item, "title");
    n.body = StringField(item, "body");
    const json::JsonValue* at = Field(item, "at");
    if (n.tag.empty() || n.title.empty() || !at || at->type != T::kNumber ||
        !at->is_integer)
      continue;  // a damaged entry is dropped, the rest survive
    n.at_ms = at->integer;
    if (const json::JsonValue* d = Field(item, "data")) {
      if (d->type == T::kString) {
        n.has_data = true;
        n.data = d->string;
      }
    }
    if (const json::JsonValue* s = Field(item, "silent"))
      n.silent = s->type == T::kBool && s->boolean;
    if (const json::JsonValue* r = Field(item, "requireInteraction"))
      n.require_interaction = r->type == T::kBool && r->boolean;
    std::string icon = StringField(item, "icon");
    if (!icon.empty())
      Base64Decode(icon, &n.icon_png);
    if (const json::JsonValue* acts = Field(item, "actions")) {
      if (acts->type == T::kArray) {
        for (const json::JsonValue& a : acts->array) {
          if (a.type != T::kObject)
            continue;
          NotificationAction act{StringField(a, "id"), StringField(a, "title")};
          if (!act.id.empty() && !act.title.empty())
            n.actions.push_back(std::move(act));
        }
      }
    }
    out->push_back(std::move(n));
  }
  return true;
}

std::string EncodeToastArguments(const std::string& tag, const char* action,
                                 const std::string* data) {
  std::string out = "laufey=1&tag=" + PercentEncode(tag);
  if (action)
    out += "&action=" + PercentEncode(action);
  if (data)
    out += "&data=" + PercentEncode(*data);
  return out;
}

bool DecodeToastArguments(const std::string& args, std::string* tag,
                          std::string* action, bool* has_action,
                          std::string* data, bool* has_data) {
  *has_action = false;
  *has_data = false;
  tag->clear();
  bool ours = false, has_tag = false;
  size_t pos = 0;
  while (pos <= args.size()) {
    size_t amp = args.find('&', pos);
    std::string part = args.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = part.find('=');
    if (eq != std::string::npos) {
      std::string key = part.substr(0, eq);
      std::string value;
      if (!PercentDecode(part.substr(eq + 1), &value))
        return false;
      if (key == "laufey") {
        ours = value == "1";
      } else if (key == "tag") {
        *tag = value;
        has_tag = true;
      } else if (key == "action") {
        *action = value;
        *has_action = true;
      } else if (key == "data") {
        *data = value;
        *has_data = true;
      }
    }
    if (amp == std::string::npos)
      break;
    pos = amp + 1;
  }
  return ours && has_tag && !tag->empty();
}

// --- Entry points
// ----------------------------------------------------------------

void InstallNotificationPlatform(std::unique_ptr<NotificationPlatform> p) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // The previous one is leaked on purpose (see the header).
  g_platform = p.release();
  g_platform_created = true;
}

NotificationPlatform* EnsureNotificationPlatform() {
  return Platform();
}

uint32_t ShowNotification(const NotificationOptions& in,
                          laufey_notification_event_fn on_event,
                          void* user_data) {
  std::string error;
  if (!ValidateNotificationOptions(in, &error)) {
    std::cerr << "laufey: show_notification: " << error << std::endl;
    return 0;
  }
  NotificationPlatform* platform = Platform();
  if (!platform)
    return 0;
  uint32_t id = g_next_id.fetch_add(1, std::memory_order_relaxed);
  NotificationOptions opts = in;
  if (opts.tag.empty())
    opts.tag = GenerateNotificationTag(id);
  if (opts.schedule_at_ms > 0 && opts.schedule_at_ms <= UnixTimeMs())
    opts.schedule_at_ms = 0;  // in the past: now
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // A notification replacing another with the same tag takes its place
    // (Web Notifications semantics): the old one gets no more events.
    auto prev = g_by_tag.find(opts.tag);
    if (prev != g_by_tag.end())
      ForgetLocked(prev->second);
    Live live;
    live.id = id;
    live.tag = opts.tag;
    live.on_event = on_event;
    live.user_data = user_data;
    live.has_data = opts.has_data;
    live.data = opts.data;
    g_live[id] = live;
    g_by_tag[opts.tag] = id;
    // A notification the OS never reports closed (a clicked banner on macOS,
    // one the user leaves in the notification center) stays tracked; keep
    // the newest 1024.
    while (g_live.size() > 1024)
      ForgetLocked(g_live.begin()->first);
    if (opts.has_data)
      g_data_by_tag[opts.tag] = opts.data;
    else
      g_data_by_tag.erase(opts.tag);
    // Bounded: the oldest recorded data goes first.
    while (g_data_by_tag.size() > 1024)
      g_data_by_tag.erase(g_data_by_tag.begin());
  }
  if (!platform->Show(opts)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ForgetLocked(id);
    return 0;
  }
  return id;
}

void CloseNotification(uint32_t notification_id) {
  std::string tag;
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_live.find(notification_id);
    if (it == g_live.end())
      return;
    tag = it->second.tag;
    fn = it->second.on_event;
    ud = it->second.user_data;
    ForgetLocked(notification_id);
  }
  if (NotificationPlatform* platform = Platform())
    platform->Remove(tag);
  if (fn)
    fn(ud, notification_id, LAUFEY_NOTIFICATION_CLOSED, nullptr);
}

uint32_t NotificationCapabilities() {
  NotificationPlatform* platform = Platform();
  return platform ? platform->Capabilities() : 0;
}

void SetNotificationResponseHandler(laufey_notification_response_fn handler,
                                    void* user_data) {
  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_response_fn = handler;
    g_response_data = user_data;
    if (handler) {
      pending.swap(g_pending_responses);
    }
  }
  // Outside the lock, on the registering thread (like the open-url buffer).
  for (const std::string& json : pending)
    handler(user_data, json.c_str());
}

void ListScheduledNotifications(laufey_notification_list_fn callback,
                                void* user_data) {
  if (!callback)
    return;
  NotificationPlatform* platform = Platform();
  if (!platform) {
    callback(user_data, "[]");
    return;
  }
  platform->ListScheduled(
      [callback, user_data](std::vector<ScheduledNotification> list) {
        std::string json = BuildScheduledListJson(std::move(list));
        callback(user_data, json.c_str());
      });
}

void CancelNotification(const char* tag_c) {
  if (!tag_c || !*tag_c)
    return;
  std::string tag(tag_c);
  uint32_t id = 0;
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto t = g_by_tag.find(tag);
    if (t != g_by_tag.end()) {
      id = t->second;
      auto it = g_live.find(id);
      if (it != g_live.end()) {
        fn = it->second.on_event;
        ud = it->second.user_data;
      }
      ForgetLocked(id);
    }
  }
  if (NotificationPlatform* platform = Platform())
    platform->Remove(tag);
  if (fn)
    fn(ud, id, LAUFEY_NOTIFICATION_CLOSED, nullptr);
}

bool TestNotificationRespond(const char* tag, const char* action_id) {
  if (!tag || !*tag)
    return false;
  return DispatchNotificationClick(tag, action_id, nullptr);
}

void QueryNotificationPermission(int kind, laufey_permission_callback_fn cb,
                                 void* user_data) {
  if (!cb)
    return;
  NotificationPlatform* platform = Platform();
  if (!platform || (kind != LAUFEY_PERMISSION_NOTIFICATIONS &&
                    kind != LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL)) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  platform->QueryPermission(
      kind, [cb, user_data](int status) { cb(user_data, status); });
}

void RequestNotificationPermission(int kind, laufey_permission_callback_fn cb,
                                   void* user_data) {
  if (!cb)
    return;
  NotificationPlatform* platform = Platform();
  if (!platform || (kind != LAUFEY_PERMISSION_NOTIFICATIONS &&
                    kind != LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL)) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  platform->RequestPermission(
      kind, [cb, user_data](int status) { cb(user_data, status); });
}

// --- From the platform
// ------------------------------------------------------------

void DispatchNotificationShown(const std::string& tag) {
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto t = g_by_tag.find(tag);
    if (t == g_by_tag.end())
      return;
    id = t->second;
    auto it = g_live.find(id);
    if (it == g_live.end())
      return;
    fn = it->second.on_event;
    ud = it->second.user_data;
  }
  if (fn)
    fn(ud, id, LAUFEY_NOTIFICATION_SHOWN, nullptr);
}

bool DispatchNotificationClick(const std::string& tag, const char* action,
                               const std::string* data, bool launch) {
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  uint32_t id = 0;
  laufey_notification_response_fn rfn = nullptr;
  void* rud = nullptr;
  std::string json;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto t = g_by_tag.find(tag);
    if (t != g_by_tag.end()) {
      auto it = g_live.find(t->second);
      if (it != g_live.end() && it->second.on_event) {
        id = it->second.id;
        fn = it->second.on_event;
        ud = it->second.user_data;
      }
    }
    if (!fn) {
      std::string recorded;
      const std::string* d = data;
      if (!d) {
        auto r = g_data_by_tag.find(tag);
        if (r != g_data_by_tag.end()) {
          recorded = r->second;
          d = &recorded;
        }
      }
      rfn = g_response_fn;
      rud = g_response_data;
      json = BuildNotificationResponseJson(tag, action, d,
                                           launch || rfn == nullptr);
      if (!rfn) {
        if (g_pending_responses.size() >=
            LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES)
          g_pending_responses.pop_front();
        g_pending_responses.push_back(json);
      }
    }
  }
  if (fn) {
    fn(ud, id,
       action ? LAUFEY_NOTIFICATION_ACTION : LAUFEY_NOTIFICATION_CLICKED,
       action);
    return true;
  }
  if (rfn) {
    rfn(rud, json.c_str());
    return true;
  }
  return false;
}

void DispatchNotificationClosed(const std::string& tag) {
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto t = g_by_tag.find(tag);
    if (t == g_by_tag.end())
      return;
    id = t->second;
    auto it = g_live.find(id);
    if (it != g_live.end()) {
      fn = it->second.on_event;
      ud = it->second.user_data;
    }
    ForgetLocked(id);
  }
  if (fn)
    fn(ud, id, LAUFEY_NOTIFICATION_CLOSED, nullptr);
}

void ResetNotificationsForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_live.clear();
  g_by_tag.clear();
  g_data_by_tag.clear();
  g_pending_responses.clear();
  g_response_fn = nullptr;
  g_response_data = nullptr;
}

}  // namespace laufey_common
