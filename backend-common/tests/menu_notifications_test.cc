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
#include <deque>
#ifndef _WIN32
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <mutex>
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

// The test thread plays the UI thread: the dispatcher queues tasks here and
// PumpUi runs them.
std::mutex g_ui_mutex;
std::deque<std::pair<void (*)(void*), void*>> g_ui_queue;

void PumpUi() {
  for (;;) {
    std::pair<void (*)(void*), void*> task;
    {
      std::lock_guard<std::mutex> lock(g_ui_mutex);
      if (g_ui_queue.empty())
        return;
      task = g_ui_queue.front();
      g_ui_queue.pop_front();
    }
    task.first(task.second);
  }
}

// A platform that answers permission requests on a thread of its own, as
// the Windows toast thread and Linux's D-Bus replies do.
struct ThreadedPermissionPlatform : FakePlatform {
  void QueryPermission(int, std::function<void(int)> done) override {
    std::thread([done] { done(LAUFEY_PERMISSION_STATUS_GRANTED); }).join();
  }
  void RequestPermission(int, std::function<void(int)> done) override {
    std::thread([done] { done(LAUFEY_PERMISSION_STATUS_DENIED); }).join();
  }
};

std::thread::id g_perm_thread;
void OnPermThread(void*, int status) {
  g_perm = status;
  g_perm_thread = std::this_thread::get_id();
}

// laufey.h: permission callbacks fire on the UI thread, whichever thread the
// platform answered on.
void TestPermissionCallbacksOnUiThread() {
  InstallNotificationPlatform(std::make_unique<ThreadedPermissionPlatform>());
  g_perm = -1;
  QueryNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS, OnPermThread,
                              nullptr);
  EXPECT(g_perm == -1);  // queued for the UI thread, not called off it
  PumpUi();
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_GRANTED);
  EXPECT(g_perm_thread == std::this_thread::get_id());
  g_perm = -1;
  RequestNotificationPermission(LAUFEY_PERMISSION_NOTIFICATIONS, OnPermThread,
                                nullptr);
  PumpUi();
  EXPECT(g_perm == LAUFEY_PERMISSION_STATUS_DENIED);
  EXPECT(g_perm_thread == std::this_thread::get_id());
}

