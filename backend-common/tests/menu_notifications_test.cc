// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the portable parts of API 41: menu template parsing with
// accelerators (laufey_menu.h), the accelerator lookup and key-code helpers,
// the context-menu session (exactly-once close, the dismiss hook), and the
// notification core (laufey_notifications.h) over a fake platform: option
// validation, generated tags, live routing of shown / clicks / actions /
// closes, responses for notifications no live callback owns, the response
// buffer and its "launch" flag, cancel, list JSON, the Linux schedule file,
// Windows toast arguments and base64; the native modal loop hook (one
// enter / leave pair per outermost loop, never off the UI thread). Plain
// asserts, no framework.

#include "laufey_menu.h"
#include "laufey_notifications.h"
#include "laufey_ui_tasks.h"
#include "laufey_value.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

laufey_backend_api_t g_api;

laufey_value_t* Str(const char* s) {
  return g_api.value_string(nullptr, s);
}

laufey_value_t* Item(const char* label, const char* id, const char* accel,
                     bool enabled = true) {
  laufey_value_t* d = g_api.value_dict(nullptr);
  laufey_value_t* v = Str(label);
  g_api.value_dict_set(d, "label", v);
  g_api.value_free(v);
  if (id) {
    v = Str(id);
    g_api.value_dict_set(d, "id", v);
    g_api.value_free(v);
  }
  if (accel) {
    v = Str(accel);
    g_api.value_dict_set(d, "accelerator", v);
    g_api.value_free(v);
  }
  if (!enabled) {
    v = g_api.value_bool(nullptr, false);
    g_api.value_dict_set(d, "enabled", v);
    g_api.value_free(v);
  }
  return d;
}

void Append(laufey_value_t* list, laufey_value_t* v) {
  g_api.value_list_append(list, v);
  g_api.value_free(v);
}

void TestParseMenu() {
  laufey_value_t* root = g_api.value_list(nullptr);
  laufey_value_t* file = g_api.value_dict(nullptr);
  laufey_value_t* v = Str("File");
  g_api.value_dict_set(file, "label", v);
  g_api.value_free(v);
  laufey_value_t* items = g_api.value_list(nullptr);
  Append(items, Item("Open", "open", "CommandOrControl+O"));
  Append(items, Item("Save", "save", "Ctrl+S", false));
  Append(items, Item("Bad", "bad", "Ctrl+Nope"));
  Append(items, Item("Dup", "dup", "Ctrl+O"));
  laufey_value_t* sep = g_api.value_dict(nullptr);
  v = Str("separator");
  g_api.value_dict_set(sep, "type", v);
  g_api.value_free(v);
  Append(items, sep);
  laufey_value_t* role = g_api.value_dict(nullptr);
  v = Str("selectAll");
  g_api.value_dict_set(role, "role", v);
  g_api.value_free(v);
  Append(items, role);
  g_api.value_dict_set(file, "submenu", items);
  g_api.value_free(items);
  Append(root, file);
  Append(root, Item("Help", nullptr, "F1"));

  std::vector<MenuEntry> entries = ParseMenuTemplate(root, &g_api, false);
  g_api.value_free(root);
  EXPECT(entries.size() == 2);
  EXPECT(entries[0].kind == MenuEntry::Kind::kSubmenu);
  EXPECT(entries[0].label == "File");
  const auto& kids = entries[0].children;
  EXPECT(kids.size() == 6);
  EXPECT(kids[0].id == "open" && kids[0].has_accel);
  EXPECT(CanonicalAccelerator(kids[0].accel) == "Ctrl+O");
  EXPECT(!kids[1].enabled && kids[1].has_accel);
  EXPECT(!kids[2].has_accel && kids[2].accelerator_text == "Ctrl+Nope");
  EXPECT(kids[4].kind == MenuEntry::Kind::kSeparator);
  EXPECT(kids[5].kind == MenuEntry::Kind::kRole &&
         RoleIs(kids[5].role, "selectall"));
  EXPECT(entries[1].ClickKey() == "Help");  // no id: the label

  // Bindings: enabled items with a parsed accelerator; the first of two
  // items on one combination wins.
  std::vector<MenuAccelBinding> b = CollectMenuAccelerators(entries);
  EXPECT(b.size() == 2);
  EXPECT(b[0].click_key == "open");
  EXPECT(b[1].click_key == "Help");
  Accelerator a;
  EXPECT(ParseAccelerator("ctrl+o", false, &a, nullptr));
  const MenuAccelBinding* found = FindMenuAccelerator(b, a);
  EXPECT(found && found->click_key == "open");
  EXPECT(ParseAccelerator("Ctrl+S", false, &a, nullptr));
  EXPECT(!FindMenuAccelerator(b, a));  // disabled

  // macOS resolves CommandOrControl to Command.
  root = g_api.value_list(nullptr);
  Append(root, Item("Open", "open", "CommandOrControl+O"));
  entries = ParseMenuTemplate(root, &g_api, true);
  g_api.value_free(root);
  EXPECT(entries.size() == 1 && entries[0].has_accel);
  EXPECT(CanonicalAccelerator(entries[0].accel) == "Super+O");

  EXPECT(ParseMenuTemplate(nullptr, &g_api, false).empty());
}

