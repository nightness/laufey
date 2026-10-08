// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Another program of the same user, for secret_store_mac_test: reads the
// generic password for (service, account) the way any program could (no
// creator filter), with keychain prompts off so the answer is deterministic.
// Prints "status <OSStatus>" and, when it got the secret, "value <secret>".
// With a value, it writes its own item instead (the test's control: what it
// wrote, a later run of it reads back); `--delete` deletes that item.
// `--modify <value>` replaces the value of the first item for (service,
// account) the way `security add-generic-password -U` does
// (SecKeychainItemModifyContent). `--plant <value> <app>` creates an item
// with the secure store's creator code ('Lfy1') whose access list trusts
// <app>, as any program may.
//
//   laufey_secret_store_mac_reader <service> <account>
//       [<value> | --delete | --modify <value> | --plant <value> <app>]

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <cstdio>
#include <cstring>

// SecKeychainSetUserInteractionAllowed: the login keychain's own switch.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

int main(int argc, char** argv) {
  if (argc < 3 || argc > 6) {
    std::fprintf(stderr, "usage: %s <service> <account> [<value>]\n",
                 argv[0]);
    return 2;
  }
  @autoreleasepool {
    SecKeychainSetUserInteractionAllowed(false);
    NSString* service = [NSString stringWithUTF8String:argv[1]];
    NSString* account = [NSString stringWithUTF8String:argv[2]];
    if (argc == 5 && std::strcmp(argv[3], "--modify") == 0) {
      NSDictionary* query = @{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : service,
        (id)kSecAttrAccount : account,
        (id)kSecReturnRef : @YES,
        (id)kSecMatchLimit : (id)kSecMatchLimitOne,
      };
      CFTypeRef item = nullptr;
      OSStatus status =
          SecItemCopyMatching((__bridge CFDictionaryRef)query, &item);
      if (status == errSecSuccess) {
        status = SecKeychainItemModifyContent(
            (SecKeychainItemRef)item, nullptr,
            static_cast<UInt32>(std::strlen(argv[4])), argv[4]);
        CFRelease(item);
      }
      std::printf("status %d\n", static_cast<int>(status));
      return 0;
    }
    if (argc == 6 && std::strcmp(argv[3], "--plant") == 0) {
      SecTrustedApplicationRef app = nullptr;
      OSStatus status = SecTrustedApplicationCreateFromPath(argv[5], &app);
      SecAccessRef access = nullptr;
      if (status == errSecSuccess) {
        status = SecAccessCreate((__bridge CFStringRef)service,
                                 (__bridge CFArrayRef) @[ (__bridge id)app ],
                                 &access);
        CFRelease(app);
      }
      if (status == errSecSuccess) {
        NSDictionary* item = @{
          (id)kSecClass : (id)kSecClassGenericPassword,
          (id)kSecAttrService : service,
          (id)kSecAttrAccount : account,
          (id)kSecAttrLabel : service,
          (id)kSecAttrCreator : @((FourCharCode)'Lfy1'),
          (id)kSecAttrAccess : CFBridgingRelease(access),
          (id)kSecValueData : [NSData dataWithBytes:argv[4]
                                             length:std::strlen(argv[4])],
        };
        status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
      }
      std::printf("status %d\n", static_cast<int>(status));
      return 0;
    }
    if (argc == 4 && std::strcmp(argv[3], "--delete") == 0) {
      NSDictionary* item = @{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : service,
        (id)kSecAttrAccount : account,
      };
      OSStatus status = SecItemDelete((__bridge CFDictionaryRef)item);
      std::printf("status %d\n", static_cast<int>(status));
      return 0;
    }
    if (argc == 4) {
      NSDictionary* item = @{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : service,
        (id)kSecAttrAccount : account,
        (id)kSecValueData : [NSData dataWithBytes:argv[3]
                                           length:std::strlen(argv[3])],
      };
      OSStatus status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
      std::printf("status %d\n", static_cast<int>(status));
      return 0;
    }
    NSDictionary* query = @{
      (id)kSecClass : (id)kSecClassGenericPassword,
      (id)kSecAttrService : service,
      (id)kSecAttrAccount : account,
      (id)kSecReturnData : @YES,
      (id)kSecMatchLimit : (id)kSecMatchLimitOne,
    };
    CFTypeRef out = nullptr;
    OSStatus status =
        SecItemCopyMatching((__bridge CFDictionaryRef)query, &out);
    std::printf("status %d\n", static_cast<int>(status));
    if (status == errSecSuccess && out) {
      NSData* data = CFBridgingRelease(out);
      std::printf("value %.*s\n", static_cast<int>(data.length),
                  static_cast<const char*>(data.bytes));
    }
  }
  return 0;
}
