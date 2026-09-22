// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <optional>
#include <sys/types.h>

namespace KRdp {
class VirtualSessionMaintenanceGuardTest;
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
    private:
        friend class VirtualSessionMaintenanceGuard;
        friend class VirtualSessionMaintenanceGuardTest;
        Lease() = default;
        bool associated(QString *error) const;
        int stateFile(State &state, QString *error) const;
        Publication publish(const State &, QString *error);
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
    };
    // Fixed installer-owned root paths; nonblocking, missing state denies.
    // CLOEXEC is not a fork barrier: children must promptly close inherited
    // descriptors. Inherited lease API use is refused, without LOCK_UN.
    static std::optional<Lease> admission(const QString &approvedProfile, QString *error = nullptr);
    static std::optional<Lease> maintenance(QString *error = nullptr);
private:
    friend class VirtualSessionMaintenanceGuardTest;
    static std::optional<Lease> acquire(const QString &path, uid_t owner, const QString &boot,
                                       bool exclusive, bool fixture, QString *error);
};
}
