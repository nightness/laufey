// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the portable part of laufey_system.h: accelerator parsing
// and its canonical form, the global-shortcut registry (driven through a
// fake ShortcutPlatform: exactly-once results, ALREADY_REGISTERED,
// unregister while the OS hasn't answered, dispatch, the test hook, list),
// and the XDG autostart entry format. Plain asserts, no framework: run via
// `ctest --test-dir webview/build` (or cef/build).

#include "laufey_system.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
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

std::string Canon(const char* text, bool mac = false) {
  Accelerator a;
  std::string err;
  if (!ParseAccelerator(text, mac, &a, &err))
    return "!" + err;
  return CanonicalAccelerator(a);
}

void TestParse() {
  EXPECT(Canon("CommandOrControl+Shift+K") == "Ctrl+Shift+K");
  EXPECT(Canon("CommandOrControl+Shift+K", true) == "Shift+Super+K");
  EXPECT(Canon("cmdorctrl+shift+k") == "Ctrl+Shift+K");
  EXPECT(Canon("Shift+Ctrl+k") == "Ctrl+Shift+K");
  EXPECT(Canon("Alt+F4") == "Alt+F4");
  EXPECT(Canon("Option+f24", true) == "Alt+F24");
  EXPECT(Canon("Super+Space") == "Super+Space");
  EXPECT(Canon("Meta+Return") == "Super+Enter");
  EXPECT(Canon("Ctrl+Esc") == "Ctrl+Escape");
  EXPECT(Canon("Ctrl+Num5") == "Ctrl+Num5");
  EXPECT(Canon("Ctrl+numpad0") == "Ctrl+Num0");
  EXPECT(Canon("Ctrl+numadd") == "Ctrl+NumAdd");
  EXPECT(Canon("Ctrl+NumDec") == "Ctrl+NumDecimal");
  EXPECT(Canon("Ctrl+-") == "Ctrl+-");
  EXPECT(Canon("Ctrl+`") == "Ctrl+`");
  EXPECT(Canon("Ctrl+\\") == "Ctrl+\\");
  EXPECT(Canon("Ctrl + Alt + Delete") == "Ctrl+Alt+Delete");
  EXPECT(Canon("MediaPlayPause") == "MediaPlayPause");
  EXPECT(Canon("Command+Q", true) == "Super+Q");
  EXPECT(Canon("Ctrl+PageUp") == "Ctrl+PageUp");

  // Errors.
  EXPECT(Canon("")[0] == '!');
  EXPECT(Canon("Ctrl+")[0] == '!');
  EXPECT(Canon("+K")[0] == '!');
  EXPECT(Canon("Ctrl+Shift")[0] == '!');      // no key
  EXPECT(Canon("Ctrl+K+L")[0] == '!');        // two keys
  EXPECT(Canon("K+Ctrl")[0] == '!');          // modifier after the key
  EXPECT(Canon("Ctrl+Control+K")[0] == '!');  // repeated modifier
  EXPECT(Canon("Ctrl+F0")[0] == '!');
  EXPECT(Canon("Ctrl+F25")[0] == '!');
  EXPECT(Canon("Ctrl+F01")[0] == '!');
  EXPECT(Canon("Ctrl+Plus")[0] == '!');
  EXPECT(Canon("Ctrl+!")[0] == '!');
  EXPECT(Canon("Ctrl+é")[0] == '!');
  EXPECT(Canon("Command+Q")[0] == '!');  // macOS-only off macOS
  EXPECT(Canon(std::string(200, 'a').c_str())[0] == '!');

  // Global shortcuts need a real modifier on printable keys.
  Accelerator a;
  EXPECT(ParseAccelerator("K", false, &a, nullptr));
  EXPECT(!IsAllowedGlobalShortcut(a));
  EXPECT(ParseAccelerator("Shift+K", false, &a, nullptr));
  EXPECT(!IsAllowedGlobalShortcut(a));
  EXPECT(ParseAccelerator("Shift+Space", false, &a, nullptr));
  EXPECT(!IsAllowedGlobalShortcut(a));
  EXPECT(ParseAccelerator("Alt+K", false, &a, nullptr));
  EXPECT(IsAllowedGlobalShortcut(a));
  EXPECT(ParseAccelerator("F13", false, &a, nullptr));
  EXPECT(IsAllowedGlobalShortcut(a));
  EXPECT(ParseAccelerator("VolumeUp", false, &a, nullptr));
  EXPECT(IsAllowedGlobalShortcut(a));
  // The numpad and the editing keys are typing too: refused alone or with
  // Shift, allowed with a real modifier.
  const char* typing[] = {"Num5",  "NumAdd",    "NumDecimal", "Enter",
                          "Tab",   "Backspace", "Delete",     "Num0"};
  for (const char* key : typing) {
    EXPECT(ParseAccelerator(key, false, &a, nullptr));
    EXPECT(!IsAllowedGlobalShortcut(a));
    EXPECT(ParseAccelerator(std::string("Shift+") + key, false, &a, nullptr));
    EXPECT(!IsAllowedGlobalShortcut(a));
    EXPECT(ParseAccelerator(std::string("Ctrl+") + key, false, &a, nullptr));
    EXPECT(IsAllowedGlobalShortcut(a));
  }
  // Keys nobody types with stay allowed alone.
  EXPECT(ParseAccelerator("Escape", false, &a, nullptr));
  EXPECT(IsAllowedGlobalShortcut(a));
  EXPECT(ParseAccelerator("MediaPlayPause", false, &a, nullptr));
  EXPECT(IsAllowedGlobalShortcut(a));
}

