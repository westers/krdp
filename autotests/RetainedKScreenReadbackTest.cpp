// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>
#include <QFile>

#include "RetainedKScreenReadback.h"
#include "ConsoleTopologyReadback.h"
#include "RetainedMultiResizePlan.h"
#include "RetainedMultiPositionPlan.h"
#include "RetainedMultiFitPlan.h"
#include "RetainedMultiPrimaryPlan.h"

using namespace KRdp::RetainedKScreenReadback;

namespace
{
QJsonObject output(const QString &name, int id, int x, int y, double scale, int priority)
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("id"), id},
        {QStringLiteral("connected"), true}, {QStringLiteral("enabled"), true},
        {QStringLiteral("rotation"), 1}, {QStringLiteral("replicationSource"), 0},
        {QStringLiteral("currentModeId"), QString::number(id)},
        {QStringLiteral("priority"), priority}, {QStringLiteral("scale"), scale},
        {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), x}, {QStringLiteral("y"), y}}},
        {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1280}, {QStringLiteral("height"), 720}}},
        {QStringLiteral("modes"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QString::number(id)},
            {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1280}, {QStringLiteral("height"), 720}}},
        }}}};
}

QJsonObject root()
{
    return {{QStringLiteral("screen"), QJsonObject{{QStringLiteral("maxActiveOutputsCount"), 2}}},
        {QStringLiteral("outputs"), QJsonArray{output(QStringLiteral("Virtual-0"), 1, 0, 0, 1.25, 1),
            output(QStringLiteral("Virtual-1"), 2, 1024, 100, 1.0, 2)}}};
}

std::optional<Snapshot> decoded(const QJsonObject &object)
{
    return parse(QJsonDocument(object).toJson(), QStringLiteral("lease-1"));
}

