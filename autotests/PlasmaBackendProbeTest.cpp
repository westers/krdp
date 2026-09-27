// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX F3: the Wayland globals probe behind the backend choice. Off the
// Wayland platform it reports nothing (so the server falls back to the
// portal). On a Wayland session (run by hand with QT_QPA_PLATFORM=wayland) it
// must see at least wl_compositor and leave Qt's own connection usable.

#include <QGuiApplication>
#include <QTest>

#include "PlasmaBackendProbe.h"
#include "ServerSettingsPolicy.h"

class PlasmaBackendProbeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void probe()
    {
        const QStringList globals = KRdp::advertisedWaylandGlobals();
        if (QGuiApplication::platformName() != QLatin1String("wayland")) {
            QVERIFY(globals.isEmpty());
            QVERIFY(!KRdp::ServerSettings::plasmaProtocolsAvailable(globals));
            return;
        }
        QVERIFY(globals.contains(QStringLiteral("wl_compositor")));
        // A second probe works too: the private queue left nothing behind.
        QCOMPARE(KRdp::advertisedWaylandGlobals().contains(QStringLiteral("wl_compositor")), true);
        qInfo() << "Plasma protocols available to this binary:" << KRdp::ServerSettings::plasmaProtocolsAvailable(globals);
    }
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication app(argc, argv);
    PlasmaBackendProbeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PlasmaBackendProbeTest.moc"
