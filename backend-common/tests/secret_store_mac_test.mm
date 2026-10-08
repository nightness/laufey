// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The secure store on macOS (secret_store_mac.mm) against the real login
// keychain (this test binary has no keychain entitlement, so it is the login
// keychain's access-list path): bad arguments, the round trip (store,
// replace, lookup, delete), and that the item is this program's own:
//
//   - fail first: an item written the old way (`security
//     add-generic-password`) is read back by `security find-generic-password
//     -w`, by any program of the user, without a prompt;
//   - such an item is never read here (a lookup is "not found", with no
//     prompt) and is in the way of a store (refused with the reason);
//   - an item stored here is refused to another program
//     (laufey_secret_store_mac_reader, prompts off: errSecAuthFailed), and
//     with LAUFEY_SECRET_TEST_SECURITY_CLI=1 `security find-generic-password
//     -w` doesn't print it (it is denied, or waits on macOS's prompt until
//     it is killed). Off by default: on a desktop the prompt is on screen.
//   - an item is this app's by the partition list macOS stamps on it: the
//     store's item carries SecretOwnPartitionId(); one another program of
//     the user replaced (SecKeychainItemModifyContent, what `security
//     add-generic-password -U` does; it succeeds without a prompt and
//     re-stamps the partition) or planted (the store's creator code, an
//     access list trusting this program) is, for an Apple- or team-signed
//     app (SecretRequireOwnPartitionForTesting here), "not found" without a
//     prompt, and a store refuses it. Every unsigned program shares the
//     partition "unsigned:", so an unsigned build reads both (the limit
//     docs/secure-store.md states). With prompts off throughout.
//
// Exits 77 (skipped) when there is no usable login keychain (locked, or none)
// unless LAUFEY_SECRET_TEST_REQUIRE=1.

#include "laufey_secret_store.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

// SecKeychainItemCopyAccess / SecACL* / SecKeychainSetUserInteractionAllowed:
// the login keychain's own (deprecated, still working) API.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

using laufey_common::SecretDelete;
using laufey_common::SecretLookup;
using laufey_common::SecretStatus;
using laufey_common::SecretStore;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Cleanup();                                                             \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

constexpr uint32_t kTimeout = 15000;
std::string g_service;

struct Ran {
  bool timed_out = false;
  int status = -1;
  std::string out;
};

// Runs argv with stdout captured, killed after `timeout_ms`.
Ran Run(const std::vector<std::string>& args, int timeout_ms) {
  Ran ran;
  int fds[2];
  if (pipe(fds) != 0)
    return ran;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  std::vector<char*> argv;
  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(),
                            environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (spawned != 0) {
    close(fds[0]);
    return ran;
  }
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int status = 0;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      ran.timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0)
    ran.out.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  ran.status = ran.timed_out ? -1 : status;
  return ran;
}

Ran SecurityCli(std::vector<std::string> args, int timeout_ms = 10000) {
  args.insert(args.begin(), "/usr/bin/security");
  return Run(args, timeout_ms);
}

void Cleanup() {
  for (const char* account : {"a", "legacy", "r", "planted"}) {
    std::string ignored;
    SecretDelete(g_service, account, kTimeout, &ignored);
  }
  // Each program deletes its own items (another's would need a prompt).
  SecurityCli({"delete-generic-password", "-s", g_service, "-a", "legacy"});
  Run({LAUFEY_SECRET_READER, g_service, "reader", "--delete"}, 10000);
  Run({LAUFEY_SECRET_READER, g_service, "planted", "--delete"}, 10000);
}

// This program's own path (what an access list naming it names).
std::string SelfPath() {
  char buf[PATH_MAX];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0)
    return "";
  char real[PATH_MAX];
  return realpath(buf, real) ? real : buf;
}

