// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <algorithm>

#include <QTest>

#include "OutputSnapshot.h"

using namespace KRdp::OutputSnapshot;
using namespace KRdp::ClientDisplay;

namespace
{
// Trimmed from a real `kscreen-doctor -j` on hal9000 (2026-09-17), plus one virtual output.
const QByteArray kscreenJson = R"({
  "features": 3,
  "outputs": [
    {"id": 1, "name": "DP-1", "enabled": true, "connected": true, "priority": 1, "pos": {"x": 0, "y": 0}, "size": {"width": 2560, "height": 1440}, "scale": 1, "type": 14},
    {"id": 2, "name": "HDMI-A-1", "enabled": true, "connected": true, "priority": 2, "pos": {"x": 2560, "y": 0}, "size": {"width": 2560, "height": 1440}, "scale": 1, "type": 6},
    {"id": 3, "name": "Virtual-krdp-m0-1920x1080", "enabled": true, "connected": true, "priority": 3, "pos": {"x": 5120, "y": 0}, "size": {"width": 1920, "height": 1080}, "scale": 1, "type": 0},
    {"id": 4, "name": "DP-2", "enabled": false, "connected": false, "priority": 0, "pos": {"x": 0, "y": 0}, "size": {"width": 0, "height": 0}, "scale": 1, "type": 14}
  ],
  "screen": {"currentSize": {"height": 1440, "width": 7040}}
})";
}

class OutputSnapshotTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesConnectedOutputs()
    {
        QString error;
        const auto outputs = parse(kscreenJson, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(outputs.size(), 3); // DP-2 is not connected
        QCOMPARE(outputs[0], (Output{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(2560, 1440)}));
        QCOMPARE(outputs[1], (Output{QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0), 2, QSize(2560, 1440)}));
        QCOMPARE(outputs[2].name, QStringLiteral("Virtual-krdp-m0-1920x1080"));
    }

    void parseReportsGarbage()
    {
        QString error;
        QVERIFY(parse("not json", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(parse(R"({"outputs": 5})", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void physicalFilterAndUnion()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        QCOMPARE(physical.size(), 2);
        QVERIFY(!isVirtual(physical[0].name));
        QVERIFY(isVirtual(QStringLiteral("Virtual-krdp-m0-1920x1080")));
        QCOMPARE(enabledUnion(physical), QRect(0, 0, 5120, 1440));
        auto oneOff = physical;
        oneOff[1].enabled = false;
        QCOMPARE(enabledUnion(oneOff), QRect(0, 0, 2560, 1440));
    }

    void replaceArgsDisablePhysicalsAndPlaceVirtuals()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        const auto args = replaceArgs(physical,
                                      {{QStringLiteral("Virtual-krdp-m0-1920x1080"), QPoint(0, 0)}, {QStringLiteral("Virtual-krdp-m1-1920x1280"), QPoint(1920, 0)}},
                                      QStringLiteral("Virtual-krdp-m1-1920x1280"));
        const QStringList expected{
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.priority.1"),
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.position.1920,0"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.priority.2"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.position.0,0"),
            QStringLiteral("output.DP-1.disable"),
            QStringLiteral("output.HDMI-A-1.disable"),
        };
        QCOMPARE(args, expected);
    }

    void replaceArgsSkipAlreadyDisabledPhysicals()
    {
        auto physical = physicalOnly(parse(kscreenJson));
        physical[1].enabled = false;
        const auto args = replaceArgs(physical, {{QStringLiteral("Virtual-krdp-m0-1920x1080"), QPoint(0, 0)}}, QStringLiteral("Virtual-krdp-m0-1920x1080"));
        QVERIFY(!args.contains(QStringLiteral("output.HDMI-A-1.disable")));
        QVERIFY(args.contains(QStringLiteral("output.DP-1.disable")));
    }

    void positionArgsForExtend()
    {
        const auto args = positionArgs({{QStringLiteral("Virtual-krdp-m0-1920x1080"), QPoint(5120, 100)}, {QStringLiteral("Virtual-krdp-m1-1920x1280"), QPoint(7040, 0)}},
                                       QStringLiteral("Virtual-krdp-m1-1920x1280"),
                                       3);
        const QStringList expected{
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.priority.3"),
            QStringLiteral("output.Virtual-krdp-m1-1920x1280.position.7040,0"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.priority.4"),
            QStringLiteral("output.Virtual-krdp-m0-1920x1080.position.5120,100"),
        };
        QCOMPARE(args, expected);
    }

    void restoreArgsReenableExactly()
    {
        auto physical = physicalOnly(parse(kscreenJson));
        physical[1].enabled = false; // Steve had HDMI off before the session: it stays off
        const auto args = restoreArgs(physical);
        const QStringList expected{
            QStringLiteral("output.DP-1.enable"),
            QStringLiteral("output.DP-1.position.0,0"),
            QStringLiteral("output.DP-1.priority.1"),
            QStringLiteral("output.HDMI-A-1.disable"),
        };
        QCOMPARE(args, expected);
    }

    void matchesIgnoresVirtualsAndSize()
    {
        const auto all = parse(kscreenJson);
        const auto physical = physicalOnly(all);
        QVERIFY(matches(physical, all)); // extra virtual output in "current" is fine
        auto moved = physical;
        moved[1].position = QPoint(0, 1440);
        QVERIFY(!matches(physical, moved));
        auto off = physical;
        off[0].enabled = false;
        QVERIFY(!matches(physical, off));
        auto resized = physical;
        resized[0].size = QSize(1920, 1080);
        QVERIFY(matches(physical, resized));
    }

    void allPresentIsAboutNamesOnly()
    {
        // The settle wait after a restore: "back but replayed wrongly" (re-apply)
        // versus "still being re-added" (keep waiting) is a question of names.
        const auto all = parse(kscreenJson);
        const auto physical = physicalOnly(all);
        QVERIFY(allPresent(physical, all));
        auto shuffled = physical;
        shuffled[0].priority = 3;
        shuffled[1].position = QPoint(0, 0);
        QVERIFY(allPresent(physical, shuffled)); // present, just not matching
        QVERIFY(!matches(physical, shuffled));
        QVector<Output> partial{physical[0]};
        QVERIFY(!allPresent(physical, partial)); // HDMI-A-1 still being re-added
        QVERIFY(!allPresent(physical, {}));
        QVERIFY(allPresent({}, {}));
    }

    void allPresentAndDisabledTellsAbsentFromDisabled()
    {
        // The replace's success check: an output KWin is re-adding is absent,
        // not disabled, and a read-back with no physical output at all is the
        // churn, not a replace (hw standby batch v6c, finding F).
        const auto all = parse(kscreenJson);
        const auto physical = physicalOnly(all);
        QVERIFY(!allPresentAndDisabled(physical, all)); // both still enabled
        auto replaced = all;
        replaced[0].enabled = false;
        replaced[1].enabled = false;
        QVERIFY(allPresentAndDisabled(physical, replaced)); // the virtual output's state is not looked at
        QVector<Output> onlyVirtual{all[2]};
        QVERIFY(!allPresentAndDisabled(physical, onlyVirtual)); // nothing present is not "all disabled"
        QVERIFY(!allPresentAndDisabled(physical, {}));
        QVector<Output> oneBack{replaced[0], all[2]};
        QVERIFY(!allPresentAndDisabled(physical, oneBack)); // HDMI-A-1 absent
        QVector<Output> oneEnabled{replaced[0], all[1], all[2]};
        QVERIFY(!allPresentAndDisabled(physical, oneEnabled)); // HDMI-A-1 back, but enabled
        // An output the snapshot itself had disabled only has to be present and stay disabled.
        auto snapshotWithOff = physical;
        snapshotWithOff[1].enabled = false;
        QVERIFY(allPresentAndDisabled(snapshotWithOff, replaced));
        QVERIFY(allPresentAndDisabled({}, {}));
    }

    void presentAndMissingSplitTheSnapshotByName()
    {
        // A snapshotted monitor that dropped HPD (power button, KVM, cable)
        // is absent from the read-back; kscreen-doctor refuses a command that
        // names it, so the restore is built from what is there and the rest
        // is reported.
        const auto all = parse(kscreenJson);
        const auto physical = physicalOnly(all);
        QCOMPARE(presentSubset(physical, all), physical);
        QVERIFY(missingSubset(physical, all).isEmpty());
        QVector<Output> onlyDp1{all[0], all[2]};
        QCOMPARE(presentSubset(physical, onlyDp1), (QVector<Output>{physical[0]}));
        QCOMPARE(missingSubset(physical, onlyDp1), (QVector<Output>{physical[1]}));
        QVERIFY(presentSubset(physical, {}).isEmpty());
        QCOMPARE(missingSubset(physical, {}), physical);
        // The subsets carry the SNAPSHOT's entries (the state to restore), not the read-back's.
        auto moved = onlyDp1;
        moved[0].position = QPoint(100, 100);
        QCOMPARE(presentSubset(physical, moved)[0].position, QPoint(0, 0));
        QCOMPARE(names(missingSubset(physical, onlyDp1)), QStringList{QStringLiteral("HDMI-A-1")});
        QCOMPARE(restoreArgs(presentSubset(physical, onlyDp1)),
                 (QStringList{QStringLiteral("output.DP-1.enable"), QStringLiteral("output.DP-1.position.0,0"), QStringLiteral("output.DP-1.priority.1")}));
    }

    void jsonRoundTrip()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        QCOMPARE(fromJson(toJson(physical)), physical);
        QVERIFY(fromJson("garbage").isEmpty());
    }

    void stateJsonCarriesOwnerPid()
    {
        const auto physical = physicalOnly(parse(kscreenJson));
        qint64 pid = -1;
        QCOMPARE(fromStateJson(toStateJson(physical, 4242), &pid), physical);
        QCOMPARE(pid, qint64(4242));
        // A file written before the owner PID existed is still readable.
        pid = -1;
        QCOMPARE(fromStateJson(toJson(physical), &pid), physical);
        QCOMPARE(pid, qint64(0));
        QVERIFY(fromStateJson("garbage", &pid).isEmpty());
        QVERIFY(fromStateJson("{\"pid\": 1}", &pid).isEmpty());
    }

    // toClientDisplayInfo() feeds `VirtualMonitorLayout=physical` (OPT-041 S5):
    // mirror the physical output layout instead of the client's own. The
    // caller (SessionController::buildVirtualSessions()) always runs this
    // through KRdp::ClientDisplay::sanitize() afterward - see the two cases
    // below that exercise that composition, not just this function alone.
    void toClientDisplayInfoOrdersByPriorityAndTranslatesToOrigin()
    {
        const QVector<Output> outputs{
            {.name = QStringLiteral("DP-1"), .enabled = true, .position = QPoint(0, 0), .priority = 1, .size = QSize(2560, 1440)},
            {.name = QStringLiteral("HDMI-A-1"), .enabled = true, .position = QPoint(2560, 0), .priority = 2, .size = QSize(2560, 1440)},
            {.name = QStringLiteral("Virtual-x"), .enabled = false, .position = QPoint(5120, 0), .priority = 3, .size = QSize(1920, 1080)},
        };
        const auto info = toClientDisplayInfo(outputs);
        QCOMPARE(info.monitors.size(), 2);
        QCOMPARE(info.monitors[0].geometry, QRect(0, 0, 2560, 1440));
        QVERIFY(info.monitors[0].primary);
        QCOMPARE(info.monitors[1].geometry, QRect(2560, 0, 2560, 1440));
        QVERIFY(!info.monitors[1].primary);
        QCOMPARE(info.desktopSize, QSize(5120, 1440));
    }

    void toClientDisplayInfoWithNoneEnabledIsEmptyAndInvalid()
    {
        const QVector<Output> outputs{
            {.name = QStringLiteral("DP-1"), .enabled = false, .position = QPoint(0, 0), .priority = 1, .size = QSize(2560, 1440)},
            {.name = QStringLiteral("HDMI-A-1"), .enabled = false, .position = QPoint(2560, 0), .priority = 2, .size = QSize(2560, 1440)},
        };
        const auto info = toClientDisplayInfo(outputs);
        QVERIFY(info.monitors.isEmpty());
        QVERIFY(!info.desktopSize.isValid());
    }

    void toClientDisplayInfoTranslatesNegativePositionsToOrigin()
    {
        // Physical layout with the primary panel left of (0,0), as kscreen-doctor reports it.
        const QVector<Output> outputs{
            {.name = QStringLiteral("eDP-1"), .enabled = true, .position = QPoint(-2560, 0), .priority = 1, .size = QSize(2560, 1440)},
            {.name = QStringLiteral("DP-1"), .enabled = true, .position = QPoint(0, 0), .priority = 2, .size = QSize(2560, 1440)},
        };
        const auto info = toClientDisplayInfo(outputs);
        QCOMPARE(info.monitors.size(), 2);
        QCOMPARE(info.monitors[0].geometry.topLeft(), QPoint(0, 0));
        QVERIFY(info.monitors[0].primary);
        QCOMPARE(info.monitors[1].geometry.topLeft(), QPoint(2560, 0));
    }

    void sanitizedPhysicalLayoutMatchesHal9000Unchanged()
    {
        // hal9000's own layout (DP-1 + HDMI-A-1, 5120x1440 union) passes sanitize()
        // untouched - the composition buildVirtualSessions() actually runs.
        const auto physical = physicalOnly(parse(kscreenJson));
        const auto mirrored = toClientDisplayInfo(physical);
        QCOMPARE(sanitize(mirrored, QSize(1920, 1080)), mirrored);
    }

    void sanitizeFallsBackOnAnOversizedPhysicalOutput()
    {
        // A single ultrawide physical output (5120 wide, past MaxDimension = 4096):
        // toClientDisplayInfo() has no size rules of its own and would hand this
        // straight to the encoder unchanged, so the caller must still run it
        // through sanitize() - which drops the (single-entry, so already
        // unusable-as-a-list) monitor list and falls back the unusable
        // desktopSize too, exactly as it would for a client-advertised list of
        // the same shape (ClientDisplayInfoTest::unusableMonitorDropsTheWholeList).
        const QVector<Output> outputs{
            {.name = QStringLiteral("DP-1"), .enabled = true, .position = QPoint(0, 0), .priority = 1, .size = QSize(5120, 1440)},
        };
        const auto mirrored = toClientDisplayInfo(outputs);
        QCOMPARE(mirrored.monitors.size(), 1);
        QCOMPARE(mirrored.desktopSize, QSize(5120, 1440));
        const auto sanitized = sanitize(mirrored, QSize(1920, 1080));
        QVERIFY(sanitized.monitors.isEmpty());
        QCOMPARE(sanitized.desktopSize, QSize(1920, 1080));
    }

    // hostMonitorsFrom() is what a KRDPCTL `query` is answered from (OPT-044):
    // the real outputs as HostMonitors, exactly one of them primary.

    void hostMonitorsFromHal9000()
    {
        const auto monitors = hostMonitorsFrom(physicalOnly(parse(kscreenJson)));
        QCOMPARE(monitors.size(), 2);
        QCOMPARE(monitors[0].id, QStringLiteral("DP-1"));
        QCOMPARE(monitors[0].name, QStringLiteral("DP-1"));
        QCOMPARE(monitors[0].kind, KRdp::LayoutControl::Kind::Real);
        QCOMPARE(monitors[0].size, QSize(2560, 1440));
        QCOMPARE(monitors[0].position, QPoint(0, 0));
        QCOMPARE(monitors[0].scale, 1.0);
        QVERIFY(monitors[0].primary);
        QVERIFY(monitors[0].lit);
        QVERIFY(!monitors[0].standIn);
        QVERIFY(monitors[0].owner.isEmpty());
        QCOMPARE(monitors[1].id, QStringLiteral("HDMI-A-1"));
        QCOMPARE(monitors[1].position, QPoint(2560, 0));
        QVERIFY(!monitors[1].primary);
        QVERIFY(monitors[1].lit);
    }

    void hostMonitorsFromSkipsDisabledPriorityZeroForPrimary()
    {
        // A disabled output reports priority 0; it is listed (dark) but never
        // primary, even when it comes first.
        const QVector<Output> outputs{
            {.name = QStringLiteral("DP-1"), .enabled = false, .position = QPoint(0, 0), .priority = 0, .size = QSize(2560, 1440)},
            {.name = QStringLiteral("HDMI-A-1"), .enabled = true, .position = QPoint(2560, 0), .priority = 1, .size = QSize(2560, 1440)},
        };
        const auto monitors = hostMonitorsFrom(outputs);
        QCOMPARE(monitors.size(), 2);
        QVERIFY(!monitors[0].primary);
        QVERIFY(!monitors[0].lit);
        QVERIFY(monitors[1].primary);
        QVERIFY(monitors[1].lit);
    }

    void hostMonitorsFromPicksLowestPositivePriorityWhenOneIsMissing()
    {
        // No priority 1 anywhere (KWin renumbers after an output goes away):
        // the lowest positive one among the enabled outputs is the primary.
        const QVector<Output> outputs{
            {.name = QStringLiteral("HDMI-A-1"), .enabled = true, .position = QPoint(2560, 0), .priority = 3, .size = QSize(2560, 1440)},
            {.name = QStringLiteral("DP-1"), .enabled = true, .position = QPoint(0, 0), .priority = 2, .size = QSize(2560, 1440)},
        };
        const auto monitors = hostMonitorsFrom(outputs);
        QVERIFY(!monitors[0].primary);
        QVERIFY(monitors[1].primary);
        QCOMPARE(std::count_if(monitors.cbegin(), monitors.cend(), [](const auto &m) {
                     return m.primary;
                 }),
                 1);
    }

    void hostMonitorsFromSingleOutputIsPrimaryEvenWhenDark()
    {
        // One output, disabled (a leftover replace): still described, still the
        // one primary the layout must have.
        const QVector<Output> outputs{
            {.name = QStringLiteral("DP-1"), .enabled = false, .position = QPoint(0, 0), .priority = 0, .size = QSize(2560, 1440)},
        };
        const auto monitors = hostMonitorsFrom(outputs);
        QCOMPARE(monitors.size(), 1);
        QVERIFY(monitors[0].primary);
        QVERIFY(!monitors[0].lit);
        QVERIFY(hostMonitorsFrom({}).isEmpty());
    }

    void parseCarriesScaleIntoHostMonitors()
    {
        const QByteArray scaled = R"({"outputs": [
            {"name": "eDP-1", "enabled": true, "connected": true, "priority": 1, "pos": {"x": 0, "y": 0}, "size": {"width": 2880, "height": 1800}, "scale": 1.75},
            {"name": "DP-3", "enabled": true, "connected": true, "priority": 2, "pos": {"x": 1646, "y": 0}, "size": {"width": 1920, "height": 1080}}
        ]})";
        const auto outputs = parse(scaled);
        QCOMPARE(outputs.size(), 2);
        QCOMPARE(outputs[0].scale, 1.75);
        QCOMPARE(outputs[1].scale, 1.0); // absent -> 1
        const auto monitors = hostMonitorsFrom(outputs);
        QCOMPARE(monitors[0].scale, 1.75);
        QCOMPARE(monitors[0].size, QSize(2880, 1800)); // native pixels, not divided by the scale
        QCOMPARE(monitors[1].scale, 1.0);
        // The state file keeps it too, and an old file without it reads as 1.
        QCOMPARE(fromJson(toJson(outputs)), outputs);
        QCOMPARE(fromJson(R"([{"name":"DP-1","enabled":true,"x":0,"y":0,"priority":1,"width":2560,"height":1440}])")[0].scale, 1.0);
    }

    // OPT-044: the one-invocation arrangement the layout executor asks for.
    void arrangementArgsPrioritiseEnabledInOrderAndDisableTheRest()
    {
        const QList<Arrangement> entries{
            {QStringLiteral("Virtual-krdp-si-DP-1-1920x1080-s100"), true, QPoint(0, 0)},
            {QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0)},
            {QStringLiteral("DP-1"), false, QPoint()},
        };
        const QStringList expected{
            QStringLiteral("output.Virtual-krdp-si-DP-1-1920x1080-s100.enable"),
            QStringLiteral("output.Virtual-krdp-si-DP-1-1920x1080-s100.position.0,0"),
            QStringLiteral("output.Virtual-krdp-si-DP-1-1920x1080-s100.priority.1"),
            QStringLiteral("output.HDMI-A-1.enable"),
            QStringLiteral("output.HDMI-A-1.position.2560,0"),
            QStringLiteral("output.HDMI-A-1.priority.2"),
            QStringLiteral("output.DP-1.disable"),
        };
        QCOMPARE(arrangementArgs(entries), expected);
        // The two-step form: disables alone, then everything else.
        QCOMPARE(arrangementDisableArgs(entries), QStringList{QStringLiteral("output.DP-1.disable")});
        QCOMPARE(arrangementEnableArgs(entries), expected.mid(0, 6));
    }

    void arrangementMatchesEnabledStateAndPositions()
    {
        const auto current = parse(kscreenJson); // DP-1 on 0,0; HDMI-A-1 on 2560,0; Virtual on 5120,0
        QVERIFY(arrangementMatches(
            {
                {QStringLiteral("DP-1"), true, QPoint(0, 0)},
                {QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0)},
                {QStringLiteral("Virtual-krdp-m0-1920x1080"), true, QPoint(5120, 0)},
            },
            current));
        // A wanted-disabled output that is enabled does not match.
        QVERIFY(!arrangementMatches({{QStringLiteral("DP-1"), false, QPoint()}}, current));
        // An enabled output at the wrong place does not match.
        QVERIFY(!arrangementMatches({{QStringLiteral("HDMI-A-1"), true, QPoint(0, 0)}}, current));
        // An output that is not there at all (churn, or never created) does not match, and is not present.
        QVERIFY(!arrangementMatches({{QStringLiteral("DP-2"), false, QPoint()}}, current));
        QVERIFY(!arrangementPresent({{QStringLiteral("DP-1"), true, QPoint()}, {QStringLiteral("DP-2"), false, QPoint()}}, current));
        QVERIFY(arrangementPresent({{QStringLiteral("DP-1"), true, QPoint()}, {QStringLiteral("HDMI-A-1"), false, QPoint()}}, current));
        // Priority is not part of the match: KWin may renumber.
        auto renumbered = current;
        renumbered[0].priority = 7;
        QVERIFY(arrangementMatches({{QStringLiteral("DP-1"), true, QPoint(0, 0)}}, renumbered));
        // A disabled output's position is irrelevant.
        auto dark = current;
        dark[0].enabled = false;
        dark[0].position = QPoint(999, 999);
        QVERIFY(arrangementMatches({{QStringLiteral("DP-1"), false, QPoint(0, 0)}}, dark));
    }
};

QTEST_GUILESS_MAIN(OutputSnapshotTest)

#include "OutputSnapshotTest.moc"
