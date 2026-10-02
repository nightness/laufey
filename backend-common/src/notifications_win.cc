// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications on Windows (API 41): toast notifications through the WinRT
// ToastNotificationManager, for unpackaged apps.
//
// An unpackaged Win32 app has no package identity, so it registers its own,
// per user, the way the Windows App SDK and the Community Toolkit do it:
//
//   HKCU\Software\Classes\AppUserModelId\<AUMID>
//       DisplayName      the app's name (version resource, else the exe's)
//       IconUri          a PNG of the executable's icon (in the temp folder)
//       CustomActivator  {CLSID}
//   HKCU\Software\Classes\CLSID\{CLSID}\LocalServer32
//       (default)        "<exe>" -ToastActivated
//
// The AUMID is LAUFEY_APP_ID (or the launch file's "appId"), else the
// executable's name; the CLSID is derived from it, so it is stable across
// runs and differs between apps. No Start menu shortcut is needed. At launch
// (InitNotificationsAtLaunch) the process registers the activator class
// object (CoRegisterClassObject), so a click on a toast reaches
// INotificationActivationCallback::Activate: in this process when it runs,
// or, when it doesn't, in the process COM starts from LocalServer32 (with
// "-ToastActivated -Embedding" on its command line): the cold-start click.
//
// Toasts carry their tag, action and "data" in the activation arguments
// (EncodeToastArguments), so a click needs nothing but the arguments. A
// scheduled notification is a ScheduledToastNotification: Windows delivers
// it at its time whether or not the app runs.
//
// Every WinRT / COM call runs on one STA thread owned by this file (see
// ToastThread for why not the MTA).

#include "laufey_launch_config.h"
#include "laufey_notifications.h"
#include "laufey_system.h"

#include <windows.h>
#include <NotificationActivationCallback.h>
#include <objbase.h>
#include <roapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace laufey_common {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace winui = ABI::Windows::UI::Notifications;
namespace xml = ABI::Windows::Data::Xml::Dom;
namespace wf = ABI::Windows::Foundation;

const wchar_t kGroup[] = L"laufey";
// Windows shows at most five buttons.
constexpr size_t kMaxActions = 5;

// --- The toast thread ---
//
// A single-threaded apartment with its own message loop. Not the MTA: an
// MTA that lives as long as the process makes every COM-uninitialized
// thread an implicit MTA member, and a DLL that releases an out-of-process
// object while the process exits (Windows.Media.dll, which Chromium loads for
// the media keys, does) then waits in an MTA call no RPC thread is left to
// answer: ExitProcess hangs. In an STA the activator's incoming calls arrive
// as messages this loop dispatches.

constexpr UINT kRunTasks = WM_APP + 0x4e;

class ToastThread {
 public:
  static ToastThread& Get() {
    static ToastThread* t = new ToastThread();  // never destroyed
    return *t;
  }

  // Runs `fn` on the thread and waits.
  void RunSync(const std::function<void()>& fn) {
    if (GetCurrentThreadId() == thread_id_) {
      fn();
      return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    Post([&] {
      fn();
      // Notify under the lock: once `done` is seen the waiter returns and
      // m / cv (its stack frame) are gone.
      std::lock_guard<std::mutex> lock(m);
      done = true;
      cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return done; });
  }

  void Post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(fn));
    }
    PostThreadMessageW(thread_id_, kRunTasks, 0, 0);
  }

 private:
  ToastThread() {
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    std::thread([this, &m, &cv, &started] {
      RoInitialize(RO_INIT_SINGLETHREADED);
      // Create the message queue before anyone posts to it.
      MSG msg;
      PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      {
        std::lock_guard<std::mutex> lock(m);
        thread_id_ = GetCurrentThreadId();
        started = true;
        cv.notify_one();
      }
      Loop();
    }).detach();
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return started; });
  }

  void Loop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.hwnd == nullptr && msg.message == kRunTasks) {
        RunTasks();
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  // Runs the queued tasks. Not re-entered: a COM call a task makes may pump
  // this thread's messages, and a nested run would interleave tasks; the
  // outer run picks up whatever was queued meanwhile.
  void RunTasks() {
    if (running_)
      return;
    running_ = true;
    for (;;) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.empty())
          break;
        fn = std::move(tasks_.front());
        tasks_.pop_front();
      }
      fn();
    }
    running_ = false;
  }

  DWORD thread_id_ = 0;
  bool running_ = false;  // toast thread only
  std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;
};