// The partition list of the store's item for `account` (its access list's
// ACLAuthorizationPartitionID entry: a hex-encoded plist {Partitions: [..]}),
// read without authorization; printed.
std::vector<std::string> Partitions(const char* account) {
  std::vector<std::string> out;
  NSDictionary* query = @{
    (id)kSecClass : (id)kSecClassGenericPassword,
    (id)kSecAttrService : @(g_service.c_str()),
    (id)kSecAttrAccount : @(account),
    (id)kSecAttrCreator : @((FourCharCode)'Lfy1'),
    (id)kSecReturnRef : @YES,
  };
  CFTypeRef item = nullptr;
  SecAccessRef access = nullptr;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &item) ==
      errSecSuccess) {
    SecKeychainItemCopyAccess((SecKeychainItemRef)item, &access);
    CFRelease(item);
  }
  CFArrayRef acls = nullptr;
  if (access && SecAccessCopyACLList(access, &acls) == errSecSuccess) {
    for (id entry in (__bridge NSArray*)acls) {
      SecACLRef acl = (__bridge SecACLRef)entry;
      NSArray* auths = CFBridgingRelease(SecACLCopyAuthorizations(acl));
      if (![auths containsObject:(id)kSecACLAuthorizationPartitionID])
        continue;
      CFArrayRef apps = nullptr;
      CFStringRef desc = nullptr;
      SecKeychainPromptSelector selector = 0;
      SecACLCopyContents(acl, &apps, &desc, &selector);
      if (apps)
        CFRelease(apps);
      NSString* hex = CFBridgingRelease(desc);
      NSMutableData* bytes = [NSMutableData data];
      for (NSUInteger i = 0; i + 1 < hex.length; i += 2) {
        unsigned value = 0;
        sscanf([hex substringWithRange:NSMakeRange(i, 2)].UTF8String, "%2x",
               &value);
        uint8_t b = static_cast<uint8_t>(value);
        [bytes appendBytes:&b length:1];
      }
      NSDictionary* plist =
          [NSPropertyListSerialization propertyListWithData:bytes
                                                    options:0
                                                     format:nullptr
                                                      error:nil];
      for (NSString* p in plist[@"Partitions"])
        out.push_back(p.UTF8String);
    }
    CFRelease(acls);
  }
  if (access)
    CFRelease(access);
  std::printf("partitions of %s:", account);
  for (const std::string& p : out)
    std::printf(" %s", p.c_str());
  std::printf("\n");
  return out;
}

bool Has(const std::vector<std::string>& list, const std::string& item) {
  return std::find(list.begin(), list.end(), item) != list.end();
}

bool Env(const char* name) {
  const char* v = std::getenv(name);
  return v && std::strcmp(v, "1") == 0;
}

}  // namespace

