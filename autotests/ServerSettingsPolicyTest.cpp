// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-K4 (port range) and AUD-K5 (backend choice), shared by krdpserver and the KCM.

#include <QTest>

#include "ServerSettingsPolicy.h"

using namespace Qt::StringLiterals;

using namespace KRdp::ServerSettings;

class ServerSettingsPolicyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void portRange()
    {
        QVERIFY(!isValidListenPort(0));
        QVERIFY(isValidListenPort(1));
        QVERIFY(isValidListenPort(3389));
        QVERIFY(isValidListenPort(65535));
        QVERIFY(!isValidListenPort(65536));
        QVERIFY(!isValidListenPort(-1));
        // What used to wrap: 3389 + 65536 would have become 3389 as a quint16.
        QVERIFY(!isValidListenPort(3389 + 65536));
    }

    void parsePort_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("expected"); // -1 = rejected
        QTest::newRow("default") << u"3389"_s << 3389;
        QTest::newRow("min") << u"1"_s << 1;
        QTest::newRow("max") << u"65535"_s << 65535;
        QTest::newRow("zero") << u"0"_s << -1;
        QTest::newRow("too big") << u"65536"_s << -1;
        QTest::newRow("way too big") << u"99999999"_s << -1;
        QTest::newRow("empty") << QString() << -1;
        QTest::newRow("sign") << u"+3389"_s << -1;
        QTest::newRow("negative") << u"-1"_s << -1;
        QTest::newRow("space") << u" 3389"_s << -1;
        QTest::newRow("letters") << u"33a9"_s << -1;
    }
    void parsePort()
    {
        QFETCH(QString, text);
        QFETCH(int, expected);
        const auto parsed = parseListenPort(text);
        if (expected < 0) {
            QVERIFY(!parsed);
        } else {
            QVERIFY(parsed);
            QCOMPARE(int(*parsed), expected);
        }
    }

    void backendChoice()
    {
        auto choose = [](BackendOverride override, const QString &mode, bool built, bool available) {
            BackendRequest request;
            request.override = override;
            request.monitorMode = mode;
            request.plasmaBuilt = built;
            request.plasmaAvailable = available;
            return chooseBackend(request);
        };
        const auto allModes = {u"workspace"_s, u"primary"_s, u"specific"_s, u"multi"_s, u"virtual"_s, u" Virtual "_s, QString()};

        // AUD-FIX F3: no flag on a Plasma session -> Plasma for every mode, so
        // the packaged unit never waits for a portal dialog.
        for (const auto &mode : allModes) {
            const auto choice = choose(BackendOverride::None, mode, true, true);
            QCOMPARE(choice.backend, Backend::Plasma);
            QVERIFY(!choice.reason.isEmpty());
            QVERIFY(choice.warning.isEmpty());
        }
        // Protocols missing -> portal fallback; warn only when the mode needed Plasma.
        for (const auto &mode : {u"workspace"_s, u"specific"_s, QString()}) {
            const auto choice = choose(BackendOverride::None, mode, true, false);
            QCOMPARE(choice.backend, Backend::Portal);
            QVERIFY(choice.warning.isEmpty());
        }
        for (const auto &mode : {u"multi"_s, u"virtual"_s}) {
            const auto choice = choose(BackendOverride::None, mode, true, false);
            QCOMPARE(choice.backend, Backend::Portal);
            QVERIFY(!choice.warning.isEmpty());
        }
        // Without Plasma support there is nothing to choose.
        QCOMPARE(choose(BackendOverride::None, u"multi"_s, false, true).backend, Backend::Portal);

        // --plasma wins, even when the probe saw nothing (with a warning).
        QCOMPARE(choose(BackendOverride::Plasma, u"workspace"_s, true, true).backend, Backend::Plasma);
        QVERIFY(choose(BackendOverride::Plasma, u"workspace"_s, true, true).warning.isEmpty());
        QCOMPARE(choose(BackendOverride::Plasma, u"workspace"_s, true, false).backend, Backend::Plasma);
        QVERIFY(!choose(BackendOverride::Plasma, u"workspace"_s, true, false).warning.isEmpty());
        QCOMPARE(choose(BackendOverride::Plasma, u"workspace"_s, false, false).backend, Backend::Portal);

        // --portal wins over an available Plasma session.
        for (const auto &mode : allModes) {
            QCOMPARE(choose(BackendOverride::Portal, mode, true, true).backend, Backend::Portal);
        }
        QVERIFY(!choose(BackendOverride::Portal, u"virtual"_s, true, true).warning.isEmpty());
        QVERIFY(choose(BackendOverride::Portal, u"workspace"_s, true, true).warning.isEmpty());
    }

    void plasmaProtocolsDetected()
    {
        const QStringList kwin{u"wl_compositor"_s, u"zkde_screencast_unstable_v1"_s, u"org_kde_kwin_fake_input"_s, u"wl_seat"_s};
        QVERIFY(plasmaProtocolsAvailable(kwin));
        // KWin withholds the restricted globals from an unlisted client.
        QVERIFY(!plasmaProtocolsAvailable({u"wl_compositor"_s, u"wl_seat"_s}));
        QVERIFY(!plasmaProtocolsAvailable({u"zkde_screencast_unstable_v1"_s}));
        QVERIFY(!plasmaProtocolsAvailable({u"org_kde_kwin_fake_input"_s}));
        QVERIFY(!plasmaProtocolsAvailable({}));
    }
};

QTEST_GUILESS_MAIN(ServerSettingsPolicyTest)
#include "ServerSettingsPolicyTest.moc"