// --- Identity ---

std::string Narrow(HSTRING h) {
  UINT32 len = 0;
  const wchar_t* raw = WindowsGetStringRawBuffer(h, &len);
  return raw ? WideToUtf8(std::wstring(raw, len)) : std::string();
}

void SetHString(HString* h, const std::wstring& s) {
  h->Set(s.c_str(), static_cast<unsigned>(s.size()));
}

uint64_t Fnv64(const std::string& s, uint64_t seed) {
  uint64_t h = seed;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string Hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(v));
  return buf;
}

constexpr uint64_t kFnvBasis = 1469598103934665603ull;

// The AppUserModelID: the app id (or the executable's name), reduced to the
// characters an AUMID may hold, at most 128.
const std::wstring& Aumid() {
  static const std::wstring aumid = [] {
    std::string name = LoginItemName();
    std::string out;
    for (char c : name) {
      bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
      out += ok ? c : '_';
    }
    if (out.empty())
      out = "laufey.app";
    if (out.size() > 128)
      out.resize(128);
    return Utf8ToWide(out);
  }();
  return aumid;
}

// A name-based GUID for the activator: two FNV-1a hashes of the AUMID, with
// the RFC 4122 version (5) and variant bits.
const GUID& ActivatorClsid() {
  static const GUID clsid = [] {
    std::string key = "laufey-toast-activator:" + WideToUtf8(Aumid());
    uint64_t a = Fnv64(key, kFnvBasis);
    uint64_t b = Fnv64(key, 0x84222325cbf29ce4ull ^ a);
    GUID g;
    g.Data1 = static_cast<unsigned long>(a >> 32);
    g.Data2 = static_cast<unsigned short>(a >> 16);
    g.Data3 = static_cast<unsigned short>((a & 0x0fff) | 0x5000);
    for (int i = 0; i < 8; ++i)
      g.Data4[i] = static_cast<unsigned char>(b >> (56 - 8 * i));
    g.Data4[0] = static_cast<unsigned char>((g.Data4[0] & 0x3f) | 0x80);
    return g;
  }();
  return clsid;
}

std::wstring ClsidString() {
  wchar_t buf[64] = {};
  StringFromGUID2(ActivatorClsid(), buf, 64);
  return buf;
}

std::wstring ExePathW() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, path.data(),
                                 static_cast<DWORD>(path.size()));
    if (n == 0)
      return std::wstring();
    if (n < path.size()) {
      path.resize(n);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

// The app's display name: the version resource's FileDescription (else
// ProductName), else the executable's name.
std::wstring DisplayName() {
  std::wstring exe = ExePathW();
  DWORD ignored = 0;
  DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
  if (size > 0) {
    std::vector<BYTE> info(size);
    if (GetFileVersionInfoW(exe.c_str(), 0, size, info.data())) {
      struct LangCp {
        WORD lang;
        WORD cp;
      }* trans = nullptr;
      UINT trans_len = 0;
      if (VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation",
                         reinterpret_cast<void**>(&trans), &trans_len) &&
          trans && trans_len >= sizeof(LangCp)) {
        for (const wchar_t* field : {L"FileDescription", L"ProductName"}) {
          wchar_t query[128];
          swprintf(query, 128, L"\\StringFileInfo\\%04x%04x\\%ls", trans->lang,
                   trans->cp, field);
          wchar_t* value = nullptr;
          UINT len = 0;
          if (VerQueryValueW(info.data(), query,
                             reinterpret_cast<void**>(&value), &len) &&
              value && len > 1)
            return std::wstring(value, len - 1);
        }
      }
    }
  }
  size_t slash = exe.find_last_of(L"\\/");
  std::wstring base = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
  size_t dot = base.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0)
    base.resize(dot);
  return base.empty() ? Aumid() : base;
}

std::wstring TempDir() {
  wchar_t buf[MAX_PATH + 1] = {};
  DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  std::wstring dir = n ? std::wstring(buf, n) : std::wstring(L".\\");
  dir += L"laufey-notifications\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t len) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  bool ok = WriteFile(f, data, static_cast<DWORD>(len), &written, nullptr) &&
            written == len;
  CloseHandle(f);
  return ok;
}

