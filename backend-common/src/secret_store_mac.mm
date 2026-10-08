// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The secure store on macOS (API 47): a generic password per (service,
// account) in the Keychain, through Security.framework (SecItem*) in this
// process, so the item is the app's own. See laufey_secret_store.h and
// docs/secure-store.md.
//
// Which keychain:
//   - The data-protection keychain (kSecUseDataProtectionKeychain) when the
//     app is signed with an entitlement that names it
//     (com.apple.application-identifier or keychain-access-groups, which
//     macOS honours only with a provisioning profile). Its items belong to
//     the app's access group: no other app reads them, with or without a
//     prompt, and `security` does not see them. An item the app wrote to the
//     login keychain before it had the entitlement moves over on its first
//     lookup.
//   - Otherwise (Developer ID without a profile, ad-hoc, unsigned) the login
//     keychain, with an access list whose only trusted application is this
//     app (SecAccess + SecTrustedApplication of the host executable: its
//     designated requirement when signed, so updates signed by the same
//     identity keep access; its code hash when unsigned or ad-hoc, so
//     unsigned or ad-hoc builds whose hosts are byte-identical trust each
//     other's items). No other program of the user reads the value without
//     macOS's prompt (the keychain password, or the person's "Allow"):
//     `security find-generic-password -w` included. Code injected into this
//     app's process reads it like the app.
//
// The login keychain guards reading an item, not writing it: another
// program of the user can replace an item's value without a prompt
// (SecKeychainItemModifyContent, `security add-generic-password -U`), and
// can create an item with this store's creator code ('Lfy1') whose access
// list trusts this app. macOS stamps the writer's partition ID on an item
// it creates and re-stamps it on such a replacement, and no program can
// stamp another's without the keychain password. So an Apple- or
// team-signed app (partition "apple:" / "teamid:<team>") takes an item for
// its own only when its partition list names the app's partition
// (IsAnotherProgramsItem): another program's item reads as "not found",
// without a prompt, and a store refuses it rather than write the secret
// into an item whose access list that program chose. An ad-hoc signed
// build (partition "cdhash:<hex>") can't tell its own earlier build from
// another program: macOS asks on the lookup. Every unsigned program shares
// the partition "unsigned:": an unsigned build reads a planted or replaced
// item without a prompt.
//
// Items written here carry kSecAttrCreator 'Lfy1', and every query matches it:
// an item another tool wrote for the same service and account (the
// `security` CLI an embedder used before) is never read here (no prompt for
// it: lookup is "not found"), and a store refuses while it is in the way.
// The embedder moves such items over with the tool that wrote them.
//
// A delete macOS refuses without asking (only the item's owner may remove a
// login-keychain item, and an ad-hoc signed CEF bundle isn't always taken for
// it) wipes the value instead: empty, marked deleted (kSecAttrComment), read
// as "not found", reused by the next store.
//
// Every call runs on one serial queue. A store or delete never prompts
// (user interaction is off while it runs: a locked keychain is refused);
// a lookup may (an unlock, or "allow" for a rebuilt ad-hoc app), and the
// caller stops waiting after its timeout. A call still queued when its caller
// gave up never runs, so nothing is written after the call gave up.

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "laufey_secret_store.h"

// SecAccess / SecTrustedApplication (the login keychain's access lists) and
// SecKeychainSetUserInteractionAllowed are deprecated with the file-based
// keychain itself, with no replacement for it; they still work and are what
// an app without the data-protection entitlement has.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace laufey_common {