QByteArray keyframe()
{
    QFile file(QFINDTESTDATA("data/virtual-fit/1280x720.h264"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
}

class RetainedKScreenReadbackTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void multiPositionPreflightsPeersAndExactReadback()
    {
        const auto before = decoded(root());
        QVERIFY(before);
        const QVector<Placement> move{{QStringLiteral("Virtual-1"), QPoint(1024, 120)}};
        const auto plan = KRdp::RetainedMultiPositionPlan::make(*before, QStringLiteral("lease-1"), move);
        QVERIFY(plan);
        QVERIFY(plan->changed);
        QCOMPARE(plan->after[0], before->outputs[0]);
        QCOMPARE(plan->after[1].logicalGeometry, QRect(1024, 120, 1280, 720));
        auto landed = *before;
        landed.outputs[1].logicalGeometry.moveTo(1024, 120);
        QVERIFY(KRdp::RetainedMultiPositionPlan::matches(*plan, landed));
        landed.outputs[0].logicalGeometry.moveTo(0, 1);
        QVERIFY(!KRdp::RetainedMultiPositionPlan::matches(*plan, landed));
        QVERIFY(!KRdp::RetainedMultiPositionPlan::make(*before, QStringLiteral("other-owner"), move));
        QVERIFY(!KRdp::RetainedMultiPositionPlan::make(*before, QStringLiteral("lease-1"),
            {{QStringLiteral("Virtual-1"), QPoint(1000, 120)}})); // Overlap.
        QVERIFY(!KRdp::RetainedMultiPositionPlan::make(*before, QStringLiteral("lease-1"),
            {{QStringLiteral("Virtual-1"), QPoint(1024, 120)},
             {QStringLiteral("Virtual-1"), QPoint(1024, 130)}})); // Duplicate.
        auto physical = *before;
        physical.outputs[1].physical = true;
        QVERIFY(!KRdp::RetainedMultiPositionPlan::make(physical, QStringLiteral("lease-1"), move));
        // A future Fit can move both outputs in one backend command. Validate
        // the final arrangement, not an overlapping intermediate order.
        const auto swap = KRdp::RetainedMultiPositionPlan::make(*before, QStringLiteral("lease-1"),
            {{QStringLiteral("Virtual-0"), QPoint(1280, 0)},
             {QStringLiteral("Virtual-1"), QPoint(0, 0)}});
        QVERIFY(swap);
        QVERIFY(swap->changed);
        QCOMPARE(swap->after[0].logicalGeometry.topLeft(), QPoint(1280, 0));
        QCOMPARE(swap->after[1].logicalGeometry.topLeft(), QPoint(0, 0));
        const auto noOp = KRdp::RetainedMultiPositionPlan::make(*before, QStringLiteral("lease-1"),
            {{QStringLiteral("Virtual-1"), QPoint(1024, 100)}});
        QVERIFY(noOp);
        QVERIFY(!noOp->changed);
    }

    void multiOutputResizePreflightAndExactAfterState()
    {
        const auto before = decoded(root());
        QVERIFY(before);
        const auto plan = KRdp::RetainedMultiResizePlan::make(*before, QStringLiteral("lease-1"),
            QStringLiteral("Virtual-1"), QSize(1600, 900), 1.25);
        QVERIFY(plan);
        QVERIFY(plan->changed);
        QCOMPARE(plan->after[0], before->outputs[0]);
        QCOMPARE(plan->after[1].logicalGeometry, QRect(1024, 100, 1280, 720));
        auto landed = *before;
        landed.outputs[1].nativePixels = QSize(1600, 900);
        landed.outputs[1].scale = 1.25;
        QVERIFY(KRdp::RetainedMultiResizePlan::matches(*plan, landed));
        landed.outputs[0].logicalGeometry.moveTo(0, 1);
        QVERIFY(!KRdp::RetainedMultiResizePlan::matches(*plan, landed));
        QVERIFY(!KRdp::RetainedMultiResizePlan::make(*before, QStringLiteral("lease-1"),
            QStringLiteral("Virtual-0"), QSize(1600, 900), 1.25)); // Expands into Virtual-1.
        QVERIFY(!KRdp::RetainedMultiResizePlan::make(*before, QStringLiteral("other-owner"),
            QStringLiteral("Virtual-1"), QSize(1600, 900), 1.25));
        QVERIFY(!KRdp::RetainedMultiResizePlan::make(*before, QStringLiteral("lease-1"),
            QStringLiteral("Virtual-1"), QSize(1601, 900), 1.25));
        auto physical = *before;
        physical.outputs[1].physical = true;
        QVERIFY(!KRdp::RetainedMultiResizePlan::make(physical, QStringLiteral("lease-1"),
            QStringLiteral("Virtual-1"), QSize(1600, 900), 1.25));
    }

    void managedFitPreflightsResizeAndDependentMoveTogether()
    {
        namespace Fit = KRdp::RemoteTopologyFit;
        namespace WorkerFit = KRdp::RetainedMultiFitPlan;
        const auto before = decoded(root());
        QVERIFY(before);
        const QVector<Fit::Relation> relations{{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"),
            Fit::Relation::Edge::Right, 100}};
        const auto plan = WorkerFit::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.25, relations);
        QVERIFY(plan);
        QVERIFY(plan->changed);
        QVERIFY(plan->resizeChanged);
        QCOMPARE(plan->placements.size(), 1);
        QCOMPARE(plan->placements[0].outputName, QStringLiteral("Virtual-1"));
        QCOMPARE(plan->placements[0].logicalPosition, QPoint(1280, 100));
        QCOMPARE(plan->after[0].nativePixels, QSize(1600, 900));
        QCOMPARE(plan->after[0].logicalGeometry, QRect(0, 0, 1280, 720));
        QCOMPARE(plan->after[1].logicalGeometry, QRect(1280, 100, 1280, 720));
        KRdp::VirtualResize::Snapshot target;
        target.name = QStringLiteral("Virtual-0");
        target.position = QPoint(0, 0);
        target.scale = 1.25;
        target.current = {QStringLiteral("current"), QSize(1280, 720), 60000};
        target.modes = {target.current, {QStringLiteral("fit-mode"), QSize(1600, 900), 60000}};
        const auto command = WorkerFit::arguments(*plan, target, target.modes.last());
        QVERIFY(command);
        QCOMPARE(*command, (QStringList{QStringLiteral("output.Virtual-0.mode.fit-mode"),
            QStringLiteral("output.Virtual-0.scale.1.25"),
            QStringLiteral("output.Virtual-1.position.1280,100")}));
        target.position.setX(1);
        QVERIFY(!WorkerFit::arguments(*plan, target, target.modes.last()));
        target.position.setX(0);
        QVERIFY(!WorkerFit::arguments(*plan, target, target.current));
        auto landed = *before;
        landed.outputs = plan->after;
        QVERIFY(WorkerFit::matches(*plan, landed));
        landed.outputs[1].logicalGeometry.moveLeft(1281);
        QVERIFY(!WorkerFit::matches(*plan, landed));
        const auto noOp = WorkerFit::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1280, 720), 1.25, relations);
        QVERIFY(noOp);
        QVERIFY(!noOp->changed);
        QVERIFY(!noOp->resizeChanged);
        QVERIFY(noOp->placements.isEmpty());
        const auto scaleOnly = WorkerFit::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.5625, relations);
        QVERIFY(scaleOnly);
        QVERIFY(scaleOnly->changed);
        QVERIFY(scaleOnly->resizeChanged);
        QVERIFY(scaleOnly->placements.isEmpty());
        QCOMPARE(scaleOnly->after[0].logicalGeometry, before->outputs[0].logicalGeometry);
        QCOMPARE(scaleOnly->after[1], before->outputs[1]);
        QVERIFY(!WorkerFit::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.25, {})); // Unmanaged neighbor would be overlapped, never repacked.
        QVERIFY(!WorkerFit::make(*before, QStringLiteral("other-owner"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.25, relations));
        QVERIFY(!WorkerFit::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.25, {{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"),
                Fit::Relation::Edge::Right, 99}})); // Stale edge.
        auto physical = *before;
        physical.outputs[1].physical = true;
        QVERIFY(!WorkerFit::make(physical, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.25, relations));
    }

    void managedFitRecoveryRefusesUnrelatedChanges()
    {
        namespace Fit = KRdp::RemoteTopologyFit;
        namespace WorkerFit = KRdp::RetainedMultiFitPlan;
        const auto before = decoded(root());
        QVERIFY(before);
        const auto plan = WorkerFit::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"),
            QSize(1600, 900), 1.25, {{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"),
                Fit::Relation::Edge::Right, 100}});
        QVERIFY(plan);
        KRdp::VirtualResize::Snapshot original;
        original.name = QStringLiteral("Virtual-0");
        original.id = 1;
        original.position = QPoint(0, 0);
        original.scale = 1.25;
        original.current = {QStringLiteral("old"), QSize(1280, 720), 60000};
        const KRdp::VirtualResize::Mode applied{QStringLiteral("new"), QSize(1600, 900), 60000};
        original.modes = {original.current, applied};
        auto selected = original;
        auto partial = *before;
        partial.outputs[0] = plan->after[0]; // Mode landed; dependent position did not.
        selected.current = applied;
        const auto restore = WorkerFit::recoveryArguments(*plan, partial, selected, original, applied);
        QVERIFY(restore);
        QCOMPARE(*restore, (QStringList{QStringLiteral("output.Virtual-0.mode.old"),
            QStringLiteral("output.Virtual-0.scale.1.25")}));
        partial.outputs[1] = plan->after[1];
        const auto full = WorkerFit::recoveryArguments(*plan, partial, selected, original, applied);
        QVERIFY(full);
        QCOMPARE(*full, (QStringList{QStringLiteral("output.Virtual-0.mode.old"),
            QStringLiteral("output.Virtual-0.scale.1.25"),
            QStringLiteral("output.Virtual-1.position.1024,100")}));
        partial.outputs[1].logicalGeometry.moveLeft(1279);
        QVERIFY(!WorkerFit::recoveryArguments(*plan, partial, selected, original, applied));
        partial.outputs[1] = plan->after[1];
        partial.outputs[1].primary = true;
        QVERIFY(!WorkerFit::recoveryArguments(*plan, partial, selected, original, applied));
        partial = *before;
        selected = original;
        const auto unchanged = WorkerFit::recoveryArguments(*plan, partial, selected, original, applied);
        QVERIFY(unchanged);
        QVERIFY(unchanged->isEmpty());
        selected.current = {QStringLiteral("third-party"), QSize(1280, 720), 60000};
        QVERIFY(!WorkerFit::recoveryArguments(*plan, partial, selected, original, applied));
    }

    void managedPrimaryPreservesPeersAndRestoresOnlyOwnPriorityChange()
    {
        namespace Primary = KRdp::RetainedMultiPrimaryPlan;
        const auto json = QJsonDocument(root()).toJson();
        const auto before = parse(json, QStringLiteral("lease-1"));
        QVERIFY(before);
        const auto original = Primary::priorities(json, *before);
        QVERIFY(original);
        const auto plan = Primary::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-1"), *original);
        QVERIFY(plan);
        QVERIFY(plan->changed);
        QCOMPARE(plan->after[0].primary, false);
        QCOMPARE(plan->after[1].primary, true);
        QCOMPARE(Primary::arguments(plan->requested), (QStringList{
            QStringLiteral("output.Virtual-0.priority.2"), QStringLiteral("output.Virtual-1.priority.1")}));
        auto landed = *before;
        landed.outputs = plan->after;
        QVERIFY(Primary::matches(*plan, landed));
        const auto restore = Primary::recoveryArguments(*plan, landed, plan->requested);
        QVERIFY(restore);
        QCOMPARE(*restore, (QStringList{QStringLiteral("output.Virtual-0.priority.1"),
            QStringLiteral("output.Virtual-1.priority.2")}));
        landed.outputs[0].logicalGeometry.moveTop(1);
        QVERIFY(!Primary::recoveryArguments(*plan, landed, plan->requested));
        landed.outputs[0].logicalGeometry.moveTop(0);
        landed.outputs[1].nativePixels = QSize(1600, 900);
        QVERIFY(!Primary::recoveryArguments(*plan, landed, plan->requested));
        const auto noOp = Primary::make(*before, QStringLiteral("lease-1"), QStringLiteral("Virtual-0"), *original);
        QVERIFY(noOp);
        QVERIFY(!noOp->changed);
        QVERIFY(!Primary::make(*before, QStringLiteral("other-owner"), QStringLiteral("Virtual-1"), *original));
        auto physical = *before;
        physical.outputs[1].physical = true;
        QVERIFY(!Primary::make(physical, QStringLiteral("lease-1"), QStringLiteral("Virtual-1"), *original));
    }

    void physicalReadOnlyInventoryNeedsCapturedKeyframe()
    {
        const QJsonObject physical{{QStringLiteral("screen"), QJsonObject{{QStringLiteral("maxActiveOutputsCount"), 1}}},
            {QStringLiteral("outputs"), QJsonArray{output(QStringLiteral("DP-1"), 1, 0, 0, 1.0, 1)}}};
        const auto state = decoded(physical);
        QVERIFY(state);
        KRdp::ConsoleWorkerWire::Outputs worker{{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1.0, true}}};
        KRdp::VideoFrame frame;
        frame.monitorIndex = 0;
        frame.size = QSize(1280, 720);
        frame.monitors = {{.geometry = QRect(0, 0, 1280, 720), .primary = true}};
        frame.data = keyframe();
        frame.isKeyFrame = true;
        QVERIFY(!frame.data.isEmpty());
        const auto confirmed = KRdp::ConsoleTopologyReadback::confirmed(*state, worker, frame);
        QVERIFY(confirmed);
        QCOMPARE(confirmed->outputs[0].name, QStringLiteral("DP-1"));
        QCOMPARE(confirmed->outputs[0].pixels, QSize(1280, 720));
        frame.data = QByteArrayLiteral("fake-keyframe");
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(*state, worker, frame));
        frame.data = keyframe();
        worker.monitors[0].geometry.moveTo(1, 0);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(*state, worker, frame));
        worker.monitors[0].geometry.moveTo(0, 0);
        auto tooLarge = *state;
        tooLarge.outputs[0].logicalGeometry = QRect(0, 0, 2560, 720);
        worker.monitors[0].geometry = QRect(0, 0, 2560, 720);
        frame.monitors[0].geometry = worker.monitors[0].geometry;
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(tooLarge, worker, frame));
    }

    void privateTwoOutputReadbackAndPositionCommands()
    {
        const auto state = decoded(root());
        QVERIFY(state);
        QCOMPARE(state->maxActiveOutputs, 2);
        QCOMPARE(state->outputs.size(), 2);
        QCOMPARE(state->outputs[0].logicalGeometry, QRect(0, 0, 1024, 576));
        QCOMPARE(state->outputs[1].logicalGeometry, QRect(1024, 100, 1280, 720));
        QVERIFY(state->outputs[0].primary);
        QVERIFY(!state->outputs[1].primary);
        QVERIFY(!state->outputs[1].physical);
        QCOMPARE(state->outputs[1].owner, QStringLiteral("lease-1"));
        const auto args = positionArguments(*state, {{QStringLiteral("Virtual-1"), QPoint(-1280, 0)}});
        QVERIFY(args);
        QCOMPARE(*args, QStringList{QStringLiteral("output.Virtual-1.position.-1280,0")});
        QVERIFY(!positionArguments(*state, {{QStringLiteral("DP-1"), QPoint(0, 0)}}));
        QVERIFY(!positionArguments(*state, {{QStringLiteral("Virtual-1"), QPoint(0, 0)},
            {QStringLiteral("Virtual-1"), QPoint(0, 0)}}));
    }

    void independentReadbackMustAgreeWithBothDecodedCaptures()
    {
        const auto state = decoded(root());
        QVERIFY(state);
        const auto data = keyframe();
        QVERIFY(!data.isEmpty());
        KRdp::ConsoleWorkerWire::Outputs worker{{
            {QStringLiteral("Virtual-0"), QRect(0, 0, 1024, 576), 1.25, true},
            {QStringLiteral("Virtual-1"), QRect(1024, 100, 1280, 720), 1.0, false},
        }};
        KRdp::VideoFrame first;
        first.monitorIndex = 0;
        first.size = QSize(1280, 720);
        first.data = data;
        first.isKeyFrame = true;
        auto second = first;
        second.monitorIndex = 1;
        QVERIFY(matchesPublished(*state, worker, {first, second}));
        worker.monitors[1].geometry.moveTo(1025, 100);
        QVERIFY(!matchesPublished(*state, worker, {first, second}));
        worker.monitors[1].geometry.moveTo(1024, 100);
        second.size = QSize(1600, 900);
        QVERIFY(!matchesPublished(*state, worker, {first, second}));
        second.size = QSize(1280, 720);
        second.data = QByteArrayLiteral("fake-keyframe");
        QVERIFY(!matchesPublished(*state, worker, {first, second}));
    }

    void preservesNegativeCompositorOriginSeparateFromAtlas()
    {
        auto object = root();
        object.insert(QStringLiteral("outputs"), QJsonArray{
            output(QStringLiteral("Virtual-0"), 1, -1024, -100, 1.25, 1),
            output(QStringLiteral("Virtual-1"), 2, 0, 0, 1.0, 2)});
        const auto state = decoded(object);
        QVERIFY(state);
        const auto data = keyframe();
        QVERIFY(!data.isEmpty());
        KRdp::ConsoleWorkerWire::Outputs worker{{
            {QStringLiteral("Virtual-0"), QRect(0, 0, 1024, 576), 1.25, true},
            {QStringLiteral("Virtual-1"), QRect(1024, 100, 1280, 720), 1.0, false}}, QPoint(-1024, -100)};
        KRdp::VideoFrame first;
        first.monitorIndex = 0;
        first.size = QSize(1280, 720);
        first.data = data;
        first.isKeyFrame = true;
        auto second = first;
        second.monitorIndex = 1;
        QVERIFY(matchesPublished(*state, worker, {first, second}));
        worker.compositorOrigin = QPoint(0, 0);
        QVERIFY(!matchesPublished(*state, worker, {first, second}));
    }

    void refusesAmbiguousOrUnsupportedReadback()
    {
        auto object = root();
        auto outputs = object.value(QStringLiteral("outputs")).toArray();
        outputs[1] = output(QStringLiteral("Virtual-0"), 2, 1024, 100, 1.0, 2);
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object)); // Duplicate name cannot be an identity.
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        auto second = outputs[1].toObject();
        second.insert(QStringLiteral("currentModeId"), QStringLiteral("missing"));
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        second = outputs[1].toObject();
        second.insert(QStringLiteral("rotation"), 2);
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        second = outputs[1].toObject();
        second.insert(QStringLiteral("replicationSource"), 1);
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        second = outputs[1].toObject();
        second.insert(QStringLiteral("enabled"), false);
        outputs[1] = second;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
    }

    void refusesMissingPrimaryWrongModeAndCapacity()
    {
        auto object = root();
        auto outputs = object.value(QStringLiteral("outputs")).toArray();
        auto first = outputs[0].toObject();
        first.insert(QStringLiteral("priority"), 2);
        outputs[0] = first;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        outputs = object.value(QStringLiteral("outputs")).toArray();
        first = outputs[0].toObject();
        first.insert(QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 800}, {QStringLiteral("height"), 600}});
        outputs[0] = first;
        object.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!decoded(object));
        object = root();
        object.insert(QStringLiteral("screen"), QJsonObject{{QStringLiteral("maxActiveOutputsCount"), 1}});
        QVERIFY(!decoded(object));
        QVERIFY(!parse(QJsonDocument(root()).toJson(), {}));
    }

    void realPrivateKScreenFixtureIfProvided()
    {
        const QString path = qEnvironmentVariable("KRDP_KSCREEN_FIXTURE");
        if (path.isEmpty()) QSKIP("No isolated KScreen fixture requested");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto state = parse(file.readAll(), QStringLiteral("fixture-owner"));
        QVERIFY(state);
        QCOMPARE(state->outputs.size(), 2);
        QCOMPARE(state->outputs[0].logicalGeometry, QRect(0, 0, 1024, 576));
        QCOMPARE(state->outputs[1].logicalGeometry, QRect(1024, 100, 1280, 720));
    }
};

QTEST_GUILESS_MAIN(RetainedKScreenReadbackTest)

#include "RetainedKScreenReadbackTest.moc"
