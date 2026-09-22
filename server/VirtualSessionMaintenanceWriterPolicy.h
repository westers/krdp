// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QDBusMessage>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <optional>

namespace KRdp {
class VirtualSessionCoordinatorIdentity;
class VirtualSessionRuntimeProfile;
class VirtualSessionMaintenanceWriterPolicyTest;
/** Fixed supported-writer policy, not a general manifest-command executor.
 * Effective properties describe loaded manager state, not final execve argv,
 * writer quiescence, or an atomic snapshot. Caller retains ordered exclusion.
 * The runtime gate refuses until the reviewed finite inventory is implemented.
 */
class VirtualSessionMaintenanceWriterPolicy {
public:
    static bool validate(const VirtualSessionCoordinatorIdentity &identity,
        const VirtualSessionRuntimeProfile &profile, QString *error = nullptr);
private:
    friend class VirtualSessionMaintenanceWriterPolicyTest;
    struct Command {
        QString path;
        QStringList argv, flags;
        bool operator==(const Command &) const = default;
    };
    struct Service {
        QMap<QString, QList<Command>> commands;
        QList<qint32> successStatuses, successSignals;
    };
    struct Unit {
        QString id, loadState, activeState, subState, fragment, source;
        QStringList dropins;
        bool transient = false, needReload = false;
    };
    static std::optional<QMap<QString, QVariant>> properties(const QDBusMessage &reply);
    static std::optional<QList<Command>> commands(const QVariant &value);
    static std::optional<Service> service(const QDBusMessage &reply);
    static std::optional<Unit> unit(const QDBusMessage &reply);
};
}