int main() {
  @autoreleasepool {
    g_service = "dev.laufey.secret-store-test." + std::to_string(getpid());
    std::string value, reason;

    // Bad arguments: refused before the keychain.
    EXPECT(SecretLookup("", "a", kTimeout, &value, &reason) ==
           SecretStatus::kFailed);
    EXPECT(SecretStore(g_service, "", "", "v", kTimeout, &reason) ==
           SecretStatus::kFailed);
    EXPECT(SecretLookup(g_service, "\xff\xfe", kTimeout, &value, &reason) ==
           SecretStatus::kFailed);
    EXPECT(SecretStore(g_service, "a", "", std::string("a\0b", 3), kTimeout,
                       &reason) == SecretStatus::kFailed);
    EXPECT(reason.find("UTF-8") != std::string::npos);

    EXPECT(std::strcmp(laufey_common::SecretKeychainKind(), "login") == 0);

    // A usable login keychain? (A locked one can't ask here: unavailable.)
    reason.clear();
    SecretStatus first = SecretLookup(g_service, "a", kTimeout, &value, &reason);
    if (first == SecretStatus::kUnavailable) {
      std::printf("laufey_secret_store_mac_test: no usable keychain: %s\n",
                  reason.c_str());
      if (Env("LAUFEY_SECRET_TEST_REQUIRE"))
        return 1;
      std::printf("laufey_secret_store_mac_test: skipped\n");
      return 77;
    }
    EXPECT(first == SecretStatus::kNotFound);

    // Fail first: an item the `security` CLI wrote (the old path) is read
    // back by any program of the user through `security`, with no prompt.
    Ran added = SecurityCli({"add-generic-password", "-s", g_service, "-a",
                             "legacy", "-w", "old-secret"});
    EXPECT(!added.timed_out && added.status == 0);
    Ran leaked = SecurityCli(
        {"find-generic-password", "-s", g_service, "-a", "legacy", "-w"});
    EXPECT(!leaked.timed_out && leaked.out == "old-secret\n");

    // Such an item is not this app's: never read here (no prompt), and in
    // the way of a store until the tool that wrote it deletes it.
    reason.clear();
    EXPECT(SecretLookup(g_service, "legacy", kTimeout, &value, &reason) ==
           SecretStatus::kNotFound);
    reason.clear();
    EXPECT(SecretStore(g_service, "legacy", "", "new", kTimeout, &reason) ==
           SecretStatus::kUnavailable);
    EXPECT(reason.find("in the way") != std::string::npos);
    SecurityCli({"delete-generic-password", "-s", g_service, "-a", "legacy"});
    reason.clear();
    EXPECT(SecretStore(g_service, "legacy", "", "new", kTimeout, &reason) ==
           SecretStatus::kOk);
    EXPECT(SecretLookup(g_service, "legacy", kTimeout, &value, &reason) ==
               SecretStatus::kOk &&
           value == "new");

    // The round trip: store, replace, lookup.
    const std::string secret = "laufey \xe2\x9c\x93 \"quoted\"\nline 2";
    reason.clear();
    EXPECT(SecretStore(g_service, "a", "laufey test", "one", kTimeout,
                       &reason) == SecretStatus::kOk);
    EXPECT(SecretStore(g_service, "a", "laufey test", secret, kTimeout,
                       &reason) == SecretStatus::kOk);
    value.clear();
    EXPECT(SecretLookup(g_service, "a", kTimeout, &value, &reason) ==
           SecretStatus::kOk);
    EXPECT(value == secret);

    // Another program of the user is refused: no prompt allowed, no secret.
    Ran other = Run({LAUFEY_SECRET_READER, g_service, "a"}, 10000);
    std::printf("reader: %s", other.out.c_str());
    EXPECT(!other.timed_out);
    EXPECT(other.out.find("value") == std::string::npos);
    EXPECT(other.out.find("status " +
                          std::to_string(static_cast<int>(
                              errSecAuthFailed))) != std::string::npos ||
           other.out.find("status " +
                          std::to_string(static_cast<int>(
                              errSecInteractionNotAllowed))) !=
               std::string::npos);
    // The control (its query is right): the reader does read back an item
    // it wrote itself, in a run of its own.
    Ran wrote = Run({LAUFEY_SECRET_READER, g_service, "reader", "its-own"},
                    10000);
    EXPECT(wrote.out.find("status 0") != std::string::npos);
    Ran control = Run({LAUFEY_SECRET_READER, g_service, "reader"}, 10000);
    EXPECT(control.out.find("value its-own") != std::string::npos);

    if (Env("LAUFEY_SECRET_TEST_SECURITY_CLI")) {
      // `security find-generic-password -w`, the command that read the old
      // items: denied, or held at macOS's prompt (killed): never the secret.
      Ran cli = SecurityCli(
          {"find-generic-password", "-s", g_service, "-a", "a", "-w"}, 8000);
      std::printf("security -w: %s (exit %d)\n",
                  cli.timed_out ? "held at the prompt, killed" : "answered",
                  cli.status);
      EXPECT(cli.out.find("laufey") == std::string::npos);
      EXPECT(cli.timed_out || cli.status != 0);
    }

    // The item is this app's by its partition (see the top). Prompts off:
    // a lookup that would have to ask is refused instead.
    {
      SecKeychainSetUserInteractionAllowed(false);
      const std::string own = laufey_common::SecretOwnPartitionId();
      std::printf("own partition: %s\n", own.c_str());
      EXPECT(!own.empty());
      const bool shared = own == "unsigned:";
      reason.clear();
      EXPECT(SecretStore(g_service, "r", "", "mine", kTimeout, &reason) ==
             SecretStatus::kOk);
      EXPECT(Has(Partitions("r"), own));
      laufey_common::SecretRequireOwnPartitionForTesting(true);
      value.clear();
      EXPECT(SecretLookup(g_service, "r", kTimeout, &value, &reason) ==
                 SecretStatus::kOk &&
             value == "mine");

      // Replaced by another program: it can, without a prompt.
      Ran modified = Run({LAUFEY_SECRET_READER, g_service, "r", "--modify",
                          "replaced"},
                         10000);
      std::printf("replaced by another program: %s", modified.out.c_str());
      EXPECT(modified.out.find("status 0") != std::string::npos);
      std::vector<std::string> after = Partitions("r");
      value.clear();
      reason.clear();
      SecretStatus replaced =
          SecretLookup(g_service, "r", kTimeout, &value, &reason);
      std::printf("lookup of the replaced item: %d %s %s\n",
                  static_cast<int>(replaced), value.c_str(), reason.c_str());
      if (shared) {
        EXPECT(replaced == SecretStatus::kOk && value == "replaced");
      } else {
        EXPECT(!Has(after, own));
        EXPECT(replaced == SecretStatus::kNotFound);
        reason.clear();
        EXPECT(SecretStore(g_service, "r", "", "again", kTimeout, &reason) ==
               SecretStatus::kUnavailable);
        EXPECT(reason.find("in the way") != std::string::npos);
      }

      // Planted by another program: the store's creator code, an access list
      // trusting this program.
      Ran planted = Run({LAUFEY_SECRET_READER, g_service, "planted",
                         "--plant", "planted-value", SelfPath()},
                        10000);
      std::printf("planted by another program: %s", planted.out.c_str());
      EXPECT(planted.out.find("status 0") != std::string::npos);
      std::vector<std::string> theirs = Partitions("planted");
      value.clear();
      reason.clear();
      SecretStatus read =
          SecretLookup(g_service, "planted", kTimeout, &value, &reason);
      std::printf("lookup of the planted item: %d %s %s\n",
                  static_cast<int>(read), value.c_str(), reason.c_str());
      if (shared) {
        EXPECT(read == SecretStatus::kOk && value == "planted-value");
      } else {
        EXPECT(!Has(theirs, own));
        EXPECT(read == SecretStatus::kNotFound);
        reason.clear();
        EXPECT(SecretStore(g_service, "planted", "", "secret", kTimeout,
                           &reason) == SecretStatus::kUnavailable);
        EXPECT(reason.find("in the way") != std::string::npos);
      }
      laufey_common::SecretRequireOwnPartitionForTesting(false);
      SecKeychainSetUserInteractionAllowed(true);
    }

    // Delete, then it is gone; deleting nothing succeeds.
    reason.clear();
    EXPECT(SecretDelete(g_service, "a", kTimeout, &reason) ==
           SecretStatus::kOk);
    EXPECT(SecretLookup(g_service, "a", kTimeout, &value, &reason) ==
           SecretStatus::kNotFound);
    EXPECT(SecretDelete(g_service, "a", kTimeout, &reason) ==
           SecretStatus::kOk);
    EXPECT(SecretDelete(g_service, "legacy", kTimeout, &reason) ==
           SecretStatus::kOk);

    // A delete macOS refused wipes the value and marks the item deleted (see
    // DeleteIn): such an item reads as "not found", and a store reuses it.
    NSDictionary* wiped = @{
      (id)kSecClass : (id)kSecClassGenericPassword,
      (id)kSecAttrService : @(g_service.c_str()),
      (id)kSecAttrAccount : @"a",
      (id)kSecAttrCreator : @((FourCharCode)'Lfy1'),
      (id)kSecAttrComment : @"laufey:deleted",
      (id)kSecValueData : [NSData data],
    };
    EXPECT(SecItemAdd((__bridge CFDictionaryRef)wiped, nullptr) ==
           errSecSuccess);
    EXPECT(SecretLookup(g_service, "a", kTimeout, &value, &reason) ==
           SecretStatus::kNotFound);
    EXPECT(SecretStore(g_service, "a", "", "back", kTimeout, &reason) ==
           SecretStatus::kOk);
    EXPECT(SecretLookup(g_service, "a", kTimeout, &value, &reason) ==
               SecretStatus::kOk &&
           value == "back");
    EXPECT(SecretDelete(g_service, "a", kTimeout, &reason) ==
           SecretStatus::kOk);

    // The C ABI: out strings malloc'd, NULL out pointers allowed.
    char* out = nullptr;
    char* why = nullptr;
    EXPECT(laufey_common::SecretStoreForAbi(g_service.c_str(), "a", nullptr,
                                            "abi", kTimeout, nullptr) == 0);
    EXPECT(laufey_common::SecretLookupForAbi(g_service.c_str(), "a", kTimeout,
                                             &out, &why) == 0);
    EXPECT(out && std::strcmp(out, "abi") == 0);
    std::free(out);
    std::free(why);
    EXPECT(laufey_common::SecretDeleteForAbi(g_service.c_str(), "a", kTimeout,
                                             nullptr) == 0);
    EXPECT(laufey_common::SecretStoreForAbi(g_service.c_str(), "a", nullptr,
                                            nullptr, kTimeout, &why) == 3);
    std::free(why);

    Cleanup();
  }
  std::printf("laufey_secret_store_mac_test: ok\n");
  return 0;
}