void TestKeyCodes() {
  Accelerator a;
  EXPECT(ParseAccelerator("Ctrl+K", false, &a, nullptr));
  EXPECT(AcceleratorUsVirtualKey(a) == 'K');
  EXPECT(ParseAccelerator("F5", false, &a, nullptr));
  EXPECT(AcceleratorUsVirtualKey(a) == 0x74);
  EXPECT(ParseAccelerator("Ctrl+-", false, &a, nullptr));
  EXPECT(AcceleratorUsVirtualKey(a) == 0xBD);
  EXPECT(ParseAccelerator("Ctrl+Num7", false, &a, nullptr));
  EXPECT(AcceleratorUsVirtualKey(a) == 0x67);
  EXPECT(ParseAccelerator("Alt+PageDown", false, &a, nullptr));
  EXPECT(AcceleratorUsVirtualKey(a) == 0x22);
  EXPECT(ParseAccelerator("Ctrl+Shift+Super+K", false, &a, nullptr));
#ifdef _WIN32
  EXPECT(AcceleratorDisplayText(a) == "Ctrl+Shift+Win+K");
#else
  EXPECT(AcceleratorDisplayText(a) == "Ctrl+Shift+Super+K");
#endif
}

int g_closed = 0;
uint32_t g_closed_window = 0;
void OnClosed(void* data, uint32_t window_id) {
  g_closed += *static_cast<int*>(data);
  g_closed_window = window_id;
}

void TestContextMenuSession() {
  int one = 1;
  int dismissed = 0;
  EXPECT(!DismissOpenContextMenu());
  uint64_t s = BeginContextMenu(7, OnClosed, &one, [&] { dismissed++; });
  EXPECT(DismissOpenContextMenu());
  EXPECT(dismissed == 1);
  EXPECT(g_closed == 0);  // dismissing asks; the backend ends the session
  EndContextMenu(s);
  EXPECT(g_closed == 1 && g_closed_window == 7);
  EndContextMenu(s);  // exactly once
  EXPECT(g_closed == 1);
  EXPECT(!DismissOpenContextMenu());
  FireContextMenuClosedNow(3, OnClosed, &one);
  EXPECT(g_closed == 2 && g_closed_window == 3);
  FireContextMenuClosedNow(3, nullptr, nullptr);  // no callback: fine
}

// --- Notifications ---

struct FakePlatform : NotificationPlatform {
  std::vector<NotificationOptions> shown;
  std::vector<std::string> removed;
  std::vector<ScheduledNotification> pending;
  bool fail = false;
  uint32_t Capabilities() override {
    return LAUFEY_NOTIFICATION_CAP_SHOW;
  }
  bool Show(const NotificationOptions& o) override {
    if (fail)
      return false;
    shown.push_back(o);
    return true;
  }
  void Remove(const std::string& tag) override {
    removed.push_back(tag);
  }
  void ListScheduled(
      std::function<void(std::vector<ScheduledNotification>)> done) override {
    done(pending);
  }
  void QueryPermission(int, std::function<void(int)> done) override {
    done(LAUFEY_PERMISSION_STATUS_GRANTED);
  }
  void RequestPermission(int, std::function<void(int)> done) override {
    done(LAUFEY_PERMISSION_STATUS_DENIED);
  }
};

struct Event {
  uint32_t id;
  int reason;
  std::string action;
};
std::vector<Event> g_events;
void OnEvent(void*, uint32_t id, int reason, const char* action) {
  g_events.push_back({id, reason, action ? action : ""});
}

std::vector<std::string> g_responses;
void OnResponse(void*, const char* json) {
  g_responses.push_back(json);
}

