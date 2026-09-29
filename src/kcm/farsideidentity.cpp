// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "farsideidentity.h"

using namespace Qt::StringLiterals;

namespace Farside
{
Identity defaultIdentity()
{
    Identity identity;
    // RENAME STAGE: Farside's names. Change these three.
    identity.serverUnit = u"app-io.github.westers.farside.server.service"_s;
    identity.serverExecutables = {u"/usr/bin/farside-server"_s};
    identity.virtualHostUnit = u"farside-virtual-host.service"_s;

    // KDE's stock krdp package: keep.
    identity.stockUnit = u"app-org.kde.krdpserver.service"_s;
    identity.stockExecutables = {u"/usr/bin/krdpserver"_s};
    identity.stockPort = 3389;

    identity.reservedPorts = {3391, 3395};

    // For tests and side-by-side checks of the two packages before the rename
    // stage: the units the page reads and controls.
    if (const auto unit = qEnvironmentVariable("FARSIDE_KCM_SERVER_UNIT"); !unit.isEmpty()) {
        identity.serverUnit = unit;
    }
    if (const auto unit = qEnvironmentVariable("FARSIDE_KCM_STOCK_UNIT"); !unit.isEmpty()) {
        identity.stockUnit = unit;
    }
    return identity;
}
}
