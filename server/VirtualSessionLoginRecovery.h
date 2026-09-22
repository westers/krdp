// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionJournal.h"
#include <QDBusConnection>
namespace KRdp {
// Process/logind reconciliation, not replacement for skipped PAM callbacks.
// Caller MUST validate immutable journal/current boot and observe desktop and
// original keeper extinction first. No journal deletion or relaunch authority.
class VirtualSessionLoginRecovery {
public:
    enum class Result { Removed, AlreadyClosed, Unresolved, Refused };
    // At most one session can be registered by the supported fixed PAM attempt.
    // An empty list before observing that session remains Unresolved: it is not
    // a cross-transport registration barrier. Counter cIDs only, pinned owner.
    static Result reconcile(const QDBusConnection &bus, const VirtualSessionJournal::Record &record,
        const VirtualSessionJournal::Keeper &keeper, int timeoutMs = 15000, bool successfulCloseRecorded = false);
};
}
