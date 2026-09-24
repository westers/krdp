// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualInitialBootstrap.h"

#include <QTest>

using namespace KRdp;

class VirtualInitialBootstrapTest : public QObject
{
    Q_OBJECT
    using Output = VirtualSessionJournal::Record::InitialOutput;
    const QVector<Output> selected{{QPoint(0, 0), QSize(1600, 900), 1.25, true},
        {QPoint(1280, 100), QSize(1280, 720), 1.0, false},
        {QPoint(2560, 100), QSize(960, 540), 1.5, false}};

    RetainedKScreenReadback::Snapshot observed() const
    {
        RetainedKScreenReadback::Snapshot result;
        for (qsizetype i = 0; i < selected.size(); ++i) {
            const auto &expected = selected[i];
            const auto name = VirtualInitialBootstrap::name(i);
            result.outputs.append({.backendKey = name, .name = name, .nativePixels = expected.pixels,
                .logicalGeometry = RemoteMonitorGeometry::logicalRect(expected.position, expected.pixels, expected.scale),
                .scale = expected.scale, .enabled = true, .primary = expected.primary,
                .physical = false, .owner = QStringLiteral("session")});
        }
        result.maxActiveOutputs = 3;
        return result;
    }
private Q_SLOTS:
    void derivesOneWholeLayoutCommandFromFreshOutputInventory()
    {
        auto current = observed();
        current.outputs[1].logicalGeometry.moveTopLeft(QPoint(0, 0));
        const auto arguments = VirtualInitialBootstrap::applyArguments(selected, current);
        QVERIFY(arguments);
        QCOMPARE(arguments->size(), 9);
        QVERIFY(arguments->contains(QStringLiteral("output.Virtual-0.scale.1.25")));
        QVERIFY(arguments->contains(QStringLiteral("output.Virtual-krdp-initial-1.position.1280,100")));
        QVERIFY(arguments->contains(QStringLiteral("output.Virtual-krdp-initial-2.priority.3")));
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
        QVERIFY(VirtualInitialBootstrap::matches(selected, observed()));
        std::reverse(current.outputs.begin(), current.outputs.end());
        current.outputs[1].logicalGeometry.moveTopLeft(selected[1].position);
        QVERIFY(VirtualInitialBootstrap::matches(selected, current)); // Backend names, not KScreen array indices.
    }

    void refusesOutputSetModeScalePositionAndPrimaryMismatch()
    {
        auto current = observed();
        current.outputs.removeLast();
        QVERIFY(!VirtualInitialBootstrap::applyArguments(selected, current));
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
        current = observed(); current.outputs[1].backendKey = QStringLiteral("unexpected");
        QVERIFY(!VirtualInitialBootstrap::applyArguments(selected, current));
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
        current = observed(); current.outputs[1].nativePixels = QSize(1282, 720);
        QVERIFY(!VirtualInitialBootstrap::applyArguments(selected, current));
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
        current = observed(); current.outputs[1].scale = 1.25;
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
        current = observed(); current.outputs[1].logicalGeometry.moveTopLeft(QPoint(1280, 101));
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
        current = observed(); current.outputs[1].primary = true;
        QVERIFY(!VirtualInitialBootstrap::matches(selected, current));
    }
};

QTEST_GUILESS_MAIN(VirtualInitialBootstrapTest)
#include "VirtualInitialBootstrapTest.moc"
