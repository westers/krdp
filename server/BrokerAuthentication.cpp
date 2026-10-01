// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerAuthentication.h"
#include <Server.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace KRdp::BrokerAuthentication
{
namespace {
std::optional<QByteArray> derive(const QString &password, const QByteArray &salt)
{
    if (password.isEmpty() || password.size() > 4096 || password.contains(QChar::Null) || salt.size() != 16) return {};
    auto bytes = password.toUtf8();
    const auto clear = qScopeGuard([&] { OPENSSL_cleanse(bytes.data(), size_t(bytes.size())); });
    QByteArray result(32, Qt::Uninitialized);
    if (PKCS5_PBKDF2_HMAC(bytes.constData(), int(bytes.size()),
        reinterpret_cast<const unsigned char *>(salt.constData()), int(salt.size()), PasswordIterations,
        EVP_sha256(), int(result.size()), reinterpret_cast<unsigned char *>(result.data())) != 1) return {};
    return result;
}

bool exactKeys(const QJsonObject &object, const QStringList &keys)
{
    if (object.size() != keys.size()) return false;
    for (const auto &key : keys) if (!object.contains(key)) return false;
    return true;
}

bool nameValid(const QString &name)
{
    if (name.isEmpty() || name.size() > 256 || name.contains(QChar::Null)) return false;
    for (const QChar c : name) if (!c.isPrint()) return false;
    return true;
}

std::optional<Route> parseRoute(const QJsonValue &value, const AccountResolver &resolve)
{
    if (!value.isObject()) return {};
    const auto object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("pam"), QStringLiteral("credentials")})
        || !object.value(QStringLiteral("pam")).isObject() || !object.value(QStringLiteral("credentials")).isArray()) return {};
    const auto pam = object.value(QStringLiteral("pam")).toObject();
    if (!exactKeys(pam, {QStringLiteral("mode"), QStringLiteral("accounts")})
        || !pam.value(QStringLiteral("mode")).isString() || !pam.value(QStringLiteral("accounts")).isArray()) return {};
    Route route;
    const auto mode = pam.value(QStringLiteral("mode")).toString();
    if (mode == QLatin1String("any")) route.pam = PamMode::Any;
    else if (mode == QLatin1String("allow-list")) route.pam = PamMode::AllowList;
    else if (mode != QLatin1String("disabled")) return {};
    const auto accounts = pam.value(QStringLiteral("accounts")).toArray();
    if (accounts.size() > 128 || (route.pam != PamMode::AllowList && !accounts.isEmpty())) return {};
    for (const auto &account : accounts) {
        if (!account.isString() || !nameValid(account.toString())) return {};
        const auto uid = resolve(account.toString());
        if (!uid || !*uid || *uid == quint32(-1) || route.pamAccounts.contains(*uid)) return {};
        route.pamAccounts.insert(*uid);
    }
    const auto credentials = object.value(QStringLiteral("credentials")).toArray();
    if (credentials.size() > 128) return {};
    for (const auto &entry : credentials) {
        if (!entry.isObject()) return {};
        const auto credential = entry.toObject();
        if (!exactKeys(credential, {QStringLiteral("alias"), QStringLiteral("owner"), QStringLiteral("verifier")})
            || !credential.value(QStringLiteral("alias")).isString() || !credential.value(QStringLiteral("owner")).isString()
            || !credential.value(QStringLiteral("verifier")).isString()) return {};
        const auto alias = credential.value(QStringLiteral("alias")).toString();
        const auto owner = credential.value(QStringLiteral("owner")).toString();
        if (!nameValid(alias) || !nameValid(owner) || route.credentials.contains(alias)) return {};
        const auto uid = resolve(owner);
        if (!uid || !*uid || *uid == quint32(-1)) return {};
        const auto verifier = credential.value(QStringLiteral("verifier")).toString();
        static const QRegularExpression format(QStringLiteral("\\Apbkdf2-sha256\\$600000\\$([0-9a-f]{32})\\$([0-9a-f]{64})\\z"));
        const auto match = format.match(verifier);
        if (!match.hasMatch()) return {};
        route.credentials.insert(alias, {*uid, QByteArray::fromHex(match.captured(1).toLatin1()),
            QByteArray::fromHex(match.captured(2).toLatin1())});
    }
    return route;
}
}