namespace {

// kSecAttrCreator of the items written here.
constexpr FourCharCode kCreator = 'Lfy1';

// kSecAttrComment of an item a delete could not remove (see DeleteIn): its
// value is emptied and it reads as "not found" until a store reuses it.
NSString* const kDeletedMarker = @"laufey:deleted";

uint32_t TimeoutOrDefault(uint32_t timeout_ms) {
  return timeout_ms ? timeout_ms : kSecretDefaultTimeoutMs;
}

NSString* Utf8(const std::string& s) {
  if (s.find('\0') != std::string::npos)
    return nil;
  return [[NSString alloc] initWithBytes:s.data()
                                  length:s.size()
                                encoding:NSUTF8StringEncoding];
}

bool Valid(const std::string& service, const std::string& account,
           std::string* reason) {
  if (service.empty() || account.empty()) {
    *reason = "the service and the account must not be empty";
    return false;
  }
  if (!Utf8(service) || !Utf8(account)) {
    *reason = "the service and the account must be UTF-8";
    return false;
  }
  return true;
}

std::string Message(OSStatus status) {
  NSString* text = CFBridgingRelease(SecCopyErrorMessageString(status, nullptr));
  std::string out = text.length ? std::string(text.UTF8String) : "error";
  return out + " (OSStatus " + std::to_string(static_cast<int>(status)) + ")";
}

// Whether this process is signed with an entitlement that gives it a
// data-protection keychain access group.
bool HasKeychainEntitlement() {
  SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
  if (!task)
    return false;
  bool found = false;
  for (CFStringRef key : {CFSTR("keychain-access-groups"),
                          CFSTR("com.apple.application-identifier")}) {
    CFTypeRef value = SecTaskCopyValueForEntitlement(task, key, nullptr);
    if (value) {
      CFRelease(value);
      found = true;
      break;
    }
  }
  CFRelease(task);
  return found;
}

std::string Hex(CFDataRef data) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  const UInt8* bytes = CFDataGetBytePtr(data);
  for (CFIndex i = 0; i < CFDataGetLength(data); i++) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0xf]);
  }
  return out;
}

bool Satisfies(SecStaticCodeRef code, CFStringRef requirement) {
  SecRequirementRef req = nullptr;
  if (SecRequirementCreateWithString(requirement, kSecCSDefaultFlags, &req) !=
      errSecSuccess)
    return false;
  OSStatus status =
      SecStaticCodeCheckValidity(code, kSecCSBasicValidateOnly, req);
  CFRelease(req);
  return status == errSecSuccess;
}

// The subject's organizational unit of a certificate (a developer
// certificate's team), or nil.
NSString* OrganizationalUnit(SecCertificateRef cert) {
  NSDictionary* values = CFBridgingRelease(SecCertificateCopyValues(
      cert, (__bridge CFArrayRef) @[ (id)kSecOIDX509V1SubjectName ], nullptr));
  NSArray* subject =
      values[(id)kSecOIDX509V1SubjectName][(id)kSecPropertyKeyValue];
  for (NSDictionary* component in subject) {
    if ([component[(id)kSecPropertyKeyLabel]
            isEqual:(id)kSecOIDOrganizationalUnitName] &&
        [component[(id)kSecPropertyKeyValue] isKindOfClass:[NSString class]])
      return component[(id)kSecPropertyKeyValue];
  }
  return nil;
}

