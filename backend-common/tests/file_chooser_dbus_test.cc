// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Linux file dialogs through xdg-desktop-portal's FileChooser (io_linux.cc),
// against a mock portal on a private D-Bus session bus (GTestDBus) and a
// private Xvfb for GTK:
//   - open: OpenFile carries the title, accept label, "multiple", the
//     filters (case-insensitive globs) and the start folder; the Response's
//     file:// URIs come back as paths (a non-local URI is dropped);
//   - save: SaveFile carries the proposed name and folder;
//   - a folder: "directory" with FileChooser version 3 or later; with an
//     older portal the dialog is GTK's (it can't pick folders through it);
//   - the user cancels in the portal (response 1): cancelled;
//   - cancel_file_dialog on a dialog the portal shows: Request.Close, and
//     cancelled once; and on one cancelled before the portal even answered
//     the call: closed as soon as the portal names the request;
//   - a portal without a FileChooser (wlroots' backend alone): GTK's chooser,
//     which the test hook can accept; asked once per process;
//   - a portal whose OpenFile fails: GTK's chooser instead;
//   - LAUFEY_FILE_CHOOSER=gtk: GTK's chooser even with a portal;
//   - the choice itself (ChooseFileChooser) and what platform_features
//     reports for it.
// Exits 77 (skipped) without dbus-daemon or Xvfb.

#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "laufey_io.h"
#include "laufey_platform_features.h"

using namespace laufey_common;

namespace {

pid_t g_xvfb = -1;

[[noreturn]] void Finish(int code) {
  if (g_xvfb > 0)
    kill(g_xvfb, SIGTERM);
  std::fflush(stdout);
  std::_Exit(code);
}

}  // namespace

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Finish(1);                                                             \
    }                                                                        \
  } while (0)

namespace {

const char kXml[] =
    "<node>"
    " <interface name='org.freedesktop.portal.FileChooser'>"
    "  <method name='OpenFile'>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/><arg type='o' direction='out'/>"
    "  </method>"
    "  <method name='SaveFile'>"
    "   <arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/><arg type='o' direction='out'/>"
    "  </method>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Request'>"
    "  <method name='Close'/>"
    "  <signal name='Response'>"
    "   <arg type='u'/><arg type='a{sv}'/>"
    "  </signal>"
    " </interface>"
    "</node>";

GDBusConnection* g_conn = nullptr;  // the mock portal's
GDBusNodeInfo* g_info = nullptr;

// How the mock answers.
enum class Mode { kAnswer, kUserCancels, kHold, kFail };

std::mutex g_mutex;
Mode g_mode = Mode::kAnswer;
uint32_t g_version = 4;  // 0: no FileChooser
std::vector<std::string> g_uris;
int g_calls = 0;
std::string g_method;
std::string g_title;
GVariant* g_options = nullptr;        // the last call's
std::vector<std::string> g_closed;    // request paths Close was called on
std::vector<std::string> g_requests;  // request paths handed out

void Configure(Mode mode, uint32_t version,
               std::vector<std::string> uris = {}) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mode = mode;
  g_version = version;
  g_uris = std::move(uris);
}

int Calls() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_calls;
}

void Emit(const std::string& path, uint32_t response,
          const std::vector<std::string>& uris) {
  GVariantBuilder results;
  g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
  if (response == 0) {
    std::vector<const char*> list;
    for (const auto& u : uris)
      list.push_back(u.c_str());
    g_variant_builder_add(
        &results, "{sv}", "uris",
        g_variant_new_strv(list.data(), static_cast<gssize>(list.size())));
  }
  EXPECT(g_dbus_connection_emit_signal(
      g_conn, nullptr, path.c_str(), "org.freedesktop.portal.Request",
      "Response", g_variant_new("(ua{sv})", response, &results), nullptr));
}

const GDBusInterfaceVTable* RequestVTable();