bool Route::allowsPam(quint32 uid) const
{
    return uid && uid != quint32(-1) && (pam == PamMode::Any || (pam == PamMode::AllowList && pamAccounts.contains(uid)));
}

std::optional<quint32> Route::authenticate(const QString &alias, const QString &password) const
{
    const auto found = credentials.constFind(alias);
    if (found == credentials.cend() || !found->ownerUid || found->ownerUid == quint32(-1) || found->digest.size() != 32) return {};
    auto digest = derive(password, found->salt);
    if (!digest) return {};
    const bool matched = CRYPTO_memcmp(digest->constData(), found->digest.constData(), 32) == 0;
    OPENSSL_cleanse(digest->data(), size_t(digest->size()));
    return matched ? std::optional(found->ownerUid) : std::nullopt;
}

std::optional<Credential> makeCredential(quint32 ownerUid, const QString &password)
{
    if (!ownerUid || ownerUid == quint32(-1)) return {};
    QByteArray salt(16, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), int(salt.size())) != 1) return {};
    const auto digest = derive(password, salt);
    if (!digest) return {};
    return Credential{ownerUid, salt, *digest};
}

Result parse(const QByteArray &contents, const AccountResolver &resolve)
{
    const auto invalid = [] { return Result{{}, QStringLiteral("invalid authentication policy")}; };
    if (!resolve || contents.isEmpty() || contents.size() > MaximumBytes || contents.contains('\0')) return invalid();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return invalid();
    const auto object = document.object();
    if (!exactKeys(object, {QStringLiteral("version"), QStringLiteral("console"), QStringLiteral("virtual")})
        || !object.value(QStringLiteral("version")).isDouble() || object.value(QStringLiteral("version")).toDouble() != 1) return invalid();
    const auto console = parseRoute(object.value(QStringLiteral("console")), resolve);
    const auto virtualDesktop = parseRoute(object.value(QStringLiteral("virtual")), resolve);
    if (!console || !virtualDesktop) return invalid();
    return {Policy{*console, *virtualDesktop}, {}};
}

std::optional<quint32> resolveAccount(const QString &name)
{
    if (!nameValid(name)) return {};
    const auto encoded = name.toLocal8Bit();
    passwd account{};
    passwd *found = nullptr;
    std::vector<char> buffer(4096);
    int status;
    while ((status = getpwnam_r(encoded.constData(), &account, buffer.data(), buffer.size(), &found)) == ERANGE && buffer.size() < 1048576)
        buffer.resize(buffer.size() * 2);
    if (status || !found || !account.pw_uid || account.pw_uid == uid_t(-1)) return {};
    return quint32(account.pw_uid);
}

Result readFile(const QString &path, bool required)
{
    const auto failed = [] { return Result{{}, QStringLiteral("authentication policy cannot be read safely")}; };
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        if (errno == ENOENT && !required) {
            Policy policy;
            policy.console.pam = policy.virtualDesktop.pam = PamMode::Any;
            return {policy, {}};
        }
        return failed();
    }
    const auto closeFile = qScopeGuard([fd] { ::close(fd); });
    struct stat before{};
    if (::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid || (before.st_mode & 0077)
        || before.st_size <= 0 || before.st_size > MaximumBytes) return failed();
    QByteArray contents(qsizetype(before.st_size), Qt::Uninitialized);
    qsizetype total = 0;
    while (total < contents.size()) {
        const auto got = ::read(fd, contents.data() + total, size_t(contents.size() - total));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return failed();
        total += got;
    }
    struct stat after{};
    if (::fstat(fd, &after) || after.st_size != before.st_size || after.st_uid != before.st_uid
        || after.st_mode != before.st_mode || after.st_mtim.tv_sec != before.st_mtim.tv_sec
        || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec || after.st_ctim.tv_sec != before.st_ctim.tv_sec
        || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec) return failed();
    auto result = parse(contents, resolveAccount);
    if (result.policy) result.document = contents;
    return result;
}

bool apply(Server &server, const Route &route)
{
    return server.setBrokerAuthenticationPolicy(route.pam != PamMode::Disabled,
        [route](quint32 uid) { return route.allowsPam(uid); },
        [route](const QString &name, const QString &password) { return route.authenticate(name, password); });
}
}
