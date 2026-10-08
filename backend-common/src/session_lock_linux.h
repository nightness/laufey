// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Whether this login session is locked, as systemd-logind reports it (the
// session's LockedHint property, which the desktop's lock screen sets). The
// clipboard paths that work without focus refuse reads while it is set.

#ifndef LAUFEY_SESSION_LOCK_LINUX_H_
#define LAUFEY_SESSION_LOCK_LINUX_H_

namespace laufey_common {

// True when logind says our session is locked. False when it isn't, or when
// that can't be told (no system bus or logind, not in a session). Asks logind
// on every call, waiting half a second at most; any thread.
bool SessionLockedLinux();

}  // namespace laufey_common

#endif  // LAUFEY_SESSION_LOCK_LINUX_H_