std::string g_list;
void OnList(void*, const char* json) {
  g_list = json;
}

int g_perm = -1;
void OnPerm(void*, int status) {
  g_perm = status;
}

NotificationOptions Opts(const char* title, const char* tag = "") {
  NotificationOptions o;
  o.title = title;
  o.tag = tag;
  return o;
}

void TestValidate() {
  std::string err;
  EXPECT(ValidateNotificationOptions(Opts("t"), &err));
  EXPECT(!ValidateNotificationOptions(Opts(""), &err));
  NotificationOptions o = Opts("t");
  o.schedule_at_ms = UnixTimeMs() + 1000;
  EXPECT(!ValidateNotificationOptions(o, &err));  // needs a tag
  o.tag = "x";
  EXPECT(ValidateNotificationOptions(o, &err));
  o.tag.assign(LAUFEY_NOTIFICATION_MAX_TAG_BYTES + 1, 'a');
  EXPECT(!ValidateNotificationOptions(o, &err));
  o = Opts("t");
  o.has_data = true;
  o.data.assign(LAUFEY_NOTIFICATION_MAX_DATA_BYTES + 1, 'a');
  EXPECT(!ValidateNotificationOptions(o, &err));
  o = Opts("t");
  o.actions = {{"a", "A"}, {"a", "B"}};
  EXPECT(!ValidateNotificationOptions(o, &err));
  EXPECT(GenerateNotificationTag(1) != GenerateNotificationTag(2));
  EXPECT(GenerateNotificationTag(1).rfind("laufey-", 0) == 0);
}

