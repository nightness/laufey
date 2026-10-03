// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications on macOS (API 41): UNUserNotificationCenter.
//
// - Delivery now, or at "schedule_at" through a UNTimeIntervalNotification
//   Trigger (under a minute away) or a UNCalendarNotificationTrigger (the
//   wall-clock date, so a sleep in between doesn't shift it). The system
//   holds a scheduled request: it fires whether or not the app runs.
// - The tag is the request identifier (a later request with the same one
//   replaces it), and the userInfo carries the tag, the "data" and the
//   actions, so a click (or the pending list) after a relaunch has them.
// - Actions are a UNNotificationCategory per action set, named by a hash of
//   the set; categories registered by earlier runs are kept (a pending
//   request from one still needs its category).
// - The delegate is installed at launch (InitNotificationsAtLaunch, from each
//   backend's applicationWillFinishLaunching:), so the click that launched
//   the app reaches didReceiveNotificationResponse: and the response buffer.
//
// UN requires the process to run inside a bundled .app with a
// CFBundleIdentifier; unbundled, every capability is off, permissions are
// UNSUPPORTED and showing fails.

#include "laufey_notifications.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace laufey_common {
namespace {

NSString* const kTagKey = @"laufey.tag";
NSString* const kDataKey = @"laufey.data";
NSString* const kActionsKey = @"laufey.actions";

bool MacProcessIsBundled() {
  static const bool bundled = [] {
    NSBundle* mb = [NSBundle mainBundle];
    if (!mb || ![mb bundleIdentifier])
      return false;
    // Reject the synthetic bundle `cargo run` etc. produce for a bare exe.
    NSString* path = [mb bundlePath];
    return path && [path hasSuffix:@".app"];
  }();
  return bundled;
}

NSString* NSStr(const std::string& s) {
  NSString* r = [NSString stringWithUTF8String:s.c_str()];
  return r ? r : @"";
}

std::string StdStr(NSString* s) {
  return s ? std::string([s UTF8String]) : std::string();
}

int MapUNStatus(UNAuthorizationStatus s) {
  switch (s) {
    case UNAuthorizationStatusNotDetermined:
      return LAUFEY_PERMISSION_STATUS_PROMPT;
    case UNAuthorizationStatusDenied:
      return LAUFEY_PERMISSION_STATUS_DENIED;
    case UNAuthorizationStatusAuthorized:
    case UNAuthorizationStatusProvisional:
      return LAUFEY_PERMISSION_STATUS_GRANTED;
    default:
      return LAUFEY_PERMISSION_STATUS_UNSUPPORTED;
  }
}

// FNV-1a 64 of the action set: the category identifier, stable across runs.
NSString* CategoryIdFor(const std::vector<NotificationAction>& actions) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const std::string& s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ull;
    }
    h ^= 0x1f;
    h *= 1099511628211ull;
  };
  for (const NotificationAction& a : actions) {
    mix(a.id);
    mix(a.title);
  }
  return [NSString stringWithFormat:@"laufey.cat.%016llx",
                                    static_cast<unsigned long long>(h)];
}

UNNotificationCategory* CategoryFor(
    NSString* category_id, const std::vector<NotificationAction>& actions) {
  NSMutableArray<UNNotificationAction*>* arr = [NSMutableArray array];
  for (const NotificationAction& a : actions) {
    [arr addObject:
             [UNNotificationAction
                 actionWithIdentifier:NSStr(a.id)
                                title:NSStr(a.title)
                              options:UNNotificationActionOptionForeground]];
  }
  return [UNNotificationCategory
      categoryWithIdentifier:category_id
                     actions:arr
           intentIdentifiers:@[]
                     options:UNNotificationCategoryOptionCustomDismissAction];
}

