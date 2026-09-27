// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-S4: ListenAddress / --address parsing.

#include <QTest>

#include "ListenAddress.h"

class ListenAddressTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parse_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<QString>("expected");
        QTest::newRow("default: all interfaces") << QString() << true << QString();
        QTest::newRow("blank") << QStringLiteral("  ") << true << QString();
        QTest::newRow("star") << QStringLiteral("*") << true << QString();
        QTest::newRow("ipv4") << QStringLiteral("192.168.1.20") << true << QStringLiteral("192.168.1.20");
        QTest::newRow("ipv4 padded") << QStringLiteral(" 10.0.0.2 ") << true << QStringLiteral("10.0.0.2");
        QTest::newRow("loopback") << QStringLiteral("127.0.0.1") << true << QStringLiteral("127.0.0.1");
        QTest::newRow("ipv6") << QStringLiteral("fd00::20") << true << QStringLiteral("fd00::20");
        QTest::newRow("hostname") << QStringLiteral("hal9000.lan") << false << QString();
        QTest::newRow("garbage") << QStringLiteral("300.1.1.1") << false << QString();
    }

    // AUD-INT: the KCM decides "all interfaces" with the same helper, so a
    // hand-written `*` is not shown as a single address to connect to.
    void allInterfaces()
    {
        QVERIFY(KRdp::listensOnAllInterfaces(QString()));
        QVERIFY(KRdp::listensOnAllInterfaces(QStringLiteral("  ")));
        QVERIFY(KRdp::listensOnAllInterfaces(QStringLiteral("*")));
        QVERIFY(KRdp::listensOnAllInterfaces(QStringLiteral(" * ")));
        QVERIFY(!KRdp::listensOnAllInterfaces(QStringLiteral("192.168.1.20")));
        QVERIFY(!KRdp::listensOnAllInterfaces(QStringLiteral("0.0.0.0")));
    }

    void parse()
    {
        QFETCH(QString, value);
        QFETCH(bool, valid);
        QFETCH(QString, expected);
        const auto address = KRdp::parseListenAddress(value);
        QCOMPARE(address.has_value(), valid);
        if (!valid) {
            return;
        }
        if (expected.isEmpty()) {
            QCOMPARE(*address, QHostAddress(QHostAddress::Any));
        } else {
            QCOMPARE(*address, QHostAddress(expected));
        }
    }
};

QTEST_GUILESS_MAIN(ListenAddressTest)
#include "ListenAddressTest.moc"
