// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <optional>
#include <sys/types.h>

namespace KRdp {
// Authoritative logind snapshot, not PAM-provided environment strings.
struct VirtualSessionLogin {
    QString id, service, type, sessionClass, state, seat, tty, display, runtime, scope;
    quint32 uid = 0, leader = 0, virtualTerminal = 0;
    bool matches(uid_t owner, pid_t expectedLeader, const QString &pamId, const QString &pamRuntime) const;
    // Read-only system-bus lookup. Bounded calls, no session creation/activation.
    static std::optional<VirtualSessionLogin> read(pid_t leader);
};
}