// Makes sure `category_id` is registered (merged with the categories already
// registered, this run's or an earlier one's), then runs `then`. Main queue.
//
// setNotificationCategories replaces the whole set, and the existing set
// arrives asynchronously: two notifications with new categories shown at
// once each read a set without the other's and the second set clobbered the
// first. So every category this run adds is kept here (`added`, main queue
// only) and every set written includes all of them.
void EnsureCategory(NSString* category_id,
                    const std::vector<NotificationAction>& actions,
                    void (^then)(void)) {
  static NSMutableSet<NSString*>* known = [NSMutableSet set];
  static NSMutableDictionary<NSString*, UNNotificationCategory*>* added =
      [NSMutableDictionary dictionary];
  if (!category_id || [known containsObject:category_id]) {
    then();
    return;
  }
  added[category_id] = CategoryFor(category_id, actions);
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center getNotificationCategoriesWithCompletionHandler:^(
              NSSet<UNNotificationCategory*>* existing) {
    dispatch_async(dispatch_get_main_queue(), ^{
      NSMutableSet<UNNotificationCategory*>* all = [NSMutableSet set];
      for (UNNotificationCategory* c in existing) {
        if (!added[c.identifier])
          [all addObject:c];
      }
      [all addObjectsFromArray:added.allValues];
      [center setNotificationCategories:all];
      for (UNNotificationCategory* c in all)
        [known addObject:c.identifier];
      then();
    });
  }];
}

UNNotificationTrigger* TriggerFor(int64_t at_ms) {
  if (at_ms <= 0)
    return nil;
  double delay = (at_ms - UnixTimeMs()) / 1000.0;
  if (delay <= 0)
    return nil;
  if (delay < 60) {
    return [UNTimeIntervalNotificationTrigger
        triggerWithTimeInterval:std::max(delay, 0.1)
                        repeats:NO];
  }
  NSDate* date = [NSDate dateWithTimeIntervalSince1970:at_ms / 1000.0];
  NSDateComponents* parts = [[NSCalendar currentCalendar]
      components:NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay |
                 NSCalendarUnitHour | NSCalendarUnitMinute |
                 NSCalendarUnitSecond
        fromDate:date];
  return [UNCalendarNotificationTrigger triggerWithDateMatchingComponents:parts
                                                                  repeats:NO];
}

std::string TagOf(UNNotificationRequest* request) {
  id tag = request.content.userInfo[kTagKey];
  if ([tag isKindOfClass:[NSString class]])
    return StdStr(tag);
  return StdStr(request.identifier);
}

bool DataOf(UNNotificationRequest* request, std::string* out) {
  id data = request.content.userInfo[kDataKey];
  if (![data isKindOfClass:[NSString class]])
    return false;
  *out = StdStr(data);
  return true;
}

class MacNotificationPlatform : public NotificationPlatform {
 public:
  uint32_t Capabilities() override {
    if (!MacProcessIsBundled())
      return 0;
    return LAUFEY_NOTIFICATION_CAP_SHOW | LAUFEY_NOTIFICATION_CAP_SCHEDULE |
           LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS |
           LAUFEY_NOTIFICATION_CAP_ACTIONS | LAUFEY_NOTIFICATION_CAP_CLICKS |
           LAUFEY_NOTIFICATION_CAP_COLD_START;
  }