// --- The registry over a fake OS side ------------------------------------------

struct FakePlatform : ShortcutPlatform {
  uint32_t caps = LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS;
  int next_status = LAUFEY_SHORTCUT_OK;
  bool defer = false;  // hold Bind's answer until Answer()
  std::map<uint32_t, std::function<void(int)>> pending;
  std::vector<uint32_t> bound;
  std::vector<uint32_t> unbound;

  uint32_t Capabilities() override { return caps; }
  void Bind(uint32_t id, const Accelerator&,
            std::function<void(int)> done) override {
    if (defer) {
      pending[id] = std::move(done);
      return;
    }
    if (next_status == LAUFEY_SHORTCUT_OK)
      bound.push_back(id);
    done(next_status);
  }
  void Unbind(uint32_t id) override { unbound.push_back(id); }
  void Answer(uint32_t id, int status) {
    auto done = std::move(pending[id]);
    pending.erase(id);
    if (status == LAUFEY_SHORTCUT_OK)
      bound.push_back(id);
    done(status);
  }
};

struct Result {
  int calls = 0;
  int status = -1;
  std::string accelerator;
};

void OnResult(void* data, int status, const char* accelerator) {
  auto* r = static_cast<Result*>(data);
  r->calls++;
  r->status = status;
  r->accelerator = accelerator ? accelerator : "";
}

std::vector<std::string> g_pressed;
void OnPress(void* /*data*/, const char* accelerator) {
  g_pressed.push_back(accelerator);
}

std::string List() {
  char* s = ListShortcuts();
  std::string out = s ? s : "<null>";
  free(s);
  return out;
}

