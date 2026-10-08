// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux clipboard through ext-data-control-v1 against the session's real
// compositor (clipboard_data_control_linux.h): text, HTML with its text
// alternative, a PNG larger than a pipe's buffer (our own source answers our
// own read on the same connection), the formats list, change events, and
// the hostile cases against a second client (this binary run as a peer
// process with a data-control connection of its own):
//   - a read larger than its max_bytes is not delivered;
//   - an owner that never writes: the read gives up after its timeout;
//   - a peer that asks for our selection again and again and never reads:
//     at most 8 writer threads at once, each gone after its idle timeout;
//   - the connection breaking during a read: the read is answered at once,
//     every later call says "unavailable", no descriptor is left open.
// And, without any compositor: a "compositor" that accepts the connection
// and never answers. Connecting gives up after its timeout instead of
// hanging the caller (the only case run when no compositor is there).
// No window is shown, so nothing here has keyboard focus: the core Wayland
// clipboard would refuse every one of these. Exits 77 (skipped) outside a
// Wayland session or where the compositor has no data-control (GNOME). CI
// runs it under a headless sway.

#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/clipboard_data_control_linux.h"
#include "../src/wayland/ext-data-control-v1-client-protocol.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"

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

std::atomic<int> g_changes{0};

void OnChange(void*) {
  g_changes++;
}

template <typename F>
bool WaitFor(F cond) {
  for (int i = 0; i < 300; i++) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

// A noise image: its PNG is far larger than a pipe's 64 KiB buffer.
std::string NoisePng() {
  GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 400, 400);
  guchar* px = gdk_pixbuf_get_pixels(pixbuf);
  int stride = gdk_pixbuf_get_rowstride(pixbuf);
  uint32_t x = 2463534242u;
  for (int y = 0; y < 400; y++) {
    for (int i = 0; i < 400 * 3; i++) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      px[y * stride + i] = static_cast<guchar>(x);
    }
  }
  gchar* buf = nullptr;
  gsize len = 0;
  EXPECT(
      gdk_pixbuf_save_to_buffer(pixbuf, &buf, &len, "png", nullptr, nullptr));
  std::string png(buf, len);
  g_free(buf);
  g_object_unref(pixbuf);
  return png;
}

std::string Take(char* s) {
  std::string out = s ? s : "";
  free(s);
  return out;
}

// --- The peer: a second data-control client, in a process of its own ------

constexpr char kText[] = "text/plain;charset=utf-8";

struct Peer {
  wl_display* display = nullptr;
  ext_data_control_manager_v1* manager = nullptr;
  wl_seat* seat = nullptr;
  ext_data_control_device_v1* device = nullptr;
  ext_data_control_offer_v1* selection = nullptr;
  std::vector<int> held;  // descriptors kept open, never written or read
};

Peer g_peer;

void PeerGlobal(void*, wl_registry* registry, uint32_t name,
                const char* interface, uint32_t) {
  if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
    g_peer.manager = static_cast<ext_data_control_manager_v1*>(wl_registry_bind(
        registry, name, &ext_data_control_manager_v1_interface, 1));
  else if (strcmp(interface, wl_seat_interface.name) == 0 && !g_peer.seat)
    g_peer.seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface, 1));
}
void PeerGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kPeerRegistry = {PeerGlobal, PeerGlobalRemove};

void PeerOffer(void*, ext_data_control_offer_v1*, const char*) {}
const ext_data_control_offer_v1_listener kPeerOffer = {PeerOffer};
void PeerDataOffer(void*, ext_data_control_device_v1*,
                   ext_data_control_offer_v1* offer) {
  ext_data_control_offer_v1_add_listener(offer, &kPeerOffer, nullptr);
}
void PeerSelection(void*, ext_data_control_device_v1*,
                   ext_data_control_offer_v1* offer) {
  g_peer.selection = offer;
}
void PeerFinished(void*, ext_data_control_device_v1*) {}
void PeerPrimary(void*, ext_data_control_device_v1*,
                 ext_data_control_offer_v1*) {}
const ext_data_control_device_v1_listener kPeerDevice = {
    PeerDataOffer, PeerSelection, PeerFinished, PeerPrimary};