// securityd's partitionIdForProcess (securityd/src/clientid.cpp), for this
// process's own code: the partition ID macOS stamps on the login-keychain
// items this process writes, and checks a reader's against.
std::string ComputeOwnPartitionId() {
  SecCodeRef self = nullptr;
  if (SecCodeCopySelf(kSecCSDefaultFlags, &self) != errSecSuccess)
    return "";
  SecStaticCodeRef code = nullptr;
  OSStatus status = SecCodeCopyStaticCode(self, kSecCSDefaultFlags, &code);
  CFRelease(self);
  if (status != errSecSuccess)
    return "";
  std::string partition;
  SecRequirementRef apple = nullptr;
  SecRequirementCreateWithString(CFSTR("anchor apple"), kSecCSDefaultFlags,
                                 &apple);
  OSStatus rc = apple ? SecStaticCodeCheckValidity(
                            code, kSecCSBasicValidateOnly, apple)
                      : errSecCSReqFailed;
  if (apple)
    CFRelease(apple);
  CFDictionaryRef info = nullptr;
  if (rc == errSecCSUnsigned) {
    partition = "unsigned:";
  } else if ((rc == errSecSuccess || rc == errSecCSReqFailed) &&
             SecCodeCopySigningInformation(code, kSecCSSigningInformation,
                                           &info) == errSecSuccess) {
    NSDictionary* signing = (__bridge NSDictionary*)info;
    NSString* team = signing[(id)kSecCodeInfoTeamIdentifier];
    if (rc == errSecSuccess) {
      partition = [signing[(id)kSecCodeInfoIdentifier]
               isEqualToString:@"com.apple.security"]
               ? "apple-tool:"
               : "apple:";
    } else if (Satisfies(code, CFSTR("anchor apple generic and certificate "
                                     "leaf[field.1.2.840.113635.100.6.1.9]")) ||
               Satisfies(code,
                         CFSTR("anchor apple generic and certificate "
                               "1[field.1.2.840.113635.100.6.2.1] exists and "
                               "certificate "
                               "leaf[field.1.2.840.113635.100.6.1.25.1] "
                               "exists"))) {
      // Mac App Store or TestFlight: the embedded team identifier.
      partition = std::string("teamid:") + (team.UTF8String ?: "");
    } else if (Satisfies(
                   code,
                   CFSTR("anchor apple generic and certificate "
                         "1[field.1.2.840.113635.100.6.2.6] and certificate "
                         "leaf[field.1.2.840.113635.100.6.1.13] or anchor "
                         "apple generic and certificate "
                         "1[field.1.2.840.113635.100.6.2.1] and certificate "
                         "leaf[field.1.2.840.113635.100.6.1.12] or anchor "
                         "apple generic and certificate "
                         "1[field.1.2.840.113635.100.6.2.1] and certificate "
                         "leaf[field.1.2.840.113635.100.6.1.7]"))) {
      // Developer ID, development or distribution: the signing
      // certificate's organizational unit (the team).
      NSArray* certs = signing[(id)kSecCodeInfoCertificates];
      NSString* unit = certs.count ? OrganizationalUnit((
                                         __bridge SecCertificateRef)certs[0])
                                   : nil;
      partition = std::string("teamid:") + ((unit ?: team).UTF8String ?: "");
    } else if (NSData* cdhash = signing[(id)kSecCodeInfoUnique]) {
      partition = "cdhash:" + Hex((__bridge CFDataRef)cdhash);
    }
    CFRelease(info);
  }
  CFRelease(code);
  return partition;
}

// Only touched on the queue.
bool g_own_partition_decided = false;
std::string g_own_partition;
bool g_require_own_partition_for_testing = false;

const std::string& OwnPartitionId() {
  if (!g_own_partition_decided) {
    g_own_partition = ComputeOwnPartitionId();
    g_own_partition_decided = true;
  }
  return g_own_partition;
}

// An Apple- or team-signed app takes a login-keychain item for its own only
// when the item's partition list names this app's partition (see
// IsAnotherProgramsItem). An ad-hoc signed or unsigned build can't tell its
// own earlier build from another program, and leaves it to macOS's prompt
// (ad-hoc) or reads it (unsigned: every unsigned program shares the
// partition).
bool RequireOwnPartition() {
  const std::string& own = OwnPartitionId();
  if (g_require_own_partition_for_testing)
    return true;
  return own.rfind("teamid:", 0) == 0 || own.rfind("apple:", 0) == 0 ||
         own.rfind("apple-tool:", 0) == 0;
}