// The executable's icon as a PNG file (for the AUMID's IconUri), or "".
std::wstring ExeIconPng() {
  std::wstring path = TempDir() +
                      Utf8ToWide(Hex16(Fnv64(WideToUtf8(Aumid()), kFnvBasis))) +
                      L"-app.png";
  HICON icon = nullptr;
  if (ExtractIconExW(ExePathW().c_str(), 0, &icon, nullptr, 1) == 0 || !icon)
    return std::wstring();
  std::wstring result;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateBitmapFromHICON(icon, &bitmap)) &&
      SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
      SUCCEEDED(
          factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
      SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
      SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
      SUCCEEDED(frame->Initialize(nullptr)) &&
      SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
      SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
    HGLOBAL global = nullptr;
    if (SUCCEEDED(GetHGlobalFromStream(stream.Get(), &global))) {
      SIZE_T size = GlobalSize(global);
      void* bytes = GlobalLock(global);
      if (bytes && WriteFileBytes(path, bytes, size))
        result = path;
      GlobalUnlock(global);
    }
  }
  DestroyIcon(icon);
  return result;
}

bool ReadRegString(HKEY root, const std::wstring& key, const wchar_t* name,
                   std::wstring* out) {
  wchar_t buf[2048];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf,
                   &size) != ERROR_SUCCESS)
    return false;
  *out = buf;
  return true;
}

bool WriteRegString(HKEY root, const std::wstring& key, const wchar_t* name,
                    const std::wstring& value) {
  std::wstring current;
  if (ReadRegString(root, key, name, &current) && current == value)
    return true;
  HKEY h = nullptr;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                      &h, nullptr) != ERROR_SUCCESS)
    return false;
  LONG rc = RegSetValueExW(
      h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(h);
  return rc == ERROR_SUCCESS;
}

// Writes the AUMID and activator registration (only what changed). Toast
// thread.
bool EnsureRegistered() {
  static int state = 0;  // 0 not tried, 1 ok, -1 failed
  if (state != 0)
    return state > 0;
  std::wstring aumid_key = L"Software\\Classes\\AppUserModelId\\" + Aumid();
  std::wstring clsid = ClsidString();
  std::wstring server_key = L"Software\\Classes\\CLSID\\" + clsid;
  std::wstring command = L"\"" + ExePathW() + L"\" -ToastActivated";
  std::wstring name = DisplayName();
  bool ok =
      WriteRegString(HKEY_CURRENT_USER, aumid_key, L"DisplayName", name) &&
      WriteRegString(HKEY_CURRENT_USER, aumid_key, L"CustomActivator", clsid) &&
      WriteRegString(HKEY_CURRENT_USER, server_key, nullptr,
                     name + L" notification activator") &&
      WriteRegString(HKEY_CURRENT_USER, server_key, L"LaufeyAppUserModelId",
                     Aumid()) &&
      WriteRegString(HKEY_CURRENT_USER, server_key + L"\\LocalServer32",
                     nullptr, command);
  if (ok) {
    std::wstring icon = ExeIconPng();
    if (!icon.empty())
      WriteRegString(HKEY_CURRENT_USER, aumid_key, L"IconUri", icon);
  } else {
    std::cerr << "laufey: could not register the app for notifications"
              << std::endl;
  }
  state = ok ? 1 : -1;
  return ok;
}

// --- Activation ---

// A click can reach this process twice (the toast's Activated event and the
// COM activator): the first one wins for a few seconds.
bool FirstDelivery(const std::string& args) {
  static std::mutex m;
  static std::map<std::string, std::chrono::steady_clock::time_point> seen;
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(m);
  for (auto it = seen.begin(); it != seen.end();) {
    if (now - it->second > std::chrono::seconds(5))
      it = seen.erase(it);
    else
      ++it;
  }
  return seen.emplace(args, now).second;
}

// Whether COM started this process for a toast click (LocalServer32 runs it
// with -ToastActivated) and that click hasn't been delivered yet: it is the
// launch, however soon the app registered its response handler.
std::atomic<bool> g_launch_click_pending{false};

bool StartedForToastClick() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    return false;
  bool found = false;
  for (int i = 1; i < argc && !found; ++i)
    found = _wcsicmp(argv[i], L"-ToastActivated") == 0;
  LocalFree(argv);
  return found;
}

void HandleActivationArguments(const std::string& args) {
  std::string tag, action, data;
  bool has_action = false, has_data = false;
  if (!DecodeToastArguments(args, &tag, &action, &has_action, &data, &has_data))
    return;
  if (!FirstDelivery(args))
    return;
  bool launch = g_launch_click_pending.exchange(false);
  DispatchNotificationClick(tag, has_action ? action.c_str() : nullptr,
                            has_data ? &data : nullptr, launch);
}

