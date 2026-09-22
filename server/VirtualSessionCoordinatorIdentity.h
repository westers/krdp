// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
#include <QDBusMessage>
#include <array>
#include <memory>
#include <optional>
#include <sys/types.h>

class QDBusConnection;
class VirtualSessionCoordinatorIdentityTest;
namespace KRdp {
class VirtualSessionMaintenanceWriterPolicyTest;
/** Identity of this root process running krdp-maintenance-validate.service.
 * The caller supplies an executable from its approved profile, not CLI authority,
 * and separately validates its bytes/loading closure. No activation or mutation.
 * Revalidation checks observed endpoints and invocation, not continuous ownership
 * (including ABA), an atomic property snapshot, or historical cleanup authority.
 */
class VirtualSessionCoordinatorIdentity {
public:
    ~VirtualSessionCoordinatorIdentity();
    VirtualSessionCoordinatorIdentity(VirtualSessionCoordinatorIdentity &&) noexcept;
    VirtualSessionCoordinatorIdentity &operator=(VirtualSessionCoordinatorIdentity &&) noexcept;
    VirtualSessionCoordinatorIdentity(const VirtualSessionCoordinatorIdentity &) = delete;
    VirtualSessionCoordinatorIdentity &operator=(const VirtualSessionCoordinatorIdentity &) = delete;
    static std::optional<VirtualSessionCoordinatorIdentity> pin(const QString &approvedExecutable,
                                                               int timeoutMs = 3000);
    // Each operation has one total 1..3000ms deadline. Retain this object through
    // validation, then require revalidate() immediately before private completion.
    bool revalidate(int timeoutMs = 3000) const;
    QString invocation() const;
    QString busId() const;
    QString uniqueOwner() const;
    QString bootId() const;
private:
    friend class ::VirtualSessionCoordinatorIdentityTest;
    friend class VirtualSessionMaintenanceWriterPolicyTest;
    friend class VirtualSessionMaintenanceWriterPolicy;
    enum class PolicyUnit : size_t {
        Coordinator, ShutdownWaiter, AptDaily, AptDailyUpgrade,
        UpdateNotifierDownload, UpdateNotifierMotd, UaTimer, PackageKit,
        PackageKitOfflineUpdate, UaRebootCommands, AptNews, EsmCache, Count
    };
    struct UnitInputs {
        QDBusMessage unit, service;
        // Hidden from GetAll; fetched explicitly, never defaulted when absent.
        bool permissionsStartOnly = false;
    };
    struct PolicyInputs {
        QDBusMessage manager;
        std::array<UnitInputs, size_t(PolicyUnit::Count)> units;
    };
    static QString policyUnitName(PolicyUnit);
    static QString policyUnitPath(PolicyUnit);
    // Synchronous whole-inventory observation under one deadline; no escaping
    // batch, arbitrary destinations, activation or partial-success result.
    // Raw dictionaries retain duplicate keys for WriterPolicy's strict parser.
    std::optional<PolicyInputs> readPolicyInputs(int timeoutMs = 3000) const;
    struct Data;
    static bool validBusId(const QString &);
    static bool validUniqueOwner(const QString &);
    explicit VirtualSessionCoordinatorIdentity(std::unique_ptr<Data>);
    static std::optional<VirtualSessionCoordinatorIdentity> pinOnBus(const QDBusConnection &,
        const QString &approvedExecutable, uid_t managerUid, pid_t managerPid, bool fixture, int timeoutMs);
    std::unique_ptr<Data> d;
};
}
