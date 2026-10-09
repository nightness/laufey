// Experiment (temporary): how WebAuthNAuthenticatorGetAssertion ends on a CI
// runner with nobody at the dialog, depending on when
// WebAuthNCancelCurrentOperation is called relative to the call's start.
//
//   probe.exe <case> [arg]
//     timeout-only            no cancel; dwTimeoutMilliseconds = 3000
//     cancel-before           cancel(id) before the call; dwTimeout 12000
//     cancel-at <ms>          one cancel <ms> after the call starts; 12000
//     cancel-repeat <ms>      cancel at <ms>, then every 250 ms until it ends
//
// Prints one line: case, ms the call took, its HRESULT, each cancel's HRESULT
// and when it was sent. A watchdog ends the process after 45 s ("HUNG").

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// After windows.h.
#include <webauthn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

static decltype(&WebAuthNAuthenticatorGetAssertion) g_get = nullptr;
static decltype(&WebAuthNGetCancellationId) g_get_id = nullptr;
static decltype(&WebAuthNCancelCurrentOperation) g_cancel = nullptr;

static std::chrono::steady_clock::time_point g_t0;
static std::mutex g_log_mutex;
static std::string g_cancels;
static std::atomic<bool> g_done{false};

static long long Ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - g_t0)
      .count();
}

static void Cancel(GUID* id) {
  HRESULT hr = g_cancel(id);
  char b[64];
  std::snprintf(b, sizeof(b), " cancel@%lld=0x%08lX", Ms(),
                static_cast<unsigned long>(hr));
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_cancels += b;
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
  if (argc < 2)
    return 2;
  std::string mode = argv[1];
  int arg = argc > 2 ? std::atoi(argv[2]) : 0;
  HMODULE dll = LoadLibraryW(L"webauthn.dll");
  if (!dll) {
    std::printf("%s no webauthn.dll\n", mode.c_str());
    return 3;
  }
  g_get = reinterpret_cast<decltype(g_get)>(
      GetProcAddress(dll, "WebAuthNAuthenticatorGetAssertion"));
  g_get_id = reinterpret_cast<decltype(g_get_id)>(
      GetProcAddress(dll, "WebAuthNGetCancellationId"));
  g_cancel = reinterpret_cast<decltype(g_cancel)>(
      GetProcAddress(dll, "WebAuthNCancelCurrentOperation"));
  if (!g_get || !g_get_id || !g_cancel) {
    std::printf("%s missing exports\n", mode.c_str());
    return 3;
  }

  WNDCLASSW wc{};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"pkprobe";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, L"pkprobe", L"pkprobe", WS_OVERLAPPEDWINDOW,
                              100, 100, 400, 300, nullptr, nullptr,
                              wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOW);

  static GUID id;
  g_get_id(&id);
  DWORD os_timeout = mode == "timeout-only" ? 3000 : 12000;

  std::thread watchdog([mode] {
    for (int i = 0; i < 450 && !g_done.load(); ++i)
      Sleep(100);
    if (!g_done.load()) {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::printf("%-16s HUNG after %lld ms;%s\n", mode.c_str(), Ms(),
                  g_cancels.c_str());
      std::fflush(stdout);
      TerminateProcess(GetCurrentProcess(), 99);
    }
  });
  watchdog.detach();

  g_t0 = std::chrono::steady_clock::now();
  if (mode == "cancel-before")
    Cancel(&id);

  std::thread worker([hwnd, os_timeout, mode, arg] {
    const char* json =
        "{\"type\":\"webauthn.get\",\"challenge\":\"AAAAAAAAAAAAAAAAAAAAAA\","
        "\"origin\":\"https://example.com\",\"crossOrigin\":false}";
    WEBAUTHN_CLIENT_DATA cd{};
    cd.dwVersion = 1;
    cd.cbClientDataJSON = static_cast<DWORD>(std::strlen(json));
    cd.pbClientDataJSON = reinterpret_cast<BYTE*>(const_cast<char*>(json));
    cd.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;
    WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS o{};
    o.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_4;
    o.dwTimeoutMilliseconds = os_timeout;
    o.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    o.dwUserVerificationRequirement =
        WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
    o.pCancellationId = &id;
    std::thread canceller;
    if (mode == "cancel-at" || mode == "cancel-repeat") {
      canceller = std::thread([mode, arg] {
        Sleep(arg);
        Cancel(&id);
        if (mode == "cancel-repeat") {
          while (!g_done.load()) {
            Sleep(250);
            if (!g_done.load())
              Cancel(&id);
          }
        }
      });
    }
    long long start = Ms();
    PWEBAUTHN_ASSERTION a = nullptr;
    HRESULT hr = g_get(hwnd, L"example.com", &cd, &o, &a);
    g_done.store(true);
    long long end = Ms();
    if (canceller.joinable())
      canceller.join();
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::printf("%-16s %5d took %6lld ms (called @%lld) hr=0x%08lX;%s\n",
                mode.c_str(), arg, end - start, start,
                static_cast<unsigned long>(hr), g_cancels.c_str());
    std::fflush(stdout);
    PostMessageW(hwnd, WM_QUIT, 0, 0);
  });

  MSG msg;
  while (!g_done.load()) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(10);
  }
  worker.join();
  // The dialog may still be closing; end outright.
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}
