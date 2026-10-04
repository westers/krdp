// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerauthenticationsettings.h"
#include <KLocalizedString>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace Qt::StringLiterals;
namespace {
constexpr qsizetype MaximumBytes = 65536;
bool validSnapshot(const QJsonObject &snapshot)
{
    if (snapshot.size() != 4 || !snapshot.value(u"version"_s).isDouble() || snapshot.value(u"version"_s).toDouble() != 1
        || !snapshot.value(u"revision"_s).isString()
        || !QRegularExpression(u"\\A[0-9a-f]{64}\\z"_s).match(snapshot.value(u"revision"_s).toString()).hasMatch()) return false;
    for (const auto &name : {u"console"_s, u"virtual"_s}) {
        if (!snapshot.value(name).isObject()) return false;
        const auto route = snapshot.value(name).toObject();
        if (route.size() != 2 || !route.value(u"pam"_s).isObject() || !route.value(u"credentials"_s).isArray()) return false;
        const auto pam = route.value(u"pam"_s).toObject();
        if (pam.size() != 2 || !pam.value(u"mode"_s).isString() || !pam.value(u"accounts"_s).isArray()
            || !QStringList{u"any"_s, u"allow-list"_s, u"disabled"_s}.contains(pam.value(u"mode"_s).toString())) return false;
        for (const auto &account : pam.value(u"accounts"_s).toArray()) if (!account.isString()) return false;
        for (const auto &value : route.value(u"credentials"_s).toArray()) {
            if (!value.isObject()) return false;
            const auto alias = value.toObject();
            if (alias.size() != 2 || !alias.value(u"alias"_s).isString() || !alias.value(u"owner"_s).isString()) return false;
        }
    }
    return true;
}
}

BrokerAuthenticationSettings::BrokerAuthenticationSettings(QObject *parent)
    : BrokerAuthenticationSettings(u"/usr/bin/pkexec"_s,
        {u"--disable-internal-agent"_s, QString::fromUtf8(FARSIDE_AUTHENTICATION_HELPER)}, parent) {}

BrokerAuthenticationSettings::BrokerAuthenticationSettings(const QString &program, const QStringList &arguments, QObject *parent)
    : QObject(parent), m_program(program), m_arguments(arguments) {}

QVariantMap BrokerAuthenticationSettings::policy() const
{
    auto result = m_pending;
    for (const auto &name : {u"console"_s, u"virtual"_s}) {
        auto route = result.value(name).toObject();
        QJsonArray aliases;
        for (const auto &value : route.value(u"credentials"_s).toArray()) {
            auto alias = value.toObject(); alias.remove(u"password"_s); aliases.append(alias);
        }
        route.insert(u"credentials"_s, aliases); result.insert(name, route);
    }
    return result.toVariantMap();
}

bool BrokerAuthenticationSettings::reject(const QString &error)
{
    m_error = error; Q_EMIT changed(); return false;
}

bool BrokerAuthenticationSettings::editableRoute(const QString &route) const
{
    return loaded() && !busy() && (route == u"console"_s || route == u"virtual"_s);
}

bool BrokerAuthenticationSettings::setPam(const QString &name, const QString &mode, const QStringList &accounts)
{
    if (!editableRoute(name) || !QStringList{u"any"_s, u"allow-list"_s, u"disabled"_s}.contains(mode)
        || accounts.size() > 128 || (mode != u"allow-list"_s && !accounts.isEmpty())) return reject(i18nc("@info", "The sign-in rule is not valid."));
    auto route = m_pending.value(name).toObject();
    route.insert(u"pam"_s, QJsonObject{{u"mode"_s, mode}, {u"accounts"_s, QJsonArray::fromStringList(accounts)}});
    m_pending.insert(name, route); m_error.clear(); Q_EMIT changed(); return true;
}

bool BrokerAuthenticationSettings::setAlias(const QString &name, const QString &alias, const QString &owner, const QString &password)
{
    if (!editableRoute(name) || alias.isEmpty() || owner.isEmpty() || alias.size() > 256 || owner.size() > 256
        || password.size() > 4096 || password.contains(QChar::Null)) return reject(i18nc("@info", "The remote login is not valid."));
    auto route = m_pending.value(name).toObject(); auto aliases = route.value(u"credentials"_s).toArray();
    int found = -1;
    for (int i = 0; i < aliases.size(); ++i) if (aliases[i].toObject().value(u"alias"_s).toString() == alias) found = i;
    if (password.isEmpty() && (found < 0 || aliases[found].toObject().value(u"owner"_s).toString() != owner))
        return reject(i18nc("@info", "A new password is required for a new remote login or a changed desktop account."));
    auto entry = found < 0 ? QJsonObject{{u"alias"_s, alias}, {u"owner"_s, owner}} : aliases[found].toObject();
    entry.insert(u"owner"_s, owner);
    if (!password.isEmpty()) entry.insert(u"password"_s, password);
    if (found < 0) {
        if (aliases.size() >= 128) return reject(i18nc("@info", "There are too many remote logins."));
        aliases.append(entry);
    } else aliases[found] = entry;
    route.insert(u"credentials"_s, aliases); m_pending.insert(name, route); m_error.clear(); Q_EMIT changed(); return true;
}

