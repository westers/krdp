// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "BrokerAuthentication.h"
#include <QJsonObject>

namespace KRdp::BrokerAuthenticationAdmin
{
using AccountName = std::function<std::optional<QString>(quint32)>;
struct View {
    QJsonObject value;
    QString error;
};
struct Update {
    QByteArray document; // Privileged storage bytes only, never a UI reply.
    QString error;
};

QString revision(const BrokerAuthentication::Result &current);
View view(const BrokerAuthentication::Result &current, const AccountName &name);
Update prepare(const BrokerAuthentication::Result &current, const QJsonObject &request,
    const BrokerAuthentication::AccountResolver &resolve);
std::optional<QString> accountName(quint32 uid);
}