// The Linux activation and scheduled-launch helpers (portable).
void TestLinuxActivationHelpers() {
  std::string id;
  std::string tag_id = NotificationTagId("daily");
  EXPECT(tag_id.size() == 16);
  EXPECT(tag_id == NotificationTagId("daily"));
  EXPECT(tag_id != NotificationTagId("daily2"));
  EXPECT(ParseNotifyLaunch({"--laufey-notify", tag_id}, &id) && id == tag_id);
  // Only exactly that shape: no extra argument, a 16-hex id, the flag first.
  EXPECT(!ParseNotifyLaunch({"--laufey-notify"}, &id));
  EXPECT(!ParseNotifyLaunch({"--laufey-notify", tag_id, "x"}, &id));
  EXPECT(!ParseNotifyLaunch({"--laufey-notify", "daily"}, &id));
  EXPECT(!ParseNotifyLaunch({"--laufey-notify", "0123456789ABCDEF"}, &id));
  EXPECT(!ParseNotifyLaunch({"x", "--laufey-notify", tag_id}, &id));

  EXPECT(HasDBusActivationArg({"--laufey-dbus-activated"}));
  EXPECT(HasDBusActivationArg({"--foo", "--laufey-dbus-activated"}));
  // After "--" it is a positional argument (a link), not the marker.
  EXPECT(!HasDBusActivationArg({"--", "--laufey-dbus-activated"}));
  EXPECT(!HasDBusActivationArg({"acme://x"}));

  // The portal's registry refused: not running (XFCE / i3 under GDM, whose
  // portal unit needs graphical-session.target) is not "too old".
  {
    auto has = [](const std::string& s, const char* needle) {
      return s.find(needle) != std::string::npos;
    };
    std::string r = PortalRegistryFailureReason(
        "org.freedesktop.DBus.Error.NameHasNoOwner",
        "Could not activate remote peer 'org.freedesktop.portal.Desktop': "
        "activation request failed: a concurrent deactivation request is "
        "already in progress");
    EXPECT(has(r, "not running and D-Bus could not start it"));
    EXPECT(!has(r, "1.19"));
    EXPECT(has(r, "activation request failed"));
    EXPECT(has(PortalRegistryFailureReason(
                   "org.freedesktop.DBus.Error.ServiceUnknown", "x"),
               "not running"));
    EXPECT(has(PortalRegistryFailureReason(
                   "org.freedesktop.DBus.Error.Spawn.ChildExited", "x"),
               "not running"));
    EXPECT(has(PortalRegistryFailureReason("", ""), "not running"));
    EXPECT(has(PortalRegistryFailureReason("", ""), "no answer"));
    r = PortalRegistryFailureReason("org.freedesktop.DBus.Error.UnknownMethod",
                                    "No such interface");
    EXPECT(has(r, "1.19 or later"));
    EXPECT(has(PortalRegistryFailureReason(
                   "org.freedesktop.DBus.Error.UnknownInterface", "x"),
               "1.19 or later"));
    r = PortalRegistryFailureReason("org.freedesktop.DBus.Error.AccessDenied",
                                    "denied");
    EXPECT(has(r, "refused") && !has(r, "1.19") && !has(r, "not running"));
  }

  EXPECT(IsValidApplicationId("dev.denext.kitchen-sink"));
  EXPECT(IsValidApplicationId("org.example.App_2"));
  EXPECT(!IsValidApplicationId("app"));     // one element
  EXPECT(!IsValidApplicationId("dev..x"));  // an empty element
  EXPECT(!IsValidApplicationId(".dev.x"));
  EXPECT(!IsValidApplicationId("dev.x."));
  EXPECT(!IsValidApplicationId("dev.2x"));  // starts with a digit
  EXPECT(!IsValidApplicationId("dev.x y"));
  EXPECT(!IsValidApplicationId(""));
  EXPECT(!IsValidApplicationId("a." + std::string(260, 'b')));

  EXPECT(ApplicationObjectPath("dev.denext.kitchen-sink") ==
         "/dev/denext/kitchen_sink");

  EXPECT(NotificationTimerUnit("dev.denext.kitchen-sink", "daily") ==
         "laufey-dev.denext.kitchen-sink-" + tag_id);
  EXPECT(NotificationTimerUnit("a.b c/d", "t").rfind("laufey-a.b_c_d-", 0) ==
         0);
  // A long app id: cut, with 8 hex digits of its own hash, deterministic,
  // so the unit name (".service" the longer suffix) fits systemd's 255.
  {
    std::string long_id = "dev." + std::string(240, 'x');
    std::string part = NotificationTimerAppPart(long_id);
    EXPECT(part.size() == kTimerUnitAppIdMax);
    EXPECT(part == std::string("dev.") + std::string(kTimerUnitAppIdMax - 13,
                                                     'x') +
                       "_" + NotificationTagId(long_id).substr(0, 8));
    EXPECT(part == NotificationTimerAppPart(long_id));
    EXPECT(part != NotificationTimerAppPart(long_id + "y"));
    std::string unit = NotificationTimerUnit(long_id, "t") + ".service";
    EXPECT(unit.size() <= 255);
    EXPECT(NotificationTimerAppPart("dev." + std::string(196, 'x')) ==
           "dev." + std::string(196, 'x'));  // 200: kept whole
  }
  // The removal's glob: this app's timers, never one whose id extends it.
  {
    std::string glob = NotificationTimerGlob("com.acme.app");
    EXPECT(glob.rfind("laufey-com.acme.app-[0-9a-f][0-9a-f]", 0) == 0);
    EXPECT(glob.size() == std::string("laufey-com.acme.app-").size() +
                              16 * 8 + std::string(".timer").size());
#ifndef _WIN32
    auto matches = [&](const std::string& name) {
      return fnmatch(glob.c_str(), name.c_str(), 0) == 0;
    };
    EXPECT(matches(NotificationTimerUnit("com.acme.app", "daily") + ".timer"));
    EXPECT(!matches(NotificationTimerUnit("com.acme.app-extra", "daily") +
                    ".timer"));
    EXPECT(!matches(NotificationTimerUnit("com.acme.app.extra", "daily") +
                    ".timer"));
    EXPECT(!matches(
        NotificationTimerUnit("com.acme.app-0123456789abcdef", "daily") +
        ".timer"));
    EXPECT(!matches(NotificationTimerUnit("com.acme.app", "daily") +
                    ".service"));
    EXPECT(!matches("laufey-com.acme.app-0123456789ABCDEF.timer"));
#endif
  }

  // D-Bus activation's argument (before any "--") is left out of a
  // NULL-terminated copy of argv; argv itself is never changed.
  {
    std::string a0 = "app", a1 = "--x", a2 = "--laufey-dbus-activated",
                a3 = "--", a4 = "acme://y";
    char* argv[] = {&a0[0], &a1[0], &a2[0], &a3[0], &a4[0], nullptr};
    const std::vector<char*> before(argv, argv + 6);
    std::vector<char*> out = {&a4[0]};  // replaced, not appended to
    EXPECT(CopyArgvWithoutDBusActivationArg(5, argv, &out));
    EXPECT((out == std::vector<char*>{&a0[0], &a1[0], &a3[0], &a4[0],
                                      nullptr}));
    EXPECT(std::vector<char*>(argv, argv + 6) == before);
    // Only the first one: a second is an argument of the app's.
    std::string d0 = "app", d1 = "--laufey-dbus-activated",
                d2 = "--laufey-dbus-activated";
    char* argv4[] = {&d0[0], &d1[0], &d2[0], nullptr};
    EXPECT(CopyArgvWithoutDBusActivationArg(3, argv4, &out));
    EXPECT((out == std::vector<char*>{&d0[0], &d2[0], nullptr}));
    std::string b0 = "app", b1 = "--", b2 = "--laufey-dbus-activated";
    char* argv2[] = {&b0[0], &b1[0], &b2[0], nullptr};
    // A link after "--" is the app's.
    EXPECT(!CopyArgvWithoutDBusActivationArg(3, argv2, &out));
    EXPECT((out == std::vector<char*>{&b0[0], &b1[0], &b2[0], nullptr}));
    std::string c0 = "--laufey-dbus-activated";
    char* argv3[] = {&c0[0], nullptr};
    // argv[0] is the program, never the argument.
    EXPECT(!CopyArgvWithoutDBusActivationArg(1, argv3, &out));
    EXPECT((out == std::vector<char*>{&c0[0], nullptr}));
    // A NULL entry ends argv even where argc counts further; no argv at
    // all is an empty copy.
    char* argv5[] = {&a0[0], nullptr, &a2[0], nullptr};
    EXPECT(!CopyArgvWithoutDBusActivationArg(3, argv5, &out));
    EXPECT((out == std::vector<char*>{&a0[0], nullptr}));
    EXPECT(!CopyArgvWithoutDBusActivationArg(0, nullptr, &out));
    EXPECT((out == std::vector<char*>{nullptr}));
  }

  // UTC, rounded up to the next whole second.
  EXPECT(SystemdCalendarUtc(0) == "1970-01-01 00:00:00 UTC");
  EXPECT(SystemdCalendarUtc(1) == "1970-01-01 00:00:01 UTC");
  EXPECT(SystemdCalendarUtc(1000) == "1970-01-01 00:00:01 UTC");
  EXPECT(SystemdCalendarUtc(951782400000LL) == "2000-02-29 00:00:00 UTC");
  EXPECT(SystemdCalendarUtc(1791252750676LL) == "2026-10-06 02:12:31 UTC");
  EXPECT(SystemdCalendarUtc(4102444799000LL) == "2099-12-31 23:59:59 UTC");

  EXPECT(EscapeNotificationMarkup("a < b && c > d") ==
         "a &lt; b &amp;&amp; c &gt; d");
  EXPECT(EscapeNotificationMarkup("plain") == "plain");
}