void TestRegistry() {
  // No platform: NOT_SUPPORTED, synchronously.
  InstallShortcutPlatform(nullptr);
  Result r;
  RegisterShortcut("Ctrl+Shift+K", OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_NOT_SUPPORTED);
  EXPECT(ShortcutCapabilities() == 0);

  auto owned = std::make_unique<FakePlatform>();
  FakePlatform* fake = owned.get();
  InstallShortcutPlatform(std::move(owned));
  EXPECT(ShortcutCapabilities() == LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS);
  SetShortcutHandler(OnPress, nullptr);

  // Invalid accelerators never reach the OS.
  r = Result();
  RegisterShortcut("Ctrl+Nope", OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_INVALID);
  r = Result();
  RegisterShortcut("K", OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_INVALID);
  r = Result();
  RegisterShortcut(nullptr, OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_INVALID);
  EXPECT(fake->bound.empty());

  // OK, with the canonical form; another spelling is ALREADY_REGISTERED.
  // (CommandOrControl resolves per OS, so the registry tests spell Ctrl.)
  r = Result();
  RegisterShortcut("Control+Shift+K", OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_OK);
  EXPECT(r.accelerator == "Ctrl+Shift+K");
  EXPECT(fake->bound.size() == 1 && fake->bound[0] == 1);
  r = Result();
  RegisterShortcut("shift+ctrl+k", OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_ALREADY_REGISTERED);
  EXPECT(r.accelerator == "Ctrl+Shift+K");
  EXPECT(List() == "Ctrl+Shift+K");
  char* canon = CanonicalizeAccelerator("shift+control+k");
  EXPECT(canon && std::string(canon) == "Ctrl+Shift+K");
  free(canon);
  EXPECT(CanonicalizeAccelerator("Ctrl+Nope") == nullptr);
  EXPECT(CanonicalizeAccelerator(nullptr) == nullptr);

  // A press reaches the handler with the canonical form; the test hook takes
  // any spelling of a registered shortcut and nothing else.
  DispatchShortcut(1);
  EXPECT(g_pressed.size() == 1 && g_pressed[0] == "Ctrl+Shift+K");
  EXPECT(TestTriggerShortcut("Shift+Control+K"));
  EXPECT(g_pressed.size() == 2);
  EXPECT(!TestTriggerShortcut("Ctrl+Shift+L"));
  EXPECT(!TestTriggerShortcut("garbage"));
  DispatchShortcut(99);  // unknown id: ignored
  EXPECT(g_pressed.size() == 2);

  // The OS refuses: CONFLICT, nothing kept.
  fake->next_status = LAUFEY_SHORTCUT_CONFLICT;
  r = Result();
  RegisterShortcut("Alt+F5", OnResult, &r);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_CONFLICT);
  EXPECT(r.accelerator.empty());
  EXPECT(List() == "Ctrl+Shift+K");
  fake->next_status = LAUFEY_SHORTCUT_OK;

  // Ids are reused once released.
  r = Result();
  RegisterShortcut("Alt+F6", OnResult, &r);
  EXPECT(r.status == LAUFEY_SHORTCUT_OK && fake->bound.back() == 2);
  EXPECT(List() == "Ctrl+Shift+K\nAlt+F6");
  EXPECT(UnregisterShortcut("Shift+Control+k"));
  EXPECT(fake->unbound.size() == 1 && fake->unbound[0] == 1);
  EXPECT(!UnregisterShortcut("Ctrl+Shift+K"));  // already gone
  EXPECT(!UnregisterShortcut("not an accelerator"));
  DispatchShortcut(1);  // a late press for a released id: ignored
  EXPECT(g_pressed.size() == 2);
  r = Result();
  RegisterShortcut("Alt+F7", OnResult, &r);
  EXPECT(r.status == LAUFEY_SHORTCUT_OK && fake->bound.back() == 1);

  // Unregistered while the OS hadn't answered: the binding is given back as
  // soon as it arrives, and the result still comes exactly once.
  fake->defer = true;
  r = Result();
  RegisterShortcut("Alt+F8", OnResult, &r);
  EXPECT(r.calls == 0 && fake->pending.count(3) == 1);
  Result dup;
  RegisterShortcut("Alt+F8", OnResult, &dup);  // pending counts as registered
  EXPECT(dup.status == LAUFEY_SHORTCUT_ALREADY_REGISTERED);
  EXPECT(List() == "Alt+F6\nAlt+F7");  // pending isn't listed
  EXPECT(UnregisterShortcut("Alt+F8"));
  size_t unbound_before = fake->unbound.size();
  fake->Answer(3, LAUFEY_SHORTCUT_OK);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_OK);
  EXPECT(fake->unbound.size() == unbound_before + 1 &&
         fake->unbound.back() == 3);
  EXPECT(List() == "Alt+F6\nAlt+F7");
  // ... and may be registered again.
  r = Result();
  RegisterShortcut("Alt+F8", OnResult, &r);
  fake->Answer(3, LAUFEY_SHORTCUT_DENIED);
  EXPECT(r.calls == 1 && r.status == LAUFEY_SHORTCUT_DENIED);
  fake->defer = false;

  // Unregister all.
  UnregisterAllShortcuts();
  EXPECT(List().empty());
  EXPECT(!TestTriggerShortcut("Alt+F6"));

  // No handler: the test hook says so.
  r = Result();
  RegisterShortcut("Alt+F9", OnResult, &r);
  SetShortcutHandler(nullptr, nullptr);
  EXPECT(!TestTriggerShortcut("Alt+F9"));
  UnregisterAllShortcuts();

  // A NULL callback is fine.
  RegisterShortcut("Alt+F10", nullptr, nullptr);
  EXPECT(List() == "Alt+F10");
  UnregisterAllShortcuts();

  // The cap on shortcuts held at once.
  // Ten modifier sets x 26 letters: more distinct shortcuts than the cap.
  static const char* kMods[] = {"Ctrl+",       "Alt+",        "Super+",
                                "Ctrl+Alt+",   "Ctrl+Super+", "Alt+Super+",
                                "Ctrl+Shift+", "Alt+Shift+",  "Super+Shift+",
                                "Ctrl+Alt+Shift+"};
  for (int i = 0; i < LAUFEY_MAX_SHORTCUTS; i++) {
    std::string accel = std::string(kMods[i / 26]) +
                        std::string(1, static_cast<char>('A' + i % 26));
    Result each;
    RegisterShortcut(accel.c_str(), OnResult, &each);
    EXPECT(each.status == LAUFEY_SHORTCUT_OK);
  }
  r = Result();
  RegisterShortcut("Ctrl+Alt+Shift+Super+F1", OnResult, &r);
  EXPECT(r.status == LAUFEY_SHORTCUT_FAILED);
  UnregisterAllShortcuts();
  EXPECT(List().empty());

  // Without the capability (e.g. Wayland with no portal): NOT_SUPPORTED.
  fake->caps = 0;
  r = Result();
  RegisterShortcut("Alt+F11", OnResult, &r);
  EXPECT(r.status == LAUFEY_SHORTCUT_NOT_SUPPORTED);
  InstallShortcutPlatform(nullptr);
}

