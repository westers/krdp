// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "settingsdefaults.h"

#include "krdpserversettings.h"

#include <KConfigGroup>

PreservedSettings PreservedSettings::capture(const KRDPServerSettings *settings)
{
    return {settings->certificate(), settings->certificateKey(), settings->users(), settings->systemUserEnabled()};
}

void PreservedSettings::restore(KRDPServerSettings *settings) const
{
    settings->setCertificate(certificate);
    settings->setCertificateKey(certificateKey);
    settings->setUsers(users);
    settings->setSystemUserEnabled(systemUserEnabled);
}

QStringList preservedSettingNames()
{
    return {QStringLiteral("Certificate"), QStringLiteral("CertificateKey"), QStringLiteral("Users"), QStringLiteral("SystemUserEnabled")};
}

bool resettableSettingsAreDefault(const KRDPServerSettings *settings)
{
    const auto preserved = preservedSettingNames();
    const auto items = settings->items();
    for (const auto *item : items) {
        if (!preserved.contains(item->key()) && !item->isDefault()) {
            return false;
        }
    }
    return true;
}

KRdp::ServerSettings::StartupSettings startupSettingsFrom(const KRDPServerSettings *settings)
{
    return {settings->listenPort(),
            settings->listenAddress(),
            settings->autogenerateCertificates(),
            settings->certificate(),
            settings->certificateKey(),
            settings->users(),
            settings->systemUserEnabled()};
}

bool saveSettingNow(KRDPServerSettings *settings, const QString &key, const QVariant &value)
{
    auto *item = settings->findItem(key);
    if (!item) {
        return false;
    }
    // The generated setter emits <Key>Changed, so bindings on the page follow.
    QString property = key;
    property[0] = property.at(0).toLower();
    settings->setProperty(property.toLatin1().constData(), value);
    KConfigGroup group(settings->config(), item->group());
    group.writeEntry(key, value);
    settings->config()->sync();
    item->readConfig(settings->config());
    return true;
}