void TestRouting() {
  ResetNotificationsForTest();
  auto* fake = new FakePlatform();
  InstallNotificationPlatform(std::unique_ptr<NotificationPlatform>(fake));
  EXPECT(NotificationCapabilities() == LAUFEY_NOTIFICATION_CAP_SHOW);

  NotificationOptions o = Opts("Hello", "greet");
  o.has_data = true;
  o.data = "{\"k\":1}";
  uint32_t id = ShowNotification(o, OnEvent, nullptr);
  EXPECT(id > 0);
  EXPECT(fake->shown.size() == 1 && fake->shown[0].tag == "greet");

  // An untagged one gets a generated tag.
  uint32_t id2 = ShowNotification(Opts("Second"), OnEvent, nullptr);
  EXPECT(id2 > id && fake->shown[1].tag.rfind("laufey-", 0) == 0);

  g_events.clear();
  DispatchNotificationShown("greet");
  EXPECT(TestNotificationRespond("greet", nullptr));
  EXPECT(TestNotificationRespond("greet", "reply"));
  EXPECT(g_events.size() == 3);
  EXPECT(g_events[0].id == id &&
         g_events[0].reason == LAUFEY_NOTIFICATION_SHOWN);
  EXPECT(g_events[1].reason == LAUFEY_NOTIFICATION_CLICKED);
  EXPECT(g_events[2].reason == LAUFEY_NOTIFICATION_ACTION &&
         g_events[2].action == "reply");

  // A replacement with the same tag takes the old one's place.
  uint32_t id3 = ShowNotification(Opts("Again", "greet"), OnEvent, nullptr);
  g_events.clear();
  DispatchNotificationClosed("greet");
  EXPECT(g_events.size() == 1 && g_events[0].id == id3 &&
         g_events[0].reason == LAUFEY_NOTIFICATION_CLOSED);

  // No live callback any more: the click is a response, buffered with
  // "launch": true until a handler comes, with the data this process
  // recorded for the tag ... (the replacement had none).
  g_events.clear();
  g_responses.clear();
  EXPECT(!TestNotificationRespond("greet", "open"));
  std::string recorded = "{\"x\":2}";
  EXPECT(!DispatchNotificationClick("old-tag", nullptr, &recorded));
  EXPECT(g_events.empty());
  SetNotificationResponseHandler(OnResponse, nullptr);
  EXPECT(g_responses.size() == 2);
  EXPECT(g_responses[0] ==
         "{\"tag\":\"greet\",\"action\":\"open\",\"data\":null,"
         "\"launch\":true}");
  EXPECT(g_responses[1] ==
         "{\"tag\":\"old-tag\",\"action\":null,\"data\":\"{\\\"x\\\":2}\","
         "\"launch\":true}");
  // With a handler: delivered at once, launch false.
  EXPECT(DispatchNotificationClick("old-tag", "a", nullptr));
  EXPECT(g_responses.size() == 3 &&
         g_responses[2] ==
             "{\"tag\":\"old-tag\",\"action\":\"a\",\"data\":null,"
             "\"launch\":false}");

  // The buffer keeps the newest LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES.
  SetNotificationResponseHandler(nullptr, nullptr);
  for (int i = 0; i < LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES + 4; ++i)
    DispatchNotificationClick("t" + std::to_string(i), nullptr, nullptr);
  g_responses.clear();
  SetNotificationResponseHandler(OnResponse, nullptr);
  EXPECT(g_responses.size() == LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES);
  EXPECT(g_responses[0].find("\"t4\"") != std::string::npos);

  // close_notification: the OS side removes it and CLOSED fires.
  g_events.clear();
  CloseNotification(id2);
  EXPECT(fake->removed.back() == fake->shown[1].tag);
  EXPECT(g_events.size() == 1 && g_events[0].id == id2 &&
         g_events[0].reason == LAUFEY_NOTIFICATION_CLOSED);
  CloseNotification(id2);  // unknown now: no-op
  EXPECT(g_events.size() == 1);

  // cancel_notification by tag: live -> CLOSED, and the OS removal.
  NotificationOptions s = Opts("Later", "later");
  s.schedule_at_ms = UnixTimeMs() + 60000;
  uint32_t sid = ShowNotification(s, OnEvent, nullptr);
  EXPECT(sid > 0 && fake->shown.back().schedule_at_ms == s.schedule_at_ms);
  g_events.clear();
  CancelNotification("later");
  EXPECT(fake->removed.back() == "later");
  EXPECT(g_events.size() == 1 && g_events[0].id == sid);
  CancelNotification("never-shown");  // still asks the OS
  EXPECT(fake->removed.back() == "never-shown");

  // A schedule in the past shows now.
  NotificationOptions past = Opts("Past", "past");
  past.schedule_at_ms = 1000;
  EXPECT(ShowNotification(past, nullptr, nullptr) > 0);
  EXPECT(fake->shown.back().schedule_at_ms == 0);

  // A failed show returns 0 and tracks nothing.
  fake->fail = true;
  EXPECT(ShowNotification(Opts("x", "failing"), OnEvent, nullptr) == 0);
  fake->fail = false;
  g_responses.clear();
  EXPECT(TestNotificationRespond("failing", nullptr));
  EXPECT(g_responses.size() == 1);  // to the handler, not a callback

  // Lists and permissions go through the platform.
  ScheduledNotification p1;
  p1.tag = "b";
  p1.title = "B";
  p1.at_ms = 200;
  ScheduledNotification p0;
  p0.tag = "a";
  p0.title = "A";
  p0.at_ms = 100;
  p0.has_data = true;
  p0.data = "d";
  p0.actions = {{"x", "X"}};
  fake->pending = {p1, p0};
  ListScheduledNotifications(OnList, nullptr);
  EXPECT(g_list ==
         "[{\"tag\":\"a\",\"title\":\"A\",\"body\":\"\",\"at\":100,"
         "\"data\":\"d\",\"actions\":[{\"id\":\"x\",\"title\":\"X\"}]},"
         "{\"tag\":\"b\",\"title\":\"B\",\"body\":\"\",\"at\":200,"
         "\"data\":null,\"actions\":[]}]");
  QueryNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS, OnPerm, nullptr);
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_GRANTED);
  RequestNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL,
                                OnPerm, nullptr);
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_DENIED);
  QueryNotificationPermission(99, OnPerm, nullptr);
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  SetNotificationResponseHandler(nullptr, nullptr);
}

