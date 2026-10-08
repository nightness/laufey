# Secure store

A small secret per (service, account) in the OS's secret store (API 47):

```rust
use std::time::Duration;
let t = Duration::from_secs(20);
laufey::secret_store("com.example.app", "refresh-token", "Example", "s3cret", t)?;
let value = laufey::secret_lookup("com.example.app", "refresh-token", t)?; // Some("s3cret")
laufey::secret_delete("com.example.app", "refresh-token", t)?;
```

The C ABI entry points are `secret_lookup`, `secret_store` and `secret_delete`.
Each returns a `LAUFEY_SECRET_*` status (`OK`, `NOT_FOUND`, `UNAVAILABLE`,
`FAILED`) with the value and a reason as strings freed with `string_free`. Each
**blocks the calling thread** for at most about its timeout (0: 20 s): call it
off the UI thread.

They exist on Linux and macOS, in the CEF and WebView backends. On Windows (and
on Winit) they are `NULL`: the embedder uses the Credential Locker itself.

## macOS

The secret is a generic password in the Keychain (`kSecAttrService` = service,
`kSecAttrAccount` = account, the label as `kSecAttrLabel`), written and read by
the app's own process through Security.framework (`SecItemAdd` /
`SecItemCopyMatching` / `SecItemUpdate` / `SecItemDelete`), so the item belongs
to the app, not to a tool it ran. Which keychain depends on how the app is
signed:

| The app                                                                                                                                                                   | Keychain                                                                                                                                                                                                        | Who can read the item                                                                                                                                                                                                                                                                                            |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Signed with a keychain access group: the `com.apple.application-identifier` or `keychain-access-groups` entitlement, which macOS honours only with a provisioning profile | The data-protection keychain (`kSecUseDataProtectionKeychain`), in the app's default access group, `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`                                                           | The app (and apps the developer put in the same access group). No other program, with or without a prompt; `security` doesn't list it.                                                                                                                                                                           |
| Signed with a Developer ID, no profile                                                                                                                                    | The login keychain, with an access list (`SecAccessCreate`) whose only trusted application is the app's host executable (`SecTrustedApplicationCreateFromPath(NULL)`), identified by its designated requirement | The app, and its updates signed by the same identity, without a prompt. Any other program of the user, `security find-generic-password -w` included, gets macOS's prompt (the keychain password, or the person's Allow / Always Allow), never the secret silently.                                               |
| Ad-hoc signed or unsigned (a development build)                                                                                                                           | The same, the host executable identified by its code hash                                                                                                                                                       | The same, but the access list names this exact build: a rebuilt app is another program to macOS, so its first lookup shows the prompt (Always Allow adds it). Builds whose host executables are byte-identical are the same program to macOS: they read each other's items. Other programs are refused as above. |

In every case, code injected into the app's process (a loaded library, a
debugger attached to it) reads the secret as the app does.

The entitlement is read from the running app's signature
(`SecTaskCopyValueForEntitlement`); if the data-protection keychain refuses it
anyway (`errSecMissingEntitlement`), the login keychain is used from then on. An
item the app wrote to the login keychain before it had the entitlement moves to
the data-protection keychain on its first lookup.

