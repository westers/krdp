// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "RemoteTopologyFit.h"

using namespace KRdp::RemoteTopologyFit;
using namespace KRdp::RemoteTopologyDraft;

namespace
{
Snapshot baseline()
{
    KRdp::RemoteTopologyCatalog catalog;
    return *catalog.observe({
        {.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"),
            .nativePixels = {1600, 900}, .logicalGeometry = {0, 0, 1280, 720},
            .scale = 1.25, .enabled = true, .primary = true, .physical = true, .owner = {}},
        {.backendKey = QStringLiteral("Virtual-A"), .name = QStringLiteral("Virtual-A"),
            .nativePixels = {800, 600}, .logicalGeometry = {1280, 0, 800, 600},
            .scale = 1.0, .enabled = true, .primary = false, .physical = false,
            .owner = QStringLiteral("lease-1")},
        {.backendKey = QStringLiteral("Virtual-B"), .name = QStringLiteral("Virtual-B"),
            .nativePixels = {800, 600}, .logicalGeometry = {2080, 0, 800, 600},
            .scale = 1.0, .enabled = true, .primary = false, .physical = false,
            .owner = QStringLiteral("lease-1")},
        {.backendKey = QStringLiteral("Virtual-C"), .name = QStringLiteral("Virtual-C"),
            .nativePixels = {800, 400}, .logicalGeometry = {1280, 600, 800, 400},
            .scale = 1.0, .enabled = true, .primary = false, .physical = false,
            .owner = QStringLiteral("lease-1")},
    });
}

Capabilities caps()
{
    return {.addVirtual = true, .removeVirtual = true, .moveVirtual = true,
        .resizeVirtual = true, .changePrimary = true, .changePhysical = false,
        .maxOutputs = 8, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
}

Request request(const Snapshot &snapshot)
{
    return {.generation = snapshot.generation, .expectedRevision = snapshot.revision,
        .owner = QStringLiteral("lease-1"), .operations = {}};
}

QVector<Relation> managed(const Snapshot &snapshot)
{
    const auto &out = snapshot.outputs;
    return {{out[0].id, out[1].id, Relation::Edge::Right, 0},
        {out[1].id, out[2].id, Relation::Edge::Right, 0},
        {out[1].id, out[3].id, Relation::Edge::Below, 0}};
}

QRect output(const Plan &result, const QString &id)
{
    for (const auto &entry : result.proposal.after) if (entry.id == id) return entry.output.logicalGeometry;
    return {};
}
}

class RemoteTopologyFitTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void branchReflowsWithoutMovingAnchor()
    {
        const auto source = baseline();
        const auto result = plan(source, caps(), request(source), source.outputs[1].id,
            QSize(1000, 700), 1.0, managed(source));
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(result.request.operations.size(), 3); // Resize A, move B and C.
        QCOMPARE(output(result, source.outputs[0].id), QRect(0, 0, 1280, 720));
        QCOMPARE(output(result, source.outputs[1].id), QRect(1280, 0, 1000, 700));
        QCOMPARE(output(result, source.outputs[2].id), QRect(2280, 0, 800, 600));
        QCOMPARE(output(result, source.outputs[3].id), QRect(1280, 700, 800, 400));
    }

    void physicalFitNeedsConsentAndReflowsChain()
    {
        const auto source = baseline();
        auto capabilities = caps();
        capabilities.changePhysical = true;
        auto draft = request(source);
        QCOMPARE(plan(source, capabilities, draft, source.outputs[0].id,
            QSize(1280, 900), 1.25, managed(source)).error, QStringLiteral("physical-change-requires-confirmation"));
        draft.allowPhysicalChange = true;
        const auto result = plan(source, capabilities, draft, source.outputs[0].id,
            QSize(1280, 900), 1.25, managed(source));
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(output(result, source.outputs[0].id), QRect(0, 0, 1024, 720));
        QCOMPARE(output(result, source.outputs[1].id), QRect(1024, 0, 800, 600));
        QCOMPARE(output(result, source.outputs[2].id), QRect(1824, 0, 800, 600));
        QCOMPARE(output(result, source.outputs[3].id), QRect(1024, 600, 800, 400));
    }

