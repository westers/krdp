// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <optional>
#include <sys/types.h>
#include "PackageLease.h"
#include "VirtualSessionMaintenanceRecord.h"

namespace KRdp {
class VirtualSessionMaintenanceGuardTest;
class VirtualSessionMaintenanceCoordinator;
/** Coordinated maintenance admission only; never registration/cleanup proof.
 * No clean-state bootstrap or installation is performed by this core. */
class VirtualSessionMaintenanceGuard {
public:
    struct State {
        bool clean = false;
        QString boot, epoch, profile;
        bool operator==(const State &) const = default;
    };
    enum class Publication { Durable, FailedBeforeRename, UncertainAfterRename };
    struct Diagnostic {
        VirtualSessionMaintenanceRecord record;
        bool currentBoot = false;
    };
    // Gate-only shared diagnostic read. Never evaluates admission, acquires
    // package exclusion, publishes state, or resolves durability uncertainty.
    static std::optional<Diagnostic> status(QString *error = nullptr);
    class Lease {
    public:
        ~Lease();
        Lease(Lease &&) noexcept;
        Lease &operator=(Lease &&) noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        std::optional<State> read(QString *error = nullptr) const;
        // Exclusive lease only. A new transaction is generated even on retries.
        // Run no writer unless Durable. Failure never implies admission is safe.
        Publication block(QString *transaction, QString *error = nullptr);
        // V2 invalidation never bootstraps clean or restores an initial attempt.
        std::optional<VirtualSessionMaintenanceRecord> record(QString *error = nullptr) const;
        Publication invalidate(QString *error = nullptr);
    private:
        friend class VirtualSessionMaintenanceGuard;
        friend class VirtualSessionMaintenanceGuardTest;
        friend class VirtualSessionMaintenanceCoordinator;
        Lease() = default;
        bool associated(QString *error) const;
        int stateFile(State &state, QString *error) const;
        Publication publish(const State &, QString *error);
        int recordFile(VirtualSessionMaintenanceRecord &, QString *error) const;
        Publication publishRecord(const VirtualSessionMaintenanceRecord &, QString *error);
        Publication publishBytes(const QByteArray &, bool version2, QString *error);
        Publication initializeRecord(const VirtualSessionMaintenanceRecord &, QString *error);
        Publication claimRecord(const VirtualSessionMaintenanceRecord &, const QString &invocation,
            VirtualSessionMaintenanceRecord *claimed, QString *error);
        Publication completeRecord(const VirtualSessionMaintenanceRecord &, QString *error);
        bool admitRecord(const QString &profile, QString *error);
        // Reserved for a future validator running under this exclusive lease.
        // Transaction matching is necessary, NOT evidence of writer quiescence.
        Publication publishClean(const QString &transaction, const QString &profile, QString *error = nullptr);
        bool admit(const QString &profile, QString *error);
        void close();
        int m_directory = -1, m_lock = -1;
        pid_t m_process = 0;
        uid_t m_owner = 0;
        bool m_exclusive = false, m_fixture = false;
        QString m_path, m_boot;
        std::optional<PackageLease> m_packages;
    };
    // Fixed root paths: package frontend/backend OFD read locks, then shared
    // guard flock, all nonblocking and retained through the lease lifetime.
    // Missing/unsafe package files or state deny; nothing is created/repaired.
    // CLOEXEC is not a fork barrier: children must promptly close inherited
    // descriptors. Inherited lease API use is refused, without LOCK_UN.
    static std::optional<Lease> admission(const QString &approvedProfile, QString *error = nullptr);
    // Gate-only invalidation, deliberately no package locks (installer hooks
    // may already hold them). This is not a validation/rearm entry point.
    static std::optional<Lease> maintenance(QString *error = nullptr);
private:
    friend class VirtualSessionMaintenanceGuardTest;
    friend class VirtualSessionMaintenanceCoordinator;
    static std::optional<Diagnostic> statusAt(const QString &path, uid_t owner,
        const QString &boot, bool fixture, QString *error);
    static std::optional<Lease> acquire(const QString &path, uid_t owner, const QString &boot,
                                       bool exclusive, bool fixture, QString *error);
    static std::optional<Lease> admissionAt(const QString &path, const QString &packages, uid_t owner,
        const QString &boot, const QString &profile, bool fixture, QString *error);
    static std::optional<Lease> orderedAt(const QString &path, const QString &packages, uid_t owner,
        const QString &boot, bool exclusive, bool fixture, QString *error);
};
}