// The partition list macOS stamped on a login-keychain item (its access
// list's ACLAuthorizationPartitionID entry, a hex-encoded plist
// {Partitions: [...]}), read without authorization. False when it has none.
bool ItemPartitions(SecKeychainItemRef item, std::vector<std::string>* out) {
  SecAccessRef access = nullptr;
  if (SecKeychainItemCopyAccess(item, &access) != errSecSuccess || !access)
    return false;
  bool found = false;
  CFArrayRef acls = nullptr;
  if (SecAccessCopyACLList(access, &acls) == errSecSuccess && acls) {
    for (id entry in (__bridge NSArray*)acls) {
      SecACLRef acl = (__bridge SecACLRef)entry;
      NSArray* auths = CFBridgingRelease(SecACLCopyAuthorizations(acl));
      if (![auths containsObject:(id)kSecACLAuthorizationPartitionID])
        continue;
      CFArrayRef apps = nullptr;
      CFStringRef desc = nullptr;
      SecKeychainPromptSelector selector = 0;
      if (SecACLCopyContents(acl, &apps, &desc, &selector) != errSecSuccess)
        continue;
      if (apps)
        CFRelease(apps);
      NSString* hex = CFBridgingRelease(desc);
      NSMutableData* bytes = [NSMutableData dataWithCapacity:hex.length / 2];
      const char* text = hex.UTF8String ?: "";
      for (size_t i = 0; text[i] && text[i + 1]; i += 2) {
        char pair[3] = {text[i], text[i + 1], 0};
        uint8_t b = static_cast<uint8_t>(std::strtoul(pair, nullptr, 16));
        [bytes appendBytes:&b length:1];
      }
      NSDictionary* plist =
          [NSPropertyListSerialization propertyListWithData:bytes
                                                    options:0
                                                     format:nullptr
                                                      error:nil];
      if (![plist isKindOfClass:[NSDictionary class]])
        continue;
      found = true;
      for (id p in plist[@"Partitions"]) {
        if ([p isKindOfClass:[NSString class]])
          out->push_back([p UTF8String]);
      }
    }
    CFRelease(acls);
  }
  CFRelease(access);
  return found;
}

// Whether a login-keychain item with this store's creator code, service and
// account is another program's, for an app that takes only its own
// (RequireOwnPartition). macOS stamps the writer's partition on an item it
// creates, and re-stamps it when another program replaces the value
// (SecKeychainItemModifyContent, `security add-generic-password -U`, which
// needs no prompt); a program can't put another's partition on it without
// the keychain password. So an item planted by another program (whatever
// access list it gave it) or replaced by one lacks this app's partition.
bool IsAnotherProgramsItem(SecKeychainItemRef item) {
  if (!RequireOwnPartition())
    return false;
  const std::string& own = OwnPartitionId();
  std::vector<std::string> partitions;
  if (!item || !ItemPartitions(item, &partitions))
    return true;
  return own.empty() ||
         std::find(partitions.begin(), partitions.end(), own) ==
             partitions.end();
}

// Only touched on the queue.
bool g_data_protection_decided = false;
bool g_data_protection = false;

bool UseDataProtection() {
  if (!g_data_protection_decided) {
    g_data_protection = HasKeychainEntitlement();
    g_data_protection_decided = true;
  }
  return g_data_protection;
}

NSMutableDictionary* Query(NSString* service, NSString* account, bool dp) {
  NSMutableDictionary* q = [@{
    (id)kSecClass : (id)kSecClassGenericPassword,
    (id)kSecAttrService : service,
    (id)kSecAttrAccount : account,
    (id)kSecAttrCreator : @(kCreator),
  } mutableCopy];
  if (dp)
    q[(id)kSecUseDataProtectionKeychain] = @YES;
  return q;
}

// User interaction (keychain prompts) off for the scope: a store or delete
// is refused rather than wait for a person. Process-wide, so only while one
// call of this queue runs.
class NoPrompts {
 public:
  NoPrompts() {
    SecKeychainGetUserInteractionAllowed(&previous_);
    SecKeychainSetUserInteractionAllowed(false);
  }
  ~NoPrompts() { SecKeychainSetUserInteractionAllowed(previous_); }

 private:
  Boolean previous_ = true;
};