Items written here carry `kSecAttrCreator` `'Lfy1'`, and every query matches it.
An item another tool wrote for the same service and account (for instance with
`security add-generic-password`, which makes `/usr/bin/security` the item's
trusted application, so every program of the user can read it through
`security`) is never read here: a lookup is `NOT_FOUND`, without a prompt. A
store refuses while such an item is in the way (`UNAVAILABLE`, "another
program's keychain item for this service and account is in the way"). Moving
those items over is the embedder's job, with the tool that wrote them: read it,
delete it, then store it here.

### Another program writing the item

The login keychain's access list guards reading an item, not writing it. Another
program of the same user can, without a prompt, replace the value of the app's
item (`SecKeychainItemModifyContent`, what `security add-generic-password -U`
does; the access list stays as it was), or create an item with the store's
creator code whose access list trusts the app (so the app could read a value
that program chose, or store its secret into an item that program can also
read). macOS stamps the writing program's partition ID on an item it creates and
re-stamps it when another program replaces the value, and a program can't stamp
another's without the keychain password. What the store makes of it depends on
the app's signature:

| The app                                                       | Its partition   | An item another program created or replaced                                                                                                                  |
| ------------------------------------------------------------- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Developer ID, development, Mac App Store or TestFlight signed | `teamid:<team>` | Read as `NOT_FOUND`, without a prompt; a store refuses it (`UNAVAILABLE`, "in the way"). Removing it (Keychain Access) lets the app store again.             |
| Ad-hoc signed                                                 | `cdhash:<hash>` | Can't be told from the app's own earlier build: a lookup shows macOS's prompt (the keychain password), and a store writes into it.                           |
| Unsigned                                                      | `unsigned:`     | Shared by every unsigned program: read and written as the app's own, without a prompt. An unsigned build has no protection against another program's writes. |

The data-protection keychain has none of this: no other program reaches the
app's access group at all.

Prompts: every call runs on one serial queue. A store or a delete never prompts
(user interaction is off while it runs): a locked keychain, or an item this
build may not change, is `UNAVAILABLE` with the reason. A lookup may show
macOS's prompt: a locked login keychain (unlock), or an ad-hoc signed app
rebuilt since it stored the item (Allow); it gives up after its timeout
(`UNAVAILABLE`), and the prompt stays up for the person. A call still queued
when its caller gave up never runs, so nothing is written after the call gave
up.

A delete that macOS refuses without asking (only an item's owner may remove a
login-keychain item, and an ad-hoc signed CEF bundle isn't always taken for it,
although its access list lets it change the value) wipes the value instead: the
item is left empty, marked deleted (`kSecAttrComment` `laufey:deleted`), reads
as `NOT_FOUND`, and the next store reuses it.

There is never a plaintext fallback.

## Linux

The secret lives in the Secret Service (`org.freedesktop.secrets`:
gnome-keyring, KWallet's Secret Service, KeePassXC), read and written through
libsecret (`libsecret-1.so.0`, loaded at run time: no build dependency, and
every desktop ships it). Items carry the attributes `service` and `account`, the
shape `secret-tool store --label=… service S account A` makes, so either reads
the other's items; a store replaces what is there (and clears duplicates written
before).

Around the secret, plain D-Bus reads that never prompt decide what can happen:

| Situation                                                                                                                                                                      | Answer                                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| No session bus                                                                                                                                                                 | `UNAVAILABLE`, at once.                                                                                                                                                                                                                                                                                                                                                                                      |
| libsecret isn't installed                                                                                                                                                      | `UNAVAILABLE`, naming the package (`libsecret-1-0` on Debian / Ubuntu, `libsecret` on Fedora).                                                                                                                                                                                                                                                                                                               |
| No provider, and KWallet runs without serving the Secret Service                                                                                                               | `UNAVAILABLE`: "enable KWallet's Secret Service (System Settings > KWallet: Use KWallet for the Secret Service interface), or install gnome-keyring".                                                                                                                                                                                                                                                        |
| No provider at all                                                                                                                                                             | `UNAVAILABLE`: "install gnome-keyring, or enable KWallet's Secret Service".                                                                                                                                                                                                                                                                                                                                  |
| A matching item (or, for a write, the default collection) is locked, and no one here can answer an unlock prompt (no graphical session, or gnome-keyring without its prompter) | `UNAVAILABLE` at once, naming the keyring ("the gnome-keyring keyring is locked …", "the KWallet wallet is closed …"). Never `NOT_FOUND`.                                                                                                                                                                                                                                                                    |
| Locked, and someone could answer                                                                                                                                               | The Secret Service is asked to unlock (its own prompt, on a thread of its own) and the call waits up to its timeout: an answer in time goes on, a prompt closed or unanswered is `UNAVAILABLE`. Never a hang. The prompt is never dismissed from here: it stays up for the person to answer, and a later answer only unlocks (nothing is written after the call gave up). One unlock is in flight at a time. |
| No default keyring (a write)                                                                                                                                                   | `UNAVAILABLE`: creating one is the desktop's keyring manager's job.                                                                                                                                                                                                                                                                                                                                          |
| The provider doesn't answer                                                                                                                                                    | `UNAVAILABLE` after the timeout.                                                                                                                                                                                                                                                                                                                                                                             |
| Nothing matches (and nothing locked does)                                                                                                                                      | `NOT_FOUND` (a lookup); `OK` (a delete).                                                                                                                                                                                                                                                                                                                                                                     |

There is never a plaintext fallback.

Why the prompt is never dismissed: gnome-keyring (48) aborts when a client
dismisses its unlock prompt while it is up (`gkd-secret-unlock.c`
`perform_next_unlock`: assertion `!self->current`), which is what libsecret does
when a call it prompted for is cancelled. So the unlock is asked for directly
(`Service.Unlock`, then the prompt's `Prompt`), and libsecret runs only once
nothing is locked any more. "Someone could answer an unlock prompt" is
`platform_features`' `secretServicePrompt` (see
[platform-features.md](platform-features.md)).

When another daemon takes `org.freedesktop.secrets` over (a session running two
gnome-keyring daemons: PAM's `--login` one and a D-Bus-activated
`--components=secrets` one), the next call opens a new transfer session with it.
libsecret keeps one service, and the session it opened with the first owner, for
the process: it watches the name from the private main context of the `*_sync`
call that made the service, which is never iterated again, so it never notices
the change, and the new owner refused every later write ("The session wrapping
the secret does not exist", or "The secret was transferred or encrypted in an
invalid way"). Each call now notes the owner it is made with and drops
libsecret's service (`secret_service_disconnect`) when it changed; a call that
still fails with such an error is retried once with a new service.

## Testing

- `laufey_secret_store_mac_test` (ctest, macOS): against the real login
  keychain: bad arguments, the round trip, an item written the old way
  (`security add-generic-password`, which `security find-generic-password -w`
  then reads back: the fail-first) is never read here and is in the way of a
  store, and an item stored here is refused to another program (a reader with
  prompts off gets `errSecAuthFailed`; with `LAUFEY_SECRET_TEST_SECURITY_CLI=1`,
  `security find-generic-password -w` never prints it). Skipped without a usable
  login keychain unless `LAUFEY_SECRET_TEST_REQUIRE=1`. The data-protection
  keychain needs a provisioning profile, which CI's ad-hoc builds can't carry.

- `laufey_secret_store_dbus_test` (ctest, Linux): on a private bus, libsecret
  missing, no session bus, no provider, KWallet without the Secret Service, a
  provider that never answers, a locked item where no one can answer (refused at
  once for lookup, store and delete), and a real `gnome-keyring-daemon`: the
  round trips, a second daemon taking the name over (written to and read from),
  then locked without a prompter (at once) and with a prompter that never
  answers (after the timeout).
- `scripts/native-e2e-run.sh <backend> --secret-store`: every call answers
  within its timeout through the backend (macOS: the round trip, in the host's
  login keychain); `LAUFEY_E2E_EXPECT_SECRET=ok|unavailable` for a real session.
