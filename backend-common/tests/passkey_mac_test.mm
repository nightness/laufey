// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS-only unit tests for passkey_mac.mm: how ASAuthorizationController
// errors map to envelope codes, and the capability flags. Synthetic NSErrors
// only; no ceremony is started. Run via `ctest --test-dir webview/build`.

#include "laufey_passkey.h"

#import <AuthenticationServices/AuthenticationServices.h>
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using laufey_common::MapAuthorizationErrorMac;
using laufey_common::PasskeyCapabilitiesMac;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static NSError* AuthError(NSInteger code, NSString* description,
                          NSString* reason) {
  NSMutableDictionary* info = [NSMutableDictionary dictionary];
  if (description)
    info[NSLocalizedDescriptionKey] = description;
  if (reason)
    info[NSLocalizedFailureReasonErrorKey] = reason;
  return [NSError errorWithDomain:ASAuthorizationErrorDomain
                             code:code
                         userInfo:info];
}

static std::string CodeOf(NSError* error) {
  return MapAuthorizationErrorMac(error).code;
}

int main() {
  @autoreleasepool {
    if (@available(macOS 12.0, *)) {
      EXPECT(PasskeyCapabilitiesMac() ==
             (LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR |
              LAUFEY_PASSKEY_SECURITY_KEYS));
    } else {
      EXPECT(PasskeyCapabilitiesMac() == 0);
    }

    // The user dismissed the sheet.
    EXPECT(CodeOf(AuthError(1001, nil, nil)) == "cancelled");

    // The app may not use the RP ID: the text Apple returns for a signed app
    // without a matching webcredentials entry...
    EXPECT(CodeOf(AuthError(1004,
                            @"Application with identifier "
                            @"ABCDE12345.com.example.app is not associated "
                            @"with domain example.com",
                            nil)) == "invalid_rp");
    // ...and, observed on macOS 15 from an unsigned process, the failure
    // reason of one without an application identifier at all.
    NSError* unentitled = AuthError(
        1004, nil,
        @"The calling process does not have an application identifier. Make "
        @"sure it is properly configured.");
    EXPECT(CodeOf(unentitled) == "invalid_rp");
    // The reason reaches the message.
    EXPECT(MapAuthorizationErrorMac(unentitled)
               .message.find("application identifier") != std::string::npos);
    // The same text under an underlying error.
    NSError* underlying = [NSError
        errorWithDomain:ASAuthorizationErrorDomain
                   code:1004
               userInfo:@{
                 NSUnderlyingErrorKey : [NSError
                     errorWithDomain:@"com.apple.AuthenticationServicesCore."
                                     @"AuthorizationError"
                                code:7
                            userInfo:@{
                              NSLocalizedDescriptionKey :
                                  @"Application is not associated with "
                                  @"domain example.com"
                            }]
               }];
    EXPECT(CodeOf(underlying) == "invalid_rp");

    // The request can't be served here.
    EXPECT(CodeOf(AuthError(1003, nil, nil)) == "not_supported");
    EXPECT(CodeOf(AuthError(1005, nil, nil)) == "not_supported");

    // A timeout, by its text (AuthenticationServices has no timeout code).
    EXPECT(CodeOf(AuthError(1004, @"The operation timed out.", nil)) ==
           "timeout");

    // Everything else.
    EXPECT(CodeOf(AuthError(1000, nil, nil)) == "unknown");
    EXPECT(CodeOf(AuthError(1002, nil, nil)) == "unknown");
    EXPECT(CodeOf(AuthError(1004, @"Something else failed.", nil)) ==
           "unknown");
    EXPECT(CodeOf(AuthError(1006, nil, nil)) == "unknown");
    // A foreign domain is never an RP decision, whatever it says.
    EXPECT(CodeOf([NSError
               errorWithDomain:@"com.example.Other"
                          code:1001
                      userInfo:@{
                        NSLocalizedDescriptionKey : @"not associated with x"
                      }]) == "unknown");
    // A message is always there.
    EXPECT(
        !MapAuthorizationErrorMac(AuthError(1000, nil, nil)).message.empty());
  }
  std::printf("passkey_mac_test: all tests passed\n");
  return 0;
}
