// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
// Test helper: the root-written public-metadata snapshot a settings page reads
// on open (OPT-057 S1), for a scope with the given host document bytes.
#include "BrokerHostAdmin.h"
#include "BrokerHostPublicSnapshot.h"
#include <QJsonArray>
#include <QJsonObject>

namespace HostSnapshotFixture {
using Scope = KRdp::BrokerHostSettings::Scope;
inline QJsonObject fullView(Scope scope, const QByteArray &document = {})
{
    using namespace Qt::StringLiterals;
    auto view = KRdp::BrokerHostAdmin::view(scope, true, document).value;
    if (scope == Scope::VirtualSession) {
        view.insert(u"renderDevices"_s, QJsonArray{QJsonObject{{u"pci"_s, u"0000:01:00.0"_s}, {u"driver"_s, u"nvidia"_s}, {u"render"_s, u"/dev/dri/renderD128"_s}}});
    } else {
        view.insert(u"tls"_s, QJsonObject{{u"state"_s, u"valid"_s}, {u"administratorManaged"_s, false}, {u"fingerprint"_s, u"AA:BB"_s},
            {u"algorithm"_s, u"ECDSA"_s}, {u"notBefore"_s, u"2026-01-01T00:00:00Z"_s}, {u"notAfter"_s, u"2036-01-01T00:00:00Z"_s}});
        view.insert(u"cameraLoopback"_s, QJsonObject{{u"supported"_s, scope == Scope::Console}, {u"state"_s, scope == Scope::Console ? u"disabled"_s : u"namespace-unavailable"_s}});
    }
    view.insert(u"runtimeVerified"_s, false);
    view.insert(u"application"_s, scope == Scope::VirtualSession ? u"new-desktops"_s : u"broker-restart"_s);
    return view;
}
inline bool publishAll(const QString &directory, const QByteArray &document = {})
{
    for (auto scope : {Scope::Console, Scope::Virtual, Scope::VirtualSession})
        if (!KRdp::BrokerHostPublicSnapshot::write(directory, scope, fullView(scope, document))) return false;
    return true;
}
}
