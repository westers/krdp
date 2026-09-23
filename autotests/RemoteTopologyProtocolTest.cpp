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

    void strictPreviewWithoutClientOwner()
    {
        QJsonObject add{{QStringLiteral("op"), QStringLiteral("add")},
            {QStringLiteral("output"), QStringLiteral("new:second")},
            {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), -1280}, {QStringLiteral("y"), 100}}},
            {QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), 1600}, {QStringLiteral("height"), 900}}},
            {QStringLiteral("scale"), 1.25}};
        QJsonObject record{{QStringLiteral("type"), QStringLiteral("topology-preview")}, {QStringLiteral("v"), 1},
            {QStringLiteral("id"), QStringLiteral("p_1")}, {QStringLiteral("generation"), QStringLiteral("generation-1")},
            {QStringLiteral("expectedRevision"), 3}, {QStringLiteral("allowRemoval"), false},
            {QStringLiteral("allowPhysicalChange"), false}, {QStringLiteral("operations"), QJsonArray{add}}};
        auto parsed = previewRequest(record);
        QVERIFY(parsed);
        QCOMPARE(parsed->id, QStringLiteral("p_1"));
        QVERIFY(parsed->draft.owner.isEmpty());
        QCOMPARE(parsed->draft.operations.first().kind, KRdp::RemoteTopologyDraft::Operation::Kind::AddVirtual);
        QCOMPARE(parsed->draft.operations.first().position, QPoint(-1280, 100));
        QCOMPARE(parsed->draft.operations.first().pixels, QSize(1600, 900));
        QCOMPARE(parsed->draft.operations.first().scale, 1.25);
        add.insert(QStringLiteral("owner"), QStringLiteral("forged"));
        record.insert(QStringLiteral("operations"), QJsonArray{add});
        QVERIFY(!previewRequest(record));
        add.remove(QStringLiteral("owner"));
        record.insert(QStringLiteral("operations"), QJsonArray{add});
        record.insert(QStringLiteral("owner"), QStringLiteral("forged"));
        QVERIFY(!previewRequest(record));
        record.remove(QStringLiteral("owner"));
        record.insert(QStringLiteral("expectedRevision"), 9007199254740992.0);
        QVERIFY(!previewRequest(record));
        record.insert(QStringLiteral("expectedRevision"), 3);
        add.insert(QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), 1.5}, {QStringLiteral("y"), 100}});
        record.insert(QStringLiteral("operations"), QJsonArray{add});
        QVERIFY(!previewRequest(record));
        add.insert(QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), -1280}, {QStringLiteral("y"), 100}});
        add.insert(QStringLiteral("output"), QStringLiteral("new:../bad"));
        record.insert(QStringLiteral("operations"), QJsonArray{add});
        QVERIFY(!previewRequest(record));
        add.insert(QStringLiteral("output"), QStringLiteral("new:second"));
        record.insert(QStringLiteral("operations"), QJsonArray{add});
        QVERIFY(previewRequest(record));
    }
};

QTEST_GUILESS_MAIN(RemoteTopologyProtocolTest)
#include "RemoteTopologyProtocolTest.moc"
