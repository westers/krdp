// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualResizeTestFixture.h"
#include "../src/WorkspaceFrameGeometry.h"
#include <QTest>
#include <limits>

using namespace KRdp;
using namespace VirtualResizeFixture;
namespace V = KRdp::VirtualResize;
class VirtualResizeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void selectsTargetModeInventoryFromMultiOutputReadback()
    {
        auto first = output();
        auto second = output();
        second.insert(QStringLiteral("name"), QStringLiteral("Virtual-1"));
        second.insert(QStringLiteral("id"), 2);
        second.insert(QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 1280}, {QStringLiteral("y"), 100}});
        const auto json = QJsonDocument(QJsonObject{{QStringLiteral("outputs"), QJsonArray{first, second}}}).toJson();
        QString error;
        QVERIFY(!V::snapshot(json, &error)); // Legacy single-output Fit stays strict.
        const auto selected = V::snapshotForOutput(json, QStringLiteral("Virtual-1"), &error);
        QVERIFY2(selected.has_value(), qPrintable(error));
        QCOMPARE(selected->name, QStringLiteral("Virtual-1"));
        QCOMPARE(selected->id, 2);
        QCOMPARE(selected->position, QPoint(1280, 100));
        QCOMPARE(selected->current.pixels, QSize(1280, 720));
        QVERIFY(!V::snapshotForOutput(json, QStringLiteral("Virtual-2"), &error));
        QVERIFY(!V::snapshotForOutput(json, QStringLiteral("Virtual-1.scale.2"), &error));
        QVERIFY(!V::snapshotForOutput(QByteArray(1024 * 1024 + 1, ' '), QStringLiteral("Virtual-1"), &error));
        const auto ambiguous = QJsonDocument(QJsonObject{{QStringLiteral("outputs"), QJsonArray{first, second, second}}}).toJson();
        QVERIFY(!V::snapshotForOutput(ambiguous, QStringLiteral("Virtual-1"), &error));
    }

    void boundsAndSourceSchema()
    {
        QVERIFY(V::validRequest({320, 200}, 1)); QVERIFY(V::validRequest({4096, 4096}, 4));
        QVERIFY(!V::validRequest({321, 200}, 1)); QVERIFY(!V::validRequest({320, 201}, 1));
        QVERIFY(!V::validRequest({4098, 720}, 1)); QVERIFY(!V::validRequest({1280, 720}, std::numeric_limits<double>::infinity()));
        QString error;
        const auto s = V::snapshot(json(output()), &error);
        QVERIFY2(s.has_value(), qPrintable(error));
        QCOMPARE(s->current.refresh, 60000); // JSON Hz, command mHz.
        const V::Plan plan{*s, {1920, 1080}, 1.25};
        QCOMPARE(V::add(plan), QStringList{QStringLiteral("output.Virtual-0.addCustomMode.1920.1080.60000.full")});
        QVERIFY(V::sameScale(1.333333333, 1.33203125));
        QVERIFY(!V::sameScale(1.333333333, 1.341666666));
        QCOMPARE(V::normalizedScale(1.331), 160.0 / 120);
        // Sol's Qt Wayland QScreen reported DPR 2 for a KScreen 1.25 output.
        // Encoded pixels and frame logical geometry recover the useful ratio.
        QCOMPARE(V::frameScale({1600, 900}, {1280, 720}), 1.25);
        QVERIFY(V::frameScaleMatches(1.25, 1.25, {1280, 720}));
        QVERIFY(!V::frameScaleMatches(2, 1.25, {1280, 720}));
        QVERIFY(!V::frameScale({1600, 900}, {1280, 710}));
        QVERIFY(!V::frameScale({1600, 900}, {}));
        QCOMPARE(V::frameScale({320, 202}, {80, 51}), 4.0);
        QCOMPARE(V::frameScale({322, 4096}, {81, 1024}), 4.0);
        QVERIFY(!V::frameScale({322, 4096}, {81, 980}));
        QVERIFY(V::geometryMatchesScale({322, 4096}, {81, 1024}, 4));
        QVERIFY(!V::geometryMatchesScale({322, 4096}, {81, 1024}, 3.95));
        QVERIFY(!V::geometryMatchesScale({1600, 900}, {1280, 720}, 2));
        // At the smallest supported logical dimensions, one output pixel of
        // compositor rounding must not invalidate a verified exact scale.
        QVERIFY(V::frameScaleMatches(322.0 / 81, 4, {81, 50}));
        // The first packet after a 1920 -> 1600@125% resize must adopt the
        // new 1280x720 logical geometry even though Qt reports DPR 2. An old
        // 1920-pixel packet or an unrelated logical rectangle cannot do so.
        QVERIFY(WorkspaceFrameGeometry::matches({1600, 900}, {1280, 720}, 1.25));
        QVERIFY(!WorkspaceFrameGeometry::matches({1920, 1080}, {1280, 720}, 1.25));
        QVERIFY(!WorkspaceFrameGeometry::matches({1600, 900}, {1920, 1080}, 1.25));
        QVERIFY(WorkspaceFrameGeometry::matches({322, 4096}, {81, 1024}, 4));
        QVERIFY(!WorkspaceFrameGeometry::matches({322, 4096}, {81, 980}, 4));
    }
    void refusesBadSnapshots_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("rotated") << QStringLiteral("rotation") << QJsonValue(2);
        QTest::newRow("rotation-string") << QStringLiteral("rotation") << QJsonValue(QStringLiteral("1"));
        QTest::newRow("disabled") << QStringLiteral("enabled") << QJsonValue(false);
        QTest::newRow("boolean-string") << QStringLiteral("connected") << QJsonValue(QStringLiteral("true"));
        QTest::newRow("name-injection") << QStringLiteral("name") << QJsonValue(QStringLiteral("Virtual-0.scale.2"));
        QTest::newRow("missing-current") << QStringLiteral("currentModeId") << QJsonValue(QStringLiteral("99"));
        QTest::newRow("scale-zero") << QStringLiteral("scale") << QJsonValue(0);
        QTest::newRow("scale-null") << QStringLiteral("scale") << QJsonValue();
        QTest::newRow("id-fraction") << QStringLiteral("id") << QJsonValue(1.5);
        QTest::newRow("position-missing") << QStringLiteral("pos") << QJsonValue(QJsonObject{});
    }
    void refusesBadSnapshots()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        auto out = output(); out[field] = value;
        QString error; QVERIFY(!V::snapshot(json(out), &error)); QVERIFY(!error.isEmpty());
    }
    void malformedAmbiguousAndDuplicateModes()
    {
        QString error;
        QVERIFY(!V::snapshot(QByteArrayLiteral("{"), &error));
        QVERIFY(!V::snapshot(QByteArray(1024 * 1024 + 1, ' '), &error));
        QVERIFY(!V::snapshot(QJsonDocument(QJsonObject{{QStringLiteral("outputs"), QJsonArray{output(), output()}}}).toJson(), &error));
        auto out = output(); auto modes = out[QStringLiteral("modes")].toArray(); modes.append(modes.first()); out[QStringLiteral("modes")] = modes;
        QVERIFY(!V::snapshot(json(out), &error));
    }
    void addsDiscoversNewIdSelectsAndVerifies()
    {
        Deferred io; VirtualResizeExecutor executor(nullptr, io.runner());
        QList<VirtualResizeExecutor::Result> results;
        connect(&executor, &VirtualResizeExecutor::finished, this, [&](auto r) { results.append(r); });
        QVERIFY(executor.resize({1920, 1080}, 1.25));
        io.state(output(false));
        QCOMPARE(io.calls.first().arguments, QStringList{QStringLiteral("output.Virtual-0.addCustomMode.1920.1080.60000.full")});
        io.answer(true);
        QCOMPARE(io.calls.first().arguments, QStringList{QStringLiteral("-j")});
        auto discovered = output(false);
        auto modes = discovered[QStringLiteral("modes")].toArray(); modes.append(mode(QStringLiteral("42"), 1920, 1080)); discovered[QStringLiteral("modes")] = modes;
        io.state(discovered);
        QCOMPARE(io.calls.first().arguments, (QStringList{QStringLiteral("output.Virtual-0.mode.42"), QStringLiteral("output.Virtual-0.scale.1.25")}));
        QVERIFY(results.isEmpty());
        io.answer(true); QVERIFY(results.isEmpty());
        discovered[QStringLiteral("currentModeId")] = QStringLiteral("42"); discovered[QStringLiteral("scale")] = 1.25;
        io.state(discovered);
        QCOMPARE(results.size(), 1); QVERIFY(results.first().error.isEmpty()); QVERIFY(results.first().mutated);
    }
    void modeLimitStillAllowsExistingAndNoop()
    {
        Deferred io; VirtualResizeExecutor executor(nullptr, io.runner());
        auto out = output(); auto modes = out[QStringLiteral("modes")].toArray();
        for (int i = 3; i <= 64; ++i) modes.append(mode(QString::number(i), 320 + i * 2, 200));
        out[QStringLiteral("modes")] = modes;
        QList<VirtualResizeExecutor::Result> results;
        connect(&executor, &VirtualResizeExecutor::finished, this, [&](auto r) { results.append(r); });
        QVERIFY(executor.resize({1600, 900}, 1)); io.state(out);
        QCOMPARE(results.size(), 1); QVERIFY(results.last().error.contains(QStringLiteral("64"))); QVERIFY(io.calls.isEmpty());
        QVERIFY(executor.resize({1280, 720}, 1)); io.state(out);
        QVERIFY(results.last().error.isEmpty()); QVERIFY(!results.last().mutated); QVERIFY(io.calls.isEmpty());
        QVERIFY(executor.resize({1920, 1080}, 1)); io.state(out);
        QVERIFY(io.calls.first().arguments.first().contains(QStringLiteral("mode.2")));
    }
    void failedAddNeverSelects()
    {
        Deferred io; VirtualResizeExecutor executor(nullptr, io.runner());
        QString error;
        connect(&executor, &VirtualResizeExecutor::finished, this, [&](auto r) { error = r.error; });
        QVERIFY(executor.resize({1920, 1080}, 1)); io.state(output(false)); io.answer(false); io.state(output(false));
        QVERIFY(error.contains(QStringLiteral("not advertised"))); QVERIFY(io.calls.isEmpty());
    }
    void conditionalRollbackPreservesIndependentScale()
    {
        QString error; const auto before = V::snapshot(json(output()), &error); QVERIFY(before);
        const V::Plan plan{*before, {1920, 1080}, 1.25};
        Deferred io; VirtualResizeExecutor executor(nullptr, io.runner());
        QList<VirtualResizeExecutor::Result> results;
        connect(&executor, &VirtualResizeExecutor::finished, this, [&](auto r) { results.append(r); });
        QVERIFY(executor.rollback(plan));
        auto changed = output(); changed[QStringLiteral("currentModeId")] = QStringLiteral("2"); changed[QStringLiteral("scale")] = 1.5;
        io.state(changed);
        QCOMPARE(io.calls.first().arguments, QStringList{QStringLiteral("output.Virtual-0.mode.1")});
        io.answer(true); changed[QStringLiteral("currentModeId")] = QStringLiteral("1"); io.state(changed);
        QCOMPARE(results.size(), 1); QVERIFY(results.first().error.isEmpty()); QCOMPARE(results.first().observed->scale, 1.5);
    }
    void destructionAndCancellationAtMutationSignal()
    {
        Deferred io; auto *executor = new VirtualResizeExecutor(nullptr, io.runner());
        connect(executor, &VirtualResizeExecutor::mutationStarting, this, [&] { delete executor; executor = nullptr; });
        QVERIFY(executor->resize({1920, 1080}, 1)); io.state(output());
        QVERIFY(!executor); QVERIFY(io.calls.isEmpty());
        VirtualResizeExecutor next(nullptr, io.runner());
        QString error;
        connect(&next, &VirtualResizeExecutor::mutationStarting, &next, [&] { next.cancelBeforeApply(); });
        connect(&next, &VirtualResizeExecutor::finished, this, [&](auto r) { error = r.error; });
        QVERIFY(next.resize({1920, 1080}, 1)); io.state(output());
        QVERIFY(error.contains(QStringLiteral("cancelled"))); QVERIFY(io.calls.isEmpty());
    }
    void delayedCallbackAfterDestructionIsIgnored()
    {
        Deferred io; auto *executor = new VirtualResizeExecutor(nullptr, io.runner());
        QVERIFY(executor->resize({1920, 1080}, 1)); delete executor; io.state(output()); QVERIFY(io.calls.isEmpty());
    }
};
QTEST_GUILESS_MAIN(VirtualResizeTest)
#include "VirtualResizeTest.moc"