class ToastActivator : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                           INotificationActivationCallback> {
 public:
  IFACEMETHODIMP Activate(LPCWSTR /*app_user_model_id*/, LPCWSTR invoked_args,
                          const NOTIFICATION_USER_INPUT_DATA* /*data*/,
                          ULONG /*count*/) override {
    if (invoked_args)
      HandleActivationArguments(WideToUtf8(invoked_args));
    return S_OK;
  }
};

class ToastActivatorFactory
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
 public:
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                                void** ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;
    if (outer)
      return CLASS_E_NOAGGREGATION;
    ComPtr<ToastActivator> activator = Make<ToastActivator>();
    return activator ? activator.CopyTo(riid, ppv) : E_OUTOFMEMORY;
  }
  IFACEMETHODIMP LockServer(BOOL /*lock*/) override {
    return S_OK;
  }
};

// --- Toast XML ---

std::wstring XmlEscape(const std::string& s) {
  std::wstring w = Utf8ToWide(s), out;
  for (wchar_t c : w) {
    switch (c) {
      case L'&':
        out += L"&amp;";
        break;
      case L'<':
        out += L"&lt;";
        break;
      case L'>':
        out += L"&gt;";
        break;
      case L'"':
        out += L"&quot;";
        break;
      case L'\'':
        out += L"&apos;";
        break;
      default:
        // XML 1.0 forbids most control characters.
        if (c < 0x20 && c != L'\t' && c != L'\n' && c != L'\r')
          out += L' ';
        else
          out += c;
    }
  }
  return out;
}

std::wstring IconFile(const std::vector<uint8_t>& png) {
  if (png.empty())
    return std::wstring();
  std::string bytes(png.begin(), png.end());
  std::wstring path =
      TempDir() + Utf8ToWide(Hex16(Fnv64(bytes, kFnvBasis))) + L".png";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
      !WriteFileBytes(path, png.data(), png.size()))
    return std::wstring();
  return path;
}

std::wstring ToastXml(const NotificationOptions& opts) {
  const std::string* data = opts.has_data ? &opts.data : nullptr;
  std::wstring x = L"<toast launch=\"" +
                   XmlEscape(EncodeToastArguments(opts.tag, nullptr, data)) +
                   L"\"";
  if (opts.require_interaction)
    x += L" duration=\"long\"";
  x += L"><visual><binding template=\"ToastGeneric\"><text>" +
       XmlEscape(opts.title) + L"</text>";
  if (!opts.body.empty())
    x += L"<text>" + XmlEscape(opts.body) + L"</text>";
  std::wstring icon = IconFile(opts.icon_png);
  if (!icon.empty()) {
    std::wstring uri = L"file:///" + icon;
    for (wchar_t& c : uri) {
      if (c == L'\\')
        c = L'/';
    }
    x += L"<image placement=\"appLogoOverride\" src=\"" +
         XmlEscape(WideToUtf8(uri)) + L"\"/>";
  }
  x += L"</binding></visual>";
  if (opts.silent)
    x += L"<audio silent=\"true\"/>";
  if (!opts.actions.empty()) {
    x += L"<actions>";
    for (size_t i = 0; i < opts.actions.size() && i < kMaxActions; ++i) {
      const NotificationAction& a = opts.actions[i];
      x += L"<action content=\"" + XmlEscape(a.title) + L"\" arguments=\"" +
           XmlEscape(EncodeToastArguments(opts.tag, a.id.c_str(), data)) +
           L"\" activationType=\"foreground\"/>";
    }
    x += L"</actions>";
  }
  x += L"</toast>";
  return x;
}

// Windows' Tag holds at most 64 characters.
std::wstring WinTag(const std::string& tag) {
  std::wstring w = Utf8ToWide(tag);
  if (w.size() <= 64)
    return w;
  return Utf8ToWide("h" + Hex16(Fnv64(tag, kFnvBasis)));
}

// A ScheduledToastNotification's Id holds at most 16 characters.
std::string ScheduleId(const std::string& tag) {
  return Hex16(Fnv64(tag, kFnvBasis));
}

