// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>
#include <limits>

#include "RemoteTopologyCatalog.h"

using KRdp::RemoteTopologyCatalog;

namespace
{
RemoteTopologyCatalog::Output output(const QString &key, const QPoint &position = {})
{
    return {.backendKey = key, .name = key, .nativePixels = {1920, 1080},
        .logicalGeometry = QRect(position, QSize(1536, 864)), .scale = 1.25, .owner = {}};
}
}

class RemoteTopologyCatalogTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void stableWithinContinuousPresence()
    {
        RemoteTopologyCatalog catalog;
        const auto first = catalog.observe({output(QStringLiteral("DP-1")), output(QStringLiteral("Virtual-x"), {1536, 0})});
        QVERIFY(first);
        QCOMPARE(first->revision, quint64(1));
        QCOMPARE(first->outputs.size(), 2);
        const auto unchanged = catalog.observe({output(QStringLiteral("Virtual-x"), {1536, 0}), output(QStringLiteral("DP-1"))});
        QVERIFY(unchanged);
        QCOMPARE(*unchanged, *first); // Enumeration order is not topology.
        const auto changed = catalog.observe({output(QStringLiteral("DP-1")), output(QStringLiteral("Virtual-x"), {-1536, 100})});
        QVERIFY(changed);
        QCOMPARE(changed->revision, quint64(2));
        QCOMPARE(changed->outputs[1].id, first->outputs[1].id);
        QCOMPARE(changed->outputs[1].output.logicalGeometry.topLeft(), QPoint(-1536, 100));
    }

    void neverReusesDepartedKey()
    {
        RemoteTopologyCatalog catalog;
        const auto first = catalog.observe({output(QStringLiteral("Virtual-x"))});
        QVERIFY(first);
        const auto absent = catalog.observe({});
        QVERIFY(absent);
        QCOMPARE(absent->revision, quint64(2));
        const auto returned = catalog.observe({output(QStringLiteral("Virtual-x"))});
        QVERIFY(returned);
        QCOMPARE(returned->revision, quint64(3));
        QVERIFY(returned->outputs[0].id != first->outputs[0].id);
        const auto oldGeneration = returned->generation;
        catalog.resetGeneration();
        const auto newGeneration = catalog.observe({output(QStringLiteral("Virtual-x"))});
        QVERIFY(newGeneration);
        QVERIFY(newGeneration->generation != oldGeneration);
        QCOMPARE(newGeneration->revision, quint64(1));
    }

    void refusesAmbiguousOrInvalidReadbackAtomically()
    {
        RemoteTopologyCatalog catalog;
        const auto first = catalog.observe({output(QStringLiteral("DP-1"))});
        QVERIFY(first);
        QVERIFY(!catalog.observe({output(QStringLiteral("DP-1")), output(QStringLiteral("DP-1"))}));
        auto bad = output(QStringLiteral("Virtual-x"));
        bad.scale = std::numeric_limits<qreal>::infinity();
        QVERIFY(!catalog.observe({bad}));
        QCOMPARE(catalog.snapshot(), *first);
        const auto valid = catalog.observe({output(QStringLiteral("DP-1")), output(QStringLiteral("Virtual-x"))});
        QVERIFY(valid);
        QCOMPARE(valid->revision, quint64(2));
        QCOMPARE(valid->outputs[1].id, QStringLiteral("o-2"));
    }
};

QTEST_GUILESS_MAIN(RemoteTopologyCatalogTest)
#include "RemoteTopologyCatalogTest.moc"
