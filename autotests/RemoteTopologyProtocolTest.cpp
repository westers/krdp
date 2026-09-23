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

    void physicalConsoleReplyNeverAdvertisesWrites()
    {
        KRdp::RemoteTopologyCatalog catalog;
        const auto observed = catalog.observe({{
            .backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"),
            .nativePixels = QSize(2560, 1440), .logicalGeometry = QRect(0, 0, 2560, 1440),
            .scale = 1.0, .enabled = true, .primary = true, .physical = true, .owner = {},
        }});
        QVERIFY(observed);
        const auto record = consoleReadOnly(QStringLiteral("q-console"), *observed);
        const auto output = record.value(QStringLiteral("outputs")).toArray().first().toObject();
        QCOMPARE(output.value(QStringLiteral("kind")).toString(), QStringLiteral("physical"));
        QCOMPARE(output.value(QStringLiteral("lifetime")).toString(), QStringLiteral("lease"));
        const auto caps = record.value(QStringLiteral("capabilities")).toObject();
        QCOMPARE(caps.value(QStringLiteral("lifetime")).toString(), QStringLiteral("lease"));
        QVERIFY(caps.value(QStringLiteral("enumerate")).toBool());
        for (const auto *name : {"add", "remove", "position", "resize", "scale", "primary", "multiOutputCapture"})
            QVERIFY(!caps.value(QLatin1String(name)).toBool());
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

    void strictManagedFitPreview()
    {
        QJsonObject record{{QStringLiteral("type"), QStringLiteral("topology-fit-preview")},
            {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("fit-1")},
            {QStringLiteral("generation"), QStringLiteral("generation-1")},
            {QStringLiteral("expectedRevision"), 2}, {QStringLiteral("output"), QStringLiteral("o-1")},
            {QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), 1600}, {QStringLiteral("height"), 900}}},
            {QStringLiteral("scale"), 1.25}, {QStringLiteral("relations"), QJsonArray{
                QJsonObject{{QStringLiteral("parent"), QStringLiteral("o-1")},
                    {QStringLiteral("child"), QStringLiteral("o-2")},
                    {QStringLiteral("edge"), QStringLiteral("right")}, {QStringLiteral("offset"), 100}}}}};
        const auto parsed = fitPreviewRequest(record);
        QVERIFY(parsed);
        QCOMPARE(parsed->pixels, QSize(1600, 900));
        QCOMPARE(parsed->relations.size(), 1);
        QCOMPARE(parsed->relations[0].offset, 100);
        record.insert(QStringLiteral("owner"), QStringLiteral("forged"));
        QVERIFY(!fitPreviewRequest(record));
        record.remove(QStringLiteral("owner"));
        record.insert(QStringLiteral("expectedRevision"), 2.5);
        QVERIFY(!fitPreviewRequest(record));
        record.insert(QStringLiteral("expectedRevision"), 2);
        record.insert(QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), 1601}, {QStringLiteral("height"), 900}});
        QVERIFY(!fitPreviewRequest(record));
        record.insert(QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), 1600}, {QStringLiteral("height"), 900}});
        record.insert(QStringLiteral("relations"), QJsonArray{
            QJsonObject{{QStringLiteral("parent"), QStringLiteral("o-1")},
                {QStringLiteral("child"), QStringLiteral("o-2")},
                {QStringLiteral("edge"), QStringLiteral("right")}, {QStringLiteral("offset"), 100}},
            QJsonObject{{QStringLiteral("parent"), QStringLiteral("o-3")},
                {QStringLiteral("child"), QStringLiteral("o-2")},
                {QStringLiteral("edge"), QStringLiteral("below")}, {QStringLiteral("offset"), 0}}});
        QVERIFY(!fitPreviewRequest(record));
    }

    void strictCommitAndCorrelatedPreviewReply()
    {
        QJsonObject commit{{QStringLiteral("type"), QStringLiteral("topology-commit")}, {QStringLiteral("v"), 1},
            {QStringLiteral("id"), QStringLiteral("p_1")}, {QStringLiteral("token"), QStringLiteral("token-1")},
            {QStringLiteral("generation"), QStringLiteral("generation-1")}, {QStringLiteral("expectedRevision"), 3}};
        const auto parsed = commitRequest(commit);
        QVERIFY(parsed);
        QCOMPARE(parsed->expectedRevision, quint64(3));
        commit.insert(QStringLiteral("owner"), QStringLiteral("forged"));
        QVERIFY(!commitRequest(commit));
        commit.remove(QStringLiteral("owner"));
        commit.insert(QStringLiteral("expectedRevision"), 3.5);
        QVERIFY(!commitRequest(commit));
        commit.insert(QStringLiteral("expectedRevision"), 3);
        commit.insert(QStringLiteral("token"), QStringLiteral("../bad"));
        QVERIFY(!commitRequest(commit));

        KRdp::RemoteTopologyCatalog catalog;
        const auto snapshot = catalog.observe({{.backendKey = QStringLiteral("Virtual-0"), .name = QStringLiteral("Virtual-0"),
            .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720), .scale = 1,
            .enabled = true, .primary = true, .physical = false, .owner = QStringLiteral("lease-1")}});
        QVERIFY(snapshot);
        KRdp::RemoteTopologyDraft::Preview proposal{snapshot->outputs, snapshot->outputs, {}};
        proposal.after.first().output.logicalGeometry.moveTo(-1280, 100);
        const auto reply = previewReply(QStringLiteral("p_1"), QStringLiteral("token-1"), proposal, *snapshot);
        QCOMPARE(reply.value(QStringLiteral("id")).toString(), QStringLiteral("p_1"));
        QCOMPARE(reply.value(QStringLiteral("before")).toArray().first().toObject().value(QStringLiteral("logical")).toObject()
            .value(QStringLiteral("x")).toInt(), 0);
        QCOMPARE(reply.value(QStringLiteral("after")).toArray().first().toObject().value(QStringLiteral("logical")).toObject()
            .value(QStringLiteral("x")).toInt(), -1280);
    }
};

QTEST_GUILESS_MAIN(RemoteTopologyProtocolTest)
#include "RemoteTopologyProtocolTest.moc"