// What a refusal means, for the reason.
SecretStatus Refused(OSStatus status, const char* what, std::string* reason) {
  switch (status) {
    case errSecInteractionNotAllowed:
      *reason = std::string("the keychain can't ") + what +
                " without asking (it is locked, or this build of the app is "
                "not on the item's access list): " +
                Message(status);
      break;
    case errSecAuthFailed:
    case errSecInvalidOwnerEdit:
    case errSecUserCanceled:
      *reason = std::string("access to the keychain item was refused: ") +
                "this build of the app is not on its access list (an "
                "unsigned or ad-hoc build that changed asks the person on a "
                "lookup), or the person denied it: " +
                Message(status);
      break;
    case errSecNoDefaultKeychain:
    case errSecNoSuchKeychain:
      *reason = "there is no default keychain: " + Message(status);
      break;
    default:
      *reason = std::string("the keychain could not ") + what + ": " +
                Message(status);
      break;
  }
  return SecretStatus::kUnavailable;
}

// The login keychain's access list: this app, and nothing else, may read the
// item without a prompt.
SecAccessRef CreateOwnAccess(NSString* label, std::string* reason) {
  SecTrustedApplicationRef self_app = nullptr;
  OSStatus status = SecTrustedApplicationCreateFromPath(nullptr, &self_app);
  if (status != errSecSuccess || !self_app) {
    Refused(status, "name this app on the item's access list", reason);
    return nullptr;
  }
  NSArray* trusted = @[ (__bridge id)self_app ];
  SecAccessRef access = nullptr;
  status = SecAccessCreate((__bridge CFStringRef)label,
                           (__bridge CFArrayRef)trusted, &access);
  CFRelease(self_app);
  if (status != errSecSuccess || !access) {
    Refused(status, "create the item's access list", reason);
    return nullptr;
  }
  return access;
}

SecretStatus LookupIn(NSString* service, NSString* account, bool dp,
                      std::string* value, std::string* reason,
                      OSStatus* raw = nullptr) {
  NSMutableDictionary* q = Query(service, account, dp);
  // The login keychain: the item and its attributes first, without its
  // value (no decryption, so no prompt), to see whose it is.
  q[dp ? (id)kSecReturnData : (id)kSecReturnRef] = @YES;
  q[(id)kSecReturnAttributes] = @YES;
  q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
  CFTypeRef out = nullptr;
  OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &out);
  if (raw)
    *raw = status;
  if (status == errSecItemNotFound)
    return SecretStatus::kNotFound;
  if (status != errSecSuccess)
    return Refused(status, "read the item", reason);
  NSDictionary* item = CFBridgingRelease(out);
  if ([item[(id)kSecAttrComment] isEqual:kDeletedMarker])
    return SecretStatus::kNotFound;
  NSData* data = item[(id)kSecValueData] ?: [NSData data];
  if (!dp) {
    SecKeychainItemRef ref = (__bridge SecKeychainItemRef)item[(id)kSecValueRef];
    // Another program's (planted, or its value replaced): not this app's
    // secret, and never a prompt for it.
    if (IsAnotherProgramsItem(ref))
      return SecretStatus::kNotFound;
    UInt32 length = 0;
    void* bytes = nullptr;
    status = ref ? SecKeychainItemCopyContent(ref, nullptr, nullptr, &length,
                                              &bytes)
                 : errSecItemNotFound;
    if (raw)
      *raw = status;
    if (status == errSecItemNotFound)
      return SecretStatus::kNotFound;
    if (status != errSecSuccess)
      return Refused(status, "read the item", reason);
    data = [NSData dataWithBytes:bytes length:length];
    SecKeychainItemFreeContent(nullptr, bytes);
  }
  NSString* text = [[NSString alloc] initWithData:data
                                         encoding:NSUTF8StringEncoding];
  if (!text) {
    *reason = "the stored value is not UTF-8 text";
    return SecretStatus::kFailed;
  }
  *value = std::string(static_cast<const char*>(data.bytes), data.length);
  return SecretStatus::kOk;
}

