// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceWriterPolicy.h"
#include <QDBusArgument>
#include <QDBusVariant>
#include <QSet>

namespace KRdp {
namespace {
bool string(const QString &s) { return s.size() <= 4096 && !s.contains(QChar::Null); }
bool strings(const QStringList &ss) {
    if (ss.size() > 256) return false;
    qsizetype total = 0;
    for (const auto &s : ss) { if (!string(s)) return false; total += s.size(); }
    return total <= 65536;
}
bool typed(const QVariant &v, int type) { return v.metaType().id() == type; }
}
bool VirtualSessionMaintenanceWriterPolicy::validate(const VirtualSessionCoordinatorIdentity &,
    const VirtualSessionRuntimeProfile &, QString *error) {
    // Deliberate refusal, never implicit success while the connected inventory
    // and activation/descendant checks are awaiting review and implementation.
    if (error) *error = QStringLiteral("Fixed maintenance writer policy not configured");
    return false;
}
std::optional<QMap<QString, QVariant>> VirtualSessionMaintenanceWriterPolicy::properties(const QDBusMessage &reply) {
    if (reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QStringLiteral("a{sv}")
        || reply.arguments().size() != 1 || !typed(reply.arguments()[0], qMetaTypeId<QDBusArgument>())) return {};
    const auto a = qvariant_cast<QDBusArgument>(reply.arguments()[0]);
    if (a.currentSignature() != QStringLiteral("a{sv}")) return {};
    QMap<QString, QVariant> result;
    a.beginMap();
    while (!a.atEnd()) {
        if (result.size() >= 512) return {};
        QString key; QDBusVariant value;
        a.beginMapEntry(); a >> key >> value; a.endMapEntry();
        if (key.isEmpty() || !string(key) || result.contains(key)) return {};
        result.insert(key, value.variant());
    }
    a.endMap();
    return result;
}
std::optional<QList<VirtualSessionMaintenanceWriterPolicy::Command>> VirtualSessionMaintenanceWriterPolicy::commands(const QVariant &value) {
    if (!typed(value, qMetaTypeId<QDBusArgument>())) return {};
    const auto a = qvariant_cast<QDBusArgument>(value);
    if (a.currentSignature() != QStringLiteral("a(sasasttttuii)")) return {};
    QList<Command> result;
    a.beginArray();
    while (!a.atEnd()) {
        if (result.size() >= 64) return {};
        Command c; quint64 sr, sm, er, em; quint32 pid; qint32 code, status;
        a.beginStructure(); a >> c.path >> c.argv >> c.flags >> sr >> sm >> er >> em >> pid >> code >> status; a.endStructure();
        // Status fields are parsed at exact widths but are not configuration.
        if (c.path.isEmpty() || !string(c.path) || c.argv.isEmpty() || !strings(c.argv) || !strings(c.flags)) return {};
        QSet<QString> seen;
        for (const auto &flag : c.flags) {
            if (seen.contains(flag) || (flag != QStringLiteral("ignore-failure") && flag != QStringLiteral("privileged")
                && flag != QStringLiteral("no-setuid") && flag != QStringLiteral("no-env-expand") && flag != QStringLiteral("via-shell"))) return {};
            seen.insert(flag);
        }
        result.append(c);
    }
    a.endArray();
    return result;
}
std::optional<VirtualSessionMaintenanceWriterPolicy::Service> VirtualSessionMaintenanceWriterPolicy::service(const QDBusMessage &reply) {
    const auto p = properties(reply); if (!p) return {};
    Service result;
    for (const auto *name : {"ExecConditionEx", "ExecStartPreEx", "ExecStartEx", "ExecStartPostEx", "ExecReloadEx",
             "ExecReloadPostEx", "ExecStopEx", "ExecStopPostEx"}) {
        const auto key = QString::fromLatin1(name);
        const auto parsed = commands(p->value(key)); if (!parsed) return {};
        result.commands.insert(key, *parsed);
    }
    const auto v = p->value(QStringLiteral("SuccessExitStatus"));
    if (!typed(v, qMetaTypeId<QDBusArgument>())) return {};
    const auto a = qvariant_cast<QDBusArgument>(v);
    if (a.currentSignature() != QStringLiteral("(aiai)")) return {};
    a.beginStructure();
    for (auto *list : {&result.successStatuses, &result.successSignals}) {
        a.beginArray();
        while (!a.atEnd()) {
            qint32 n; a >> n;
            if (list->size() >= 256 || n < 0 || n > 255 || (!list->isEmpty() && n <= list->last())) return {};
            list->append(n);
        }
        a.endArray();
    }
    a.endStructure();
    return result;
}
std::optional<VirtualSessionMaintenanceWriterPolicy::Unit> VirtualSessionMaintenanceWriterPolicy::unit(const QDBusMessage &reply) {
    const auto p = properties(reply); if (!p) return {};
    Unit result;
    const QString keys[] = {QStringLiteral("Id"), QStringLiteral("LoadState"), QStringLiteral("ActiveState"),
        QStringLiteral("SubState"), QStringLiteral("FragmentPath"), QStringLiteral("SourcePath")};
    QString *fields[] = {&result.id, &result.loadState, &result.activeState, &result.subState, &result.fragment, &result.source};
    for (int i = 0; i < 6; ++i) {
        const auto v = p->value(keys[i]); if (!typed(v, QMetaType::QString) || !string(v.toString())) return {};
        *fields[i] = v.toString();
    }
    const auto dropins = p->value(QStringLiteral("DropInPaths"));
    const auto transient = p->value(QStringLiteral("Transient")), reload = p->value(QStringLiteral("NeedDaemonReload"));
    if (!typed(dropins, QMetaType::QStringList) || !strings(dropins.toStringList())
        || !typed(transient, QMetaType::Bool) || !typed(reload, QMetaType::Bool)) return {};
    result.dropins = dropins.toStringList(); result.transient = transient.toBool(); result.needReload = reload.toBool();
    return result;
}
}
