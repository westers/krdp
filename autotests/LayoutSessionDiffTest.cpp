// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "LayoutSessionDiff.h"

using namespace KRdp::LayoutSessions;

namespace
{
const QString DP1 = QStringLiteral("DP-1");
const QString HDMI = QStringLiteral("HDMI-A-1");
const QString StandInDP1 = QStringLiteral("Virtual-krdp-si-DP-1-2560x1440-s100");
const QString StandInHDMI = QStringLiteral("Virtual-krdp-si-HDMI-A-1-2560x1440-s100");
const QString Fit = QStringLiteral("Virtual-krdp-si-DP-1-1920x1080-s100");
const QString Extra = QStringLiteral("Virtual-krdp-v1-1920x1080-s125");
}

/**
 * The per-connection session diff a live `KRDPCTL` apply runs (OPT-044,
 * Task 4): which of a connection's per-output sessions keep streaming, which
 * are dropped, which are created. Names are the KWin outputs each session
 * captures, in RDP surface order.
 */
class LayoutSessionDiffTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void identicalSetsKeepEverything()
    {
        const auto d = diff({DP1, HDMI}, {DP1, HDMI});
        QVERIFY(d.unchanged());
        QCOMPARE(d.source, (QList<qsizetype>{0, 1}));
        QCOMPARE(d.kept, (QStringList{DP1, HDMI}));
        QVERIFY(d.created.isEmpty());
        QVERIFY(d.dropped.isEmpty());
    }

    void darkeningOneRealReplacesOnlyItsSession()
    {
        // DP-1 goes dark: its carrying output becomes the stand-in; HDMI-A-1
        // stays lit and its session must not be restarted.
        const auto d = diff({DP1, HDMI}, {StandInDP1, HDMI}, {StandInDP1});
        QVERIFY(!d.unchanged());
        QCOMPARE(d.source, (QList<qsizetype>{-1, 1}));
        QCOMPARE(d.kept, QStringList{HDMI});
        QCOMPARE(d.created, QStringList{StandInDP1});
        QCOMPARE(d.dropped, QStringList{DP1});
    }

    void lightingARealDropsTheStandInAndCreatesTheReal()
    {
        // The reverse: Private (two stand-ins) → DP-1 lit again.
        const auto d = diff({StandInDP1, StandInHDMI}, {DP1, StandInHDMI}, {StandInDP1});
        QCOMPARE(d.source, (QList<qsizetype>{-1, 1}));
        QCOMPARE(d.kept, QStringList{StandInHDMI});
        QCOMPARE(d.created, QStringList{DP1});
        QCOMPARE(d.dropped, QStringList{StandInDP1});
    }

    void reorderKeepsSessionsAndRemapsIndices()
    {
        // A primary change reorders nothing on the host, but the layout may
        // list the monitors differently; the sessions follow by name.
        const auto d = diff({DP1, HDMI}, {HDMI, DP1});
        QVERIFY(d.unchanged());
        QCOMPARE(d.source, (QList<qsizetype>{1, 0}));
    }

    void changedOutputIsRecreatedEvenWhenStillWanted()
    {
        // The executor says it removed and recreated this output (the belt
        // the controller's ruling asks for): the session over the old one
        // captures nothing any more, whatever the name says.
        const auto d = diff({DP1, Extra}, {DP1, Extra}, {Extra});
        QCOMPARE(d.source, (QList<qsizetype>{0, -1}));
        QCOMPARE(d.kept, QStringList{DP1});
        QCOMPARE(d.created, QStringList{Extra});
        QCOMPARE(d.dropped, QStringList{Extra});
    }

    void extraVirtualAddedKeepsTheReals()
    {
        const auto d = diff({DP1, HDMI}, {DP1, HDMI, Extra}, {Extra});
        QCOMPARE(d.source, (QList<qsizetype>{0, 1, -1}));
        QCOMPARE(d.kept, (QStringList{DP1, HDMI}));
        QCOMPARE(d.created, QStringList{Extra});
        QVERIFY(d.dropped.isEmpty());
    }

    void fitResizeReplacesTheStandInOnly()
    {
        // A Fit stand-in resized: another name (size is part of it), so the
        // old one is dropped and the new one created; the extra stays.
        const auto d = diff({StandInDP1, HDMI, Extra}, {Fit, HDMI, Extra}, {Fit, StandInDP1});
        QCOMPARE(d.source, (QList<qsizetype>{-1, 1, 2}));
        QCOMPARE(d.kept, (QStringList{HDMI, Extra}));
        QCOMPARE(d.created, QStringList{Fit});
        QCOMPARE(d.dropped, QStringList{StandInDP1});
    }

    void emptyWantedDropsEverything()
    {
        const auto d = diff({DP1, HDMI}, {});
        QVERIFY(d.source.isEmpty());
        QVERIFY(d.kept.isEmpty());
        QVERIFY(d.created.isEmpty());
        QCOMPARE(d.dropped, (QStringList{DP1, HDMI}));
    }

    void firstBuildCreatesEverything()
    {
        const auto d = diff({}, {DP1, HDMI});
        QCOMPARE(d.source, (QList<qsizetype>{-1, -1}));
        QCOMPARE(d.created, (QStringList{DP1, HDMI}));
        QVERIFY(d.dropped.isEmpty());
        QVERIFY(!d.unchanged());
    }
};

QTEST_GUILESS_MAIN(LayoutSessionDiffTest)

#include "LayoutSessionDiffTest.moc"
