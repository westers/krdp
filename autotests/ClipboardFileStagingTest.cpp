// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: the staging planner: the start-up sweep decision, part-file names, and when a staged selection is offered. Pure (no file system).

#include <QTest>

#include "ClipboardFileStaging.h"

using namespace KRdp::ClipboardFileStaging;

namespace
{
constexpr qint64 Now = 2'000'000'000;
RootEntry dir(const QString &name, qint64 age)
{
    return {name, Now - age, true, false};
}
}

class ClipboardFileStagingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void connectionNames()
    {
        QVERIFY(isConnectionDirName(QStringLiteral("c-0")));
        QVERIFY(isConnectionDirName(QStringLiteral("c-deadbeef01")));
        QVERIFY(isConnectionDirName(QStringLiteral("c-") + QString(32, QLatin1Char('a'))));
        for (const char *bad : {"", "c-", "c-G", "c-DEAD", "x-1", "c-1/..", "c-1 ", "../c-1", "c-../x", ".", "..", "c-1\n"}) {
            QVERIFY2(!isConnectionDirName(QString::fromLatin1(bad)), bad);
        }
        QVERIFY(!isConnectionDirName(QStringLiteral("c-") + QString(33, QLatin1Char('a'))));
    }
    void partNames()
    {
        QCOMPARE(partNameFor(QStringLiteral("a.txt")), QStringLiteral("a.txt.part"));
        QVERIFY(isPartName(QStringLiteral("a.txt.part")));
        QVERIFY(!isPartName(QStringLiteral("a.txt")));
        QVERIFY(!isPartName(QStringLiteral(".part"))); // a file that is only the suffix is a final name
    }
    void sweepDecisionTable()
    {
        const QSet<QString> live{QStringLiteral("c-1a"), QStringLiteral("c-2b")};
        const QList<RootEntry> entries{
            dir(QStringLiteral("c-1a"), 3600),                          // live, young
            dir(QStringLiteral("c-2b"), 2 * MaxAgeSeconds),             // live but over a day old
            dir(QStringLiteral("c-3c"), 60),                            // not live (its connection ended)
            dir(QStringLiteral("c-3d"), 5 * MaxAgeSeconds),             // not live and old
            dir(QStringLiteral("notes"), 5 * MaxAgeSeconds),            // not ours
            {QStringLiteral("c-4e"), Now, false, true},                 // a symlink named like ours
            {QStringLiteral("random-link"), Now, false, true},          // a symlink not named like ours
            {QStringLiteral("c-5f"), Now, false, false},                // a regular file named like ours
            {QStringLiteral("README"), Now, false, false},              // a regular file not ours
            dir(QStringLiteral("c-1a"), MaxAgeSeconds),                 // exactly one day: still young
            dir(QStringLiteral("c-1a"), MaxAgeSeconds + 1),
        };
        const auto plan = planSweep(entries, live, Now);
        QCOMPARE(plan.size(), entries.size());
        const QList<SweepItem> expected{
            {QStringLiteral("c-1a"), SweepAction::Keep, SweepReason::Live},
            {QStringLiteral("c-2b"), SweepAction::RemoveTree, SweepReason::TooOld},
            {QStringLiteral("c-3c"), SweepAction::RemoveTree, SweepReason::UnknownConnection},
            {QStringLiteral("c-3d"), SweepAction::RemoveTree, SweepReason::TooOld},
            {QStringLiteral("notes"), SweepAction::LeaveAlone, SweepReason::NotOurs},
            {QStringLiteral("c-4e"), SweepAction::UnlinkOnly, SweepReason::Symlink},
            {QStringLiteral("random-link"), SweepAction::LeaveAlone, SweepReason::NotOurs},
            {QStringLiteral("c-5f"), SweepAction::UnlinkOnly, SweepReason::NotADirectory},
            {QStringLiteral("README"), SweepAction::LeaveAlone, SweepReason::NotOurs},
            {QStringLiteral("c-1a"), SweepAction::Keep, SweepReason::Live},
            {QStringLiteral("c-1a"), SweepAction::RemoveTree, SweepReason::TooOld},
        };
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QVERIFY2(plan[i] == expected[i], qPrintable(QStringLiteral("row %1 (%2)").arg(i).arg(entries[i].name)));
        }
    }
    void aSymlinkIsNeverPlannedForTreeRemoval()
    {
        // Whatever the name, age or liveness: following a link out of the root must be impossible by construction.
        for (const QString &name : {QStringLiteral("c-1a"), QStringLiteral("c-ff"), QStringLiteral("x")}) {
            for (const qint64 age : {qint64(0), MaxAgeSeconds * 10}) {
                const auto plan = planSweep({{name, Now - age, true, true}}, {QStringLiteral("c-1a")}, Now);
                QVERIFY(plan[0].action != SweepAction::RemoveTree && plan[0].action != SweepAction::Keep);
                QCOMPARE(plan[0].action, isConnectionDirName(name) ? SweepAction::UnlinkOnly : SweepAction::LeaveAlone);
            }
        }
    }
    void aLiveConnectionWithAYoungDirectoryIsNeverRemoved()
    {
        const auto plan = planSweep({dir(QStringLiteral("c-1a"), 0)}, {QStringLiteral("c-1a")}, Now);
        QCOMPARE(plan[0].action, SweepAction::Keep);
        // a clock that went backwards (mtime in the future) is not "too old"
        QCOMPARE(planSweep({{QStringLiteral("c-1a"), Now + 100000, true, false}}, {QStringLiteral("c-1a")}, Now)[0].action, SweepAction::Keep);
    }
    void offerTimingFollowsTheTotal()
    {
        constexpr quint64 MiB = 1024 * 1024;
        QCOMPARE(offerTiming({}), OfferTiming::Never);
        QCOMPARE(offerTiming({0}), OfferTiming::Now);
        QCOMPARE(offerTiming({1, 2, 3}), OfferTiming::Now);
        QCOMPARE(offerTiming({256 * MiB}), OfferTiming::Now);
        QCOMPARE(offerTiming({256 * MiB + 1}), OfferTiming::AfterFirstFile);
        QCOMPARE(offerTiming({128 * MiB, 128 * MiB}), OfferTiming::Now);
        QCOMPARE(offerTiming({128 * MiB, 128 * MiB, 1}), OfferTiming::AfterFirstFile);
        QCOMPARE(offerTiming({5ULL * 1024 * MiB}), OfferTiming::AfterFirstFile);
        QCOMPARE(offerTiming({~quint64(0), ~quint64(0)}), OfferTiming::AfterFirstFile); // saturates, no wrap to a small number
        QCOMPARE(offerTiming({~quint64(0) - 1, 2}), OfferTiming::AfterFirstFile);
    }
};

QTEST_GUILESS_MAIN(ClipboardFileStagingTest)

#include "ClipboardFileStagingTest.moc"
