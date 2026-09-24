// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualInitialLayout.h"

#include <QTest>
#include <limits>

using namespace KRdp;

namespace
{
RemoteTopologyDraft::Capabilities caps()
{
    return {.addVirtual = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
}
}

class VirtualInitialLayoutTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void selectedPrimaryAndMixedScaleKeepClientAdjacency()
    {
        const QVector<VirtualInitialLayout::Screen> screens{
            {QStringLiteral("left"), QPoint(-1000, 100), QSize(1000, 800), 1.25, false},
            {QStringLiteral("right"), QPoint(-200, 100), QSize(1280, 720), 1, true},
        };
        const auto result = VirtualInitialLayout::plan(screens, QStringLiteral("session"), caps());
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(result.outputs.size(), 2);
        QCOMPARE(result.outputs[0].id, QStringLiteral("new:initial-1"));
        QCOMPARE(result.outputs[0].output.logicalGeometry, QRect(800, 0, 1280, 720));
        QVERIFY(result.outputs[0].output.primary);
        QCOMPARE(result.outputs[1].output.logicalGeometry, QRect(0, 0, 800, 640));
        QCOMPARE(result.outputs[1].output.nativePixels, QSize(1000, 800));
        QVERIFY(!result.outputs[1].output.primary);
        QCOMPARE(result.outputs[0].output.owner, QStringLiteral("session"));
        QVERIFY(result.outputs[0].output.backendKey.isEmpty()); // No compositor identity before creation.
    }

    void refusesDisconnectedOverlappingAndCornerOnlyScreens()
    {
        const VirtualInitialLayout::Screen first{QStringLiteral("first"), {}, QSize(1280, 720), 1, true};
        auto second = VirtualInitialLayout::Screen{QStringLiteral("second"), QPoint(1300, 0), QSize(960, 540), 1, false};
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("unreachable"));
        second.logicalPosition = QPoint(1000, 0);
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("overlap"));
        second.logicalPosition = QPoint(1280, 720);
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("unreachable"));
    }

    void validatesSelectionIdentityPrimaryAndLimits()
    {
        const VirtualInitialLayout::Screen first{QStringLiteral("first"), {}, QSize(1280, 720), 1, true};
        auto second = VirtualInitialLayout::Screen{QStringLiteral("second"), QPoint(1280, 0), QSize(960, 540), 1, false};
        QCOMPARE(VirtualInitialLayout::plan({}, QStringLiteral("session"), caps()).error, QStringLiteral("invalid"));
        QCOMPARE(VirtualInitialLayout::plan({first}, {}, caps()).error, QStringLiteral("invalid"));
        second.id = first.id;
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("invalid"));
        second.id = QStringLiteral("second");
        second.primary = true;
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("invalid-primary"));
        second.primary = false;
        second.pixels = QSize(5000, 540);
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("limit"));
        second.pixels = QSize(960, 540);
        second.pixels.setWidth(961);
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("limit"));
        second.pixels = QSize(960, 540);
        second.scale = 0.75;
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("limit"));
        second.scale = 1;
        auto limited = caps();
        limited.maxOutputs = 1;
        QCOMPARE(VirtualInitialLayout::plan({first, second}, QStringLiteral("session"), limited).error,
            QStringLiteral("limit"));
        second.logicalPosition = QPoint(std::numeric_limits<int>::max(), 0);
        auto negative = first;
        negative.logicalPosition = QPoint(std::numeric_limits<int>::min(), 0);
        QCOMPARE(VirtualInitialLayout::plan({negative, second}, QStringLiteral("session"), caps()).error,
            QStringLiteral("limit"));
    }
};

QTEST_GUILESS_MAIN(VirtualInitialLayoutTest)
#include "VirtualInitialLayoutTest.moc"
