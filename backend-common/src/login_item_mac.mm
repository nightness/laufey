// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Launch at login on macOS (API 40): SMAppService.mainAppService (macOS 13+),
// which registers the app bundle itself as a login item. The user may have to
// allow it in System Settings > General > Login Items first; that state is
// LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL. An executable outside an app bundle
// (no bundle identifier) and macOS before 13 report NOT_SUPPORTED.

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

#include "laufey_system.h"

namespace laufey_common {

namespace {

bool InAppBundle() {
  NSBundle* bundle = [NSBundle mainBundle];
  return bundle.bundleIdentifier.length > 0 &&
         [bundle.bundlePath.pathExtension isEqualToString:@"app"];
}

API_AVAILABLE(macos(13.0))
int StateOf(SMAppService* service) {
  switch (service.status) {
    case SMAppServiceStatusEnabled:
      return LAUFEY_LOGIN_ITEM_ENABLED;
    case SMAppServiceStatusRequiresApproval:
      return LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL;
    case SMAppServiceStatusNotRegistered:
    case SMAppServiceStatusNotFound:
    default:
      return LAUFEY_LOGIN_ITEM_DISABLED;
  }
}

}  // namespace

int GetLaunchAtLogin() {
  if (@available(macOS 13.0, *)) {
    if (!InAppBundle())
      return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
    @autoreleasepool {
      return StateOf([SMAppService mainAppService]);
    }
  }
  return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
}

int SetLaunchAtLogin(bool enabled, std::string* error) {
  if (@available(macOS 13.0, *)) {
    if (!InAppBundle())
      return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
    @autoreleasepool {
      SMAppService* service = [SMAppService mainAppService];
      NSError* err = nil;
      BOOL ok;
      if (enabled) {
        // Registering an already-registered item succeeds; one awaiting
        // approval stays that way (only the user can allow it).
        ok = [service registerAndReturnError:&err];
      } else {
        ok = [service unregisterAndReturnError:&err];
        // Unregistering something that isn't registered is not a failure.
        if (!ok && service.status == SMAppServiceStatusNotRegistered)
          ok = YES;
      }
      if (!ok) {
        if (error) {
          *error = std::string(enabled ? "SMAppService register failed: "
                                       : "SMAppService unregister failed: ") +
                   (err.localizedDescription.UTF8String ?: "unknown error") +
                   " (" + std::to_string(static_cast<long>(err.code)) + ")";
        }
        return LAUFEY_LOGIN_ITEM_FAILED;
      }
      return StateOf(service);
    }
  }
  return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
}

}  // namespace laufey_common