wf::DateTime ToDateTime(int64_t unix_ms) {
  wf::DateTime t;
  t.UniversalTime = (unix_ms + 11644473600000LL) * 10000LL;
  return t;
}

int64_t FromDateTime(const wf::DateTime& t) {
  return t.UniversalTime / 10000LL - 11644473600000LL;
}

template <typename T>
HRESULT GetFactory(const wchar_t* class_id, T** out) {
  return RoGetActivationFactory(HStringReference(class_id).Get(),
                                IID_PPV_ARGS(out));
}

HRESULT LoadXml(const std::wstring& text, ComPtr<xml::IXmlDocument>* out) {
  ComPtr<IInspectable> inspectable;
  HRESULT hr = RoActivateInstance(
      HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
      &inspectable);
  if (FAILED(hr))
    return hr;
  ComPtr<xml::IXmlDocumentIO> io;
  hr = inspectable.As(&io);
  if (FAILED(hr))
    return hr;
  HString h;
  SetHString(&h, text);
  hr = io->LoadXml(h.Get());
  if (FAILED(hr))
    return hr;
  return inspectable.As(out);
}

std::string Attribute(xml::IXmlElement* el, const wchar_t* name) {
  HString value;
  if (!el || FAILED(el->GetAttribute(HStringReference(name).Get(),
                                     value.GetAddressOf())))
    return std::string();
  return Narrow(value.Get());
}

// The texts, launch arguments and actions of a toast's XML. False for a
// toast that isn't laufey's.
bool ReadToastXml(xml::IXmlDocument* doc, ScheduledNotification* n) {
  ComPtr<xml::IXmlElement> root;
  if (FAILED(doc->get_DocumentElement(&root)) || !root)
    return false;
  std::string action;
  bool has_action = false;
  if (!DecodeToastArguments(Attribute(root.Get(), L"launch"), &n->tag, &action,
                            &has_action, &n->data, &n->has_data))
    return false;
  ComPtr<xml::IXmlNodeList> texts;
  if (SUCCEEDED(
          doc->GetElementsByTagName(HStringReference(L"text").Get(), &texts))) {
    UINT32 count = 0;
    texts->get_Length(&count);
    for (UINT32 i = 0; i < count && i < 2; ++i) {
      ComPtr<xml::IXmlNode> node;
      ComPtr<xml::IXmlNodeSerializer> ser;
      HString text;
      if (SUCCEEDED(texts->Item(i, &node)) && SUCCEEDED(node.As(&ser)) &&
          SUCCEEDED(ser->get_InnerText(text.GetAddressOf())))
        (i == 0 ? n->title : n->body) = Narrow(text.Get());
    }
  }
  ComPtr<xml::IXmlNodeList> actions;
  if (SUCCEEDED(doc->GetElementsByTagName(HStringReference(L"action").Get(),
                                          &actions))) {
    UINT32 count = 0;
    actions->get_Length(&count);
    for (UINT32 i = 0; i < count; ++i) {
      ComPtr<xml::IXmlNode> node;
      ComPtr<xml::IXmlElement> el;
      if (FAILED(actions->Item(i, &node)) || FAILED(node.As(&el)))
        continue;
      std::string atag, aid, adata;
      bool has_aid = false, has_adata = false;
      if (DecodeToastArguments(Attribute(el.Get(), L"arguments"), &atag, &aid,
                               &has_aid, &adata, &has_adata) &&
          has_aid)
        n->actions.push_back({aid, Attribute(el.Get(), L"content")});
    }
  }
  ComPtr<xml::IXmlNodeList> audio;
  if (SUCCEEDED(doc->GetElementsByTagName(HStringReference(L"audio").Get(),
                                          &audio))) {
    UINT32 count = 0;
    audio->get_Length(&count);
    ComPtr<xml::IXmlNode> node;
    ComPtr<xml::IXmlElement> el;
    if (count > 0 && SUCCEEDED(audio->Item(0, &node)) &&
        SUCCEEDED(node.As(&el)))
      n->silent = Attribute(el.Get(), L"silent") == "true";
  }
  return true;
}

// --- The platform ---

struct LiveToast {
  ComPtr<winui::IToastNotification> toast;
  EventRegistrationToken activated{}, dismissed{}, failed{};
};

using ActivatedHandler =
    wf::ITypedEventHandler<winui::ToastNotification*, IInspectable*>;
using DismissedHandler =
    wf::ITypedEventHandler<winui::ToastNotification*,
                           winui::ToastDismissedEventArgs*>;
