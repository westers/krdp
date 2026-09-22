// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceWriterPolicy.h"
#include <QDBusArgument>
#include <QDBusVariant>
#include <QSet>
#include <QScopeGuard>
#include "VirtualSessionRuntimeProfile.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
#include <algorithm>
#include <limits>

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
bool VirtualSessionMaintenanceWriterPolicy::validate(const VirtualSessionCoordinatorIdentity &identity,
    const VirtualSessionRuntimeProfile &profile, QString *error) {
    const auto bytes = profile.approvedWriterPolicy(error);
    if (!bytes) return false;
    const auto policy = parseApproved(*bytes, error);
    if (!policy) return false;
    const auto inputs = identity.readPolicyInputs();
    if (!inputs) {
        if (error) *error = QStringLiteral("writerPolicyInputsUnavailable");
        return false;
    }
    return finishComparison(*policy, *inputs, error);
}
std::optional<QMap<QString, QVariant>> VirtualSessionMaintenanceWriterPolicy::properties(const QDBusMessage &reply) {
    if (reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QStringLiteral("a{sv}")
        || reply.arguments().size() != 1 || !typed(reply.arguments()[0], qMetaTypeId<QDBusArgument>())) return {};
    const auto a = qvariant_cast<QDBusArgument>(reply.arguments()[0]);
    if (a.currentSignature() != QStringLiteral("a{sv}")) return {};
    QMap<QString, QVariant> result;
    a.beginMap();
    const auto endMap = qScopeGuard([&] { a.endMap(); });
    while (!a.atEnd()) {
        if (result.size() >= 512) return {};
        QString key; QDBusVariant value;
        a.beginMapEntry(); a >> key >> value; a.endMapEntry();
        if (key.isEmpty() || !string(key) || result.contains(key)) return {};
        result.insert(key, value.variant());
    }
    return result;
}
std::optional<QList<VirtualSessionMaintenanceWriterPolicy::Command>> VirtualSessionMaintenanceWriterPolicy::commands(const QVariant &value) {
    if (!typed(value, qMetaTypeId<QDBusArgument>())) return {};
    const auto a = qvariant_cast<QDBusArgument>(value);
    if (a.currentSignature() != QStringLiteral("a(sasasttttuii)")) return {};
    QList<Command> result;
    a.beginArray();
    const auto endArray = qScopeGuard([&] { a.endArray(); });
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
    const auto endStructure = qScopeGuard([&] { a.endStructure(); });
    for (auto *list : {&result.successStatuses, &result.successSignals}) {
        a.beginArray();
        const auto endArray = qScopeGuard([&] { a.endArray(); });
        while (!a.atEnd()) {
            qint32 n; a >> n;
            if (list->size() >= 256 || n < 0 || n > 255 || (!list->isEmpty() && n <= list->last())) return {};
            list->append(n);
        }
    }
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

namespace {
bool ascii(const QString &s) {
    if (s.size() > 4096) return false;
    for (const auto c : s) if (c.unicode() == 0 || c.unicode() > 127) return false;
    return true;
}
bool integer(const QJsonValue &v, double lo, double hi) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi
        && std::floor(v.toDouble()) == v.toDouble();
}
bool uint64String(const QJsonValue &v) {
    if (!v.isString()) return false;
    const auto s = v.toString();
    if (s.isEmpty() || s.size() > 20 || (s.size() > 1 && s[0] == QLatin1Char('0'))) return false;
    for (const auto c : s) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    bool ok; s.toULongLong(&ok); return ok;
}
bool keys(const QJsonValue &v, std::initializer_list<const char *> names) {
    if (!v.isObject() || v.toObject().size() != qsizetype(names.size())) return false;
    for (const auto name : names) if (!v.toObject().contains(QString::fromLatin1(name))) return false;
    return true;
}
bool stringArray(const QJsonValue &v) {
    if (!v.isArray() || v.toArray().size() > 256) return false;
    for (const auto &s : v.toArray()) if (!s.isString() || !ascii(s.toString())) return false;
    return true;
}
std::optional<QJsonArray> sortedSet(const QJsonValue &v, bool wire) {
    if (!stringArray(v)) return {};
    QStringList values;
    for (const auto &s : v.toArray()) values.append(s.toString());
    auto sorted = values; std::sort(sorted.begin(), sorted.end());
    for (qsizetype i = 1; i < sorted.size(); ++i) if (sorted[i] == sorted[i-1]) return {};
    if (!wire && values != sorted) return {};
    return QJsonArray::fromStringList(sorted);
}
bool path(const QJsonValue &v) {
    if (!v.isString() || !ascii(v.toString()) || !v.toString().startsWith(QLatin1Char('/'))) return false;
    const auto s = v.toString();
    if (s == QStringLiteral("/")) return true;
    for (const auto &part : s.mid(1).split(QLatin1Char('/'))) {
        if (part.isEmpty() || part.size() > 255 || part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
        for (const auto c : part) {
            const ushort n = c.unicode();
            if (!((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9')
                || QStringLiteral("_+@.,:-").contains(c))) return false;
        }
    }
    return true;
}
bool errorAt(QString *error, const QString &what) {
    if (error) *error = what;
    return false;
}
// Canonical syntax preflight before Qt parsing: bound depth even for hostile
// input; no regex (PCRE JIT creates unapproved executable anonymous mappings).
bool preflight(const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > 1024 * 1024 || !bytes.endsWith('\n')) return false;
    int depth = 0; bool quoted = false, escaped = false;
    for (unsigned char c : bytes) {
        if (!c || c > 127) return false;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 12) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return !quoted && !escaped && depth == 0;
}
bool boundedJson(const QJsonValue &v, int depth = 0) {
    if (depth > 12 || v.isNull() || v.isUndefined()) return false;
    if (v.isString()) return ascii(v.toString());
    if (v.isBool()) return true;
    if (v.isDouble()) return integer(v, -2147483648.0, 4294967295.0);
    if (v.isArray()) {
        if (v.toArray().size() > 256) return false;
        for (const auto &item : v.toArray()) if (!boundedJson(item, depth + 1)) return false;
        return true;
    }
    if (v.isObject()) {
        const auto o = v.toObject();
        if (o.size() > 512) return false;
        for (auto i = o.begin(); i != o.end(); ++i) if (!ascii(i.key()) || !boundedJson(i.value(), depth + 1)) return false;
        return true;
    }
    return false;
}
}
std::span<const VirtualSessionMaintenanceWriterPolicy::Descriptor> VirtualSessionMaintenanceWriterPolicy::descriptors() {
    static constexpr Descriptor table[] = {
#include "VirtualSessionMaintenanceWriterPolicyTable.inc"
    };
    static_assert(std::size(table) == 321);
    return table;
}
QString VirtualSessionMaintenanceWriterPolicy::inventoryName(size_t index) {
    return VirtualSessionCoordinatorIdentity::policyUnitName(static_cast<Inventory>(index));
}
QByteArray VirtualSessionMaintenanceWriterPolicy::canonical(const QJsonValue &v) {
    if (v.isObject()) {
        QByteArray out("{"); const auto o = v.toObject(); bool first = true;
        for (auto i = o.begin(); i != o.end(); ++i) {
            if (!first) out += ',';
            first = false;
            out += canonical(QJsonValue(i.key())) + ':' + canonical(i.value());
        }
        return out + '}';
    }
    if (v.isArray()) {
        QByteArray out("["); bool first = true;
        for (const auto &item : v.toArray()) { if (!first) out += ','; first = false; out += canonical(item); }
        return out + ']';
    }
    if (v.isString()) {
        const auto encoded = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        return encoded.mid(1, encoded.size()-2);
    }
    if (v.isBool()) return v.toBool() ? QByteArray("true") : QByteArray("false");
    if (v.isDouble() && integer(v, -2147483648.0, 4294967295.0)) return QByteArray::number(qint64(v.toDouble()));
    return {}; // Null/undefined are never canonical policy values.
}
std::optional<QJsonValue> VirtualSessionMaintenanceWriterPolicy::normalize(const Descriptor &d, const QJsonValue &v,
    const QString &unitName, bool wire) {
    if (v.isNull() || v.isUndefined()) return {};
    const QByteArray sig(d.signature); QJsonValue result = v;
    if (sig == "s") { if (!v.isString() || !ascii(v.toString())) return {}; }
    else if (sig == "b") { if (!v.isBool()) return {}; }
    else if (sig == "u" || sig == "q" || sig == "i") {
        if (!integer(v, sig == "i" ? -2147483648.0 : 0.0, sig == "i" ? 2147483647.0 : sig == "q" ? 65535.0 : 4294967295.0)) return {};
    } else if (sig == "t") { if (!uint64String(v)) return {}; }
    else if (sig == "as") { if (!stringArray(v)) return {}; }
    else if (sig == "(bs)") {
        if (!keys(v, {"ignoreFailure", "value"}) || !v[QStringLiteral("ignoreFailure")].isBool()
            || !v[QStringLiteral("value")].isString() || !ascii(v[QStringLiteral("value")].toString())) return {};
    } else if (sig == "(bas)") {
        if (!keys(v, {"allowList", "items"}) || !v[QStringLiteral("allowList")].isBool() || !stringArray(v[QStringLiteral("items")])) return {};
    } else if (sig == "(ss)") {
        if (!stringArray(v) || v.toArray().size() != 2) return {};
    } else if (sig == "(tus)") {
        if (!keys(v, {"bytes", "scale", "enforce"}) || !uint64String(v[QStringLiteral("bytes")])
            || !integer(v[QStringLiteral("scale")], 0, 4294967295.0) || !v[QStringLiteral("enforce")].isString()
            || (v[QStringLiteral("enforce")].toString() != QStringLiteral("yes") && v[QStringLiteral("enforce")].toString() != QStringLiteral("no"))) return {};
    } else if (sig == "(aiai)") {
        if (!keys(v, {"statuses", "signals"})) return {};
        for (const auto *k : {"statuses", "signals"}) {
            const auto a = v[QString::fromLatin1(k)]; const bool isSignalList = QByteArray(k) == "signals";
            if (!a.isArray() || a.toArray().size() > 256) return {};
            int previous = -1;
            for (const auto &n : a.toArray()) {
                if (!integer(n, isSignalList ? 1 : 0, isSignalList ? 64 : 255) || n.toInt() <= previous) return {};
                previous = n.toInt();
            }
        }
    } else if (sig == "a(sasasttttuii)") {
        if (!v.isArray() || v.toArray().size() > 64) return {};
        QJsonArray commands;
        for (const auto &c : v.toArray()) {
            if (!keys(c, {"path", "argv", "flags"}) || !path(c[QStringLiteral("path")])
                || !stringArray(c[QStringLiteral("argv")]) || c[QStringLiteral("argv")].toArray().isEmpty()) return {};
            const auto flags = sortedSet(c[QStringLiteral("flags")], wire); if (!flags) return {};
            for (const auto &f : *flags) if (f != QStringLiteral("ignore-failure") && f != QStringLiteral("privileged")
                && f != QStringLiteral("no-setuid") && f != QStringLiteral("no-env-expand") && f != QStringLiteral("via-shell")) return {};
            auto command = c.toObject(); command[QStringLiteral("flags")] = *flags; commands.append(command);
        }
        result = commands;
    } else if (sig == "a(sbbsi)") {
        if (!v.isArray() || v.toArray().size() > 256) return {};
        for (const auto &c : v.toArray()) if (!keys(c, {"type", "trigger", "negate", "parameter"})
            || !c[QStringLiteral("type")].isString() || !ascii(c[QStringLiteral("type")].toString())
            || !c[QStringLiteral("parameter")].isString() || !ascii(c[QStringLiteral("parameter")].toString())
            || !c[QStringLiteral("trigger")].isBool() || !c[QStringLiteral("negate")].isBool()) return {};
    } else if (sig == "a(ss)") {
        if (!v.isArray() || v.toArray().size() > 256) return {};
        for (const auto &pair : v.toArray()) if (!stringArray(pair) || pair.toArray().size() != 2) return {};
    } else {
        // All remaining supported signatures are explicitly empty-only. Wire
        // decoder still checks the exact declared signature before accepting [].
        if (d.rule != Rule::Empty || !v.isArray() || !v.toArray().isEmpty()) return {};
    }
    const QString name = QString::fromLatin1(d.name);
    if (name == QStringLiteral("Names") && !result.toArray().contains(QJsonValue(unitName))) return {};
    switch (d.rule) {
    case Rule::Exact: break;
    case Rule::Set: { auto s = sortedSet(result, wire); if (!s) return {}; result = *s; break; }
    case Rule::Empty: if (!result.isArray() || !result.toArray().isEmpty()) return {}; break;
    case Rule::False: if (!result.isBool() || result.toBool()) return {}; break;
    case Rule::Zero: if (!integer(result, 0, 0)) return {}; break;
    case Rule::Blank: if (result != QJsonValue(QStringLiteral(""))) return {}; break;
    case Rule::Loaded: if (result != QStringLiteral("loaded")) return {}; break;
    case Rule::Id: if (result != unitName) return {}; break;
    case Rule::Fragment: if (!path(result) || result == QStringLiteral("/")) return {}; break;
    case Rule::RootUser: if (result != QStringLiteral("") && result != QStringLiteral("root") && result != QStringLiteral("0")) return {}; break;
    case Rule::WorkingRoot: if (result != QStringLiteral("") && result != QStringLiteral("/")) return {}; break;
    case Rule::No: if (result != QStringLiteral("no")) return {}; break;
    case Rule::Init: if (result != QStringLiteral("init")) return {}; break;
    case Rule::Max64: if (result != QStringLiteral("18446744073709551615")) return {}; break;
    case Rule::EmptyHostname: if (result.toArray()[1] != QStringLiteral("")) return {}; break;
    case Rule::DisabledQuota:
        if (result[QStringLiteral("bytes")] != QStringLiteral("0") || result[QStringLiteral("scale")] != 0
            || result[QStringLiteral("enforce")] != QStringLiteral("no")) return {};
        break;
    case Rule::Label: {
        QString label;
        if (name == QStringLiteral("AppArmorProfile")) {
            if (unitName == inventoryName(size_t(Inventory::AptNews))) label = QStringLiteral("ubuntu_pro_apt_news");
            if (unitName == inventoryName(size_t(Inventory::EsmCache))) label = QStringLiteral("ubuntu_pro_esm_cache");
        }
        if (result[QStringLiteral("value")] != QJsonValue(label)
            || result[QStringLiteral("ignoreFailure")].toBool() != !label.isEmpty()) return {};
        break;
    }
    case Rule::Mode: if (!integer(result, 0, 4095)) return {}; break;
    case Rule::Input: if (result != QStringLiteral("null")) return {}; break;
    case Rule::Output: if (result != QStringLiteral("null") && result != QStringLiteral("journal")) return {}; break;
    case Rule::Error: if (result != QStringLiteral("null") && result != QStringLiteral("journal") && result != QStringLiteral("inherit")) return {}; break;
    case Rule::EmptyFilter: if (!result[QStringLiteral("items")].toArray().isEmpty()) return {}; break;
    }
    if (unitName == inventoryName(size_t(Inventory::Coordinator))
        && ((name == QStringLiteral("Type") && result != QStringLiteral("oneshot"))
            || (name == QStringLiteral("Restart") && result != QStringLiteral("no")))) return {};
    return result;
}
std::optional<VirtualSessionMaintenanceWriterPolicy::Policy> VirtualSessionMaintenanceWriterPolicy::parseApproved(const QByteArray &bytes, QString *error) {
    const auto bad = [&](const QString &where) -> std::optional<Policy> { errorAt(error, QStringLiteral("writerPolicySchema: ") + where); return {}; };
    if (!preflight(bytes)) return bad(QStringLiteral("bounds/encoding"));
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() || !boundedJson(doc.object())
        || canonical(doc.object()) + '\n' != bytes) return bad(QStringLiteral("canonical JSON"));
    const auto root = doc.object();
    if (!keys(root, {"v", "manager", "coordinator", "writers", "shutdownWaiter"}) || root[QStringLiteral("v")] != 1)
        return bad(QStringLiteral("root"));
    const auto validateGroup = [&](const QJsonValue &group, const QString &groupName, const QString &name) {
        if (!group.isObject()) return false;
        qsizetype count = 0;
        for (const auto &d : descriptors()) if (QString::fromLatin1(d.group) == groupName) {
            ++count;
            const auto v = group[QString::fromLatin1(d.name)];
            const auto n = normalize(d, v, name, false);
            if (!n || *n != v) return false;
        }
        return count > 0 && group.toObject().size() == count;
    };
    QSet<QString> groups;
    for (const auto &d : descriptors()) if (QByteArray(d.group) != "manager") groups.insert(QString::fromLatin1(d.group));
    const auto baseline = [&](const QJsonValue &v, const QString &name) {
        if (!v.isObject() || v.toObject().size() != groups.size()) return false;
        for (const auto &g : groups) if (!validateGroup(v[g], g, name)) return false;
        return true;
    };
    if (!validateGroup(root[QStringLiteral("manager")], QStringLiteral("manager"), QString())) return bad(QStringLiteral("manager"));
    if (!baseline(root[QStringLiteral("coordinator")], inventoryName(size_t(Inventory::Coordinator)))) return bad(QStringLiteral("coordinator"));
    const auto writers = root[QStringLiteral("writers")];
    if (!writers.isObject() || writers.toObject().size() != qsizetype(Inventory::Count) - 2) return bad(QStringLiteral("writers inventory"));
    for (size_t i = 2; i < size_t(Inventory::Count); ++i) {
        const auto name = inventoryName(i);
        if (!baseline(writers[name], name)) return bad(name);
    }
    const auto waiter = root[QStringLiteral("shutdownWaiter")];
    if (!keys(waiter, {"baseline", "interpreter", "script", "wrapper", "backend", "logPaths", "lockPaths"})
        || !baseline(waiter[QStringLiteral("baseline")], inventoryName(size_t(Inventory::ShutdownWaiter)))) return bad(QStringLiteral("shutdownWaiter"));
    for (const char *key : {"interpreter", "script", "wrapper", "backend"})
        if (!path(waiter[QString::fromLatin1(key)])) return bad(QString::fromLatin1(key));
    for (const char *key : {"logPaths", "lockPaths"}) {
        const auto list = waiter[QString::fromLatin1(key)];
        if (!stringArray(list)) return bad(QString::fromLatin1(key));
        for (const auto &item : list.toArray()) if (!path(item)) return bad(QString::fromLatin1(key));
    }
    if (error) error->clear();
    return Policy{root};
}
std::optional<QJsonValue> VirtualSessionMaintenanceWriterPolicy::decode(const Descriptor &d, const QVariant &v) {
    const QByteArray sig(d.signature);
    // No QVariant coercion or fromVariant: null QString is a typed empty string.
    if (sig == "s") return typed(v, QMetaType::QString) ? std::optional(QJsonValue(v.toString())) : std::nullopt;
    if (sig == "b") return typed(v, QMetaType::Bool) ? std::optional(QJsonValue(v.toBool())) : std::nullopt;
    if (sig == "i") return typed(v, QMetaType::Int) ? std::optional(QJsonValue(v.toInt())) : std::nullopt;
    if (sig == "u") return typed(v, QMetaType::UInt) ? std::optional(QJsonValue(qint64(v.toUInt()))) : std::nullopt;
    if (sig == "q") return typed(v, QMetaType::UShort) ? std::optional(QJsonValue(v.value<ushort>())) : std::nullopt;
    if (sig == "t") return typed(v, QMetaType::ULongLong) ? std::optional(QJsonValue(QString::number(v.toULongLong()))) : std::nullopt;
    if (sig == "as" && typed(v, QMetaType::QStringList)) return QJsonArray::fromStringList(v.toStringList());
    if (sig == "ay" && typed(v, QMetaType::QByteArray)) return v.toByteArray().isEmpty() ? std::optional(QJsonValue(QJsonArray{})) : std::nullopt;
    if (!typed(v, qMetaTypeId<QDBusArgument>())) return {};
    const auto a = qvariant_cast<QDBusArgument>(v);
    if (a.currentSignature().toLatin1() != sig) return {};
    if (sig == "as") { QStringList values; a >> values; return QJsonArray::fromStringList(values); }
    if (sig == "(bs)") {
        bool flag; QString value; a.beginStructure(); a >> flag >> value; a.endStructure();
        return QJsonObject{{QStringLiteral("ignoreFailure"), flag}, {QStringLiteral("value"), QJsonValue(value)}};
    }
    if (sig == "(bas)") {
        bool flag; QStringList values; a.beginStructure(); a >> flag >> values; a.endStructure();
        return QJsonObject{{QStringLiteral("allowList"), flag}, {QStringLiteral("items"), QJsonArray::fromStringList(values)}};
    }
    if (sig == "(ss)") {
        QString first, second; a.beginStructure(); a >> first >> second; a.endStructure();
        return QJsonArray{QJsonValue(first), QJsonValue(second)};
    }
    if (sig == "(tus)") {
        quint64 bytes; quint32 scale; QString enforce; a.beginStructure(); a >> bytes >> scale >> enforce; a.endStructure();
        return QJsonObject{{QStringLiteral("bytes"), QString::number(bytes)}, {QStringLiteral("scale"), qint64(scale)}, {QStringLiteral("enforce"), QJsonValue(enforce)}};
    }
    if (sig == "(aiai)") {
        a.beginStructure(); QJsonObject obj;
        const auto endStructure = qScopeGuard([&] { a.endStructure(); });
        for (const char *key : {"statuses", "signals"}) {
            QJsonArray values; a.beginArray();
            const auto endArray = qScopeGuard([&] { a.endArray(); });
            while (!a.atEnd()) { if (values.size() >= 256) return {}; qint32 n; a >> n; values.append(n); }
            obj[QString::fromLatin1(key)] = values;
        }
        return obj;
    }
    a.beginArray(); QJsonArray array;
    const auto endArray = qScopeGuard([&] { a.endArray(); });
    while (!a.atEnd()) {
        if (array.size() >= 256 || (sig == "a(sasasttttuii)" && array.size() >= 64)) return {};
        if (sig == "a(sasasttttuii)") {
            QString executable; QStringList argv, flags; quint64 sr, sm, er, em; quint32 pid; qint32 code, status;
            a.beginStructure(); a >> executable >> argv >> flags >> sr >> sm >> er >> em >> pid >> code >> status; a.endStructure();
            array.append(QJsonObject{{QStringLiteral("path"), QJsonValue(executable)}, {QStringLiteral("argv"), QJsonArray::fromStringList(argv)}, {QStringLiteral("flags"), QJsonArray::fromStringList(flags)}});
        } else if (sig == "a(sbbsi)") {
            QString type, parameter; bool trigger, negate; qint32 result;
            a.beginStructure(); a >> type >> trigger >> negate >> parameter >> result; a.endStructure();
            if (result < -1 || result > 1) return {};
            array.append(QJsonObject{{QStringLiteral("type"), QJsonValue(type)}, {QStringLiteral("parameter"), QJsonValue(parameter)}, {QStringLiteral("trigger"), trigger}, {QStringLiteral("negate"), negate}});
        } else if (sig == "a(ss)") {
            QString first, second; a.beginStructure(); a >> first >> second; a.endStructure();
            array.append(QJsonArray{QJsonValue(first), QJsonValue(second)});
        } else return {}; // Remaining array types are required to be empty.
    }
    return array;
}
bool VirtualSessionMaintenanceWriterPolicy::compareEffective(const Policy &policy, const Inputs &inputs, QString *error) {
    const auto manager = properties(inputs.manager);
    if (!manager) return errorAt(error, QStringLiteral("writerPolicyWire: manager"));
    const auto compare = [&](const Descriptor &d, const QJsonValue &expected, const QVariant &actual, const QString &name) {
        const auto decoded = decode(d, actual);
        const auto normalized = decoded ? normalize(d, *decoded, name, true) : std::nullopt;
        if (!normalized || *normalized != expected)
            return errorAt(error, QStringLiteral("writerPolicyMismatch: ") + name + QLatin1Char('/') + QString::fromLatin1(d.name));
        return true;
    };
    for (const auto &d : descriptors()) if (QByteArray(d.group) == "manager")
        if (!compare(d, policy.document[QStringLiteral("manager")].toObject().value(QString::fromLatin1(d.name)),
                     manager->value(QString::fromLatin1(d.name)), QString())) return false;
    for (size_t i = 0; i < inputs.units.size(); ++i) {
        const auto name = inventoryName(i);
        const QJsonValue baseline = i == size_t(Inventory::Coordinator) ? policy.document[QStringLiteral("coordinator")]
            : i == size_t(Inventory::ShutdownWaiter) ? policy.document[QStringLiteral("shutdownWaiter")].toObject().value(QStringLiteral("baseline"))
            : policy.document[QStringLiteral("writers")].toObject().value(name);
        const auto unit = properties(inputs.units[i].unit), service = properties(inputs.units[i].service);
        if (!unit || !service) return errorAt(error, QStringLiteral("writerPolicyWire: ") + name);
        if (inputs.units[i].permissionsStartOnly) return errorAt(error, QStringLiteral("writerPolicyMismatch: PermissionsStartOnly"));
        for (const auto &d : descriptors()) {
            const QByteArray group(d.group); if (group == "manager") continue;
            const auto key = QString::fromLatin1(d.name);
            const auto &map = group == "unit" ? *unit : *service;
            QVariant actual = map.value(key);
            if (key == QStringLiteral("PermissionsStartOnly")) {
                if (map.contains(key) && (!typed(actual, QMetaType::Bool) || actual.toBool() != inputs.units[i].permissionsStartOnly))
                    return errorAt(error, QStringLiteral("writerPolicyWire: inconsistent hidden property"));
                actual = inputs.units[i].permissionsStartOnly;
            }
            if (!compare(d, baseline[QString::fromLatin1(d.group)][key], actual, name)) return false;
        }
    }
    if (error) error->clear();
    return true;
}
bool VirtualSessionMaintenanceWriterPolicy::finishComparison(const Policy &policy, const Inputs &inputs, QString *error) {
    if (!compareEffective(policy, inputs, error)) return false;
    return errorAt(error, QStringLiteral("runtimeChecksIncomplete: writer quiescence, routing and path binding are not implemented"));
}
}
