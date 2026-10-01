// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerHostSettings.h"
#include <QJsonObject>
namespace KRdp::BrokerHostAdmin {
constexpr qsizetype MaximumRequestBytes = 262144;
constexpr qsizetype MaximumPemBytes = 65536;
QString scopeName(BrokerHostSettings::Scope scope);
std::optional<BrokerHostSettings::Scope> scope(const QString &name);
QString revision(BrokerHostSettings::Scope scope, bool exists, const QByteArray &document);
struct View { QJsonObject value; QString error; };
View view(BrokerHostSettings::Scope scope, bool exists, const QByteArray &document);
enum class TlsMode { Keep, Standard, Existing, Import };
struct Update {
    QByteArray document;
    QVariantMap effective;
    TlsMode tls = TlsMode::Keep;
    QByteArray certificatePem;
    QByteArray privateKeyPem; // privileged process memory only, never a reply
    QString error;
};
// Pure preparation; filesystem/TLS path/device checks still required by helper.
Update prepare(BrokerHostSettings::Scope scope, bool exists, const QByteArray &current, const QJsonObject &request);
}
