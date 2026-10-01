// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerAuthenticationAdmin.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cerrno>
#include <pwd.h>
#include <vector>

using namespace Qt::StringLiterals;
namespace KRdp::BrokerAuthenticationAdmin
{
namespace {
bool keys(const QJsonObject &object, const QStringList &required)
{
    if (object.size() != required.size()) return false;
    return std::all_of(required.cbegin(), required.cend(), [&](const auto &key) { return object.contains(key); });
}

std::optional<QJsonObject> publicRoute(const BrokerAuthentication::Route &route, const AccountName &name)
{
    QJsonArray accounts, credentials;
    auto ids = route.pamAccounts.values();
    std::sort(ids.begin(), ids.end());
    for (const auto uid : ids) {
        const auto account = name(uid);
        if (!account || account->isEmpty()) return {};
        accounts.append(*account);
    }
    auto aliases = route.credentials.keys();
    std::sort(aliases.begin(), aliases.end());
    for (const auto &alias : aliases) {
        const auto owner = name(route.credentials.value(alias).ownerUid);
        if (!owner || owner->isEmpty()) return {};
        credentials.append(QJsonObject{{u"alias"_s, alias}, {u"owner"_s, *owner}});
    }
    const auto mode = route.pam == BrokerAuthentication::PamMode::Any ? u"any"_s
        : route.pam == BrokerAuthentication::PamMode::AllowList ? u"allow-list"_s : u"disabled"_s;
    return QJsonObject{{u"pam"_s, QJsonObject{{u"mode"_s, mode}, {u"accounts"_s, accounts}}}, {u"credentials"_s, credentials}};
}

std::optional<QJsonObject> prepareRoute(const QJsonValue &value, const BrokerAuthentication::Route &current,
    const BrokerAuthentication::AccountResolver &resolve)
{
    if (!value.isObject()) return {};
    auto result = value.toObject();
    if (!keys(result, {u"pam"_s, u"credentials"_s}) || !result.value(u"credentials"_s).isArray()) return {};
    const auto desired = result.value(u"credentials"_s).toArray();
    if (desired.size() > 128) return {};
    QJsonArray credentials;
    for (const auto &entry : desired) {
        if (!entry.isObject()) return {};
        auto credential = entry.toObject();
        const bool password = credential.contains(u"password"_s);
        if (!keys(credential, password ? QStringList{u"alias"_s, u"owner"_s, u"password"_s} : QStringList{u"alias"_s, u"owner"_s})
            || !credential.value(u"alias"_s).isString() || !credential.value(u"owner"_s).isString()) return {};
        const auto alias = credential.value(u"alias"_s).toString();
        const auto owner = resolve(credential.value(u"owner"_s).toString());
        if (!owner || !*owner || *owner == quint32(-1)) return {};
        std::optional<BrokerAuthentication::Credential> verifier;
        if (password) {
            if (!credential.value(u"password"_s).isString()) return {};
            verifier = BrokerAuthentication::makeCredential(*owner, credential.value(u"password"_s).toString());
        } else {
            const auto found = current.credentials.constFind(alias);
            if (found != current.credentials.cend() && found->ownerUid == *owner) verifier = *found;
        }
        if (!verifier) return {}; // No reuse across routes, aliases or owner UIDs.
        credential.remove(u"password"_s);
        const QString encoded = u"pbkdf2-sha256$600000$"_s + QString::fromLatin1(verifier->salt.toHex())
            + u"$"_s + QString::fromLatin1(verifier->digest.toHex());
        credential.insert(u"verifier"_s, encoded);
        credentials.append(credential);
    }
    result.insert(u"credentials"_s, credentials);
    return result;
}
}

QString revision(const BrokerAuthentication::Result &current)
{
    return QString::fromLatin1(QCryptographicHash::hash(current.document.isEmpty() ? QByteArrayLiteral("absent-policy-v1") : current.document,
        QCryptographicHash::Sha256).toHex());
}

View view(const BrokerAuthentication::Result &current, const AccountName &name)
{
    if (!current.policy || !current.error.isEmpty() || !name) return {{}, u"authentication policy is unavailable"_s};
    const auto console = publicRoute(current.policy->console, name);
    const auto virtualDesktop = publicRoute(current.policy->virtualDesktop, name);
    if (!console || !virtualDesktop) return {{}, u"authentication policy account cannot be resolved"_s};
    return {{{u"version"_s, 1}, {u"revision"_s, revision(current)}, {u"console"_s, *console}, {u"virtual"_s, *virtualDesktop}}, {}};
}

Update prepare(const BrokerAuthentication::Result &current, const QJsonObject &request,
    const BrokerAuthentication::AccountResolver &resolve)
{
    const auto invalid = [] { return Update{{}, u"invalid authentication update"_s}; };
    if (!current.policy || !current.error.isEmpty() || !resolve) return invalid();
    if (!keys(request, {u"version"_s, u"revision"_s, u"console"_s, u"virtual"_s})
        || !request.value(u"version"_s).isDouble() || request.value(u"version"_s).toDouble() != 1
        || !request.value(u"revision"_s).isString() || QJsonDocument(request).toJson(QJsonDocument::Compact).size() > BrokerAuthentication::MaximumBytes)
        return invalid();
    if (request.value(u"revision"_s).toString() != revision(current)) return {{}, u"authentication policy changed; reload before saving"_s};
    const auto console = prepareRoute(request.value(u"console"_s), current.policy->console, resolve);
    const auto virtualDesktop = prepareRoute(request.value(u"virtual"_s), current.policy->virtualDesktop, resolve);
    if (!console || !virtualDesktop) return invalid();
    const auto document = QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"console"_s, *console}, {u"virtual"_s, *virtualDesktop}}).toJson();
    if (!BrokerAuthentication::parse(document, resolve).policy) return invalid();
    return {document, {}};
}

std::optional<QString> accountName(quint32 uid)
{
    if (!uid || uid == quint32(-1)) return {};
    passwd account{}; passwd *found = nullptr;
    std::vector<char> buffer(4096);
    int status;
    while ((status = getpwuid_r(uid, &account, buffer.data(), buffer.size(), &found)) == ERANGE && buffer.size() < 1048576)
        buffer.resize(buffer.size() * 2);
    if (status || !found || !account.pw_name) return {};
    const auto name = QString::fromLocal8Bit(account.pw_name);
    return BrokerAuthentication::resolveAccount(name) == std::optional<quint32>(uid) ? std::optional(name) : std::nullopt;
}
}
