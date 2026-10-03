// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications (API 41): the portable core every CEF and WebView backend
// shares, over one per-OS NotificationPlatform:
//
//   notifications_mac.mm    UNUserNotificationCenter (calendar /
//                           time-interval triggers, categories, the delegate
//                           installed at launch for cold-start clicks)
//   notifications_win.cc    toast notifications (ScheduledToastNotification,
//                           the per-user AppUserModelID registration and its
//                           COM activator for clicks while not running)
//   notifications_linux.cc  org.freedesktop.Notifications over D-Bus, with
//                           laufey's own scheduler (persisted and re-armed at
//                           launch)
//
// The core keeps the notifications this process shows (their callbacks, by
// tag), routes the OS's events to them, sends clicks no live callback owns to
// the response handler (buffering them until one is registered: the
// cold-start click), validates options and builds the JSON the C ABI hands
// out. See docs/notifications.md.

#ifndef LAUFEY_NOTIFICATIONS_H_
#define LAUFEY_NOTIFICATIONS_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_backend_common.h"

namespace laufey_common {

// A notification the OS holds for later delivery (or the store re-arms).
struct ScheduledNotification {
  std::string tag;
  std::string title;
  std::string body;
  int64_t at_ms = 0;  // Unix time
  bool has_data = false;
  std::string data;
  std::vector<NotificationAction> actions;
  bool silent = false;
  bool require_interaction = false;
  std::vector<uint8_t> icon_png;
};

// The OS side, one per process (InstallNotificationPlatform). Every method
// may be called from any thread.
class NotificationPlatform {
 public:
  virtual ~NotificationPlatform() = default;
  // LAUFEY_NOTIFICATION_CAP_* bits.
  virtual uint32_t Capabilities() = 0;
  // Show `opts` now, or schedule it for opts.schedule_at_ms. `opts.tag` is
  // set (the caller's, or a generated one). Returns false when it failed at
  // once; a later failure is reported with DispatchNotificationClosed.
  virtual bool Show(const NotificationOptions& opts) = 0;
  // Remove the delivered notification and the pending request with `tag`.
  virtual void Remove(const std::string& tag) = 0;
  // The pending scheduled notifications this app made, any order.
  virtual void ListScheduled(
      std::function<void(std::vector<ScheduledNotification>)> done) = 0;
  // LAUFEY_PERMISSION_STATUS_* for LAUFEY_PERMISSION_NOTIFICATIONS(_
  // PROVISIONAL); `done` fires exactly once, on any thread.
  virtual void QueryPermission(int kind, std::function<void(int)> done) = 0;
  virtual void RequestPermission(int kind, std::function<void(int)> done) = 0;
};

// Installs the platform (later calls replace it; a replaced platform is
// leaked on purpose, as its threads may still run). Without one every
// notification fails and capabilities are 0.
void InstallNotificationPlatform(std::unique_ptr<NotificationPlatform> p);

// The installed platform, created with CreateNotificationPlatform on first
// use.
NotificationPlatform* EnsureNotificationPlatform();

// The per-OS platform, created on first use by EnsureNotificationPlatform.
std::unique_ptr<NotificationPlatform> CreateNotificationPlatform();

// Starts what must run at launch, before the runtime loads: the macOS
// notification-center delegate (a click that launched the app is delivered
// to it as AppKit finishes launching), the Windows COM activator (registered
// so a click on a toast while the app isn't running can launch it and be
// delivered), the Linux scheduler (re-arming the persisted schedule). Call
// once from each backend's startup; installs the platform. Idempotent.
void InitNotificationsAtLaunch();

// --- The laufey_backend_api_t entry points (see laufey.h) ---

uint32_t ShowNotification(const NotificationOptions& opts,
                          laufey_notification_event_fn on_event,
                          void* user_data);
void CloseNotification(uint32_t notification_id);
uint32_t NotificationCapabilities();
void SetNotificationResponseHandler(laufey_notification_response_fn handler,
                                    void* user_data);
void ListScheduledNotifications(laufey_notification_list_fn callback,
                                void* user_data);
void CancelNotification(const char* tag);
bool TestNotificationRespond(const char* tag, const char* action_id);
void QueryNotificationPermission(int kind, laufey_permission_callback_fn cb,
                                 void* user_data);
void RequestNotificationPermission(int kind, laufey_permission_callback_fn cb,
                                   void* user_data);

// --- From the platform ---

// The OS showed the notification `tag`.
void DispatchNotificationShown(const std::string& tag);
// The user clicked the notification `tag`: its body (`action` null) or an
// action button. `data` is what the OS kept with it (null if unknown; the
// core falls back to what this process recorded). Goes to the live
// notification's callback, else to the response handler (or its buffer).
// Returns true if a callback or handler received it. `launch` marks the
// response as the click that started this process (Windows' cold start) even
// when a handler was already registered when it arrived.
bool DispatchNotificationClick(const std::string& tag, const char* action,
                               const std::string* data, bool launch = false);
// The notification `tag` was dismissed, expired or failed to show.
void DispatchNotificationClosed(const std::string& tag);

// --- Portable helpers (tested) ---

// Checks the API 41 rules: a non-empty title, the tag and data byte limits,
// a tag for "schedule_at", action ids unique. False with `error` set.
bool ValidateNotificationOptions(const NotificationOptions& opts,
                                 std::string* error);

// The tag a notification without one gets: unique to this process and run.
std::string GenerateNotificationTag(uint32_t notification_id);

// Unix time now, in milliseconds.
int64_t UnixTimeMs();

// JSON string literal for `s` (quotes included), escaping per RFC 8259.
std::string JsonQuote(const std::string& s);

// The response object (see laufey_notification_response_fn).
std::string BuildNotificationResponseJson(const std::string& tag,
                                          const char* action,
                                          const std::string* data, bool launch);

// The list_scheduled_notifications array, soonest first.
std::string BuildScheduledListJson(std::vector<ScheduledNotification> list);

// The Linux schedule file (also used by tests): a JSON object
// {"version":1,"notifications":[...]} with the fields of
// ScheduledNotification (the icon base64-encoded).
std::string SerializeSchedule(const std::vector<ScheduledNotification>& list);
bool ParseSchedule(const std::string& text,
                   std::vector<ScheduledNotification>* out, std::string* error);

// The key=value argument string a Windows toast carries back on activation
// (and its parser): tag, action and data, percent-encoded.
std::string EncodeToastArguments(const std::string& tag, const char* action,
                                 const std::string* data);
bool DecodeToastArguments(const std::string& args, std::string* tag,
                          std::string* action, bool* has_action,
                          std::string* data, bool* has_data);

// Base64 (RFC 4648, padded).
std::string Base64Encode(const std::vector<uint8_t>& bytes);
bool Base64Decode(const std::string& text, std::vector<uint8_t>* out);

// Test-only: forget every live notification, buffered response and the
// handler (between test cases).
void ResetNotificationsForTest();

}  // namespace laufey_common

#endif  // LAUFEY_NOTIFICATIONS_H_
