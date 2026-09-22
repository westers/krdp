// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QDBusMessage>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <QJsonObject>
#include <span>
#include "VirtualSessionCoordinatorIdentity.h"
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
    // Comparison-only internals. No runtime approvals or fixture-selected live calls.
    using Inputs = VirtualSessionCoordinatorIdentity::PolicyInputs;
    using Inventory = VirtualSessionCoordinatorIdentity::PolicyUnit;
    enum class Rule { Exact, Set, Empty, False, Zero, Blank, Loaded, Id, Fragment,
        RootUser, WorkingRoot, No, Init, Max64, EmptyHostname, DisabledQuota,
        Label, Mode, Input, Output, Error, EmptyFilter };
    struct Descriptor { const char *group, *name, *signature; Rule rule; };
    struct Policy { QJsonObject document; };
    static std::span<const Descriptor> descriptors();
    static QString inventoryName(size_t index);
    static QByteArray canonical(const QJsonValue &value);
    static std::optional<Policy> parseApproved(const QByteArray &, QString *error = nullptr);
    static std::optional<QJsonValue> normalize(const Descriptor &, const QJsonValue &,
        const QString &unitName, bool wire);
    static std::optional<QJsonValue> decode(const Descriptor &, const QVariant &);
    static bool compareEffective(const Policy &, const Inputs &, QString *error = nullptr);
    static bool finishComparison(const Policy &, const Inputs &, QString *error);
};
}
