// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "RemoteTopologyProtocol.h"

using namespace KRdp::RemoteTopologyProtocol;

class RemoteTopologyProtocolTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void strictQuery()
    {
        QJsonObject record{{QStringLiteral("type"), QStringLiteral("topology-query")},
            {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("q_123")}};
        QCOMPARE(queryId(record), std::optional(QStringLiteral("q_123")));
        record.insert(QStringLiteral("extra"), true);
        QVERIFY(!queryId(record));
        record.remove(QStringLiteral("extra"));
        record.insert(QStringLiteral("id"), QStringLiteral("../bad"));
        QVERIFY(!queryId(record));
        record.insert(QStringLiteral("id"), QString(65, QLatin1Char('a')));
        QVERIFY(!queryId(record));
        record.insert(QStringLiteral("id"), QStringLiteral("q123"));
        record.insert(QStringLiteral("v"), 2);
        QVERIFY(!queryId(record));
    }

    void readOnlyCapabilitiesAndGeometry()
    {
        KRdp::RemoteTopologyCatalog catalog;
        const auto observed = catalog.observe({{
            .backendKey = QStringLiteral("Virtual-A"), .name = QStringLiteral("Virtual-A"),
            .nativePixels = QSize(1600, 900), .logicalGeometry = QRect(-1280, 100, 1280, 720),
            .scale = 1.25, .enabled = true, .primary = true, .physical = false,
            .owner = QStringLiteral("desktop-1"),
        }});
        QVERIFY(observed);
        const auto record = retainedReadOnly(QStringLiteral("q1"), *observed);
        QCOMPARE(record.value(QStringLiteral("generation")).toString(), observed->generation);
        QCOMPARE(record.value(QStringLiteral("revision")).toDouble(), 1.0);
        const auto output = record.value(QStringLiteral("outputs")).toArray().first().toObject();
        QCOMPARE(output.value(QStringLiteral("id")).toString(), observed->outputs.first().id);
        QCOMPARE(output.value(QStringLiteral("logical")).toObject().value(QStringLiteral("x")).toInt(), -1280);
        QCOMPARE(output.value(QStringLiteral("pixels")).toObject().value(QStringLiteral("width")).toInt(), 1600);
        const auto caps = record.value(QStringLiteral("capabilities")).toObject();
        QVERIFY(caps.value(QStringLiteral("enumerate")).toBool());
        QVERIFY(!caps.value(QStringLiteral("add")).toBool());
        QVERIFY(!caps.value(QStringLiteral("multiOutputCapture")).toBool());
    }
};

QTEST_GUILESS_MAIN(RemoteTopologyProtocolTest)
#include "RemoteTopologyProtocolTest.moc"
