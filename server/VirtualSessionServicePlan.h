// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionJournal.h"
#include "VirtualSessionLaunchPlan.h"

namespace KRdp {
/** Pure root-service argv construction. Installed path ownership, boot origin,
 * OS account resolution and existing-runtime refusal are executor checks.
 * Credential is separate from argv and must be delivered only on stdin.
 */
struct VirtualSessionServicePlan {
    QString program;
    QStringList arguments;
    QString runtime;
    QByteArray credential;
    static std::optional<VirtualSessionServicePlan> build(const VirtualSessionJournal::Record &record,
        const QString &currentBoot, const VirtualSessionLaunchPlan::Account &account,
        const VirtualSessionLaunchPlan::Configuration &configuration,
        const QString &deviceEntry, const QString &guardian)
    {
        if (!record.valid() || record.boot != currentBoot
            || !VirtualSessionLaunchPlan::absoluteCleanPath(deviceEntry)
            || !VirtualSessionLaunchPlan::absoluteCleanPath(guardian)
            || configuration.allowedRenderPci.size() > 32) return {};
        const auto desktop = VirtualSessionLaunchPlan::build(record.uid, account, record.session,
            configuration, nullptr, record.launch);
        if (!desktop) return {};
        VirtualSessionServicePlan plan{deviceEntry, {QStringLiteral("--uid"), QString::number(record.uid)},
            desktop->runtimeDirectory, record.token};
        for (const auto &pci : configuration.allowedRenderPci)
            plan.arguments.append({QStringLiteral("--allow-render-pci"), pci});
        plan.arguments.append({QStringLiteral("--"), guardian, QStringLiteral("--session"), record.session,
            QStringLiteral("--instance"), record.incarnation, QStringLiteral("--launch-id"), record.launch,
            QStringLiteral("--token-fd"), QStringLiteral("0"), QStringLiteral("--"), desktop->program});
        plan.arguments.append(desktop->arguments);
        return plan;
    }
};
}