  bool Show(const NotificationOptions& opts) override {
    if (!MacProcessIsBundled())
      return false;
    InitNotificationsAtLaunch();
    std::string tag = opts.tag;
    NSString* ident = NSStr(tag);
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    info[kTagKey] = ident;
    if (opts.has_data)
      info[kDataKey] = NSStr(opts.data);
    if (!opts.actions.empty()) {
      NSMutableArray* acts = [NSMutableArray array];
      for (const NotificationAction& a : opts.actions)
        [acts addObject:@[ NSStr(a.id), NSStr(a.title) ]];
      info[kActionsKey] = acts;
    }
    UNMutableNotificationContent* content =
        [[UNMutableNotificationContent alloc] init];
    content.title = NSStr(opts.title);
    content.body = NSStr(opts.body);
    content.userInfo = info;
    if (!opts.silent)
      content.sound = [UNNotificationSound defaultSound];
    NSString* category_id =
        opts.actions.empty() ? nil : CategoryIdFor(opts.actions);
    if (category_id)
      content.categoryIdentifier = category_id;
    UNNotificationTrigger* trigger = TriggerFor(opts.schedule_at_ms);
    UNNotificationRequest* request =
        [UNNotificationRequest requestWithIdentifier:ident
                                             content:content
                                             trigger:trigger];
    std::vector<NotificationAction> actions = opts.actions;
    dispatch_async(dispatch_get_main_queue(), ^{
      EnsureCategory(category_id, actions, ^{
        [[UNUserNotificationCenter currentNotificationCenter]
            addNotificationRequest:request
             withCompletionHandler:^(NSError* error) {
               if (!error)
                 return;
               // Most often: not authorized. The ABI has no "error" event;
               // the lifecycle closes out.
               NSLog(@"laufey: notification not posted: %@", error);
               DispatchNotificationClosed(tag);
             }];
      });
    });
    return true;
  }

  void Remove(const std::string& tag) override {
    if (!MacProcessIsBundled())
      return;
    NSString* ident = NSStr(tag);
    UNUserNotificationCenter* center =
        [UNUserNotificationCenter currentNotificationCenter];
    [center removePendingNotificationRequestsWithIdentifiers:@[ ident ]];
    [center removeDeliveredNotificationsWithIdentifiers:@[ ident ]];
  }

  void ListScheduled(
      std::function<void(std::vector<ScheduledNotification>)> done) override {
    if (!MacProcessIsBundled()) {
      done({});
      return;
    }
    auto* heap = new std::function<void(std::vector<ScheduledNotification>)>(
        std::move(done));
    [[UNUserNotificationCenter currentNotificationCenter]
        getPendingNotificationRequestsWithCompletionHandler:^(
            NSArray<UNNotificationRequest*>* requests) {
          std::vector<ScheduledNotification> list;
          for (UNNotificationRequest* r in requests) {
            if (![r.content.userInfo[kTagKey] isKindOfClass:[NSString class]])
              continue;  // not one of laufey's
            ScheduledNotification n;
            n.tag = TagOf(r);
            n.title = StdStr(r.content.title);
            n.body = StdStr(r.content.body);
            n.has_data = DataOf(r, &n.data);
            NSDate* next = nil;
            if ([r.trigger isKindOfClass:[UNCalendarNotificationTrigger class]])
              next =
                  [(UNCalendarNotificationTrigger*)r.trigger nextTriggerDate];
            else if ([r.trigger isKindOfClass:[UNTimeIntervalNotificationTrigger
                                                  class]])
              next = [(UNTimeIntervalNotificationTrigger*)
                          r.trigger nextTriggerDate];
            n.at_ms = next ? static_cast<int64_t>([next timeIntervalSince1970] *
                                                  1000.0)
                           : 0;
            id acts = r.content.userInfo[kActionsKey];
            if ([acts isKindOfClass:[NSArray class]]) {
              for (id pair in (NSArray*)acts) {
                if ([pair isKindOfClass:[NSArray class]] &&
                    [(NSArray*)pair count] == 2)
                  n.actions.push_back({StdStr(pair[0]), StdStr(pair[1])});
              }
            }
            n.silent = r.content.sound == nil;
            list.push_back(std::move(n));
          }
          (*heap)(std::move(list));
          delete heap;
        }];
  }