void CallMethod(GDBusConnection*, const gchar* sender, const gchar* path,
                const gchar* iface, const gchar* method, GVariant* params,
                GDBusMethodInvocation* invocation, gpointer) {
  if (strcmp(iface, "org.freedesktop.portal.Request") == 0) {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_closed.push_back(path);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  Mode mode;
  std::vector<std::string> uris;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    mode = g_mode;
    uris = g_uris;
    g_calls++;
    g_method = method;
    const gchar* title = nullptr;
    GVariant* options = nullptr;
    g_variant_get(params, "(&s&s@a{sv})", nullptr, &title, &options);
    g_title = title ? title : "";
    if (g_options)
      g_variant_unref(g_options);
    g_options = options;
  }
  if (mode == Mode::kFail) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.portal.Error.Failed", "no backend");
    return;
  }
  // The request object the spec names: .../request/SENDER/TOKEN.
  const gchar* token = "t";
  GVariant* options = g_variant_get_child_value(params, 2);
  g_variant_lookup(options, "handle_token", "&s", &token);
  std::string who = sender + 1;
  for (char& c : who) {
    if (c == '.')
      c = '_';
  }
  std::string request =
      std::string("/org/freedesktop/portal/desktop/request/") + who + "/" +
      token;
  g_variant_unref(options);
  GError* error = nullptr;
  g_dbus_connection_register_object(
      g_conn, request.c_str(),
      g_dbus_node_info_lookup_interface(g_info,
                                        "org.freedesktop.portal.Request"),
      RequestVTable(), nullptr, nullptr, &error);
  g_clear_error(&error);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_requests.push_back(request);
  }
  g_dbus_method_invocation_return_value(invocation,
                                        g_variant_new("(o)", request.c_str()));
  if (mode == Mode::kAnswer)
    Emit(request, 0, uris);
  else if (mode == Mode::kUserCancels)
    Emit(request, 1, {});
}

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar*, const gchar*, GError** error, gpointer) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_version == 0) {
    // As xdg-desktop-portal without a FileChooser backend: not exported.
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE,
                "No such interface");
    return nullptr;
  }
  return g_variant_new_uint32(g_version);
}

const GDBusInterfaceVTable kVTable = {CallMethod, GetProperty, nullptr, {}};
const GDBusInterfaceVTable* RequestVTable() {
  return &kVTable;
}

// --- The dialog under test -------------------------------------------------

struct Outcome {
  std::atomic<bool> done{false};
  int status = -1;
  std::vector<std::string> paths;
};

void OnResult(void* user_data, uint32_t, int status, const char* const* paths,
              size_t count) {
  auto* o = static_cast<Outcome*>(user_data);
  o->status = status;
  for (size_t i = 0; i < count; ++i)
    o->paths.emplace_back(paths[i]);
  o->done = true;
}

// Runs the default context (the "GTK thread") until `done`, up to ~10 s.
bool Spin(const std::function<bool()>& done, int ms = 10000) {
  for (int i = 0; i < ms / 5; ++i) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

uint32_t Show(Outcome* o, int kind, uint32_t flags, const char* title,
              const char* default_path, const char* label,
              const laufey_file_filter_t* filters = nullptr,
              size_t filter_count = 0) {
  laufey_file_dialog_options_t opts = {};
  opts.kind = kind;
  opts.flags = flags;
  opts.title = title;
  opts.default_path = default_path;
  opts.button_label = label;
  opts.filters = filters;
  opts.filter_count = filter_count;
  return ShowFileDialogLinux(nullptr, &opts, OnResult, o);
}

bool OptionBool(const char* key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  gboolean b = FALSE;
  return g_options && g_variant_lookup(g_options, key, "b", &b) && b;
}

std::string OptionString(const char* key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const gchar* s = nullptr;
  if (g_options && g_variant_lookup(g_options, key, "&s", &s))
    return s;
  return "";
}

std::string OptionBytes(const char* key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const gchar* s = nullptr;
  if (g_options && g_variant_lookup(g_options, key, "^&ay", &s))
    return s;
  return "";
}

std::string FiltersText() {
  std::lock_guard<std::mutex> lock(g_mutex);
  GVariant* f = g_options ? g_variant_lookup_value(g_options, "filters",
                                                   G_VARIANT_TYPE("a(sa(us))"))
                          : nullptr;
  if (!f)
    return "";
  gchar* text = g_variant_print(f, FALSE);
  std::string out = text;
  g_free(text);
  g_variant_unref(f);
  return out;
}

pid_t StartXvfb() {
  gchar* xvfb = g_find_program_in_path("Xvfb");
  if (!xvfb)
    return -1;
  int fds[2];
  EXPECT(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    std::string fd = std::to_string(fds[1]);
    execl(xvfb, xvfb, "-displayfd", fd.c_str(), "-nolisten", "tcp", "-screen",
          "0", "800x600x24", static_cast<char*>(nullptr));
    _exit(127);
  }
  g_free(xvfb);
  close(fds[1]);
  pollfd p = {fds[0], POLLIN, 0};
  char buf[32] = {};
  if (poll(&p, 1, 10000) != 1 || read(fds[0], buf, sizeof(buf) - 1) <= 0) {
    kill(pid, SIGKILL);
    return -1;
  }
  close(fds[0]);
  std::string display = std::string(":") + strtok(buf, "\n");
  setenv("DISPLAY", display.c_str(), 1);
  return pid;
}

// The test hook acts on the open dialog (once it is attached).
bool Respond(int action, const char* path) {
  return Spin([&] { return TestFileDialogRespondLinux(action, path); }, 5000);
}

}  // namespace