std::string ToHex(const std::string& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out += kHex[c >> 4];
    out += kHex[c & 15];
  }
  return out;
}

#ifndef _WIN32
std::string ReadAll(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return out;
  char buf[512];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  std::fclose(f);
  return out;
}

void WriteAll(const std::string& path, const std::string& text, int mode) {
  FILE* f = std::fopen(path.c_str(), "wb");
  EXPECT(f);
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  EXPECT(chmod(path.c_str(), mode) == 0);
}
#endif

// Click arguments are MAC'd (S3): HMAC-SHA256 against RFC 4231, sign and
// verify, the per-install key file, and what an activation accepts.
void TestClickAuth() {
  // RFC 4231 test cases 1, 2 and 6 (a key longer than the block).
  EXPECT(ToHex(HmacSha256(std::string(20, '\x0b'), "Hi There")) ==
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  EXPECT(ToHex(HmacSha256("Jefe", "what do ya want for nothing?")) ==
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  EXPECT(ToHex(HmacSha256(
             std::string(131, '\xaa'),
             "Test Using Larger Than Block-Size Key - Hash Key First")) ==
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

  const std::string key(32, 'k'), other(32, 'o');
  std::string plain = "laufey=1&tag=t&action=a&data=%7B%7D";
  std::string signed_args = SignToastArguments(key, plain);
  EXPECT(signed_args.rfind(plain + "&mac=", 0) == 0);
  EXPECT(VerifyToastArguments(key, signed_args));
  EXPECT(!VerifyToastArguments(other, signed_args));     // another install
  EXPECT(!VerifyToastArguments(std::string(), signed_args));  // no key
  EXPECT(!VerifyToastArguments(key, plain));             // no MAC: forged
  EXPECT(!VerifyToastArguments(key, plain + "&mac="));
  EXPECT(!VerifyToastArguments(key, plain + "&mac=" + std::string(64, '0')));
  std::string tampered = signed_args;
  tampered.replace(tampered.find("tag=t"), 5, "tag=u");
  EXPECT(!VerifyToastArguments(key, tampered));
  EXPECT(!VerifyToastArguments(
      key, signed_args.substr(0, signed_args.size() - 1)));
  // Appending after the MAC (a second data field) breaks it.
  EXPECT(!VerifyToastArguments(key, signed_args + "&data=x"));
  // Over the cap: dropped unread, even when signed.
  std::string huge =
      "laufey=1&tag=t&data=" +
      std::string(kMaxNotificationClickArgumentsBytes, 'x');
  EXPECT(!VerifyToastArguments(key, SignToastArguments(key, huge)));

  // What an activation accepts: arguments this process's key signed.
  SetNotificationClickKeyForTesting(key);
  std::string data = "{\"q\":\"a&mac=b\"}";
  std::string args = EncodeToastArguments("tag 1", "act", &data);
  std::string tag, action, d;
  bool has_action = false, has_data = false;
  EXPECT(DecodeClickArguments(args, &tag, &action, &has_action, &d, &has_data));
  EXPECT(tag == "tag 1" && has_action && action == "act" && has_data &&
         d == data);
  // What a forger would send: well-formed, but no (or another key's) MAC.
  EXPECT(!DecodeClickArguments("laufey=1&tag=tag%201", &tag, &action,
                               &has_action, &d, &has_data));
  EXPECT(!DecodeClickArguments(
      SignToastArguments(other, "laufey=1&tag=tag%201"), &tag, &action,
      &has_action, &d, &has_data));
  // Signed, but a field over its limit (the data cap is 4 KiB).
  EXPECT(!DecodeClickArguments(
      SignToastArguments(key, "laufey=1&tag=t&data=" +
                                  std::string(
                                      LAUFEY_NOTIFICATION_MAX_DATA_BYTES + 1,
                                      'x')),
      &tag, &action, &has_action, &d, &has_data));
  EXPECT(DecodeClickArguments(
      SignToastArguments(
          key, "laufey=1&tag=t&data=" +
                   std::string(LAUFEY_NOTIFICATION_MAX_DATA_BYTES, 'x')),
      &tag, &action, &has_action, &d, &has_data));
  EXPECT(!DecodeClickArguments(
      SignToastArguments(key, "laufey=1&tag=" +
                                  std::string(
                                      LAUFEY_NOTIFICATION_MAX_TAG_BYTES + 1,
                                      'x')),
      &tag, &action, &has_action, &d, &has_data));
  EXPECT(!DecodeClickArguments(
      SignToastArguments(key, "laufey=1&tag=t&action=" +
                                  std::string(kMaxNotificationActionBytes + 1,
                                              'x')),
      &tag, &action, &has_action, &d, &has_data));
  // Signed but not laufey's format.
  EXPECT(!DecodeClickArguments(SignToastArguments(key, "tag=t"), &tag, &action,
                               &has_action, &d, &has_data));
  // A key change (another install) drops what the old one signed.
  SetNotificationClickKeyForTesting(other);
  EXPECT(!DecodeClickArguments(args, &tag, &action, &has_action, &d,
                               &has_data));
  // No key (no entropy): nothing verifies.
  SetNotificationClickKeyForTesting(std::string());
  EXPECT(!DecodeClickArguments(EncodeToastArguments("t", nullptr, nullptr),
                               &tag, &action, &has_action, &d, &has_data));
  SetNotificationClickKeyForTesting(key);

#ifndef _WIN32
  // The key file: created owner-only, the same key on every load; one that
  // others can read, or that is damaged, is replaced.
  char dir_template[] = "/tmp/laufey-click-key-XXXXXX";
  std::string dir = mkdtemp(dir_template);
  std::string path = dir + "/" + kNotificationClickKeyFile;
  std::string k1, k2, k3;
  EXPECT(!LoadOrCreateNotificationClickKey("", &k1));
  EXPECT(LoadOrCreateNotificationClickKey(dir, &k1));
  EXPECT(k1.size() == kNotificationClickKeyBytes);
  struct stat st;
  EXPECT(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  EXPECT(ReadAll(path) == ToHex(k1) + "\n");
  EXPECT(LoadOrCreateNotificationClickKey(dir, &k2) && k2 == k1);
  EXPECT(chmod(path.c_str(), 0644) == 0);  // others could have read it
  EXPECT(LoadOrCreateNotificationClickKey(dir, &k3) && k3 != k1);
  EXPECT(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  WriteAll(path, "not a key", 0600);
  EXPECT(LoadOrCreateNotificationClickKey(dir, &k1) && k1 != k3 &&
         k1.size() == kNotificationClickKeyBytes);
  // A symlink planted in its place isn't followed (it is replaced).
  std::string target = dir + "/elsewhere";
  WriteAll(target, ToHex(std::string(32, 'p')) + "\n", 0600);
  EXPECT(unlink(path.c_str()) == 0 &&
         symlink(target.c_str(), path.c_str()) == 0);
  EXPECT(LoadOrCreateNotificationClickKey(dir, &k2) &&
         k2 != std::string(32, 'p'));
  EXPECT(lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
  // Processes starting together agree on one key.
  EXPECT(unlink(path.c_str()) == 0);
  std::vector<std::string> keys(8);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < keys.size(); ++i) {
    threads.emplace_back([&, i] {
      EXPECT(LoadOrCreateNotificationClickKey(dir, &keys[i]));
    });
  }
  for (std::thread& t : threads)
    t.join();
  for (const std::string& k : keys)
    EXPECT(k == keys[0]);
  unlink(path.c_str());
  unlink(target.c_str());
  rmdir(dir.c_str());
#endif
}

}  // namespace

int main() {
  laufey_register_value_api(&g_api);
  // First: it checks the state before the dispatcher is bound.
  TestNativeModalLoop();
  // From here this thread is the UI thread; PumpUi runs what is queued.
  UiTaskDispatcher::Get().Bind([](void (*task)(void*), void* data) {
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    g_ui_queue.emplace_back(task, data);
    return true;
  });
  TestParseMenu();
  TestKeyCodes();
  TestContextMenuSession();
  TestValidate();
  TestRouting();
  TestFormats();
  TestClickAuth();
  TestPermissionCallbacksOnUiThread();
  TestLinuxActivationHelpers();
  std::printf("menu_notifications_test: OK\n");
  return 0;
}
