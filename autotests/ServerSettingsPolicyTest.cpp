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
        // --plasma always wins.
        QCOMPARE(chooseBackend(true, u"workspace"_s, true).backend, Backend::Plasma);
        QVERIFY(chooseBackend(true, u"workspace"_s, true).automaticReason.isEmpty());
        // The stock unit (no flag) gets Plasma when the mode needs it...
        for (const auto &mode : {u"multi"_s, u"virtual"_s, u" Virtual "_s}) {
            const auto choice = chooseBackend(false, mode, true);
            QCOMPARE(choice.backend, Backend::Plasma);
            QVERIFY(!choice.automaticReason.isEmpty());
        }
        // ...and the portal otherwise.
        for (const auto &mode : {u"workspace"_s, u"primary"_s, u"specific"_s, QString()}) {
            QCOMPARE(chooseBackend(false, mode, true).backend, Backend::Portal);
        }
        // Without Plasma support there is nothing to choose.
        QCOMPARE(chooseBackend(false, u"multi"_s, false).backend, Backend::Portal);
    }
};

QTEST_GUILESS_MAIN(ServerSettingsPolicyTest)
#include "ServerSettingsPolicyTest.moc"

