// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "RemoteTopologyDraft.h"

using namespace KRdp::RemoteTopologyDraft;

namespace
{
Snapshot baseline()
{
    KRdp::RemoteTopologyCatalog catalog;
    const auto snapshot = catalog.observe({{
        .backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"),
        .nativePixels = {1600, 900}, .logicalGeometry = QRect(0, 0, 1280, 720),
        .scale = 1.25, .enabled = true, .primary = true, .physical = true, .owner = {},
    }});
    return *snapshot;
}

Capabilities capabilities()
{
    return {.addVirtual = true, .removeVirtual = true, .moveVirtual = true,
        .resizeVirtual = true, .changePrimary = true, .changePhysical = false,
        .maxOutputs = 4, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
}

Request requestFor(const Snapshot &snapshot)
{
    return {.generation = snapshot.generation, .expectedRevision = snapshot.revision,
        .owner = QStringLiteral("lease-1"), .operations = {}};
}
}

class RemoteTopologyDraftTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void appendAtLogicalEdgeAndMoveAroundPhysical()
    {
        const auto source = baseline();
        auto request = requestFor(source);
        request.operations.append({Operation::Kind::AddVirtual, QStringLiteral("new:west"),
            QPoint(1280, 0), QSize(1920, 1080), 1.0});
        const auto appended = preview(source, capabilities(), request);
        QVERIFY2(appended.valid(), qPrintable(appended.error));
        QCOMPARE(appended.after.size(), 2);
        QCOMPARE(appended.after[0], source.outputs[0]); // Physical unchanged.
        QCOMPARE(appended.after[1].output.logicalGeometry, QRect(1280, 0, 1920, 1080));
        request.operations.append({Operation::Kind::Move, QStringLiteral("new:west"),
            QPoint(-1920, 0), {}, 1.0});
        const auto moved = preview(source, capabilities(), request);
        QVERIFY2(moved.valid(), qPrintable(moved.error));
        QCOMPARE(moved.after[1].output.logicalGeometry, QRect(-1920, 0, 1920, 1080));
    }

    void staleAndPhysicalChangesAreRejected()
    {
        const auto source = baseline();
        auto request = requestFor(source);
        request.expectedRevision++;
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("stale-revision"));
        request.expectedRevision = source.revision;
        request.generation = QStringLiteral("old-generation");
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("stale-generation"));
        request.generation = source.generation;
        request.operations.append({Operation::Kind::Move, source.outputs.first().id, QPoint(100, 0), {}, 1.0});
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("physical-change-requires-confirmation"));
    }

    void consentAndSurvivingPrimaryAreRequired()
    {
        const auto source = baseline();
        auto request = requestFor(source);
        request.operations.append({Operation::Kind::AddVirtual, QStringLiteral("new:one"), QPoint(1280, 0), QSize(800, 600), 1.0});
        request.operations.append({Operation::Kind::Remove, QStringLiteral("new:one"), {}, {}, 1.0});
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("removal-requires-confirmation"));
        request.allowRemoval = true;
        QVERIFY(preview(source, capabilities(), request).valid());
        request.operations.clear();
        request.operations.append({Operation::Kind::Remove, source.outputs.first().id, {}, {}, 1.0});
        QVERIFY(!preview(source, capabilities(), request).valid()); // No physical removal.
    }

    void cannotEditAnothersVirtualOrRemovePrimary()
    {
        auto source = baseline();
        source.outputs.append({QStringLiteral("o-2"), {
            .backendKey = QStringLiteral("Virtual-owner-two"), .name = QStringLiteral("Virtual-owner-two"),
            .nativePixels = QSize(800, 600), .logicalGeometry = QRect(1280, 0, 800, 600),
            .scale = 1.0, .enabled = true, .primary = false, .physical = false,
            .owner = QStringLiteral("lease-2"),
        }});
        auto request = requestFor(source);
        request.operations.append({Operation::Kind::Move, QStringLiteral("o-2"), QPoint(-800, 0), {}, 1.0});
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("not-owner"));
        request.owner = QStringLiteral("lease-2");
        request.operations = {{Operation::Kind::Remove, QStringLiteral("o-2"), {}, {}, 1.0}};
        request.allowRemoval = true;
        QVERIFY(preview(source, capabilities(), request).valid());
        source.outputs.first().output.primary = false;
        source.outputs.last().output.primary = true;
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("invalid-primary"));
    }

    void overlapIslandAndAtlasLimitAreRejected()
    {
        const auto source = baseline();
        auto request = requestFor(source);
        request.operations.append({Operation::Kind::AddVirtual, QStringLiteral("new:overlap"), QPoint(1279, 0), QSize(800, 600), 1.0});
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("overlap"));
        request.operations[0].position = QPoint(5000, 0);
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("unreachable"));
        request.operations.append({Operation::Kind::AddVirtual, QStringLiteral("new:island-peer"), QPoint(5800, 0), QSize(800, 600), 1.0});
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("unreachable"));
        request.operations.removeLast();
        request.operations[0].position = QPoint(1280, 0);
        auto small = capabilities();
        small.maxAtlasDimension = 3000;
        request.operations[0].pixels = QSize(2000, 600);
        QCOMPARE(preview(source, small, request).error, QStringLiteral("limit"));
        request.operations[0].pixels = QSize(800, 600);
        request.operations[0].scale = 1e-12; // Must reject before ceil-to-int overflow.
        QCOMPARE(preview(source, capabilities(), request).error, QStringLiteral("limit"));
    }
};

QTEST_GUILESS_MAIN(RemoteTopologyDraftTest)
#include "RemoteTopologyDraftTest.moc"
