// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Auth sessions on macOS: ASWebAuthenticationSession (macOS 10.15+), a sheet
// on the app's window. See laufey_auth_session.h and docs/auth-session.md.

#import <AppKit/AppKit.h>
#import <AuthenticationServices/AuthenticationServices.h>

#include "laufey_auth_session.h"

#include <memory>
#include <string>

using laufey_common::AuthSession;

// The SDK knows ASWebAuthenticationSessionCallback (https callbacks).
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && \
    __MAC_OS_X_VERSION_MAX_ALLOWED >= 140400
#define LAUFEY_HAS_AUTH_SESSION_CALLBACK 1
#else
#define LAUFEY_HAS_AUTH_SESSION_CALLBACK 0
#endif

// Owns one session: the presentation context provider of its
// ASWebAuthenticationSession, which holds it weakly, so live providers are
// kept in LiveProviders() (main thread only) until the session ends.
@interface LaufeyAuthSessionProvider
    : NSObject <ASWebAuthenticationPresentationContextProviding>
@property(nonatomic, strong) ASWebAuthenticationSession* session;
@property(nonatomic, strong) NSWindow* anchor;
@end

@implementation LaufeyAuthSessionProvider
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:
    (ASWebAuthenticationSession*)session {
  return self.anchor;
}
@end

namespace laufey_common {

namespace {

NSMutableSet* LiveProviders() {
  static NSMutableSet* set = [NSMutableSet set];
  return set;
}

std::string ToStdString(NSString* s) {
  const char* utf8 = s ? s.UTF8String : nullptr;
  return utf8 ? std::string(utf8) : std::string();
}

// The window to anchor the sheet to when the caller named none.
NSWindow* DefaultAnchor() {
  if (NSApp.keyWindow)
    return NSApp.keyWindow;
  if (NSApp.mainWindow)
    return NSApp.mainWindow;
  for (NSWindow* w in NSApp.orderedWindows) {
    if (w.visible)
      return w;
  }
  return nil;
}

void Release(LaufeyAuthSessionProvider* provider) {
  dispatch_async(dispatch_get_main_queue(), ^{
    provider.session = nil;
    provider.anchor = nil;
    [LiveProviders() removeObject:provider];
  });
}

}  // namespace

uint32_t AuthSessionCapabilities() {
  uint32_t caps = 0;
  if (@available(macOS 10.15, *)) {
    caps |=
        LAUFEY_AUTH_SESSION_CAP_SUPPORTED | LAUFEY_AUTH_SESSION_CAP_EPHEMERAL;
  }
#if LAUFEY_HAS_AUTH_SESSION_CALLBACK
  if (@available(macOS 14.4, *)) {
    caps |= LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK;
  }
#endif
  return caps;
}

void AuthSessionStartMac(std::shared_ptr<AuthSession> session,
                         void* ns_window) {
  if (!session)
    return;
  NSWindow* window = (__bridge NSWindow*)ns_window;
  if (![NSThread isMainThread]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      AuthSessionStartMac(session, (__bridge void*)window);
    });
    return;
  }
  if (@available(macOS 10.15, *)) {
    @autoreleasepool {
      // Cancelled (window closed, test hook, shutdown) before it started.
      if (session->delivered()) {
        session->Finish(LAUFEY_AUTH_SESSION_CANCELLED,
                        "the auth session was cancelled");
        return;
      }
      if (!window)
        window = DefaultAnchor();
      LaufeyAuthSessionProvider* provider =
          [[LaufeyAuthSessionProvider alloc] init];
      // An app with no window (a menu-bar app) still gets the sheet: the OS
      // shows it on a window of its own when the anchor is not on screen.
      provider.anchor = window ? window : [[NSWindow alloc] init];

      NSURL* url = [NSURL URLWithString:@(session->url().c_str())];
      if (!url) {
        session->Finish(LAUFEY_AUTH_SESSION_INVALID, "url is not a valid URL");
        return;
      }

      std::shared_ptr<AuthSession> owner = session;
      __weak LaufeyAuthSessionProvider* weak_provider = provider;
      // Any thread (AuthenticationServices calls it on one of its own).
      auto completion = ^(NSURL* callback_url, NSError* error) {
        LaufeyAuthSessionProvider* p = weak_provider;
        if (callback_url) {
          owner->Finish(LAUFEY_AUTH_SESSION_OK,
                        ToStdString(callback_url.absoluteString));
        } else if (error &&
                   [error.domain
                       isEqualToString:ASWebAuthenticationSessionErrorDomain] &&
                   error.code ==
                       ASWebAuthenticationSessionErrorCodeCanceledLogin) {
          owner->Finish(LAUFEY_AUTH_SESSION_CANCELLED,
                        "the user cancelled the sign-in");
        } else {
          std::string message = "the auth session failed";
          if (error)
            message += ": " + ToStdString(error.localizedDescription);
          owner->Finish(LAUFEY_AUTH_SESSION_FAILED, message);
        }
        if (p)
          Release(p);
      };

      const AuthSessionCallback& cb = session->callback();
      ASWebAuthenticationSession* as = nil;
#if LAUFEY_HAS_AUTH_SESSION_CALLBACK
      if (@available(macOS 14.4, *)) {
        ASWebAuthenticationSessionCallback* callback =
            cb.https ? [ASWebAuthenticationSessionCallback
                           callbackWithHTTPSHost:@(cb.host.c_str())
                                            path:@(cb.path.c_str())]
                     : [ASWebAuthenticationSessionCallback
                           callbackWithCustomScheme:@(cb.scheme.c_str())];
        as = [[ASWebAuthenticationSession alloc] initWithURL:url
                                                    callback:callback
                                           completionHandler:completion];
      }
#endif
      if (!as) {
        if (cb.https) {
          // AuthSessionBegin refuses this without the capability.
          session->Finish(LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
                          "an https callback needs macOS 14.4 or later");
          return;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        as =
            [[ASWebAuthenticationSession alloc] initWithURL:url
                                          callbackURLScheme:@(cb.scheme.c_str())
                                          completionHandler:completion];
#pragma clang diagnostic pop
      }
      as.presentationContextProvider = provider;
      as.prefersEphemeralWebBrowserSession = session->ephemeral();
      provider.session = as;
      [LiveProviders() addObject:provider];

      session->SetWindowKey((__bridge const void*)window);
      // Window closed / test hook / shutdown: dismiss the sheet. The OS
      // reports CanceledLogin afterwards for a session past its consent
      // prompt, and nothing while the prompt is up; either way the result
      // was already delivered by AuthSession::Cancel.
      __weak ASWebAuthenticationSession* weak_session = as;
      session->SetCanceller([weak_session, weak_provider] {
        dispatch_async(dispatch_get_main_queue(), ^{
          [weak_session cancel];
          LaufeyAuthSessionProvider* p = weak_provider;
          if (p)
            Release(p);
        });
      });

      BOOL started = NO;
      @try {
        started = [as start];
      } @catch (NSException* e) {
        session->Finish(
            LAUFEY_AUTH_SESSION_FAILED,
            "starting the auth session failed: " + ToStdString(e.reason));
        Release(provider);
        return;
      }
      if (!started) {
        session->Finish(LAUFEY_AUTH_SESSION_FAILED,
                        "the OS did not start the auth session");
        Release(provider);
      }
    }
  } else {
    session->Finish(LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
                    "auth sessions need macOS 10.15 or newer");
  }
}

}  // namespace laufey_common
