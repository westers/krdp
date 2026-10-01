// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerHostSettings.h"
#include <QJsonObject>

namespace KRdp::BrokerHostRuntime {
using Scope = BrokerHostSettings::Scope;
constexpr qsizetype MaximumBytes = 262144;
struct Contract {
    QString broker, worker, runtimeDirectory;
};
Contract installedContract(Scope scope);
QString unitName(Scope scope);
struct EnvironmentFile { QString path; bool optional = false; bool operator==(const EnvironmentFile &) const = default; };
struct Command { QString path; QStringList arguments; bool ignoreFailure = false; bool operator==(const Command &) const = default; };
struct Unit {
    QString id, loadState, activeState, subState, fragment, user, type, controlGroup, pamName;
    QStringList dropIns, environment, passEnvironment, unsetEnvironment;
    QList<EnvironmentFile> files;
    QList<Command> commands;
    quint32 pid = 0;
    quint64 started = 0;
    bool needsReload = false;
    bool operator==(const Unit &) const = default;
};
// Argument and environment input is private. Only recognized normalized fields
// and fixed reason codes can leave these transformations.
struct Arguments {
    QVariantMap values;
    QStringList missing, reasons;
    bool complete() const { return missing.isEmpty() && reasons.isEmpty(); }
};
Arguments arguments(Scope scope, const QStringList &argv, const Contract &contract);
struct File { bool exists = false; QByteArray bytes; QString error; QString identity = {}; }; // private file identity
struct Projection {
    Arguments fields;
    QStringList reasons;
    bool custom = false;
    bool verified() const { return fields.complete() && reasons.isEmpty(); }
};
// Files correspond exactly to unit.files in order, and must already have safe
// root ownership/regular-file/size/stability checks in the production reader.
Projection project(Scope scope, const Unit &unit, const QList<File> &files, const Contract &contract);
struct Process {
    quint32 pid = 0;
    quint64 startTicks = 0, pidIdentity = 0;
    bool root = false, executableMatches = false, alive = false;
    QString controlGroup;
    QString executableStamp; // private stable file identity, never public
    QStringList argv;
    bool operator==(const Process &) const = default;
};
QJsonObject summarize(Scope scope, const Unit &unit, const Projection &projection,
    const Process &process, const QVariantMap &stored, const QString &storedRevision,
    const Contract &contract, const QString &failure = {});
bool validPublic(Scope scope, const QJsonObject &value);
}