    void noOpAndCollisionAreExplicit()
    {
        const auto source = baseline();
        const auto unchanged = plan(source, caps(), request(source), source.outputs[1].id,
            QSize(800, 600), 1.0, managed(source));
        QVERIFY2(unchanged.valid(), qPrintable(unchanged.error));
        QVERIFY(unchanged.request.operations.isEmpty());
        QCOMPARE(unchanged.proposal.after, source.outputs);
        // Shrinking the branch parent pulls its right and below children into
        // one another. Preview rejects the whole draft rather than repacking.
        QCOMPARE(plan(source, caps(), request(source), source.outputs[1].id,
            QSize(600, 500), 1.0, managed(source)).error, QStringLiteral("overlap"));
    }

    void scaleOnlyKeepsAnchorAndNegativeOrigin()
    {
        auto source = baseline();
        source.outputs.removeLast();
        source.outputs.removeLast();
        source.outputs[1].output.logicalGeometry.moveTo(-800, 100);
        const QVector<Relation> left{{source.outputs[0].id, source.outputs[1].id, Relation::Edge::Left, 100}};
        QCOMPARE(plan(source, caps(), request(source), source.outputs[1].id,
            QSize(800, 600), 1.25, left).error, QStringLiteral("relation-conflict"));
        // A deliberately free-positioned monitor has no managed relation;
        // changing its scale keeps the negative logical origin unchanged.
        const auto result = plan(source, caps(), request(source), source.outputs[1].id,
            QSize(800, 600), 1.25, {});
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(result.request.operations.size(), 1);
        QCOMPARE(output(result, source.outputs[1].id), QRect(-800, 100, 640, 480));
        QCOMPARE(output(result, source.outputs[0].id), QRect(0, 0, 1280, 720));
    }

    void offsetRowIsRetainedAcrossPhysicalFit()
    {
        auto source = baseline();
        source.outputs[0].output.logicalGeometry.moveTo(0, 100);
        source.outputs[1].output.logicalGeometry.moveTo(1280, 150);
        source.outputs[2].output.logicalGeometry.moveTo(2080, 150);
        source.outputs[3].output.logicalGeometry.moveTo(1280, 750);
        auto relations = managed(source);
        relations[0].offset = 50;
        auto capabilities = caps();
        capabilities.changePhysical = true;
        auto draft = request(source);
        draft.allowPhysicalChange = true;
        const auto result = plan(source, capabilities, draft, source.outputs[0].id,
            QSize(1280, 900), 1.25, relations);
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(output(result, source.outputs[1].id), QRect(1024, 150, 800, 600));
        QCOMPARE(output(result, source.outputs[2].id), QRect(1824, 150, 800, 600));
        QCOMPARE(output(result, source.outputs[3].id), QRect(1024, 750, 800, 400));
    }

    void cyclesStaleRelationsAndOtherOwnersRefused()
    {
        auto source = baseline();
        auto relations = managed(source);
        relations.removeFirst();
        relations.append({source.outputs[2].id, source.outputs[1].id, Relation::Edge::Right, 0});
        QCOMPARE(plan(source, caps(), request(source), source.outputs[1].id,
            QSize(1000, 700), 1.0, relations).error, QStringLiteral("cycle"));
        relations = managed(source);
        source.outputs[2].output.logicalGeometry.moveTo(2081, 0);
        QCOMPARE(plan(source, caps(), request(source), source.outputs[1].id,
            QSize(1000, 700), 1.0, relations).error, QStringLiteral("stale-relation"));
        source.outputs[2].output.logicalGeometry.moveTo(2080, 0);
        source.outputs[2].output.owner = QStringLiteral("someone-else");
        QCOMPARE(plan(source, caps(), request(source), source.outputs[1].id,
            QSize(1000, 700), 1.0, relations).error, QStringLiteral("not-owner"));
    }

    void pinnedOutputsAndRevisionArePreserved()
    {
        auto source = baseline();
        const auto relationships = managed(source);
        auto draft = request(source);
        draft.expectedRevision++;
        QCOMPARE(plan(source, caps(), draft, source.outputs[1].id,
            QSize(1000, 700), 1.0, relationships).error, QStringLiteral("stale-revision"));
        draft = request(source);
        source.outputs[2].output.physical = true;
        QCOMPARE(plan(source, caps(), draft, source.outputs[1].id,
            QSize(1000, 700), 1.0, relationships).error, QStringLiteral("not-owner"));
    }
};

QTEST_GUILESS_MAIN(RemoteTopologyFitTest)

#include "RemoteTopologyFitTest.moc"
