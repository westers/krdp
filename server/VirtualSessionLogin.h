// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <functional>
#include <optional>
#include <sys/types.h>

class QDBusConnection;
class QDBusMessage;
class VirtualSessionLoginTest;
namespace KRdp {
// Validated logind observations, not an atomic snapshot of mutable properties.
struct VirtualSessionLogin {
    QString id, service, type, sessionClass, state, seat, tty, display, runtime, scope;
    quint32 uid = 0, leader = 0, virtualTerminal = 0;
    QString desktop;
    // Diagnostic endpoint identity only: not registration-peer association,
    // uninterrupted name ownership (ABA), durable completion or cleanup authority.
    QString busId, uniqueOwner;
    bool matches(uid_t owner, pid_t expectedLeader, const QString &pamId, const QString &pamRuntime) const;
    bool matchesLaunch(uid_t owner, pid_t expectedLeader, const QString &pamId, const QString &pamRuntime, const QString &launch) const;
    // Read-only system-bus lookup. Bounded calls, no session creation/activation.
    static std::optional<VirtualSessionLogin> read(pid_t leader);
    static std::optional<VirtualSessionLogin> read(const QDBusConnection &bus, pid_t leader, int timeoutMs = 3000);
private:
    friend class ::VirtualSessionLoginTest;
    using Call = std::function<QDBusMessage(const QDBusMessage &, int)>;
    static std::optional<VirtualSessionLogin> readWithCalls(pid_t leader, int timeoutMs, const Call &call,
                                                         const std::function<bool()> &connected);
};
}