// --- Autostart entries -----------------------------------------------------------

void TestAutostart() {
  EXPECT(DesktopExecQuote("/opt/My App/app") == "\"/opt/My App/app\"");
  // '"', '`', '$', '\' are backslash-escaped inside the quotes, then every
  // backslash is doubled for the string value; '%' is doubled.
  EXPECT(DesktopExecQuote("/a\"b") == "\"/a\\\\\"b\"");
  EXPECT(DesktopExecQuote("/a$b`c") == "\"/a\\\\$b\\\\`c\"");
  EXPECT(DesktopExecQuote("/a\\b") == "\"/a\\\\\\\\b\"");
  EXPECT(DesktopExecQuote("/100%") == "\"/100%%\"");

  std::string entry = BuildAutostartEntry("My\nApp", "/opt/app/bin");
  EXPECT(entry.find("[Desktop Entry]\n") == 0);
  EXPECT(entry.find("Type=Application\n") != std::string::npos);
  EXPECT(entry.find("Name=My App\n") != std::string::npos);
  EXPECT(entry.find("Exec=\"/opt/app/bin\"\n") != std::string::npos);
  EXPECT(AutostartEntryEnabled(entry));
  EXPECT(!AutostartEntryEnabled(entry + "Hidden=true\n"));
  EXPECT(AutostartEntryEnabled(entry + "Hidden=false\n"));
  EXPECT(!AutostartEntryEnabled("[Desktop Entry]\r\nX-GNOME-Autostart-enabled"
                                " = false\r\n"));
  // Keys in other groups don't count.
  EXPECT(AutostartEntryEnabled(entry + "[Desktop Action x]\nHidden=true\n"));
}

}  // namespace

int main() {
  TestParse();
  TestRegistry();
  TestAutostart();
  std::printf("laufey_system_test: ok\n");
  return 0;
}
