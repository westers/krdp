// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ConsoleTopologyPlan.h"

namespace Plan = KRdp::ConsoleTopologyPlan;
using KRdp::RemoteTopologyDraft::Operation;

namespace
{
Plan::Snapshot baseline()
{
    KRdp::RemoteTopologyCatalog catalog;
    return *catalog.observe({
        {.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"),
            .nativePixels = QSize(1600, 900), .logicalGeometry = QRect(0, 0, 1280, 720),
            .scale = 1.25, .enabled = true, .primary = true, .physical = true, .owner = {}},
        {.backendKey = QStringLiteral("HDMI-A-1"), .name = QStringLiteral("HDMI-A-1"),
            .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 100, 1280, 720),
            .scale = 1.0, .enabled = true, .primary = false, .physical = true, .owner = {}},
    });
}

QString idFor(const Plan::Snapshot &snapshot, const QString &backend)
{
    for (const auto &entry : snapshot.outputs) if (entry.output.backendKey == backend) return entry.id;
    return {};
}

KRdp::RemoteTopologyDraft::Capabilities limits()
{
    return {.changePrimary = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
}

KRdp::RemoteTopologyDraft::Request request(const Plan::Snapshot &source)
{
    return {.generation = source.generation, .expectedRevision = source.revision,
        .owner = QStringLiteral("authenticated-console-controller"), .operations = {},
        .allowPhysicalChange = true};
}

Plan::Priorities priorities()
{
    return {{QStringLiteral("DP-1"), 1}, {QStringLiteral("HDMI-A-1"), 2}};
}

QJsonObject physicalOutput(const QString &name, int id, QPoint position, QSize currentPixels, double scale,
    int priority, const QJsonArray &modes)
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("id"), id},
        {QStringLiteral("connected"), true}, {QStringLiteral("enabled"), true},
        {QStringLiteral("rotation"), 1}, {QStringLiteral("replicationSource"), 0},
        {QStringLiteral("currentModeId"), QString::number(id)}, {QStringLiteral("priority"), priority},
        {QStringLiteral("scale"), scale},
        {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), position.x()}, {QStringLiteral("y"), position.y()}}},
        {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), currentPixels.width()},
            {QStringLiteral("height"), currentPixels.height()}}},
        {QStringLiteral("modes"), modes}};
}

QJsonObject mode(const QString &id, QSize pixels, double hz)
{
    return {{QStringLiteral("id"), id},
        {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), pixels.width()},
            {QStringLiteral("height"), pixels.height()}}}, {QStringLiteral("refreshRate"), hz}};
}

QJsonObject physicalKScreen()
{
    return {{QStringLiteral("screen"), QJsonObject{{QStringLiteral("maxActiveOutputsCount"), 2}}},
        {QStringLiteral("outputs"), QJsonArray{
            physicalOutput(QStringLiteral("DP-1"), 27, QPoint(0, 0), QSize(1600, 900), 1.25, 1,
                {mode(QStringLiteral("27"), QSize(1600, 900), 60),
                    mode(QStringLiteral("41"), QSize(1920, 1080), 59.94),
                    mode(QStringLiteral("42"), QSize(1920, 1080), 50)}),
            physicalOutput(QStringLiteral("HDMI-A-1"), 12, QPoint(1280, 100), QSize(1280, 720), 1, 2,
                {mode(QStringLiteral("12"), QSize(1280, 720), 60)}),
        }}};
}
}

class ConsoleTopologyPlanTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void explicitMixedPhysicalDraftPreservesExactIdentity()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {
            {Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1.0},
            {Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1.0},
            {Operation::Kind::SetPrimary, idFor(source, QStringLiteral("HDMI-A-1")), {}, {}, 1.0},
        };
        const auto result = Plan::make(source, priorities(), draft, limits());
        QVERIFY(result);
        QVERIFY(result->changed);
        QCOMPARE(result->positions.value(QStringLiteral("HDMI-A-1")), QPoint(1920, 100));
        QCOMPARE(result->modes.value(QStringLiteral("DP-1")).first, QSize(1920, 1080));
        QCOMPARE(result->afterPriorities.value(QStringLiteral("HDMI-A-1")), 1);
        QCOMPARE(result->afterPriorities.value(QStringLiteral("DP-1")), 2);
        QCOMPARE(result->after.size(), source.outputs.size());
        for (qsizetype i = 0; i < source.outputs.size(); ++i) {
            QCOMPARE(result->after[i].id, source.outputs[i].id);
            QCOMPARE(result->after[i].output.backendKey, source.outputs[i].output.backendKey);
        }
        auto observed = source;
        observed.outputs = result->after;
        QVERIFY(Plan::matches(*result, observed, result->afterPriorities));
        observed.outputs[0].output.logicalGeometry.moveLeft(1);
        QVERIFY(!Plan::matches(*result, observed, result->afterPriorities));
    }

    void consentGenerationRevisionAndOverlapFailClosed()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {{Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1.0}};
        QVERIFY(!Plan::make(source, priorities(), draft, limits())); // Would overlap unchanged HDMI-A-1.
        draft.operations = {{Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1.0}};
        draft.allowPhysicalChange = false;
        QVERIFY(!Plan::make(source, priorities(), draft, limits()));
        draft.allowPhysicalChange = true;
        draft.expectedRevision++;
        QVERIFY(!Plan::make(source, priorities(), draft, limits()));
        draft.expectedRevision = source.revision;
        draft.generation = QStringLiteral("old-greeter-generation");
        QVERIFY(!Plan::make(source, priorities(), draft, limits()));
        draft.generation = source.generation;
        QVERIFY(Plan::make(source, priorities(), draft, limits()));
        draft.operations.append(draft.operations.first());
        QVERIFY(!Plan::make(source, priorities(), draft, limits()));
    }

    void unknownEditsAndModeChangesCannotBeRestored()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {
            {Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1.0},
            {Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1.0},
        };
        const auto result = Plan::make(source, priorities(), draft, limits());
        QVERIFY(result);
        auto partial = source;
        for (qsizetype i = 0; i < partial.outputs.size(); ++i) {
            if (partial.outputs[i].output.backendKey == QStringLiteral("HDMI-A-1"))
                partial.outputs[i].output.logicalGeometry.moveTopLeft(QPoint(1920, 100));
        }
        QVERIFY(Plan::recognizedPartial(*result, partial, priorities()));
        for (auto &entry : partial.outputs) if (entry.output.backendKey == QStringLiteral("HDMI-A-1"))
            entry.output.logicalGeometry.moveTopLeft(QPoint(1800, 100));
        QVERIFY(!Plan::recognizedPartial(*result, partial, priorities()));
        partial = source;
        for (auto &entry : partial.outputs) if (entry.output.backendKey == QStringLiteral("DP-1")) {
            entry.output.nativePixels = QSize(1680, 1050);
            entry.output.logicalGeometry.setSize(QSize(1344, 840));
        }
        QVERIFY(!Plan::recognizedPartial(*result, partial, priorities()));
        partial = source;
        partial.generation = QStringLiteral("new-user-compositor");
        QVERIFY(!Plan::recognizedPartial(*result, partial, priorities()));
    }

    void noImplicitAddRemoveOrVirtualCaptureClaim()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {{Operation::Kind::AddVirtual, QStringLiteral("new:one"), QPoint(2560, 100), QSize(800, 600), 1.0}};
        QVERIFY(!Plan::make(source, priorities(), draft, limits()));
        draft.operations = {{Operation::Kind::Remove, idFor(source, QStringLiteral("HDMI-A-1")), {}, {}, 1.0}};
        QVERIFY(!Plan::make(source, priorities(), draft, limits()));
        auto virtualSource = source;
        virtualSource.outputs[0].output.physical = false;
        virtualSource.outputs[0].output.owner = QStringLiteral("someone-else");
        draft.operations = {{Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1.0}};
        QVERIFY(!Plan::make(virtualSource, priorities(), draft, limits()));
    }

    void physicalArgumentsRequireFreshAdvertisedMode()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {
            {Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1.0},
            {Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1.0},
            {Operation::Kind::SetPrimary, idFor(source, QStringLiteral("HDMI-A-1")), {}, {}, 1.0},
        };
        const auto plan = Plan::make(source, priorities(), draft, limits());
        QVERIFY(plan);
        const Plan::Mode current{QStringLiteral("27"), QSize(1600, 900), 60000};
        const Plan::Mode target{QStringLiteral("41"), QSize(1920, 1080), 60000};
        Plan::OutputState state{QStringLiteral("DP-1"), QPoint(0, 0), 1.25, current, {current, target}};
        const Plan::Mode peerMode{QStringLiteral("12"), QSize(1280, 720), 60000};
        const Plan::OutputState peer{QStringLiteral("HDMI-A-1"), QPoint(1280, 100), 1.0, peerMode, {peerMode}};
        QMap<QString, Plan::OutputState> states{{QStringLiteral("DP-1"), state}, {QStringLiteral("HDMI-A-1"), peer}};
        QMap<QString, Plan::Mode> selected{{QStringLiteral("DP-1"), target}};
        const auto args = Plan::arguments(*plan, states, priorities(), selected);
        QVERIFY(args);
        QVERIFY(args->contains(QStringLiteral("output.DP-1.mode.41")));
        QVERIFY(args->contains(QStringLiteral("output.DP-1.scale.1")));
        QVERIFY(args->contains(QStringLiteral("output.HDMI-A-1.position.1920,100")));
        QVERIFY(args->contains(QStringLiteral("output.HDMI-A-1.priority.1")));
        QVERIFY(args->contains(QStringLiteral("output.DP-1.priority.2")));
        QCOMPARE(args->size(), 5);

        selected[QStringLiteral("DP-1")].id = QStringLiteral("unadvertised");
        QVERIFY(!Plan::arguments(*plan, states, priorities(), selected));
        selected[QStringLiteral("DP-1")] = target;
        states[QStringLiteral("DP-1")].position = QPoint(1, 0);
        QVERIFY(!Plan::arguments(*plan, states, priorities(), selected));
        states[QStringLiteral("DP-1")] = state;
        states[QStringLiteral("DP-1")].scale = 1.0;
        QVERIFY(!Plan::arguments(*plan, states, priorities(), selected));
        states[QStringLiteral("DP-1")] = state;
        states[QStringLiteral("HDMI-A-1")].position = QPoint(1280, 101);
        QVERIFY(!Plan::arguments(*plan, states, priorities(), selected));
        states[QStringLiteral("HDMI-A-1")] = peer;
        auto changedPriorities = priorities();
        changedPriorities[QStringLiteral("HDMI-A-1")] = 3;
        QVERIFY(!Plan::arguments(*plan, states, changedPriorities, selected));
    }

    void freshCompletePhysicalKScreenSelectsModeAndRejectsPeerEdits()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {
            {Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1.0},
            {Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1.0},
        };
        const auto plan = Plan::make(source, priorities(), draft, limits());
        QVERIFY(plan);
        auto sourceJson = physicalKScreen();
        const auto args = Plan::arguments(*plan, QJsonDocument(sourceJson).toJson());
        QVERIFY(args);
        QVERIFY(args->contains(QStringLiteral("output.DP-1.mode.41"))); // 59.94 Hz is closest to 60 Hz.
        QVERIFY(args->contains(QStringLiteral("output.HDMI-A-1.position.1920,100")));

        auto outputs = sourceJson.value(QStringLiteral("outputs")).toArray();
        auto peer = outputs[1].toObject();
        peer.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1281}, {QStringLiteral("y"), 100}});
        outputs[1] = peer;
        sourceJson.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!Plan::arguments(*plan, QJsonDocument(sourceJson).toJson()));

        sourceJson = physicalKScreen();
        outputs = sourceJson.value(QStringLiteral("outputs")).toArray();
        auto target = outputs[0].toObject();
        target.insert(QStringLiteral("modes"), QJsonArray{mode(QStringLiteral("27"), QSize(1600, 900), 60)});
        outputs[0] = target;
        sourceJson.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!Plan::arguments(*plan, QJsonDocument(sourceJson).toJson()));

        sourceJson = physicalKScreen();
        outputs = sourceJson.value(QStringLiteral("outputs")).toArray();
        peer = outputs[1].toObject();
        peer.insert(QStringLiteral("priority"), 3);
        outputs[1] = peer;
        sourceJson.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!Plan::arguments(*plan, QJsonDocument(sourceJson).toJson()));
    }

    void recoveryRestoresOnlyOwnedFieldsFromFreshReadback()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {
            {Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1},
            {Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1},
            {Operation::Kind::SetPrimary, idFor(source, QStringLiteral("HDMI-A-1")), {}, {}, 1},
        };
        const auto plan = Plan::make(source, priorities(), draft, limits());
        QVERIFY(plan);
        const auto before = Plan::inventory(*plan, QJsonDocument(physicalKScreen()).toJson());
        QVERIFY(before);
        const auto selected = Plan::selectedModes(*plan, *before);
        QVERIFY(selected);
        auto json = physicalKScreen();
        auto outputs = json.value(QStringLiteral("outputs")).toArray();
        auto first = outputs[0].toObject();
        first.insert(QStringLiteral("currentModeId"), QStringLiteral("41"));
        first.insert(QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}});
        first.insert(QStringLiteral("scale"), 1.0);
        first.insert(QStringLiteral("priority"), 2);
        outputs[0] = first;
        auto second = outputs[1].toObject();
        second.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1920}, {QStringLiteral("y"), 100}});
        second.insert(QStringLiteral("priority"), 1);
        outputs[1] = second;
        json.insert(QStringLiteral("outputs"), outputs);
        const auto full = Plan::recoveryArguments(*plan, *before, *selected, QJsonDocument(json).toJson());
        QVERIFY(full);
        QVERIFY(full->contains(QStringLiteral("output.DP-1.mode.27")));
        QVERIFY(full->contains(QStringLiteral("output.DP-1.scale.1.25")));
        QVERIFY(full->contains(QStringLiteral("output.HDMI-A-1.position.1280,100")));
        QVERIFY(full->contains(QStringLiteral("output.DP-1.priority.1")));
        QVERIFY(full->contains(QStringLiteral("output.HDMI-A-1.priority.2")));

        // A previous physical mode ID can vanish after hotplug/driver churn.
        auto noOriginalMode = first;
        noOriginalMode.insert(QStringLiteral("modes"), QJsonArray{
            mode(QStringLiteral("41"), QSize(1920, 1080), 59.94),
            mode(QStringLiteral("42"), QSize(1920, 1080), 50)});
        auto missingModeOutputs = outputs;
        missingModeOutputs[0] = noOriginalMode;
        auto missingModeJson = json;
        missingModeJson.insert(QStringLiteral("outputs"), missingModeOutputs);
        QVERIFY(!Plan::recoveryArguments(*plan, *before, *selected, QJsonDocument(missingModeJson).toJson()));

        // KDE moved the peer elsewhere after our attempt: do not clobber it.
        second.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 2000}, {QStringLiteral("y"), 100}});
        outputs[1] = second;
        json.insert(QStringLiteral("outputs"), outputs);
        const auto independent = Plan::recoveryArguments(*plan, *before, *selected, QJsonDocument(json).toJson());
        QVERIFY(independent);
        QVERIFY(!independent->contains(QStringLiteral("output.HDMI-A-1.position.1280,100")));
        QVERIFY(independent->contains(QStringLiteral("output.DP-1.mode.27")));

        // A different refresh-rate mode with identical pixels is also an
        // independent edit; no size-based mode rollback is allowed.
        first.insert(QStringLiteral("currentModeId"), QStringLiteral("42"));
        outputs[0] = first;
        json.insert(QStringLiteral("outputs"), outputs);
        const auto changedRefresh = Plan::recoveryArguments(*plan, *before, *selected, QJsonDocument(json).toJson());
        QVERIFY(changedRefresh);
        QVERIFY(!changedRefresh->contains(QStringLiteral("output.DP-1.mode.27")));
    }

    void recoveryRefusesHotplugAndProjectedOverlap()
    {
        const auto source = baseline();
        auto draft = request(source);
        draft.operations = {
            {Operation::Kind::Move, idFor(source, QStringLiteral("HDMI-A-1")), QPoint(1920, 100), {}, 1},
            {Operation::Kind::Resize, idFor(source, QStringLiteral("DP-1")), {}, QSize(1920, 1080), 1},
        };
        const auto plan = Plan::make(source, priorities(), draft, limits());
        QVERIFY(plan);
        const auto before = Plan::inventory(*plan, QJsonDocument(physicalKScreen()).toJson());
        QVERIFY(before);
        const auto selected = Plan::selectedModes(*plan, *before);
        QVERIFY(selected);
        auto json = physicalKScreen();
        auto outputs = json.value(QStringLiteral("outputs")).toArray();
        auto first = outputs[0].toObject();
        first.insert(QStringLiteral("currentModeId"), QStringLiteral("42"));
        first.insert(QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}});
        first.insert(QStringLiteral("scale"), 1.0);
        outputs[0] = first;
        auto second = outputs[1].toObject();
        second.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1920}, {QStringLiteral("y"), 100}});
        outputs[1] = second;
        json.insert(QStringLiteral("outputs"), outputs);
        // Preserving mode 42 while reverting scale and peer position would
        // overlap the two outputs, so the entire recovery is refused.
        QVERIFY(!Plan::recoveryArguments(*plan, *before, *selected, QJsonDocument(json).toJson()));
        outputs.removeLast();
        json.insert(QStringLiteral("outputs"), outputs);
        QVERIFY(!Plan::recoveryArguments(*plan, *before, *selected, QJsonDocument(json).toJson()));
    }

    void workerWireMustMatchFullPhysicalBeforeLayout()
    {
        using KRdp::ConsoleWorkerWire::MixedOperation;
        KRdp::ConsoleWorkerWire::PhysicalLayout wire{31, 9, QStringLiteral("console-generation-1"), 4, true,
            {{QStringLiteral("DP-1"), QSize(1600, 900), QRect(0, 0, 1280, 720), 1.25, true, 1},
             {QStringLiteral("HDMI-A-1"), QSize(1280, 720), QRect(1280, 100, 1280, 720), 1, false, 2}},
            {{MixedOperation::Kind::Move, QStringLiteral("HDMI-A-1"), QPoint(1920, 100), {}, 1},
             {MixedOperation::Kind::Resize, QStringLiteral("DP-1"), {}, QSize(1920, 1080), 1},
             {MixedOperation::Kind::Primary, QStringLiteral("HDMI-A-1"), {}, {}, 1}}};
        const auto plan = Plan::fromWire(wire);
        QVERIFY(plan);
        QCOMPARE(plan->before.generation, wire.catalogGeneration);
        QCOMPARE(plan->before.revision, wire.expectedRevision);
        QCOMPARE(plan->afterPriorities.value(QStringLiteral("HDMI-A-1")), 1);
        QVERIFY(Plan::arguments(*plan, QJsonDocument(physicalKScreen()).toJson()));

        wire.allowPhysicalChange = false;
        QVERIFY(!Plan::fromWire(wire));
        wire.allowPhysicalChange = true;
        wire.before[1].logical.moveLeft(1281);
        const auto stale = Plan::fromWire(wire);
        QVERIFY(!stale || !Plan::inventory(*stale, QJsonDocument(physicalKScreen()).toJson()));
        wire.before[1].logical.moveLeft(1280);
        wire.operations[0].output = QStringLiteral("unlisted");
        QVERIFY(!Plan::fromWire(wire));
    }
};

QTEST_GUILESS_MAIN(ConsoleTopologyPlanTest)
#include "ConsoleTopologyPlanTest.moc"
