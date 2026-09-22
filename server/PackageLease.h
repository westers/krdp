// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <optional>
#include <sys/types.h>

namespace KRdp {
/** Linux OFD package exclusion, not package consistency or writer quiescence.
 * Requires stable local lock inodes. Fork copies delay release until closed;
 * CLOEXEC prevents exec inheritance, and inherited API use is refused.
 * Do not use within a process already owning POSIX locks on these files:
 * closing even an unrelated descriptor can release that process's POSIX locks.
 */
class PackageLease {
public:
    ~PackageLease();
    PackageLease(PackageLease &&) noexcept;
    PackageLease &operator=(PackageLease &&) noexcept;
    PackageLease(const PackageLease &) = delete;
    PackageLease &operator=(const PackageLease &) = delete;
    static std::optional<PackageLease> acquire(QString *error = nullptr);
    bool associated(QString *error = nullptr) const;
private:
    friend class VirtualSessionMaintenanceGuard;
    friend class VirtualSessionMaintenanceGuardTest;
    PackageLease() = default;
    static std::optional<PackageLease> acquireAt(const QString &directory, uid_t owner, bool fixture, QString *error);
    void close();
    int m_directory = -1, m_frontend = -1, m_backend = -1;
    uid_t m_owner = 0;
    pid_t m_process = 0;
    bool m_fixture = false;
    QString m_path;
};
}
