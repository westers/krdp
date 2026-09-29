// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>
#include <QStringList>

namespace Farside
{
// The installed names the settings page needs to tell Farside and KDE's own
// remote desktop (the stock `krdp` package) apart.
//
// RENAME STAGE: this is the one place to change them. Until then Farside's
// unit and binary still carry the stock names, so `stockIsDistinct()` is
// false and the page only looks at who holds the port.
struct Identity {
    // Farside's own user unit (rename: app-io.github.westers.farside.server.service).
    QString serverUnit;
    // Farside's server binaries (rename: /usr/bin/farside-server).
    QStringList serverExecutables;
    // KDE's own remote desktop: its user unit, binaries and fixed port. These
    // belong to the stock package and do not change with the rename.
    QString stockUnit;
    QStringList stockExecutables;
    quint16 stockPort = 3389;
    // The virtual desktop service (system unit); its settings are only shown
    // when it is installed (rename: farside-virtual-host.service).
    QString virtualHostUnit;
    // Ports the port suggestion must skip: the console and virtual desktop
    // services use them even when they are not running at the moment.
    QList<quint16> reservedPorts;

    bool stockIsDistinct() const
    {
        return !stockUnit.isEmpty() && stockUnit != serverUnit;
    }
};

Identity defaultIdentity();
}