SecretStatus StoreIn(NSString* service, NSString* account, NSString* label,
                     NSData* data, bool dp, std::string* reason,
                     OSStatus* raw = nullptr) {
  NSMutableDictionary* q = Query(service, account, dp);
  if (!dp) {
    // Never this app's secret into another program's item (planted, or its
    // value replaced): its access list may let that program read it.
    NSMutableDictionary* find = Query(service, account, dp);
    find[(id)kSecReturnRef] = @YES;
    find[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
    CFTypeRef found = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)find, &found) ==
        errSecSuccess) {
      bool theirs = IsAnotherProgramsItem((SecKeychainItemRef)found);
      CFRelease(found);
      if (theirs) {
        *reason =
            "another program's keychain item for this service and account is "
            "in the way (one it planted, or this app's whose value it "
            "replaced): delete it (Keychain Access, or `security "
            "delete-generic-password`) first";
        return SecretStatus::kUnavailable;
      }
    }
  }
  OSStatus status = SecItemUpdate(
      (__bridge CFDictionaryRef)q,
      (__bridge CFDictionaryRef) @{
        (id)kSecValueData : data,
        (id)kSecAttrLabel : label,
        (id)kSecAttrComment : @"",
      });
  if (raw)
    *raw = status;
  if (status == errSecSuccess)
    return SecretStatus::kOk;
  if (status != errSecItemNotFound)
    return Refused(status, "replace the item", reason);
  NSMutableDictionary* add = Query(service, account, dp);
  add[(id)kSecValueData] = data;
  add[(id)kSecAttrLabel] = label;
  if (dp) {
    // Readable once the Mac was unlocked after a restart (a background
    // refresh while the screen is locked still works), never synced or
    // restored to another Mac.
    add[(id)kSecAttrAccessible] =
        (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
  } else {
    SecAccessRef access = CreateOwnAccess(label, reason);
    if (!access)
      return SecretStatus::kUnavailable;
    add[(id)kSecAttrAccess] = CFBridgingRelease(access);
  }
  status = SecItemAdd((__bridge CFDictionaryRef)add, nullptr);
  if (raw)
    *raw = status;
  if (status == errSecSuccess)
    return SecretStatus::kOk;
  if (status == errSecDuplicateItem) {
    *reason =
        "another program's keychain item for this service and account is in "
        "the way (one this app did not write, e.g. with `security "
        "add-generic-password`): delete it with the tool that wrote it first";
    return SecretStatus::kUnavailable;
  }
  return Refused(status, "add the item", reason);
}

SecretStatus DeleteIn(NSString* service, NSString* account, bool dp,
                      std::string* reason, OSStatus* raw = nullptr) {
  OSStatus status =
      SecItemDelete((__bridge CFDictionaryRef)Query(service, account, dp));
  if (raw)
    *raw = status;
  if (status == errSecSuccess || status == errSecItemNotFound)
    return SecretStatus::kOk;
  if (!dp &&
      (status == errSecInvalidOwnerEdit || status == errSecAuthFailed)) {
    // The login keychain lets only the item's owner remove it, and macOS may
    // not take this process for it without asking (seen with an ad-hoc
    // signed CEF bundle) even though the item's access list lets it change
    // the value. Then the value is wiped instead: empty, marked deleted, read
    // as "not found", reused by the next store.
    OSStatus wiped = SecItemUpdate(
        (__bridge CFDictionaryRef)Query(service, account, dp),
        (__bridge CFDictionaryRef) @{
          (id)kSecValueData : [NSData data],
          (id)kSecAttrComment : kDeletedMarker,
        });
    if (raw)
      *raw = wiped;
    if (wiped == errSecSuccess)
      return SecretStatus::kOk;
  }
  return Refused(status, "delete the item", reason);
}

