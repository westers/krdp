// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostRuntime.h"
#include "BrokerHostAdmin.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
using namespace Qt::StringLiterals;
namespace KRdp::BrokerHostRuntime {
namespace {
QString optionKey(const QString &name)
{
    static const QMap<QString, QString> names{{u"address"_s, u"Address"_s}, {u"port"_s, u"Port"_s},
        {u"certificate"_s, u"Certificate"_s}, {u"certificate-key"_s, u"CertificateKey"_s},
        {u"quality"_s, u"Quality"_s}, {u"adaptive-quality"_s, u"AdaptiveQuality"_s},
        {u"prefer-audio-quality"_s, u"PreferAudioQuality"_s}, {u"standard-client-media"_s, u"StandardClientMedia"_s},
        {u"camera-loopback-device"_s, u"CameraLoopbackDevice"_s}, {u"software-encoding"_s, u"SoftwareEncoding"_s},
        {u"software-avc"_s, u"SoftwareAvc"_s}, {u"software-hevc"_s, u"SoftwareHevc"_s}, {u"software-av1"_s, u"SoftwareAv1"_s},
        {u"av1-tiles"_s, u"Av1Tiles"_s}, {u"vaapi-driver"_s, u"VaapiDriver"_s}};
    return names.value(name);
}
void add(QStringList &list, const QString &reason) { if (!list.contains(reason)) list.append(reason); }
bool bounded(const QStringList &values, int count = 128)
{
    if (values.size() > count) return false;
    qsizetype size = 0;
    for (const auto &value : values) { if (value.size() > 4096 || value.contains(QChar::Null)) return false; size += value.toUtf8().size() + 1; }
    return size <= MaximumBytes;
}
QStringList differences(const QVariantMap &values, const QVariantMap &stored)
{
    QStringList keys;
    for (auto it = values.begin(); it != values.end(); ++it) if (!stored.contains(it.key()) || stored[it.key()] != it.value()) keys.append(it.key());
    return keys;
}
const QStringList reasonCodes{u"unsupported-command"_s, u"unknown-option"_s, u"duplicate-option"_s, u"invalid-field"_s,
    u"missing-field"_s, u"custom-worker"_s, u"custom-authentication"_s, u"custom-runtime"_s, u"incomplete-context"_s,
    u"bounds"_s, u"unsafe-file"_s, u"missing-file"_s, u"invalid-environment"_s, u"unknown-expansion"_s,
    u"inherited-environment"_s, u"manager-reload"_s, u"custom-unit"_s, u"process-identity"_s,
    u"stale"_s, u"denied"_s, u"unavailable"_s, u"malformed"_s};
}
Contract installedContract(Scope scope)
{
    const auto bin = QString::fromUtf8(FARSIDE_RUNTIME_BINDIR);
    return {bin + (scope == Scope::Console ? u"/farside-console-host"_s : u"/farside-virtual-host"_s),
        bin + u"/farside-console-worker"_s, u"/run/farside-console"_s};
}
QString unitName(Scope scope)
{
    return scope == Scope::Console ? u"farside-console-host.service"_s
        : scope == Scope::Virtual ? u"farside-virtual-host.service"_s : QString();
}
Arguments arguments(Scope scope, const QStringList &argv, const Contract &contract)
{
    Arguments result;
    result.missing = BrokerHostSettings::keys(scope);
    if (scope == Scope::VirtualSession || argv.isEmpty() || argv.front() != contract.broker) { result.reasons.append(u"unsupported-command"_s); return result; }
    if (!bounded(argv)) { result.reasons.append(u"bounds"_s); return result; }
    QSet<QString> seen;
    bool worker = scope != Scope::Console;
    for (int i = 1; i < argv.size(); ++i) {
        const auto argument = argv[i];
        if (!argument.startsWith(u"--") || argument.size() <= 2) { add(result.reasons, u"unknown-option"_s); continue; }
        const auto equal = argument.indexOf(u'=');
        const auto name = argument.mid(2, equal < 0 ? -1 : equal - 2);
        const auto key = optionKey(name);
        const bool context = name == u"authentication-policy" || (scope == Scope::Console && (name == u"worker" || name == u"runtime-directory"));
        if ((key.isEmpty() || (!result.missing.contains(key) && !result.values.contains(key))) && !context) {
            add(result.reasons, u"unknown-option"_s);
            if (equal < 0 && i + 1 < argv.size() && !argv[i + 1].startsWith(u"--")) ++i;
            continue;
        }
        if (seen.contains(name)) { add(result.reasons, u"duplicate-option"_s); if (equal < 0 && i + 1 < argv.size()) ++i; continue; }
        seen.insert(name);
        QString value;
        if (equal >= 0) value = argument.mid(equal + 1);
        else if (i + 1 < argv.size() && !argv[i + 1].startsWith(u"--")) value = argv[++i];
        else { add(result.reasons, u"invalid-field"_s); continue; }
        if (context) {
            if (name == u"worker") { worker = true; if (value != contract.worker) add(result.reasons, u"custom-worker"_s); }
            else if (name == u"runtime-directory" && value != contract.runtimeDirectory) add(result.reasons, u"custom-runtime"_s);
            else if (name == u"authentication-policy" && value != u"/etc/farside/authentication.json") add(result.reasons, u"custom-authentication"_s);
            continue;
        }
        const auto normalized = BrokerHostSettings::normalize(scope, key, value);
        const bool boolean = key == u"AdaptiveQuality" || key == u"PreferAudioQuality" || key == u"StandardClientMedia";
        if (!normalized || (boolean && value != *normalized)) { add(result.reasons, u"invalid-field"_s); continue; }
        result.values[key] = *normalized; result.missing.removeAll(key);
    }
    if (!worker) add(result.reasons, u"incomplete-context"_s);
    if (result.values.contains(u"Certificate"_s) && result.values[u"Certificate"_s] == result.values[u"CertificateKey"_s]) add(result.reasons, u"invalid-field"_s);
    if (!result.missing.isEmpty()) add(result.reasons, u"missing-field"_s);
    return result;
}
Projection project(Scope scope, const Unit &unit, const QList<File> &files, const Contract &contract)
{
    Projection result;
    const auto reject = [&](const QString &reason) { add(result.reasons, reason); };
    result.custom = !unit.dropIns.isEmpty() || unit.files.size() != 1 || unit.files.front().path != u"/etc/farside/"_s + BrokerHostSettings::fileName(scope);
    if (scope == Scope::VirtualSession || unit.id != unitName(scope) || unit.loadState != u"loaded" || unit.commands.size() != 1
        || unit.user != u"root" || unit.type != u"exec") { reject(u"custom-unit"_s); return result; }
    if (!bounded(unit.environment, 1024) || !bounded(unit.passEnvironment) || !bounded(unit.unsetEnvironment)
        || unit.files.size() > 32 || unit.dropIns.size() > 64) { reject(u"bounds"_s); return result; }
    if (!unit.passEnvironment.isEmpty() || !unit.pamName.isEmpty()) reject(u"inherited-environment"_s);
    if (unit.needsReload) reject(u"manager-reload"_s);
    QMap<QString, QString> environment;
    for (const auto &entry : unit.environment) {
        const auto equal = entry.indexOf(u'=');
        if (equal <= 0) { reject(u"invalid-environment"_s); continue; }
        environment[entry.left(equal)] = entry.mid(equal + 1);
    }
    if (files.size() != unit.files.size()) { reject(u"malformed"_s); return result; }
    for (int i = 0; i < files.size(); ++i) {
        const auto &file = files[i];
        if (!file.error.isEmpty()) { reject(u"unsafe-file"_s); continue; }
        if (!file.exists) { if (!unit.files[i].optional) reject(u"missing-file"_s); continue; }
        const auto parsed = BrokerHostSettings::privateEnvironment(file.bytes);
        if (!parsed.error.isEmpty()) { reject(u"invalid-environment"_s); continue; }
        for (auto it = parsed.assignments.begin(); it != parsed.assignments.end(); ++it) environment[it.key()] = it.value();
    }
    for (const auto &unset : unit.unsetEnvironment) {
        const auto equal = unset.indexOf(u'='); const auto name = equal < 0 ? unset : unset.left(equal);
        if (equal < 0 || environment.value(name) == unset.mid(equal + 1)) environment.remove(name);
    }
    const auto &command = unit.commands.front();
    if (!bounded(command.arguments) || command.ignoreFailure) { reject(u"unsupported-command"_s); return result; }
    auto argv = command.arguments;
    if (argv.isEmpty() || argv.front() != command.path) { reject(u"unsupported-command"_s); return result; }
    if (scope == Scope::Virtual && command.path == u"/usr/bin/env") {
        const QStringList prefix{u"/usr/bin/env"_s, u"-i"_s, u"PATH=/usr/bin:/bin"_s, u"LANG=C.UTF-8"_s, u"HOME=/var/lib/farside/virtual-host"_s, contract.broker};
        if (argv.mid(0, prefix.size()) != prefix) { reject(u"unsupported-command"_s); return result; }
        argv = argv.mid(prefix.size() - 1);
    } else if (command.path != contract.broker) { reject(u"unsupported-command"_s); return result; }
    static const QRegularExpression expansion(u"\\A\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}\\z"_s);
    for (int i = 1; i < argv.size(); ++i) {
        if (!argv[i].startsWith(u"--")) continue;
        const auto equals = argv[i].indexOf(u'=');
        const auto key = optionKey(argv[i].mid(2, equals < 0 ? -1 : equals - 2));
        if (key.isEmpty()) continue;
        const auto value = equals >= 0 ? argv[i].mid(equals + 1) : argv.value(i + 1);
        if (value != u"${"_s + BrokerHostSettings::environmentName(scope, key) + u"}"_s) result.custom = true;
    }
    for (auto &value : argv) {
        const auto match = expansion.match(value);
        if (match.hasMatch()) {
            if (!environment.contains(match.captured(1))) { reject(u"unknown-expansion"_s); value.clear(); }
            else value = environment[match.captured(1)];
        } else if (value.contains(u'$') || value.contains(u'%')) reject(u"unknown-expansion"_s);
    }
    result.fields = arguments(scope, argv, contract);
    return result;
}
QJsonObject summarize(Scope scope, const Unit &unit, const Projection &projection,
    const Process &process, const QVariantMap &stored, const QString &revision, const Contract &contract, const QString &failure)
{
    auto configured = projection.verified() ? projection.fields.values : QVariantMap();
    Arguments running;
    QStringList reasons = projection.reasons;
    for (const auto &reason : projection.fields.reasons) add(reasons, reason);
    const bool identity = process.alive && process.root && process.executableMatches && process.pid > 1
        && process.pid == unit.pid && process.startTicks && process.pidIdentity >= 2
        && !unit.controlGroup.isEmpty() && process.controlGroup == unit.controlGroup;
    const bool active = unit.loadState == u"loaded" && unit.activeState == u"active" && unit.subState == u"running";
    if (active && identity) running = arguments(scope, process.argv, contract);
    else if (active) add(reasons, u"process-identity"_s);
    for (const auto &reason : running.reasons) add(reasons, reason);
    bool verified = active && identity && running.complete();
    bool configuredVerified = projection.verified();
    QString state;
    if (!failure.isEmpty()) {
        state = reasonCodes.contains(failure) ? failure : u"unavailable"_s; add(reasons, state);
        configured.clear(); running.values.clear(); verified = configuredVerified = false;
    } else if (unit.loadState == u"not-found") state = u"missing"_s;
    else if (!active) state = u"inactive"_s;
    else if (!identity) state = u"unavailable"_s;
    else if (!verified || !configuredVerified) state = u"partial"_s;
    else if (running.values != stored || configured != stored || running.values != configured) state = u"different"_s;
    else state = projection.custom ? u"custom"_s : u"verified"_s;
    return {{u"version"_s, 1}, {u"scope"_s, BrokerHostAdmin::scopeName(scope)}, {u"unit"_s, unitName(scope)},
        {u"storedRevision"_s, revision}, {u"state"_s, state}, {u"loadState"_s, unit.loadState}, {u"activeState"_s, unit.activeState},
        {u"subState"_s, unit.subState}, {u"pid"_s, int(unit.pid)}, {u"custom"_s, projection.custom},
        {u"needsReload"_s, unit.needsReload}, {u"configuredVerified"_s, configuredVerified}, {u"runningVerified"_s, verified},
        {u"configured"_s, QJsonObject::fromVariantMap(configured)}, {u"running"_s, QJsonObject::fromVariantMap(running.values)},
        {u"missing"_s, QJsonArray::fromStringList(running.missing)}, {u"reasons"_s, QJsonArray::fromStringList(reasons)},
        {u"configuredDifferences"_s, QJsonArray::fromStringList(differences(configured, stored))},
        {u"runningDifferences"_s, QJsonArray::fromStringList(differences(running.values, stored))}};
}
bool validPublic(Scope scope, const QJsonObject &value)
{
    const QStringList names{u"version"_s, u"scope"_s, u"unit"_s, u"storedRevision"_s, u"state"_s, u"loadState"_s,
        u"activeState"_s, u"subState"_s, u"pid"_s, u"custom"_s, u"needsReload"_s, u"configuredVerified"_s, u"runningVerified"_s,
        u"configured"_s, u"running"_s, u"missing"_s, u"reasons"_s, u"configuredDifferences"_s, u"runningDifferences"_s};
    if (scope == Scope::VirtualSession || value.size() != names.size()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) if (!names.contains(it.key())) return false;
    if (value[u"version"_s].toDouble() != 1 || value[u"scope"_s].toString() != BrokerHostAdmin::scopeName(scope)
        || value[u"unit"_s].toString() != unitName(scope) || !value[u"storedRevision"_s].isString()
        || !QRegularExpression(u"\\A[0-9a-f]{64}\\z"_s).match(value[u"storedRevision"_s].toString()).hasMatch()
        || !value[u"pid"_s].isDouble() || value[u"pid"_s].toDouble() < 0 || value[u"pid"_s].toDouble() > 2147483647
        || value[u"pid"_s].toDouble() != value[u"pid"_s].toInt()) return false;
    const QStringList states{u"missing"_s, u"inactive"_s, u"partial"_s, u"different"_s, u"custom"_s, u"verified"_s,
        u"stale"_s, u"denied"_s, u"unavailable"_s, u"malformed"_s};
    if (!value[u"state"_s].isString() || !states.contains(value[u"state"_s].toString())) return false;
    for (const auto &key : {u"loadState"_s, u"activeState"_s, u"subState"_s})
        if (!value[key].isString() || !QRegularExpression(u"\\A[a-z-]{0,32}\\z"_s).match(value[key].toString()).hasMatch()) return false;
    for (const auto &key : {u"custom"_s, u"needsReload"_s, u"configuredVerified"_s, u"runningVerified"_s}) if (!value[key].isBool()) return false;
    for (const auto &name : {u"configured"_s, u"running"_s}) {
        if (!value[name].isObject()) return false;
        const auto fields = value[name].toObject();
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (!it.value().isString()) return false;
            const auto normalized = BrokerHostSettings::normalize(scope, it.key(), it.value().toString());
            if (!normalized || *normalized != it.value().toString()) return false;
        }
        const bool verified = value[name == u"running" ? u"runningVerified"_s : u"configuredVerified"_s].toBool();
        if (verified && fields.size() != BrokerHostSettings::keys(scope).size()) return false;
    }
    for (const auto &name : {u"missing"_s, u"reasons"_s, u"configuredDifferences"_s, u"runningDifferences"_s}) {
        if (!value[name].isArray() || value[name].toArray().size() > 32) return false;
        QStringList seen;
        for (const auto &item : value[name].toArray()) {
            if (!item.isString() || seen.contains(item.toString())) return false;
            const auto allowed = name == u"reasons" ? reasonCodes : BrokerHostSettings::keys(scope);
            if (!allowed.contains(item.toString())) return false;
            seen.append(item.toString());
        }
    }
    const auto state = value[u"state"_s].toString();
    const auto configured = value[u"configured"_s].toObject(), running = value[u"running"_s].toObject();
    const bool cv = value[u"configuredVerified"_s].toBool(), rv = value[u"runningVerified"_s].toBool();
    if ((!cv && !configured.isEmpty()) || (cv && value[u"needsReload"_s].toBool())) return false;
    if (rv && (value[u"loadState"_s].toString() != u"loaded" || value[u"activeState"_s].toString() != u"active"
        || value[u"subState"_s].toString() != u"running" || value[u"pid"_s].toInt() <= 1 || !value[u"missing"_s].toArray().isEmpty())) return false;
    for (const auto &key : value[u"missing"_s].toArray()) if (running.contains(key.toString())) return false;
    for (const auto &name : {u"configured"_s, u"running"_s}) {
        const auto fields = value[name].toObject();
        if (fields.contains(u"Certificate"_s) && fields[u"Certificate"_s] == fields[u"CertificateKey"_s]) return false;
        for (const auto &key : value[name + u"Differences"_s].toArray()) if (!fields.contains(key.toString())) return false;
    }
    if (state == u"verified" || state == u"custom" || state == u"different") {
        if (!cv || !rv || !value[u"reasons"_s].toArray().isEmpty()) return false;
        if (state != u"different" && (configured != running || !value[u"configuredDifferences"_s].toArray().isEmpty()
            || !value[u"runningDifferences"_s].toArray().isEmpty() || value[u"custom"_s].toBool() != (state == u"custom"))) return false;
        if (state == u"different" && configured == running && value[u"configuredDifferences"_s].toArray().isEmpty()
            && value[u"runningDifferences"_s].toArray().isEmpty()) return false;
    }
    if ((state == u"missing" || state == u"inactive" || state == u"unavailable") && rv) return false;
    if (state == u"stale" || state == u"denied" || state == u"malformed")
        if (cv || rv || !configured.isEmpty() || !running.isEmpty() || !value[u"reasons"_s].toArray().contains(state)) return false;
    return true;
}
}
