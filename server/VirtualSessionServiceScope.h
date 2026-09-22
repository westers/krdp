// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <memory>
#include <optional>
#include <sys/types.h>
#include <unistd.h>

namespace KRdp {
// Containment boundary for the independent root service parent, never broker
// or PAM keeper. Pins only its exact canonical UUID-named system service cgroup.
class VirtualSessionServiceScope {
public:
    static std::unique_ptr<VirtualSessionServiceScope> open(const QString &session);
    ~VirtualSessionServiceScope();
    VirtualSessionServiceScope(const VirtualSessionServiceScope &) = delete;
    VirtualSessionServiceScope &operator=(const VirtualSessionServiceScope &) = delete;
    // nullopt means uncertain: do not close PAM or claim descendants are gone.
    std::optional<bool> descendantsGone() const;
    // Signals verified service members except the parent. SIGTERM/SIGKILL only.
    // Requires no external privileged migration during teardown; pidfds protect
    // PID reuse, not cgroup migration. Not an adversarial same-UID sandbox.
    // A true result is submission, not extinction; recheck descendantsGone().
    bool signalDescendants(int signal) const;
private:
    friend class VirtualSessionServiceScopeTest;
    VirtualSessionServiceScope(int fd, QByteArray membership) : m_fd(fd), m_membership(std::move(membership)), m_parent(getpid()) {}
    static QByteArray expectedMembership(const QString &session);
    static std::optional<QList<pid_t>> parseMembers(const QByteArray &bytes, pid_t parent);
    static std::optional<QList<pid_t>> readMembers(int directory, pid_t parent);
    std::optional<QList<pid_t>> members() const;
    int m_fd;
    QByteArray m_membership;
    pid_t m_parent;
};
}