// The data-protection keychain lacks the item: one this app wrote to the
// login keychain before it had the entitlement moves over (no prompt: this
// app is on its access list; a lookup that would have to ask is "not
// found").
SecretStatus MoveFromLoginKeychain(NSString* service, NSString* account,
                                   std::string* value, std::string* reason) {
  std::string old_value, ignored;
  SecretStatus found;
  {
    NoPrompts no_prompts;
    found = LookupIn(service, account, false, &old_value, &ignored);
  }
  if (found != SecretStatus::kOk)
    return SecretStatus::kNotFound;
  NoPrompts no_prompts;
  NSData* data = [NSData dataWithBytes:old_value.data()
                                length:old_value.size()];
  SecretStatus stored = StoreIn(service, account, service, data, true, reason);
  if (stored != SecretStatus::kOk)
    return stored;
  DeleteIn(service, account, false, &ignored);
  *value = old_value;
  return SecretStatus::kOk;
}

// One call's handoff between its caller and the queue.
struct Call {
  std::mutex mu;
  std::condition_variable cv;
  bool started = false;
  bool abandoned = false;
  bool done = false;
  SecretStatus status = SecretStatus::kFailed;
  std::string value;
  std::string reason;
};

dispatch_queue_t Queue() {
  static dispatch_queue_t queue =
      dispatch_queue_create("dev.laufey.secret-store", DISPATCH_QUEUE_SERIAL);
  return queue;
}

// Runs `work` on the queue and waits up to `timeout_ms` for it. A call that
// may prompt (a lookup) is given up on at the timeout; one that can't (a
// store or delete, with prompts off) is waited for once it started, so its
// answer is the truth about what was written.
SecretStatus RunOnQueue(
    uint32_t timeout_ms, bool may_prompt,
    std::function<SecretStatus(std::string* value, std::string* reason)> work,
    std::string* value, std::string* reason) {
  auto call = std::make_shared<Call>();
  dispatch_async(Queue(), ^{
    {
      std::lock_guard<std::mutex> lock(call->mu);
      if (call->abandoned)
        return;
      call->started = true;
    }
    std::string v, r;
    SecretStatus status;
    @autoreleasepool {
      status = work(&v, &r);
    }
    std::lock_guard<std::mutex> lock(call->mu);
    call->status = status;
    call->value = std::move(v);
    call->reason = std::move(r);
    call->done = true;
    call->cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(call->mu);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(TimeoutOrDefault(timeout_ms));
  if (!call->cv.wait_until(lock, deadline, [&] { return call->done; })) {
    if (call->started && !may_prompt) {
      call->cv.wait(lock, [&] { return call->done; });
    } else {
      call->abandoned = true;
      *reason =
          "the keychain did not answer within " +
          std::to_string(TimeoutOrDefault(timeout_ms)) +
          " ms (a keychain prompt is waiting for the person, or an earlier "
          "call is)";
      return SecretStatus::kUnavailable;
    }
  }
  if (value)
    *value = call->value;
  *reason = call->reason;
  return call->status;
}

char* Dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out)
    std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

int Answer(SecretStatus status, const std::string& reason, char** reason_out) {
  if (reason_out)
    *reason_out = reason.empty() ? nullptr : Dup(reason);
  return static_cast<int>(status);
}

}  // namespace

SecretStatus SecretLookup(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* value, std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  NSString* s = Utf8(service);
  NSString* a = Utf8(account);
  return RunOnQueue(
      timeout_ms, /*may_prompt=*/true,
      [s, a](std::string* v, std::string* r) {
        bool dp = UseDataProtection();
        OSStatus raw = errSecSuccess;
        SecretStatus status = LookupIn(s, a, dp, v, r, &raw);
        if (dp && raw == errSecMissingEntitlement) {
          // The entitlement isn't usable after all (see SecretStore).
          g_data_protection = dp = false;
          r->clear();
          status = LookupIn(s, a, false, v, r);
        }
        if (status == SecretStatus::kNotFound && dp)
          return MoveFromLoginKeychain(s, a, v, r);
        return status;
      },
      value, reason);
}

