// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS passkeys: ASAuthorizationController with the platform and
// security-key public-key credential providers (macOS 12+). Shared by the
// WKWebView and CEF hosts. See laufey_passkey.h and docs/passkeys.md.
//
// Everything here runs on the main thread: AuthenticationServices requires
// it, and the delegate callbacks arrive there.
//
// The OS builds clientDataJSON itself, from the RP ID: origin
// "https://<rpId>". A non-browser app may only use an RP ID listed in its
// com.apple.developer.associated-domains entitlement (webcredentials:<rp-id>)
// and confirmed by the domain's apple-app-site-association file; otherwise
// the request fails and is reported as invalid_rp.

#include "laufey_passkey.h"

#import <AppKit/AppKit.h>
#import <AuthenticationServices/AuthenticationServices.h>

#include <string>
#include <utility>

namespace laufey_common {
namespace {

NSData* ToData(const std::vector<uint8_t>& bytes) {
  return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

std::vector<uint8_t> FromData(NSData* data) {
  if (!data || data.length == 0)
    return {};
  const auto* p = static_cast<const uint8_t*>(data.bytes);
  return std::vector<uint8_t>(p, p + data.length);
}

NSString* ToNSString(const std::string& s) {
  NSString* out = [[NSString alloc] initWithBytes:s.data()
                                           length:s.size()
                                         encoding:NSUTF8StringEncoding];
  return out ?: @"";
}

std::string ToStdString(NSString* s) {
  if (!s)
    return std::string();
  const char* utf8 = [s UTF8String];
  return utf8 ? std::string(utf8) : std::string();
}

// Every human-readable string an NSError (and the errors under it) carries,
// lowercased: the association failure is only identifiable by its text.
void CollectErrorText(NSError* error, int depth, std::string* out) {
  if (!error || depth > 3)
    return;
  out->append(ToStdString(error.localizedDescription.lowercaseString));
  out->push_back('\n');
  for (NSString* key in @[
         NSLocalizedFailureReasonErrorKey, NSDebugDescriptionErrorKey,
         NSLocalizedRecoverySuggestionErrorKey
       ]) {
    id value = error.userInfo[key];
    if ([value isKindOfClass:[NSString class]]) {
      out->append(ToStdString([(NSString*)value lowercaseString]));
      out->push_back('\n');
    }
  }
  id underlying = error.userInfo[NSUnderlyingErrorKey];
  if ([underlying isKindOfClass:[NSError class]])
    CollectErrorText((NSError*)underlying, depth + 1, out);
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

PasskeyError MapAuthorizationErrorMac(NSError* error) {
  PasskeyError out;
  // The description is often generic ("The operation couldn't be
  // completed."); the failure reason says why.
  out.message = ToStdString(error.localizedDescription);
  id reason = error.userInfo[NSLocalizedFailureReasonErrorKey];
  if ([reason isKindOfClass:[NSString class]] && [(NSString*)reason length] &&
      out.message.find(ToStdString((NSString*)reason)) == std::string::npos) {
    out.message += out.message.empty() ? "" : " ";
    out.message += ToStdString((NSString*)reason);
  }
  if (out.message.empty())
    out.message = "the passkey request failed";
  std::string text;
  CollectErrorText(error, 0, &text);
  // The app may not use the RP ID: "Application with identifier <team>.<id>
  // is not associated with domain <rp>" (no webcredentials entry / AASA),
  // or "The calling process does not have an application identifier"
  // (unsigned / ad-hoc signed, no associated-domains entitlement at all).
  bool association = Contains(text, "not associated") ||
                     Contains(text, "associated domain") ||
                     Contains(text, "application identifier") ||
                     Contains(text, "webcredentials");
  bool in_domain = [error.domain isEqualToString:ASAuthorizationErrorDomain];
  NSInteger code = error.code;

  if (Contains(text, "timed out") || Contains(text, "timeout")) {
    out.code = kPasskeyTimeout;
  } else if (in_domain && code == ASAuthorizationErrorCanceled) {
    out.code = kPasskeyCancelled;
  } else if (in_domain && association) {
    // ASAuthorizationErrorFailed (1004) carrying one of the texts above.
    out.code = kPasskeyInvalidRp;
  } else if (in_domain &&
             (code == ASAuthorizationErrorNotHandled ||
              code == 1005 /* ASAuthorizationErrorNotInteractive, 12+ */)) {
    out.code = kPasskeyNotSupported;
  } else {
    out.code = kPasskeyUnknown;
  }
  return out;
}

}  // namespace laufey_common

using laufey_common::PasskeyCeremony;

// Owns one ceremony: delegate and presentation provider of its controller.
// ASAuthorizationController holds both weakly, so live delegates are kept in
// g_live_delegates (main thread only) until the ceremony finishes.
API_AVAILABLE(macos(12.0))
@interface LaufeyPasskeyDelegate
    : NSObject <ASAuthorizationControllerDelegate,
                ASAuthorizationControllerPresentationContextProviding>
@property(nonatomic, strong) ASAuthorizationController* controller;
@property(nonatomic, strong) NSWindow* anchor;
- (instancetype)initWithCeremony:(std::shared_ptr<PasskeyCeremony>)ceremony;
- (void)finishWith:(const std::string&)envelope;
@end

static NSMutableSet* LiveDelegates() {
  static NSMutableSet* set = [NSMutableSet set];
  return set;
}

@implementation LaufeyPasskeyDelegate {
  std::shared_ptr<PasskeyCeremony> _ceremony;
}

- (instancetype)initWithCeremony:(std::shared_ptr<PasskeyCeremony>)ceremony {
  if ((self = [super init])) {
    _ceremony = std::move(ceremony);
  }
  return self;
}

- (void)finishWith:(const std::string&)envelope {
  std::shared_ptr<PasskeyCeremony> ceremony = std::move(_ceremony);
  _ceremony.reset();
  self.controller = nil;
  self.anchor = nil;
  [LiveDelegates() removeObject:self];
  if (ceremony)
    ceremony->Finish(envelope);
}

- (ASPresentationAnchor)presentationAnchorForAuthorizationController:
    (ASAuthorizationController*)controller {
  return self.anchor;
}

- (void)authorizationController:(ASAuthorizationController*)controller
    didCompleteWithAuthorization:(ASAuthorization*)authorization {
  using namespace laufey_common;
  std::string envelope;
  @try {
    id credential = authorization.credential;
    bool platform_reg = [credential
        isKindOfClass:[ASAuthorizationPlatformPublicKeyCredentialRegistration
                          class]];
    bool key_reg = [credential
        isKindOfClass:[ASAuthorizationSecurityKeyPublicKeyCredentialRegistration
                          class]];
    bool platform_assert = [credential
        isKindOfClass:[ASAuthorizationPlatformPublicKeyCredentialAssertion
                          class]];
    bool key_assert = [credential
        isKindOfClass:[ASAuthorizationSecurityKeyPublicKeyCredentialAssertion
                          class]];
    if (platform_reg || key_reg) {
      id<ASAuthorizationPublicKeyCredentialRegistration> reg = credential;
      if (!reg.rawAttestationObject) {
        envelope = PasskeyErrorEnvelope(
            kPasskeyUnknown,
            "the authenticator returned no attestation object");
      } else {
        PasskeyRegistrationResult r;
        r.credential_id = FromData(reg.credentialID);
        r.client_data_json = FromData(reg.rawClientDataJSON);
        r.attestation_object = FromData(reg.rawAttestationObject);
        // As @clerk/electron-passkeys: the provider decides the attachment
        // and the transports reported.
        if (platform_reg) {
          r.attachment = "platform";
          r.transports = {"internal", "hybrid"};
        } else {
          r.attachment = "cross-platform";
          r.transports = {"usb", "nfc", "ble"};
        }
        envelope = PasskeyRegistrationEnvelope(r);
      }
    } else if (platform_assert || key_assert) {
      id<ASAuthorizationPublicKeyCredentialAssertion> as = credential;
      PasskeyAssertionResult r;
      r.credential_id = FromData(as.credentialID);
      r.client_data_json = FromData(as.rawClientDataJSON);
      r.authenticator_data = FromData(as.rawAuthenticatorData);
      r.signature = FromData(as.signature);
      r.user_handle = FromData(as.userID);  // may be absent
      r.attachment = platform_assert ? "platform" : "cross-platform";
      envelope = PasskeyAssertionEnvelope(r);
    } else {
      envelope = PasskeyErrorEnvelope(
          kPasskeyUnknown, "unexpected ASAuthorization credential type");
    }
  } @catch (NSException* e) {
    envelope = PasskeyErrorEnvelope(kPasskeyUnknown,
                                    "reading the credential failed: " +
                                        laufey_common::ToStdString(e.reason));
  }
  [self finishWith:envelope];
}

- (void)authorizationController:(ASAuthorizationController*)controller
           didCompleteWithError:(NSError*)error {
  laufey_common::PasskeyError mapped =
      laufey_common::MapAuthorizationErrorMac(error);
  [self finishWith:laufey_common::PasskeyErrorEnvelope(mapped.code,
                                                       mapped.message)];
}

@end

namespace laufey_common {
namespace {

API_AVAILABLE(macos(12.0))
ASAuthorizationPublicKeyCredentialUserVerificationPreference UvPreference(
    const std::string& uv) {
  if (uv == "required")
    return ASAuthorizationPublicKeyCredentialUserVerificationPreferenceRequired;
  if (uv == "discouraged")
    return ASAuthorizationPublicKeyCredentialUserVerificationPreferenceDiscouraged;
  return ASAuthorizationPublicKeyCredentialUserVerificationPreferencePreferred;
}

API_AVAILABLE(macos(12.0))
ASAuthorizationPublicKeyCredentialResidentKeyPreference ResidentKeyPreference(
    const std::string& rk) {
  if (rk == "required")
    return ASAuthorizationPublicKeyCredentialResidentKeyPreferenceRequired;
  if (rk == "preferred")
    return ASAuthorizationPublicKeyCredentialResidentKeyPreferencePreferred;
  return ASAuthorizationPublicKeyCredentialResidentKeyPreferenceDiscouraged;
}

API_AVAILABLE(macos(12.0))
ASAuthorizationPublicKeyCredentialAttestationKind AttestationKind(
    const std::string& a) {
  if (a == "direct")
    return ASAuthorizationPublicKeyCredentialAttestationKindDirect;
  if (a == "indirect")
    return ASAuthorizationPublicKeyCredentialAttestationKindIndirect;
  if (a == "enterprise")
    return ASAuthorizationPublicKeyCredentialAttestationKindEnterprise;
  return ASAuthorizationPublicKeyCredentialAttestationKindNone;
}

API_AVAILABLE(macos(12.0))
NSArray<ASAuthorizationPlatformPublicKeyCredentialDescriptor*>*
PlatformDescriptors(const std::vector<PasskeyCredentialDescriptor>& list) {
  NSMutableArray* out = [NSMutableArray arrayWithCapacity:list.size()];
  for (const auto& d : list) {
    [out addObject:[[ASAuthorizationPlatformPublicKeyCredentialDescriptor alloc]
                       initWithCredentialID:ToData(d.id)]];
  }
  return out;
}

API_AVAILABLE(macos(12.0))
NSArray<ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor*>*
SecurityKeyDescriptors(const std::vector<PasskeyCredentialDescriptor>& list) {
  NSMutableArray* out = [NSMutableArray arrayWithCapacity:list.size()];
  for (const auto& d : list) {
    NSMutableArray* transports = [NSMutableArray array];
    for (const auto& t : d.transports) {
      if (t == "usb")
        [transports
            addObject:
                ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportUSB];
      else if (t == "nfc")
        [transports
            addObject:
                ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportNFC];
      else if (t == "ble")
        [transports
            addObject:
                ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportBluetooth];
    }
    if (transports.count == 0) {
      // No security-key transport named: allow all of them.
      [transports addObjectsFromArray:@[
        ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportUSB,
        ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportNFC,
        ASAuthorizationSecurityKeyPublicKeyCredentialDescriptorTransportBluetooth
      ]];
    }
    [out addObject:[[ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor
                       alloc] initWithCredentialID:ToData(d.id)
                                        transports:transports]];
  }
  return out;
}

bool HasTransport(const PasskeyCredentialDescriptor& d,
                  std::initializer_list<const char*> names) {
  for (const auto& t : d.transports) {
    for (const char* n : names) {
      if (t == n)
        return true;
    }
  }
  return false;
}

API_AVAILABLE(macos(12.0))
NSArray<ASAuthorizationRequest*>* BuildCreateRequests(
    const PasskeyCreationOptions& o) {
  NSString* rp_id = ToNSString(o.rp_id);
  NSData* challenge = ToData(o.challenge);
  NSData* user_id = ToData(o.user_id);
  // As @clerk/electron-passkeys: the account name falls back to the display
  // name and vice versa.
  NSString* name =
      ToNSString(!o.user_name.empty() ? o.user_name : o.user_display_name);
  NSString* display_name = ToNSString(
      !o.user_display_name.empty() ? o.user_display_name : o.user_name);
  NSMutableArray<ASAuthorizationRequest*>* requests = [NSMutableArray array];

  if (o.authenticator_attachment != "cross-platform") {
    ASAuthorizationPlatformPublicKeyCredentialProvider* provider =
        [[ASAuthorizationPlatformPublicKeyCredentialProvider alloc]
            initWithRelyingPartyIdentifier:rp_id];
    ASAuthorizationPlatformPublicKeyCredentialRegistrationRequest* request =
        [provider createCredentialRegistrationRequestWithChallenge:challenge
                                                              name:name
                                                            userID:user_id];
    if (!o.user_verification.empty())
      request.userVerificationPreference = UvPreference(o.user_verification);
    // No attestation preference: iCloud Keychain passkeys fail when one is
    // requested (browsers downgrade platform passkeys to "none" too).
    if (!o.exclude_credentials.empty()) {
      if (@available(macOS 13.5, *)) {
        if ([request respondsToSelector:@selector(setExcludedCredentials:)])
          request.excludedCredentials =
              PlatformDescriptors(o.exclude_credentials);
      }
    }
    [requests addObject:request];
  }

  if (o.authenticator_attachment != "platform") {
    ASAuthorizationSecurityKeyPublicKeyCredentialProvider* provider =
        [[ASAuthorizationSecurityKeyPublicKeyCredentialProvider alloc]
            initWithRelyingPartyIdentifier:rp_id];
    ASAuthorizationSecurityKeyPublicKeyCredentialRegistrationRequest* request =
        [provider createCredentialRegistrationRequestWithChallenge:challenge
                                                       displayName:display_name
                                                              name:name
                                                            userID:user_id];
    // Security-key registrations need explicit algorithms; ES256 when the
    // options list none.
    NSMutableArray* params = [NSMutableArray array];
    std::vector<int32_t> algs = o.algorithms;
    if (algs.empty())
      algs.push_back(-7);
    for (int32_t alg : algs) {
      [params addObject:[[ASAuthorizationPublicKeyCredentialParameters alloc]
                            initWithAlgorithm:alg]];
    }
    request.credentialParameters = params;
    if (!o.user_verification.empty())
      request.userVerificationPreference = UvPreference(o.user_verification);
    if (!o.resident_key.empty())
      request.residentKeyPreference = ResidentKeyPreference(o.resident_key);
    if (!o.attestation.empty())
      request.attestationPreference = AttestationKind(o.attestation);
    if (!o.exclude_credentials.empty())
      request.excludedCredentials =
          SecurityKeyDescriptors(o.exclude_credentials);
    [requests addObject:request];
  }
  return requests;
}

API_AVAILABLE(macos(12.0))
NSArray<ASAuthorizationRequest*>* BuildGetRequests(
    const PasskeyRequestOptions& o) {
  NSString* rp_id = ToNSString(o.rp_id);
  NSData* challenge = ToData(o.challenge);
  const auto& allow = o.allow_credentials;

  // When every allowed credential names its transports, offer only the
  // providers that can reach one of them. Nothing reachable (e.g. only
  // "smart-card") falls back to both, as does an empty list.
  bool restrict = !allow.empty();
  for (const auto& d : allow) {
    if (d.transports.empty())
      restrict = false;
  }
  bool platform = !restrict;
  bool security_key = !restrict;
  if (restrict) {
    for (const auto& d : allow) {
      platform = platform || HasTransport(d, {"internal", "hybrid"});
      security_key = security_key || HasTransport(d, {"usb", "nfc", "ble"});
    }
    if (!platform && !security_key)
      platform = security_key = true;
  }

  NSMutableArray<ASAuthorizationRequest*>* requests = [NSMutableArray array];
  if (platform) {
    ASAuthorizationPlatformPublicKeyCredentialProvider* provider =
        [[ASAuthorizationPlatformPublicKeyCredentialProvider alloc]
            initWithRelyingPartyIdentifier:rp_id];
    ASAuthorizationPlatformPublicKeyCredentialAssertionRequest* request =
        [provider createCredentialAssertionRequestWithChallenge:challenge];
    if (!o.user_verification.empty())
      request.userVerificationPreference = UvPreference(o.user_verification);
    if (!allow.empty())
      request.allowedCredentials = PlatformDescriptors(allow);
    [requests addObject:request];
  }
  if (security_key) {
    ASAuthorizationSecurityKeyPublicKeyCredentialProvider* provider =
        [[ASAuthorizationSecurityKeyPublicKeyCredentialProvider alloc]
            initWithRelyingPartyIdentifier:rp_id];
    ASAuthorizationSecurityKeyPublicKeyCredentialAssertionRequest* request =
        [provider createCredentialAssertionRequestWithChallenge:challenge];
    if (!o.user_verification.empty())
      request.userVerificationPreference = UvPreference(o.user_verification);
    if (!allow.empty())
      request.allowedCredentials = SecurityKeyDescriptors(allow);
    [requests addObject:request];
  }
  return requests;
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

}  // namespace

uint32_t PasskeyCapabilitiesMac() {
  // Security keys go through the same OS sheet whenever the API exists.
  if (@available(macOS 12.0, *))
    return LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR | LAUFEY_PASSKEY_SECURITY_KEYS;
  return 0;
}

void PasskeyStartMac(std::shared_ptr<PasskeyCeremony> ceremony,
                     void* ns_window) {
  if (!ceremony)
    return;
  NSWindow* window = (__bridge NSWindow*)ns_window;
  if (![NSThread isMainThread]) {
    // Callers are supposed to be on the main thread already; hop rather
    // than touch AppKit from here. `window` is retained by the block.
    dispatch_async(dispatch_get_main_queue(), ^{
      PasskeyStartMac(ceremony, (__bridge void*)window);
    });
    return;
  }

  if (@available(macOS 12.0, *)) {
    @autoreleasepool {
      if (!window)
        window = DefaultAnchor();
      if (!window) {
        ceremony->Finish(PasskeyErrorEnvelope(
            kPasskeyUnknown, "no window to anchor the passkey request to"));
        return;
      }
      // Aborted (timeout / window closed) before it could start.
      if (ceremony->delivered()) {
        ceremony->Finish(
            PasskeyErrorEnvelope(kPasskeyCancelled, "the request was aborted"));
        return;
      }

      NSArray<ASAuthorizationRequest*>* requests = nil;
      @try {
        requests = ceremony->is_create()
                       ? BuildCreateRequests(ceremony->creation())
                       : BuildGetRequests(ceremony->request());
      } @catch (NSException* e) {
        ceremony->Finish(PasskeyErrorEnvelope(
            kPasskeyUnknown,
            "building the passkey request failed: " + ToStdString(e.reason)));
        return;
      }
      if (requests.count == 0) {
        ceremony->Finish(PasskeyErrorEnvelope(
            kPasskeyNotSupported, "no usable authenticator type requested"));
        return;
      }

      ASAuthorizationController* controller = [[ASAuthorizationController alloc]
          initWithAuthorizationRequests:requests];
      LaufeyPasskeyDelegate* delegate =
          [[LaufeyPasskeyDelegate alloc] initWithCeremony:ceremony];
      delegate.controller = controller;
      delegate.anchor = window;
      controller.delegate = delegate;
      controller.presentationContextProvider = delegate;
      [LiveDelegates() addObject:delegate];

      ceremony->SetWindowKey((__bridge const void*)window);
      // Timeout / window close: ask the controller to stop (macOS 13+; on
      // macOS 12 the sheet stays up until the user dismisses it, and the
      // app-wide slot with it). The OS then reports Canceled, which is
      // dropped because the abort already answered.
      __weak ASAuthorizationController* weak_controller = controller;
      ceremony->SetCanceller([weak_controller] {
        dispatch_async(dispatch_get_main_queue(), ^{
          if (@available(macOS 13.0, *)) {
            [weak_controller cancel];
          }
        });
      });
      // AuthenticationServices has no timeout of its own: run ours when the
      // options carry one (WebAuthn's `timeout`).
      if (ceremony->has_timeout())
        ceremony->StartTimeout(ceremony->timeout_ms());

      @try {
        [controller performRequests];
      } @catch (NSException* e) {
        [delegate finishWith:PasskeyErrorEnvelope(
                                 kPasskeyUnknown,
                                 "starting the passkey request failed: " +
                                     ToStdString(e.reason))];
      }
    }
  } else {
    ceremony->Finish(PasskeyErrorEnvelope(kPasskeyNotSupported,
                                          "passkeys need macOS 12 or newer"));
  }
}

}  // namespace laufey_common