// The owner that never answers: each request's descriptor is kept open,
// nothing written.
void PeerSend(void*, ext_data_control_source_v1*, const char*, int32_t fd) {
  g_peer.held.push_back(fd);
}
void PeerCancelled(void*, ext_data_control_source_v1*) {}
const ext_data_control_source_v1_listener kPeerSource = {PeerSend,
                                                         PeerCancelled};

[[noreturn]] void PeerFail(const char* what) {
  std::fprintf(stderr, "peer: %s\n", what);
  _exit(3);
}

void PeerConnect() {
  g_peer.display = wl_display_connect(nullptr);
  if (!g_peer.display)
    PeerFail("no Wayland display");
  wl_registry* registry = wl_display_get_registry(g_peer.display);
  wl_registry_add_listener(registry, &kPeerRegistry, nullptr);
  wl_display_roundtrip(g_peer.display);
  if (!g_peer.manager || !g_peer.seat)
    PeerFail("no data-control");
  g_peer.device =
      ext_data_control_manager_v1_get_data_device(g_peer.manager, g_peer.seat);
  ext_data_control_device_v1_add_listener(g_peer.device, &kPeerDevice, nullptr);
  wl_display_roundtrip(g_peer.display);
}

[[noreturn]] void PeerServe() {
  std::printf("ready\n");
  std::fflush(stdout);
  while (wl_display_dispatch(g_peer.display) >= 0) {
  }
  _exit(0);
}

// "silent-owner": owns the selection (text) and never answers a request.
// "hoarder <n>": asks for the current selection n times and never reads.
int PeerMain(const std::string& mode, int n) {
  PeerConnect();
  if (mode == "silent-owner") {
    ext_data_control_source_v1* source =
        ext_data_control_manager_v1_create_data_source(g_peer.manager);
    ext_data_control_source_v1_add_listener(source, &kPeerSource, nullptr);
    ext_data_control_source_v1_offer(source, kText);
    ext_data_control_device_v1_set_selection(g_peer.device, source);
    wl_display_roundtrip(g_peer.display);
    PeerServe();
  }
  if (mode == "hoarder") {
    if (!g_peer.selection)
      PeerFail("no selection to ask for");
    for (int i = 0; i < n; i++) {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC) != 0)
        PeerFail("pipe");
      ext_data_control_offer_v1_receive(g_peer.selection, kText, fds[1]);
      close(fds[1]);
      g_peer.held.push_back(fds[0]);  // never read
    }
    wl_display_roundtrip(g_peer.display);
    PeerServe();
  }
  PeerFail("unknown mode");
}

struct PeerProcess {
  pid_t pid = -1;
  ~PeerProcess() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
  }
};

// Starts this binary as a peer and waits for it to say it is ready.
void StartPeer(const char* self, const char* mode, int n, PeerProcess* out) {
  int out_pipe[2];
  EXPECT(pipe2(out_pipe, O_CLOEXEC) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    std::string count = std::to_string(n);
    execl(self, self, "--peer", mode, count.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  EXPECT(pid > 0);
  out->pid = pid;
  close(out_pipe[1]);
  pollfd p = {out_pipe[0], POLLIN, 0};
  EXPECT(poll(&p, 1, 5000) == 1);
  char buf[16] = {};
  EXPECT(read(out_pipe[0], buf, sizeof(buf) - 1) > 0);
  EXPECT(strncmp(buf, "ready", 5) == 0);
  close(out_pipe[0]);
}

// Open descriptors of this process.
int CountFds() {
  int n = 0;
  DIR* dir = opendir("/proc/self/fd");
  if (!dir)
    return -1;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] != '.')
      n++;
  }
  closedir(dir);
  return n;
}

// The data-control connection's socket: the only socket of this process
// connected to the compositor's (GTK isn't initialized here).
int WaylandSocket() {
  int found = -1;
  DIR* dir = opendir("/proc/self/fd");
  if (!dir)
    return -1;
  while (dirent* e = readdir(dir)) {
    int fd = atoi(e->d_name);
    if (e->d_name[0] == '.')
      continue;
    sockaddr_un addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0 &&
        addr.sun_family == AF_UNIX &&
        strstr(addr.sun_path, getenv("WAYLAND_DISPLAY")))
      found = fd;
  }
  closedir(dir);
  return found;
}

double SecondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
      .count();
}

}  // namespace

// A process of its own (the client connects once per process): a socket
// that accepts the connection and never answers. Exits 0 when connecting
// gave up in time.
int SilentCompositorMain() {
  std::string path = "/tmp/laufey-silent-wayland-" + std::to_string(getpid());
  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  EXPECT(listener >= 0);
  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  unlink(path.c_str());
  EXPECT(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  EXPECT(listen(listener, 4) == 0);
  std::thread([listener] {
    while (accept(listener, nullptr, nullptr) >= 0) {
    }  // accepted and never answered
  }).detach();
  setenv("WAYLAND_DISPLAY", path.c_str(), 1);  // absolute: libwayland 1.15+
  auto start = std::chrono::steady_clock::now();
  bool available = data_control::Available();
  double took = SecondsSince(start);
  unlink(path.c_str());
  std::printf("silent compositor: gave up after %.1f s\n", took);
  EXPECT(!available);
  EXPECT(took >= 2.5 && took < 6.0);
  // And at once from then on.
  start = std::chrono::steady_clock::now();
  EXPECT(!data_control::Available());
  EXPECT(SecondsSince(start) < 0.1);
  std::fflush(stdout);
  std::_Exit(0);
}

int main(int argc, char** argv) {
  if (argc >= 3 && strcmp(argv[1], "--peer") == 0)
    return PeerMain(argv[2], argc >= 4 ? atoi(argv[3]) : 0);
  if (argc >= 2 && strcmp(argv[1], "--silent-compositor") == 0)
    return SilentCompositorMain();

  // A compositor that never answers (no compositor needed).
  {
    PeerProcess child;
    child.pid = fork();
    if (child.pid == 0) {
      execl(argv[0], argv[0], "--silent-compositor",
            static_cast<char*>(nullptr));
      _exit(127);
    }
    int status = 0;
    EXPECT(waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

  const char* wayland = getenv("WAYLAND_DISPLAY");
  if (!wayland || !*wayland || !data_control::Available()) {
    std::printf(
        "laufey_clipboard_wayland_test: no ext-data-control here, skipped\n");
    return 77;
  }
  // Change events are delivered "on the GTK thread"; here, inline.
  SetGtkThread([](std::function<void()> fn) { fn(); }, [] { return true; });

  // Text.
  ClipboardWriteTextLinux("laufey data-control ✓");
  EXPECT(Take(ClipboardReadTextLinux()) == "laufey data-control ✓");
  std::string formats = Take(ClipboardReadFormatsLinux());
  EXPECT(formats == "text/plain");

  // HTML with a text alternative.
  EXPECT(ClipboardWriteHtmlLinux("<b>bold</b>", "bold"));
  EXPECT(Take(ClipboardReadHtmlLinux()) == "<b>bold</b>");
  EXPECT(Take(ClipboardReadTextLinux()) == "bold");
  formats = Take(ClipboardReadFormatsLinux());
  EXPECT(formats.find("text/html") != std::string::npos);
  EXPECT(formats.find("text/plain") != std::string::npos);

  // A PNG bigger than a pipe buffer, verbatim.
  std::string png = NoisePng();
  EXPECT(png.size() > 256 * 1024);
  EXPECT(ClipboardWriteImageLinux(reinterpret_cast<const uint8_t*>(png.data()),
                                  png.size()));
  size_t len = 0;
  uint8_t* got = ClipboardReadImageLinux(&len);
  EXPECT(got && len == png.size() && memcmp(got, png.data(), len) == 0);
  free(got);
  EXPECT(Take(ClipboardReadFormatsLinux()) == "image/png");

  // Change events, this process's own writes included.
  SetClipboardChangeHandler(OnChange, nullptr);
  int before = g_changes.load();
  ClipboardWriteTextLinux("changed");
  EXPECT(WaitFor([&] { return g_changes.load() > before; }));
  SetClipboardChangeHandler(nullptr, nullptr);

  // A read larger than max_bytes is not delivered (our own 1 MiB selection).
  std::string big(1024 * 1024, 'x');
  ClipboardWriteTextLinux(big);
  {
    std::string data;
    bool found = true;
    EXPECT(data_control::Read({kText}, 64 * 1024, &data, nullptr, &found));
    EXPECT(!found && data.empty());
    EXPECT(
        data_control::Read({kText}, 2 * 1024 * 1024, &data, nullptr, &found));
    EXPECT(found && data == big);
  }

  // A peer asks for our 1 MiB selection 20 times and never reads: at most 8
  // transfers run at once (the rest are refused), and each gives up after
  // its idle timeout instead of keeping a thread forever.
  {
    PeerProcess hoarder;
    StartPeer(argv[0], "hoarder", 20, &hoarder);
    EXPECT(WaitFor([] { return data_control::ActiveWriters() > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int active = data_control::ActiveWriters();
    std::printf("non-reading peer: %d writers in flight\n", active);
    EXPECT(active == 8);
    auto start = std::chrono::steady_clock::now();
    while (data_control::ActiveWriters() > 0 && SecondsSince(start) < 10)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(data_control::ActiveWriters() == 0);
    std::printf("non-reading peer: writers gone after %.1f s\n",
                SecondsSince(start));
  }

  // An owner that never writes: the read gives up after its timeout (3 s),
  // answering nothing, and the next read works.
  {
    PeerProcess owner;
    StartPeer(argv[0], "silent-owner", 0, &owner);
    std::vector<std::string> types;
    EXPECT(WaitFor([&] {
      return data_control::Types(&types) && types.size() == 1 &&
             types[0] == kText;
    }));
    auto start = std::chrono::steady_clock::now();
    EXPECT(Take(ClipboardReadTextLinux()).empty());
    double took = SecondsSince(start);
    std::printf("silent owner: read gave up after %.1f s\n", took);
    EXPECT(took >= 2.5 && took < 5.0);
  }
  ClipboardWriteTextLinux("after");
  EXPECT(Take(ClipboardReadTextLinux()) == "after");

  // The connection breaks while a read waits on an owner that never answers:
  // the read is answered at once (not after its 3 s), every later call says
  // "unavailable" without waiting, and nothing is left open. Last: the
  // protocol stays off for this process.
  {
    PeerProcess owner;
    StartPeer(argv[0], "silent-owner", 0, &owner);
    std::vector<std::string> types;
    EXPECT(WaitFor([&] {
      return data_control::Types(&types) && types.size() == 1 &&
             types[0] == kText;
    }));
    int fds_before = CountFds();
    int sock = WaylandSocket();
    EXPECT(sock >= 0);
    std::atomic<bool> read_done{false};
    std::atomic<bool> read_found{true};
    std::atomic<double> read_took{0};
    std::thread reader([&] {
      auto start = std::chrono::steady_clock::now();
      std::string data;
      bool found = true;
      data_control::Read({kText}, 1024, &data, nullptr, &found);
      read_found = found;
      read_took = SecondsSince(start);
      read_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT(!read_done.load());
    shutdown(sock, SHUT_RDWR);  // as if the compositor went away
    reader.join();
    std::printf("broken connection: pending read answered after %.1f s\n",
                read_took.load());
    EXPECT(!read_found.load());
    EXPECT(read_took.load() < 1.5);
    EXPECT(WaitFor([] { return !data_control::Available(); }));
    auto start = std::chrono::steady_clock::now();
    std::string data;
    bool found = true;
    EXPECT(!data_control::Read({kText}, 1024, &data, nullptr, &found));
    EXPECT(!data_control::Types(&types));
    data_control::Entries entries;
    entries.push_back({kText, std::make_shared<const std::string>("x")});
    EXPECT(!data_control::Write(std::move(entries)));
    EXPECT(SecondsSince(start) < 0.1);
    // The read's pipe and the connection's socket and wake descriptor are
    // closed.
    int fds_after = CountFds();
    std::printf("broken connection: %d descriptors before, %d after\n",
                fds_before, fds_after);
    EXPECT(fds_after <= fds_before - 2);
  }

  std::printf("laufey_clipboard_wayland_test: ok\n");
  std::fflush(stdout);
  // The data-control thread is detached; skip static teardown.
  std::_Exit(0);
}