SecretStatus SecretStore(const std::string& service, const std::string& account,
                         const std::string& label, const std::string& value,
                         uint32_t timeout_ms, std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  NSString* text = Utf8(value);
  if (!text) {
    *reason = "the value must be UTF-8 text";
    return SecretStatus::kFailed;
  }
  NSString* s = Utf8(service);
  NSString* a = Utf8(account);
  NSString* l = label.empty() ? s : (Utf8(label) ?: s);
  NSData* data = [NSData dataWithBytes:value.data() length:value.size()];
  return RunOnQueue(
      timeout_ms, /*may_prompt=*/false,
      [s, a, l, data](std::string*, std::string* r) {
        NoPrompts no_prompts;
        bool dp = UseDataProtection();
        OSStatus raw = errSecSuccess;
        SecretStatus status = StoreIn(s, a, l, data, dp, r, &raw);
        if (dp && raw == errSecMissingEntitlement) {
          // The entitlement isn't usable after all (a profile that doesn't
          // grant a keychain access group): the login keychain from now on.
          g_data_protection = false;
          r->clear();
          status = StoreIn(s, a, l, data, false, r);
        }
        if (status == SecretStatus::kOk && dp && g_data_protection) {
          // An older copy in the login keychain would otherwise come back on
          // a lookup after a delete.
          std::string ignored;
          DeleteIn(s, a, false, &ignored);
        }
        return status;
      },
      nullptr, reason);
}

SecretStatus SecretDelete(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  NSString* s = Utf8(service);
  NSString* a = Utf8(account);
  return RunOnQueue(
      timeout_ms, /*may_prompt=*/false,
      [s, a](std::string*, std::string* r) {
        NoPrompts no_prompts;
        bool dp = UseDataProtection();
        OSStatus raw = errSecSuccess;
        SecretStatus status = DeleteIn(s, a, dp, r, &raw);
        if (dp && raw == errSecMissingEntitlement) {
          // The entitlement isn't usable after all (see SecretStore).
          g_data_protection = dp = false;
          r->clear();
          status = DeleteIn(s, a, false, r);
        }
        // A copy this app left in the login keychain before it had the
        // entitlement would come back on the next lookup.
        if (status == SecretStatus::kOk && dp)
          status = DeleteIn(s, a, false, r);
        return status;
      },
      nullptr, reason);
}

int SecretLookupForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** value, char** reason) {
  if (value)
    *value = nullptr;
  std::string v, r;
  SecretStatus status = SecretLookup(
      service ? service : "", account ? account : "", timeout_ms, &v, &r);
  if (status == SecretStatus::kOk && value)
    *value = Dup(v);
  return Answer(status, r, reason);
}

int SecretStoreForAbi(const char* service, const char* account,
                      const char* label, const char* value, uint32_t timeout_ms,
                      char** reason) {
  std::string r;
  if (!value) {
    return Answer(SecretStatus::kFailed, "the value must not be NULL", reason);
  }
  SecretStatus status =
      SecretStore(service ? service : "", account ? account : "",
                  label ? label : "", value, timeout_ms, &r);
  return Answer(status, r, reason);
}

int SecretDeleteForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** reason) {
  std::string r;
  SecretStatus status = SecretDelete(service ? service : "",
                                     account ? account : "", timeout_ms, &r);
  return Answer(status, r, reason);
}

std::string SecretOwnPartitionId() {
  __block std::string id;
  dispatch_sync(Queue(), ^{
    id = OwnPartitionId();
  });
  return id;
}

void SecretRequireOwnPartitionForTesting(bool on) {
  dispatch_sync(Queue(), ^{
    g_require_own_partition_for_testing = on;
  });
}

const char* SecretKeychainKind() {
  __block bool dp = false;
  dispatch_sync(Queue(), ^{
    dp = UseDataProtection();
  });
  return dp ? "data-protection" : "login";
}

}  // namespace laufey_common