using FailedHandler = wf::ITypedEventHandler<winui::ToastNotification*,
                                             winui::ToastFailedEventArgs*>;
using ScheduledList =
    wf::Collections::IVectorView<winui::ScheduledToastNotification*>;

class WinNotificationPlatform : public NotificationPlatform {
 public:
  uint32_t Capabilities() override {
    uint32_t caps = 0;
    ToastThread::Get().RunSync([&] {
      if (Notifier())
        caps = LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE |
               LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS |
               LAUFEY_NOTIFICATION_CAP_ACTIONS |
               LAUFEY_NOTIFICATION_CAP_CLICKS |
               LAUFEY_NOTIFICATION_CAP_COLD_START;
    });
    return caps;
  }

  bool Show(const NotificationOptions& opts) override {
    bool ok = false;
    ToastThread::Get().RunSync([&] { ok = ShowOnThread(opts); });
    return ok;
  }

  void Remove(const std::string& tag) override {
    ToastThread::Get().RunSync([&] { RemoveOnThread(tag, true); });
  }

  void ListScheduled(
      std::function<void(std::vector<ScheduledNotification>)> done) override {
    std::vector<ScheduledNotification> list;
    ToastThread::Get().RunSync([&] { list = ListOnThread(); });
    done(std::move(list));
  }

  void QueryPermission(int /*kind*/, std::function<void(int)> done) override {
    int status = LAUFEY_PERMISSION_STATUS_UNSUPPORTED;
    ToastThread::Get().RunSync([&] {
      ComPtr<winui::IToastNotifier> notifier = Notifier();
      if (!notifier)
        return;
      winui::NotificationSetting setting;
      HRESULT hr = notifier->get_Setting(&setting);
      if (SUCCEEDED(hr)) {
        status = setting == winui::NotificationSetting_Enabled
                     ? LAUFEY_PERMISSION_STATUS_GRANTED
                     : LAUFEY_PERMISSION_STATUS_DENIED;
        return;
      }
      // An unpackaged app's notifier can't always read its setting
      // (ERROR_NOT_FOUND until the app has posted once): read the same
      // switches from where Settings keeps them.
      status = SettingFromRegistry();
    });
    done(status);
  }

