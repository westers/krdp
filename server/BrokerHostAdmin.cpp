// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostAdmin.h"
#include "ServerCertificate.h"
#include <QCryptographicHash>
#include <QRegularExpression>
using namespace Qt::StringLiterals;
namespace KRdp::BrokerHostAdmin {
using Scope = BrokerHostSettings::Scope;
QString scopeName(Scope scope)
{
    if (scope == Scope::Console) return u"console"_s;
    if (scope == Scope::Virtual) return u"virtual"_s;
    if (scope == Scope::VirtualSession) return u"session"_s;
    return {};
}
std::optional<Scope> scope(const QString &name)
{
    if (name == u"console") return Scope::Console;
    if (name == u"virtual") return Scope::Virtual;
    if (name == u"session") return Scope::VirtualSession;
    return {};
}
QString revision(Scope scope, bool exists, const QByteArray &document)
{
    QByteArray bytes("FarsideHostSettings-v1\0", 23);
    bytes += scopeName(scope).toUtf8(); bytes += '\0';
    bytes += exists ? "present\0"_ba : "absent\0"_ba;
    bytes += document;
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
View view(Scope scope, bool exists, const QByteArray &document)
{
    const auto parsed = BrokerHostSettings::parse(scope, document);
    if (!parsed.error.isEmpty()) return {{}, parsed.error};
    return {{{u"version"_s, 1}, {u"scope"_s, scopeName(scope)}, {u"revision"_s, revision(scope, exists, document)},
        {u"values"_s, QJsonObject::fromVariantMap(parsed.overrides)},
        {u"defaults"_s, QJsonObject::fromVariantMap(BrokerHostSettings::defaults(scope))},
        {u"effective"_s, QJsonObject::fromVariantMap(parsed.effective)}}, {}};
}
Update prepare(Scope scope, bool exists, const QByteArray &current, const QJsonObject &request)
{
    const auto refuse = [](const QString &error) { Update result; result.error = error; return result; };
    const auto before = BrokerHostSettings::parse(scope, current);
    if (!before.error.isEmpty()) return refuse(before.error);
    static const QStringList allowed{u"version"_s, u"operation"_s, u"scope"_s, u"revision"_s, u"values"_s, u"tls"_s};
    for (auto it = request.begin(); it != request.end(); ++it) if (!allowed.contains(it.key())) return refuse(u"invalid host settings request"_s);
    if (!request[u"version"_s].isDouble() || request[u"version"_s].toDouble() != 1 || request[u"operation"_s].toString() != u"save"
        || request[u"scope"_s].toString() != scopeName(scope) || !request[u"values"_s].isObject() || !request[u"revision"_s].isString()
        || !QRegularExpression(u"\\A[0-9a-f]{64}\\z"_s).match(request[u"revision"_s].toString()).hasMatch())
        return refuse(u"invalid host settings request"_s);
    if (request[u"revision"_s].toString() != revision(scope, exists, current)) return refuse(u"host settings changed; reload before saving"_s);
    QVariantMap desired;
    const auto values = request[u"values"_s].toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!it.value().isString() || !BrokerHostSettings::keys(scope).contains(it.key())) return refuse(u"unknown or untyped host setting"_s);
        desired.insert(it.key(), it.value().toString());
    }
    Update result;
    if (request.contains(u"tls"_s)) {
        if (scope == Scope::VirtualSession || !request[u"tls"_s].isObject()) return refuse(u"invalid TLS operation"_s);
        const auto tls = request[u"tls"_s].toObject();
        const auto mode = tls[u"mode"_s].toString();
        if (!tls[u"mode"_s].isString() || tls.size() != (mode == u"import" ? 3 : 1)) return refuse(u"invalid TLS operation"_s);
        if (mode == u"keep") result.tls = TlsMode::Keep;
        else if (mode == u"existing") result.tls = TlsMode::Existing;
        else if (mode == u"standard") result.tls = TlsMode::Standard;
        else if (mode == u"import") {
            result.tls = TlsMode::Import;
            if (!tls[u"certificatePem"_s].isString() || !tls[u"privateKeyPem"_s].isString()) return refuse(u"invalid TLS import"_s);
            result.certificatePem = tls[u"certificatePem"_s].toString().toUtf8();
            result.privateKeyPem = tls[u"privateKeyPem"_s].toString().toUtf8();
            if (result.certificatePem.size() > MaximumPemBytes || result.privateKeyPem.size() > MaximumPemBytes
                || result.certificatePem.contains('\0') || result.privateKeyPem.contains('\0')) return refuse(u"invalid TLS import"_s);
            const auto info = ServerCertificate::inspectPem(result.certificatePem, result.privateKeyPem);
            const auto now = QDateTime::currentDateTimeUtc();
            if (!info.usable() || !info.notBefore.isValid() || !info.notAfter.isValid() || info.notBefore > now || info.notAfter <= now)
                return refuse(u"TLS import requires a current matching certificate and unencrypted private key"_s);
        } else return refuse(u"invalid TLS operation"_s);
    }
    if (result.tls == TlsMode::Standard || result.tls == TlsMode::Import) {
        if (desired.contains(u"Certificate"_s) || desired.contains(u"CertificateKey"_s)) return refuse(u"TLS operation cannot include custom paths"_s);
    }
    if (result.tls == TlsMode::Existing && (!desired.contains(u"Certificate"_s) || !desired.contains(u"CertificateKey"_s)))
        return refuse(u"both existing TLS paths are required"_s);
    // The public snapshot hides the TLS path values, so a caller that keeps TLS
    // cannot echo them back: preserve the saved overrides instead of dropping them.
    if (scope != Scope::VirtualSession && result.tls == TlsMode::Keep)
        for (const auto &key : {u"Certificate"_s, u"CertificateKey"_s})
            if (!desired.contains(key) && before.overrides.contains(key)) desired.insert(key, before.overrides[key]);
    const auto edited = BrokerHostSettings::edit(scope, current, desired);
    if (!edited.error.isEmpty()) return refuse(edited.error);
    result.document = edited.contents;
    result.effective = BrokerHostSettings::parse(scope, result.document).effective;
    if (scope != Scope::VirtualSession && result.tls == TlsMode::Keep
        && (result.effective[u"Certificate"_s] != before.effective[u"Certificate"_s]
            || result.effective[u"CertificateKey"_s] != before.effective[u"CertificateKey"_s]))
        return refuse(u"choose an explicit TLS operation before changing certificate paths"_s);
    return result;
}
}