  void QueryPermission(int /*kind*/, std::function<void(int)> done) override {
    if (!MacProcessIsBundled()) {
      done(LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
      return;
    }
    auto* heap = new std::function<void(int)>(std::move(done));
    [[UNUserNotificationCenter currentNotificationCenter]
        getNotificationSettingsWithCompletionHandler:^(
            UNNotificationSettings* settings) {
          int status = MapUNStatus(settings.authorizationStatus);
          dispatch_async(dispatch_get_main_queue(), ^{
            (*heap)(status);
            delete heap;
          });
        }];
  }

  void RequestPermission(int kind, std::function<void(int)> done) override {
    if (!MacProcessIsBundled()) {
      done(LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
      return;
    }
    auto* heap = new std::function<void(int)>(std::move(done));
    UNUserNotificationCenter* center =
        [UNUserNotificationCenter currentNotificationCenter];
    UNAuthorizationOptions opts = UNAuthorizationOptionAlert |
                                  UNAuthorizationOptionSound |
                                  UNAuthorizationOptionBadge;
    if (kind == LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL)
      opts |= UNAuthorizationOptionProvisional;
    [center
        requestAuthorizationWithOptions:opts
                      completionHandler:^(BOOL granted, NSError* error) {
                        if (error)
                          NSLog(@"laufey: notification authorization: %@",
                                error);
                        // The settings tell PROVISIONAL from AUTHORIZED,
                        // both GRANTED.
                        [center getNotificationSettingsWithCompletionHandler:^(
                                    UNNotificationSettings* settings) {
                          int status;
                          if (granted) {
                            status = MapUNStatus(settings.authorizationStatus);
                          } else {
                            status =
                                settings.authorizationStatus ==
                                        UNAuthorizationStatusNotDetermined
                                    ? LAUFEY_PERMISSION_STATUS_DENIED
                                    : MapUNStatus(settings.authorizationStatus);
                          }
                          dispatch_async(dispatch_get_main_queue(), ^{
                            (*heap)(status);
                            delete heap;
                          });
                        }];
                      }];
  }
};

}  // namespace
}  // namespace laufey_common

@interface LaufeyUnDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation LaufeyUnDelegate

// Foreground delivery: UN's default is to NOT present banners when the app
// is frontmost. Override so the user sees the notification regardless of
// activation state — matches what `new Notification(...)` does in a browser.
// Also where SHOWN comes from: UN has no "did deliver" callback otherwise.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler {
  (void)center;
  std::string tag = laufey_common::TagOf(notification.request);
  dispatch_async(dispatch_get_main_queue(), ^{
    laufey_common::DispatchNotificationShown(tag);
  });
  UNNotificationPresentationOptions options =
      UNNotificationPresentationOptionSound |
      UNNotificationPresentationOptionBadge;
  if (@available(macOS 11.0, *)) {
    options |= UNNotificationPresentationOptionBanner |
               UNNotificationPresentationOptionList;
  } else {
    options |= UNNotificationPresentationOptionAlert;
  }
  completionHandler(options);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  (void)center;
  UNNotificationRequest* request = response.notification.request;
  std::string tag = laufey_common::TagOf(request);
  std::string data;
  bool has_data = laufey_common::DataOf(request, &data);
  NSString* actId = response.actionIdentifier;
  bool is_default =
      [actId isEqualToString:UNNotificationDefaultActionIdentifier];
  bool is_dismiss =
      [actId isEqualToString:UNNotificationDismissActionIdentifier];
  std::string action = laufey_common::StdStr(actId);
  dispatch_async(dispatch_get_main_queue(), ^{
    if (is_dismiss) {
      // The banner's Close button (the category opts into
      // UNNotificationCategoryOptionCustomDismissAction).
      laufey_common::DispatchNotificationClosed(tag);
    } else {
      laufey_common::DispatchNotificationClick(
          tag, is_default ? nullptr : action.c_str(),
          has_data ? &data : nullptr);
    }
  });
  completionHandler();
}
@end

namespace laufey_common {

std::unique_ptr<NotificationPlatform> CreateNotificationPlatform() {
  return std::make_unique<MacNotificationPlatform>();
}

void InitNotificationsAtLaunch() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!MacProcessIsBundled())
      return;
    void (^install)(void) = ^{
      static LaufeyUnDelegate* delegate = [[LaufeyUnDelegate alloc] init];
      [UNUserNotificationCenter currentNotificationCenter].delegate = delegate;
    };
    if ([NSThread isMainThread])
      install();
    else
      dispatch_async(dispatch_get_main_queue(), install);
  });
}

}  // namespace laufey_common