int main() {
  // --- The choice ------------------------------------------------------------
  FileDialogRequest folder;
  folder.flags = LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES;
  EXPECT(ChooseFileChooser(4, nullptr, nullptr).portal);
  EXPECT(ChooseFileChooser(1, nullptr, "portal").portal);
  FileChooserChoice none = ChooseFileChooser(0, nullptr, nullptr);
  EXPECT(!none.portal &&
         none.reason.find("no FileChooser") != std::string::npos);
  FileChooserChoice forced = ChooseFileChooser(4, nullptr, "gtk");
  EXPECT(!forced.portal && forced.reason == "LAUFEY_FILE_CHOOSER=gtk");
  EXPECT(ChooseFileChooser(3, &folder, nullptr).portal);
  FileChooserChoice old = ChooseFileChooser(2, &folder, nullptr);
  EXPECT(!old.portal && old.reason.find("version 3") != std::string::npos);

  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_file_chooser_dbus_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);
  // First: GTestDBus unsets DISPLAY.
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  g_xvfb = StartXvfb();
  if (g_xvfb < 0) {
    std::printf("laufey_file_chooser_dbus_test: Xvfb missing, skipped\n");
    g_test_dbus_down(bus);
    return 77;
  }
  unsetenv("WAYLAND_DISPLAY");
  unsetenv("LAUFEY_FILE_CHOOSER");
  unsetenv("GTK_USE_PORTAL");
  EXPECT(gtk_init_check(nullptr, nullptr));
  // This thread runs the default context: it is the GTK thread.
  SetGtkThread(nullptr, [] { return true; });

  GError* error = nullptr;
  g_conn = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(bus),
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  EXPECT(g_conn);
  g_info = g_dbus_node_info_new_for_xml(kXml, &error);
  EXPECT(g_info);
  std::atomic<bool> ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    EXPECT(g_dbus_connection_register_object(
               g_conn, "/org/freedesktop/portal/desktop",
               g_dbus_node_info_lookup_interface(
                   g_info, "org.freedesktop.portal.FileChooser"),
               &kVTable, nullptr, nullptr, &error) != 0);
    GVariant* r = g_dbus_connection_call_sync(
        g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    EXPECT(r);
    g_variant_unref(r);
    ready = true;
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  EXPECT(Spin([&] { return ready.load(); }));

  gchar* tmp = g_dir_make_tmp("laufey-fc-XXXXXX", nullptr);
  EXPECT(tmp);
  std::string dir = tmp;
  std::string file = dir + "/picked.txt";
  EXPECT(g_file_set_contents(file.c_str(), "x", -1, nullptr));
  gchar* file_uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);

  // --- Open, through the portal --------------------------------------------
  Configure(Mode::kAnswer, 4, {file_uri, "https://example.com/remote.txt"});
  const char* exts[] = {"txt", "Md"};
  laufey_file_filter_t filter = {"Text", exts, 2};
  {
    Outcome o;
    uint32_t id = Show(&o, LAUFEY_FILE_DIALOG_OPEN, LAUFEY_FILE_DIALOG_MULTIPLE,
                       "Pick some", dir.c_str(), "Import", &filter, 1);
    EXPECT(id != 0);
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_ACCEPTED);
    // The remote URI has no local path: dropped.
    EXPECT(o.paths.size() == 1 && o.paths[0] == file);
    EXPECT(g_method == "OpenFile");
    EXPECT(g_title == "Pick some");
    EXPECT(OptionString("accept_label") == "Import");
    EXPECT(OptionBool("multiple"));
    EXPECT(!OptionBool("directory"));
    EXPECT(OptionBytes("current_folder") == dir);
    EXPECT(!OptionString("handle_token").empty());
    // App-level (no parent window): not modal.
    EXPECT(!OptionBool("modal"));
    std::string filters = FiltersText();
    EXPECT(filters.find("'Text'") != std::string::npos);
    EXPECT(filters.find("'*.[tT][xX][tT]'") != std::string::npos);
    EXPECT(filters.find("'*.[mM][dD]'") != std::string::npos);
    EXPECT(PortalFileChooserVersionForTesting() == 4);
  }

  // --- Save
  // -------------------------------------------------------------------
  std::string target = dir + "/new name.txt";
  gchar* target_uri = g_filename_to_uri(target.c_str(), nullptr, nullptr);
  Configure(Mode::kAnswer, 4, {target_uri});
  {
    Outcome o;
    EXPECT(Show(&o, LAUFEY_FILE_DIALOG_SAVE, 0, nullptr, target.c_str(),
                nullptr) != 0);
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_ACCEPTED);
    EXPECT(o.paths.size() == 1 && o.paths[0] == target);
    EXPECT(g_method == "SaveFile");
    EXPECT(g_title == "Save");
    EXPECT(OptionString("current_name") == "new name.txt");
    EXPECT(OptionBytes("current_folder") == dir);
  }

  // --- A folder: "directory" (version 3 and later) --------------------------
  Configure(Mode::kAnswer, 4,
            {g_filename_to_uri(dir.c_str(), nullptr, nullptr)});
  {
    Outcome o;
    EXPECT(Show(&o, LAUFEY_FILE_DIALOG_OPEN,
                LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES, nullptr, nullptr,
                nullptr, &filter, 1) != 0);
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_ACCEPTED);
    EXPECT(o.paths.size() == 1 && o.paths[0] == dir);
    EXPECT(OptionBool("directory"));
    EXPECT(g_title == "Select Folder");
    // Filters don't apply to folders.
    EXPECT(FiltersText().empty());
  }

  // --- The user cancels in the portal ----------------------------------------
  Configure(Mode::kUserCancels, 4);
  {
    Outcome o;
    EXPECT(Show(&o, LAUFEY_FILE_DIALOG_OPEN, 0, nullptr, nullptr, nullptr) !=
           0);
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_CANCELLED && o.paths.empty());
  }

  // --- cancel_file_dialog on a dialog the portal shows
  // ------------------------
  Configure(Mode::kHold, 4);
  {
    Outcome o;
    int before = Calls();
    uint32_t id =
        Show(&o, LAUFEY_FILE_DIALOG_OPEN, 0, nullptr, nullptr, nullptr);
    EXPECT(Spin([&] { return Calls() == before + 1; }));
    std::string request;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      request = g_requests.back();
    }
    // The handle has arrived once the request is known on our side; give the
    // reply a moment, then cancel.
    Spin([] { return false; }, 200);
    EXPECT(CancelFileDialogLinux(id));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_CANCELLED);
    EXPECT(Spin([&] {
      std::lock_guard<std::mutex> lock(g_mutex);
      return !g_closed.empty() && g_closed.back() == request;
    }));
    EXPECT(!CancelFileDialogLinux(id));
  }

  // --- Cancelled before the portal answered the call
  // --------------------------
  {
    Outcome o;
    size_t closed_before;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      closed_before = g_closed.size();
    }
    uint32_t id =
        Show(&o, LAUFEY_FILE_DIALOG_SAVE, 0, nullptr, nullptr, nullptr);
    EXPECT(CancelFileDialogLinux(id));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_CANCELLED);
    // The call had gone out: its request is closed once the portal names
    // it.
    EXPECT(Spin([&] {
      std::lock_guard<std::mutex> lock(g_mutex);
      return g_closed.size() > closed_before &&
             g_closed.back() == g_requests.back();
    }));
  }

  // --- The test hook can't accept the desktop's own dialog
  // --------------------
  {
    Outcome o;
    int before = Calls();
    Show(&o, LAUFEY_FILE_DIALOG_OPEN, 0, nullptr, nullptr, nullptr);
    EXPECT(Spin([&] { return Calls() == before + 1; }));
    // ACCEPT on a portal dialog closes it as cancelled (it can't be driven).
    EXPECT(Respond(LAUFEY_TEST_DIALOG_ACCEPT, file.c_str()));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_CANCELLED);
  }

  // --- An old portal (version 2) and a folder: GTK's chooser
  // ------------------
  ResetPortalFileChooserForTesting();
  Configure(Mode::kAnswer, 2);
  {
    Outcome o;
    int before = Calls();
    Show(&o, LAUFEY_FILE_DIALOG_OPEN, LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES,
         nullptr, dir.c_str(), nullptr);
    std::string sub = dir + "/sub";
    EXPECT(g_mkdir(sub.c_str(), 0700) == 0);
    EXPECT(Respond(LAUFEY_TEST_DIALOG_ACCEPT, sub.c_str()));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_ACCEPTED);
    EXPECT(o.paths.size() == 1 && o.paths[0] == sub);
    EXPECT(Calls() == before);  // never through the portal
  }

  // --- No FileChooser in the portal: GTK's chooser, asked once
  // ----------------
  ResetPortalFileChooserForTesting();
  Configure(Mode::kAnswer, 0);
  {
    Outcome o;
    int before = Calls();
    Show(&o, LAUFEY_FILE_DIALOG_OPEN, 0, nullptr, dir.c_str(), nullptr);
    EXPECT(Respond(LAUFEY_TEST_DIALOG_ACCEPT, file.c_str()));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_ACCEPTED);
    EXPECT(o.paths.size() == 1 && o.paths[0] == file);
    EXPECT(Calls() == before);
    EXPECT(PortalFileChooserVersionForTesting() == 0);
    // What platform_features reports for it (its own probe of the portal).
    ResetPlatformFeaturesForTesting();
    PlatformFeatures f = ProbePlatformFeatures();
    EXPECT(f.file_chooser == "gtk");
    EXPECT(f.file_chooser_reason.find("no FileChooser") != std::string::npos);
    EXPECT(PlatformFeaturesToJson(f).find("\"fileChooser\":\"gtk\"") !=
           std::string::npos);
  }
  ResetPlatformFeaturesForTesting();
  Configure(Mode::kAnswer, 4);
  EXPECT(ProbePlatformFeatures().file_chooser == "portal");

  // --- OpenFile fails: GTK's chooser instead ---------------------------------
  ResetPortalFileChooserForTesting();
  Configure(Mode::kFail, 4);
  {
    Outcome o;
    int before = Calls();
    Show(&o, LAUFEY_FILE_DIALOG_OPEN, 0, nullptr, dir.c_str(), nullptr);
    EXPECT(Spin([&] { return Calls() == before + 1; }));
    EXPECT(Respond(LAUFEY_TEST_DIALOG_ACCEPT, file.c_str()));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_ACCEPTED);
    EXPECT(o.paths.size() == 1 && o.paths[0] == file);
    // Not asked again in this process.
    EXPECT(PortalFileChooserVersionForTesting() == 0);
  }

  // --- LAUFEY_FILE_CHOOSER=gtk
  // ------------------------------------------------
  ResetPortalFileChooserForTesting();
  Configure(Mode::kAnswer, 4, {file_uri});
  setenv("LAUFEY_FILE_CHOOSER", "gtk", 1);
  {
    Outcome o;
    int before = Calls();
    Show(&o, LAUFEY_FILE_DIALOG_OPEN, 0, nullptr, dir.c_str(), nullptr);
    EXPECT(Respond(LAUFEY_TEST_DIALOG_CANCEL, nullptr));
    EXPECT(Spin([&] { return o.done.load(); }));
    EXPECT(o.status == LAUFEY_FILE_DIALOG_CANCELLED);
    EXPECT(Calls() == before);
  }
  unsetenv("LAUFEY_FILE_CHOOSER");

  std::printf("laufey_file_chooser_dbus_test: ok\n");
  Finish(0);
}
