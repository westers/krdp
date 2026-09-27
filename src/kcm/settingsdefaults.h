// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ServerSettingsPolicy.h"

#include <QString>
#include <QStringList>

class KRDPServerSettings;

// AUD-K1: what the Defaults button must never reset. The user list lives
// next to passwords in the keychain (resetting it would orphan them), the
// system-user row belongs to that list, and a certificate pair the user chose
// is not a "default" to be thrown away; everything else in krdpserverrc is on
// the page and resets normally.
struct PreservedSettings {
    QString certificate;
    QString certificateKey;
    QStringList users;
    bool systemUserEnabled = false;

    static PreservedSettings capture(const KRDPServerSettings *settings);
    void restore(KRDPServerSettings *settings) const;
};

// Settings the Defaults button leaves alone, by kcfg key name.
QStringList preservedSettingNames();

// True when every setting the Defaults button resets is at its default, so
// the button is disabled again right after it was used.
bool resettableSettingsAreDefault(const KRDPServerSettings *settings);

// AUD-K6: the saved values of the settings krdpserver reads only at startup.
KRdp::ServerSettings::StartupSettings startupSettingsFrom(const KRDPServerSettings *settings);
