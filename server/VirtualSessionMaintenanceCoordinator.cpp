// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceCoordinator.h"
#include "VirtualSessionCoordinatorIdentity.h"
#include "VirtualSessionMaintenanceGuard.h"
#include "VirtualSessionMaintenanceWriterPolicy.h"
#include "VirtualSessionProvisioningReceipt.h"
#include "VirtualSessionRuntimeProfile.h"
#include <QFile>
#include <unistd.h>

namespace KRdp {
VirtualSessionMaintenanceCoordinator::Result VirtualSessionMaintenanceCoordinator::bootstrap(QString *error) {
    const auto refuse = [&](const QString &reason) {
        if (error) *error = reason;
        return Result::Refused;
    };
    if (error) error->clear();
    uid_t real, effective, saved;
    if (getresuid(&real, &effective, &saved) || real || effective || saved)
        return refuse(QStringLiteral("Explicit root coordinator required"));
    QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!bootFile.open(QIODevice::ReadOnly)) return refuse(QStringLiteral("Boot identity unavailable"));
    const auto bytes = bootFile.read(128);
    if (bytes.size() != 37 || !bytes.endsWith('\n')) return refuse(QStringLiteral("Invalid boot identity"));
    const QString boot = QString::fromLatin1(bytes.first(36));
    using Guard = VirtualSessionMaintenanceGuard;
    auto lease = Guard::orderedAt(QStringLiteral("/var/lib/krdp/maintenance"), QStringLiteral("/var/lib/dpkg"),
        0, boot, true, false, error);
    if (!lease) return Result::Refused;
    const auto initial = lease->record(error);
    if (!initial || initial->phase != VirtualSessionMaintenanceRecord::Phase::InitialBlocked || initial->boot != boot)
        return refuse(QStringLiteral("No unused current-boot provisioning attempt"));
    auto profile = VirtualSessionRuntimeProfile::loadApproved(error);
    if (!profile || profile->digest() != initial->profile)
        return refuse(QStringLiteral("Approved provisioning profile unavailable or changed"));
    if (!VirtualSessionProvisioningReceipt::validate(*initial, *profile, error)) return Result::Refused;
    auto identity = VirtualSessionCoordinatorIdentity::pin(profile->approvedExecutable(QStringLiteral("coordinator")));
    if (!identity || identity->bootId() != boot)
        return refuse(QStringLiteral("Coordinator service identity unavailable"));
    VirtualSessionMaintenanceRecord claimed;
    const auto claim = lease->claimRecord(*initial, identity->invocation(), &claimed, error);
    if (claim != Guard::Publication::Durable)
        return claim == Guard::Publication::UncertainAfterRename ? Result::Uncertain : Result::Refused;
    // From this point all failures leave the attempt consumed. The fixed policy
    // must account for relevant active writers and future activation paths;
    // neither package-lock availability nor receipt bytes prove quiescence.
    if (!VirtualSessionMaintenanceWriterPolicy::validate(*identity, *profile, error)
        || !profile->validateCurrentProcess(QStringLiteral("coordinator"), error)
        || !VirtualSessionMaintenanceWriterPolicy::validate(*identity, *profile, error)) return Result::Refused;
    if (!identity->revalidate()) return refuse(QStringLiteral("Coordinator identity changed during validation"));
    const auto publication = lease->completeRecord(claimed, error);
    if (publication == Guard::Publication::Durable) return Result::Clean;
    return publication == Guard::Publication::UncertainAfterRename ? Result::Uncertain : Result::Refused;
}
}