void TestFormats() {
  EXPECT(JsonQuote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");

  std::vector<uint8_t> bytes = {0, 1, 2, 250, 251, 252, 253};
  std::string b64 = Base64Encode(bytes);
  EXPECT(b64 == "AAEC+vv8/Q==");
  std::vector<uint8_t> back;
  EXPECT(Base64Decode(b64, &back) && back == bytes);
  EXPECT(!Base64Decode("abc", &back));
  EXPECT(!Base64Decode("ab=c", &back));
  EXPECT(Base64Decode("", &back) && back.empty());

  ScheduledNotification n;
  n.tag = "t \"1\"";
  n.title = "Title";
  n.body = "Body\nline";
  n.at_ms = 1700000000123;
  n.has_data = true;
  n.data = "{\"a\":[1,2]}";
  n.actions = {{"ok", "OK"}, {"no", "Nope"}};
  n.silent = true;
  n.icon_png = {137, 80, 78, 71};
  ScheduledNotification m;
  m.tag = "u";
  m.title = "U";
  m.at_ms = 5;
  std::vector<ScheduledNotification> parsed;
  std::string err;
  EXPECT(ParseSchedule(SerializeSchedule({n, m}), &parsed, &err));
  EXPECT(parsed.size() == 2);
  EXPECT(parsed[0].tag == n.tag && parsed[0].title == n.title &&
         parsed[0].body == n.body && parsed[0].at_ms == n.at_ms &&
         parsed[0].has_data && parsed[0].data == n.data &&
         parsed[0].actions.size() == 2 &&
         parsed[0].actions[1].title == "Nope" && parsed[0].silent &&
         !parsed[0].require_interaction && parsed[0].icon_png == n.icon_png);
  EXPECT(!parsed[1].has_data && parsed[1].icon_png.empty());
  EXPECT(!ParseSchedule("{\"version\":2,\"notifications\":[]}", &parsed, &err));
  EXPECT(!ParseSchedule("not json", &parsed, &err));
  // A damaged entry is dropped, the others survive.
  EXPECT(
      ParseSchedule("{\"version\":1,\"notifications\":[{\"tag\":\"x\"},"
                    "{\"tag\":\"y\",\"title\":\"Y\",\"at\":3}]}",
                    &parsed, &err));
  EXPECT(parsed.size() == 1 && parsed[0].tag == "y");

  std::string data = "{\"q\":\"a&b=c%\"}";
  std::string args = EncodeToastArguments("my tag/1", "act ion", &data);
  std::string tag, action, d;
  bool has_action = false, has_data = false;
  EXPECT(DecodeToastArguments(args, &tag, &action, &has_action, &d, &has_data));
  EXPECT(tag == "my tag/1" && has_action && action == "act ion" && has_data &&
         d == data);
  args = EncodeToastArguments("t", nullptr, nullptr);
  EXPECT(DecodeToastArguments(args, &tag, &action, &has_action, &d, &has_data));
  EXPECT(tag == "t" && !has_action && !has_data);
  EXPECT(!DecodeToastArguments("tag=x", &tag, &action, &has_action, &d,
                               &has_data));  // not laufey's
  EXPECT(!DecodeToastArguments("laufey=1&tag=%zz", &tag, &action, &has_action,
                               &d, &has_data));
}

std::vector<bool> g_modal_hook_calls;

// ScopedNativeModalLoop tells the hook about the outermost loop on the UI
// thread only: nested loops are counted, and a modal run on another thread
// (an app dialog shown from the runtime's thread) never calls the hook, which
// is a UI-thread call (CefSetNestableTasksAllowed).
void TestNativeModalLoop() {
  SetNativeModalLoopHook(
      [](bool entering) { g_modal_hook_calls.push_back(entering); });
  // Before Bind there is no UI thread: nothing is reported.
  {
    ScopedNativeModalLoop loop;
  }
  EXPECT(g_modal_hook_calls.empty());

  UiTaskDispatcher::Get().Bind([](void (*)(void*), void*) { return false; });
  {
    ScopedNativeModalLoop outer;
    EXPECT(g_modal_hook_calls.size() == 1 && g_modal_hook_calls[0]);
    {
      ScopedNativeModalLoop inner;
      EXPECT(g_modal_hook_calls.size() == 1);
    }
    EXPECT(g_modal_hook_calls.size() == 1);
  }
  EXPECT(g_modal_hook_calls.size() == 2 && !g_modal_hook_calls[1]);

  std::thread other([] { ScopedNativeModalLoop loop; });
  other.join();
  EXPECT(g_modal_hook_calls.size() == 2);

  // A loop off the UI thread inside one on it changes nothing either.
  {
    ScopedNativeModalLoop outer;
    std::thread nested([] { ScopedNativeModalLoop loop; });
    nested.join();
    EXPECT(g_modal_hook_calls.size() == 3);
  }
  EXPECT(g_modal_hook_calls.size() == 4 && !g_modal_hook_calls[3]);
  SetNativeModalLoopHook(nullptr);
}

}  // namespace

int main() {
  laufey_register_value_api(&g_api);
  TestParseMenu();
  TestKeyCodes();
  TestContextMenuSession();
  TestValidate();
  TestRouting();
  TestFormats();
  TestNativeModalLoop();
  std::printf("menu_notifications_test: OK\n");
  return 0;
}