bool BrokerAuthenticationSettings::removeAlias(const QString &name, const QString &alias)
{
    if (!editableRoute(name)) return false;
    auto route = m_pending.value(name).toObject(); auto aliases = route.value(u"credentials"_s).toArray();
    bool removed = false;
    for (int i = aliases.size() - 1; i >= 0; --i) if (aliases[i].toObject().value(u"alias"_s).toString() == alias) {
        m_removedAlias = aliases[i].toObject(); m_removedRoute = name;
        aliases.removeAt(i); removed = true;
    }
    if (!removed) return false;
    route.insert(u"credentials"_s, aliases); m_pending.insert(name, route); m_error.clear(); Q_EMIT changed(); return true;
}
void BrokerAuthenticationSettings::discard()
{
    if (!loaded() || busy()) return;
    m_pending = m_snapshot; m_removedAlias = {}; m_removedRoute.clear();
    m_error.clear(); Q_EMIT changed();
}
bool BrokerAuthenticationSettings::undoRemoveAlias()
{
    if (m_removedAlias.isEmpty() || !editableRoute(m_removedRoute)) return false;
    auto route = m_pending[m_removedRoute].toObject(); auto aliases = route[u"credentials"_s].toArray();
    for (const auto &entry : aliases)
        if (entry.toObject()[u"alias"_s] == m_removedAlias[u"alias"_s]) return false;
    if (aliases.size() >= 128) return false;
    aliases.append(m_removedAlias); route[u"credentials"_s] = aliases;
    m_pending[m_removedRoute] = route; m_removedAlias = {}; m_removedRoute.clear();
    m_error.clear(); Q_EMIT changed(); return true;
}

bool BrokerAuthenticationSettings::reload() { return start({{u"version"_s, 1}, {u"operation"_s, u"read"_s}}, false); }
bool BrokerAuthenticationSettings::save()
{
    if (!loaded() || !modified()) return false;
    return start({{u"version"_s, 1}, {u"operation"_s, u"save"_s}, {u"update"_s, m_pending}}, true);
}

bool BrokerAuthenticationSettings::start(const QJsonObject &request, bool saving)
{
    if (busy()) return false;
    const auto input = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (input.size() > MaximumBytes) return reject(i18nc("@info", "The sign-in settings are too large to save."));
    auto *process = new QProcess(this); m_process = process; m_error.clear();
    process->setProgram(m_program); process->setArguments(m_arguments);
    connect(process, &QProcess::started, this, [process, input] { process->write(input); process->closeWriteChannel(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_process != process) return;
        m_process = nullptr; process->deleteLater(); reject(i18nc("@info", "Farside administration could not start."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, saving](int code, QProcess::ExitStatus status) {
        if (m_process != process) return;
        m_process = nullptr;
        const auto output = process->readAllStandardOutput();
        process->deleteLater();
        if (status != QProcess::NormalExit) { reject(i18nc("@info", "Farside administration stopped unexpectedly. Unlock the page again to see the current rules.")); return; }
        if (code == 126) { reject(i18nc("@info", "Administrator authentication was cancelled. The sign-in rules are unchanged.")); return; }
        if (code == 127) { reject(i18nc("@info", "Administrator authentication was not granted. The sign-in rules are unchanged.")); return; }
        QJsonParseError error;
        const auto response = output.size() <= MaximumBytes ? QJsonDocument::fromJson(output, &error) : QJsonDocument{};
        if (error.error != QJsonParseError::NoError || !response.isObject()) { reject(i18nc("@info", "Farside administration gave an unexpected reply. Unlock the page again to see the current rules.")); return; }
        const auto value = response.object();
        if (value.value(u"saved"_s).toBool()) m_lastSaveRequiresRestart = true;
        if (code != 0 || value.contains(u"error"_s)) {
            // The helper returns fixed structural reasons, never request bytes.
            reject(value.value(u"error"_s).isString() ? value.value(u"error"_s).toString().left(256) : u"Sign-in policy could not be saved"_s); return;
        }
        const auto snapshot = value.value(u"snapshot"_s).toObject();
        if (!validSnapshot(snapshot) || (saving && !value.value(u"saved"_s).toBool())) { reject(i18nc("@info", "The sign-in rules could not be read back. Unlock the page again to see the current rules.")); return; }
        m_snapshot = m_pending = snapshot; m_removedAlias = {}; m_removedRoute.clear(); m_error = value.value(u"warning"_s).toString().left(256); Q_EMIT changed();
    });
    Q_EMIT changed(); process->start(); return true;
}