  // The user's switches for toasts: all of them (PushNotifications
  // ToastEnabled) and this app's (Notifications\Settings\<AUMID> Enabled).
  static int SettingFromRegistry() {
    auto read_dword = [](const std::wstring& key, const wchar_t* name,
                         DWORD* out) {
      DWORD size = sizeof(DWORD);
      return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name,
                          RRF_RT_REG_DWORD, nullptr, out,
                          &size) == ERROR_SUCCESS;
    };
    DWORD value = 1;
    if (read_dword(L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                   L"PushNotifications",
                   L"ToastEnabled", &value) &&
        value == 0)
      return LAUFEY_PERMISSION_STATUS_DENIED;
    if (read_dword(L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                   L"Notifications\\Settings\\" +
                       Aumid(),
                   L"Enabled", &value) &&
        value == 0)
      return LAUFEY_PERMISSION_STATUS_DENIED;
    return LAUFEY_PERMISSION_STATUS_GRANTED;
  }

  // Windows has no permission prompt for toasts: the user turns them on or
  // off in Settings.
  void RequestPermission(int kind, std::function<void(int)> done) override {
    QueryPermission(kind, std::move(done));
  }

 private:
  // Toast thread.
  ComPtr<winui::IToastNotifier> Notifier() {
    if (notifier_ || notifier_failed_)
      return notifier_;
    notifier_failed_ = true;
    if (!EnsureRegistered())
      return nullptr;
    ComPtr<winui::IToastNotificationManagerStatics> statics;
    if (FAILED(GetFactory(
            RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
            statics.GetAddressOf())))
      return nullptr;
    HString aumid;
    SetHString(&aumid, Aumid());
    if (FAILED(statics->CreateToastNotifierWithId(aumid.Get(), &notifier_)))
      return nullptr;
    statics.As(&statics2_);
    notifier_failed_ = false;
    return notifier_;
  }

  bool ShowOnThread(const NotificationOptions& opts) {
    ComPtr<winui::IToastNotifier> notifier = Notifier();
    if (!notifier)
      return false;
    ComPtr<xml::IXmlDocument> doc;
    HRESULT hr = LoadXml(ToastXml(opts), &doc);
    if (FAILED(hr)) {
      std::cerr << "laufey: toast XML rejected (0x" << std::hex << hr
                << std::dec << ")" << std::endl;
      return false;
    }
    HString tag, group;
    SetHString(&tag, WinTag(opts.tag));
    group.Set(kGroup);

    // The same tag replaces: drop a pending or live one first.
    RemoveOnThread(opts.tag, false);

    if (opts.schedule_at_ms > 0) {
      ComPtr<winui::IScheduledToastNotificationFactory> factory;
      ComPtr<winui::IScheduledToastNotification> scheduled;
      if (FAILED(GetFactory(
              RuntimeClass_Windows_UI_Notifications_ScheduledToastNotification,
              factory.GetAddressOf())) ||
          FAILED(factory->CreateScheduledToastNotification(
              doc.Get(), ToDateTime(opts.schedule_at_ms), &scheduled)))
        return false;
      HString hid;
      SetHString(&hid, Utf8ToWide(ScheduleId(opts.tag)));
      scheduled->put_Id(hid.Get());
      ComPtr<winui::IScheduledToastNotification2> scheduled2;
      if (SUCCEEDED(scheduled.As(&scheduled2))) {
        scheduled2->put_Tag(tag.Get());
        scheduled2->put_Group(group.Get());
      }
      hr = notifier->AddToSchedule(scheduled.Get());
      if (FAILED(hr)) {
        std::cerr << "laufey: scheduling the toast failed (0x" << std::hex << hr
                  << std::dec << ")" << std::endl;
        return false;
      }
      return true;
    }

    ComPtr<winui::IToastNotificationFactory> factory;
    ComPtr<winui::IToastNotification> toast;
    if (FAILED(
            GetFactory(RuntimeClass_Windows_UI_Notifications_ToastNotification,
                       factory.GetAddressOf())) ||
        FAILED(factory->CreateToastNotification(doc.Get(), &toast)))
      return false;
    ComPtr<winui::IToastNotification2> toast2;
    if (SUCCEEDED(toast.As(&toast2))) {
      toast2->put_Tag(tag.Get());
      toast2->put_Group(group.Get());
    }
    std::string t = opts.tag;
    LiveToast live;
    live.toast = toast;
    toast->add_Activated(
        Callback<ActivatedHandler>([](winui::IToastNotification*,
                                      IInspectable* args) -> HRESULT {
          ComPtr<winui::IToastActivatedEventArgs> activated;
          HString arguments;
          if (args &&
              SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&activated))) &&
              SUCCEEDED(activated->get_Arguments(arguments.GetAddressOf())))
            HandleActivationArguments(Narrow(arguments.Get()));
          return S_OK;
        }).Get(),
        &live.activated);
    toast->add_Dismissed(
        Callback<DismissedHandler>([this, t](winui::IToastNotification*,
                                             winui::IToastDismissedEventArgs*)
                                       -> HRESULT {
          // UserCanceled, ApplicationHidden, or TimedOut (moved to the
          // notification center, where a later click still reaches the
          // activator and the response handler).
          ToastThread::Get().Post([this, t] { Forget(t); });
          DispatchNotificationClosed(t);
          return S_OK;
        }).Get(),
        &live.dismissed);
    toast->add_Failed(
        Callback<FailedHandler>([this, t](
                                    winui::IToastNotification*,
                                    winui::IToastFailedEventArgs*) -> HRESULT {
          ToastThread::Get().Post([this, t] { Forget(t); });
          DispatchNotificationClosed(t);
          return S_OK;
        }).Get(),
        &live.failed);
    hr = notifier->Show(toast.Get());
    if (FAILED(hr)) {
      std::cerr << "laufey: showing the toast failed (0x" << std::hex << hr
                << std::dec << ")" << std::endl;
      return false;
    }
    live_[opts.tag] = live;
    // A clicked toast raises no Dismissed: bound what stays tracked.
    while (live_.size() > 256)
      Forget(live_.begin()->first);
    // Toasts have no "shown" event; Show() succeeding is the closest.
    ToastThread::Get().Post([t] { DispatchNotificationShown(t); });
    return true;
  }

  // Toast thread.
  void Forget(const std::string& tag) {
    auto it = live_.find(tag);
    if (it == live_.end())
      return;
    it->second.toast->remove_Activated(it->second.activated);
    it->second.toast->remove_Dismissed(it->second.dismissed);
    it->second.toast->remove_Failed(it->second.failed);
    live_.erase(it);
  }

  void RemoveOnThread(const std::string& tag, bool history) {
    ComPtr<winui::IToastNotifier> notifier = Notifier();
    if (!notifier)
      return;
    auto it = live_.find(tag);
    if (it != live_.end()) {
      ComPtr<winui::IToastNotification> toast = it->second.toast;
      Forget(tag);
      notifier->Hide(toast.Get());
    }
    // Pending: matched by the tag in the toast's own arguments (the Id a
    // ScheduledToastNotification is given isn't reliably read back for an
    // unpackaged app). Collected first: removing changes the list.
    std::vector<ComPtr<winui::IScheduledToastNotification>> doomed;
    ComPtr<ScheduledList> scheduled;
    if (SUCCEEDED(notifier->GetScheduledToastNotifications(&scheduled))) {
      UINT32 count = 0;
      scheduled->get_Size(&count);
      for (UINT32 i = 0; i < count; ++i) {
        ComPtr<winui::IScheduledToastNotification> s;
        ComPtr<xml::IXmlDocument> doc;
        ScheduledNotification n;
        if (SUCCEEDED(scheduled->GetAt(i, &s)) &&
            SUCCEEDED(s->get_Content(&doc)) && ReadToastXml(doc.Get(), &n) &&
            n.tag == tag)
          doomed.push_back(s);
      }
    }
    for (const auto& s : doomed) {
      HRESULT hr = notifier->RemoveFromSchedule(s.Get());
      if (FAILED(hr)) {
        std::cerr << "laufey: unscheduling the toast failed (0x" << std::hex
                  << hr << std::dec << ")" << std::endl;
      }
    }
    if (history && statics2_) {
      ComPtr<winui::IToastNotificationHistory> hist;
      if (SUCCEEDED(statics2_->get_History(&hist))) {
        HString htag, group, aumid;
        SetHString(&htag, WinTag(tag));
        group.Set(kGroup);
        SetHString(&aumid, Aumid());
        hist->RemoveGroupedTagWithId(htag.Get(), group.Get(), aumid.Get());
      }
    }
  }

  std::vector<ScheduledNotification> ListOnThread() {
    std::vector<ScheduledNotification> list;
    ComPtr<winui::IToastNotifier> notifier = Notifier();
    if (!notifier)
      return list;
    ComPtr<ScheduledList> scheduled;
    if (FAILED(notifier->GetScheduledToastNotifications(&scheduled)))
      return list;
    UINT32 count = 0;
    scheduled->get_Size(&count);
    for (UINT32 i = 0; i < count; ++i) {
      ComPtr<winui::IScheduledToastNotification> s;
      ComPtr<xml::IXmlDocument> doc;
      if (FAILED(scheduled->GetAt(i, &s)) || FAILED(s->get_Content(&doc)))
        continue;
      ScheduledNotification n;
      if (!ReadToastXml(doc.Get(), &n))
        continue;
      wf::DateTime when;
      if (SUCCEEDED(s->get_DeliveryTime(&when)))
        n.at_ms = FromDateTime(when);
      list.push_back(std::move(n));
    }
    return list;
  }

  ComPtr<winui::IToastNotifier> notifier_;
  ComPtr<winui::IToastNotificationManagerStatics2> statics2_;
  bool notifier_failed_ = false;
  std::map<std::string, LiveToast> live_;  // by tag; toast thread
};

}  // namespace

std::unique_ptr<NotificationPlatform> CreateNotificationPlatform() {
  return std::make_unique<WinNotificationPlatform>();
}

void InitNotificationsAtLaunch() {
  static std::once_flag once;
  std::call_once(once, [] {
    g_launch_click_pending = StartedForToastClick();
    // Register the activator class object in the toast thread's apartment:
    // from now on a click on one of this app's toasts is delivered here.
    ToastThread::Get().RunSync([] {
      static DWORD cookie = 0;
      ComPtr<ToastActivatorFactory> factory = Make<ToastActivatorFactory>();
      HRESULT hr = CoRegisterClassObject(ActivatorClsid(), factory.Get(),
                                         CLSCTX_LOCAL_SERVER,
                                         REGCLS_MULTIPLEUSE, &cookie);
      if (FAILED(hr)) {
        std::cerr << "laufey: notification activator not registered (0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
      }
    });
  });
}

}  // namespace laufey_common
