// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <algorithm>

#include <QTest>

#include "LayoutArrangement.h"
#include "OutputSnapshot.h"

using namespace KRdp::OutputSnapshot;
using namespace KRdp::ClientDisplay;
using namespace KRdp::LayoutArrangement;
using KRdp::LayoutControl::HostMonitor;
using KRdp::LayoutControl::Kind;
using KRdp::LayoutControl::Layout;

namespace
{
HostMonitor realMonitor(const QString &id, const QPoint &position, bool primary, bool lit = true)
{
    HostMonitor monitor;
    monitor.id = id;
    monitor.name = id;
    monitor.kind = Kind::Real;
    monitor.size = QSize(2560, 1440);
    monitor.position = position;
    monitor.scale = 1.0;
    monitor.primary = primary;
    monitor.lit = lit;
    return monitor;
}

HostMonitor virtualMonitor(const QString &id, const QPoint &position, const QSize &size, qreal scale, const QString &owner)
{
    HostMonitor monitor;
    monitor.id = id;
    monitor.name = id;
    monitor.kind = Kind::Virtual;
    monitor.size = size;
    monitor.position = position;
    monitor.scale = scale;
    monitor.owner = owner;
    return monitor;
}

std::optional<Arrangement> entryNamed(const QList<Arrangement> &entries, const QString &name)
{
    const auto it = std::find_if(entries.cbegin(), entries.cend(), [&name](const Arrangement &entry) {
        return entry.name == name;
    });
    return it == entries.cend() ? std::nullopt : std::optional(*it);
}
}

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

    void fractionalScaleUnionAndRightmostAnchorUseLogicalCoordinates()
    {
        QVector<Output> outputs{
            {QStringLiteral("DP-1"), true, QPoint(-100, 100), 1, QSize(1280, 720), 1.0},
            {QStringLiteral("HDMI-A-1"), true, QPoint(1180, 300), 2, QSize(1600, 900), 1.25},
        };
        QCOMPARE(enabledUnion(outputs), QRect(-100, 100, 2560, 920));
        QCOMPARE(rightmostEnabledAnchor(outputs), QPoint(2460, 300));
        outputs[1].enabled = false;
        QCOMPARE(rightmostEnabledAnchor(outputs), QPoint(1180, 100));
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

    // LayoutArrangement::derive(): the executor's pure half.
    void deriveNames()
    {
        QCOMPARE(standInName(QStringLiteral("DP-1"), QSize(1920, 1080), 1.0), QStringLiteral("krdp-si-DP-1-1920x1080-s100"));
        QCOMPARE(virtualName(QStringLiteral("virtual-1"), QSize(1920, 1080), 1.25), QStringLiteral("krdp-v1-1920x1080-s125"));
        QVERIFY(!standInName(QStringLiteral("DP-1"), QSize(1920, 1080), 1.5).contains(u'.'));
        QCOMPARE(logicalWidth(QSize(1920, 1080), 1.25), 1536);
        QCOMPARE(logicalWidth(QSize(2560, 1440), 1.0), 2560);
    }

    void derivePrivateFromIdle()
    {
        Layout resulting;
        resulting.monitors = {realMonitor(QStringLiteral("DP-1"), QPoint(0, 0), true, false), realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false, false)};
        const auto derived = derive(resulting, {}, QStringLiteral("c1"));
        // Two native-size stand-ins, primary's first, then the two disables; nothing removed.
        QCOMPARE(derived.wanted.size(), 2);
        QCOMPARE(derived.creating, derived.wanted);
        QCOMPARE(derived.wanted[0].name, QStringLiteral("Virtual-krdp-si-DP-1-2560x1440-s100"));
        QCOMPARE(derived.wanted[0].position, QPoint(0, 0));
        QVERIFY(!derived.wanted[0].standIn);
        QCOMPARE(derived.wanted[0].owner, QStringLiteral("c1"));
        QCOMPARE(derived.arrangement.size(), 4);
        QCOMPARE(derived.arrangement[0], (Arrangement{QStringLiteral("Virtual-krdp-si-DP-1-2560x1440-s100"), true, QPoint(0, 0)}));
        QCOMPARE(derived.arrangement[1], (Arrangement{QStringLiteral("Virtual-krdp-si-HDMI-A-1-2560x1440-s100"), true, QPoint(2560, 0)}));
        QCOMPARE(derived.arrangement[2], (Arrangement{QStringLiteral("DP-1"), false, QPoint()}));
        QCOMPARE(derived.arrangement[3], (Arrangement{QStringLiteral("HDMI-A-1"), false, QPoint()}));
        QVERIFY(derived.removing.isEmpty());
        QVERIFY(derived.physicalDisabled);
        QCOMPARE(derived.neededScreens, (QStringList{QStringLiteral("Virtual-krdp-si-DP-1-2560x1440-s100"), QStringLiteral("Virtual-krdp-si-HDMI-A-1-2560x1440-s100")}));
        QCOMPARE(derived.parkAnchor, QPoint(5120, 0));
    }

    void deriveParksARemovedStandInBeyondAnExtraThatStays()
    {
        // Review Important 2, example 1: a Fit stand-in on DP-1 plus virtual-1
        // at (5120,0); the client un-Fits DP-1. The stand-in must be parked
        // beyond virtual-1 (7040), not at the physical union's edge (5120)
        // where virtual-1 sits.
        const VirtualOutput standIn{QStringLiteral("DP-1"), QStringLiteral("Virtual-krdp-si-DP-1-1920x1080-s100"), QStringLiteral("c1"), QSize(1920, 1080), 1.0, QPoint(0, 0), true};
        const VirtualOutput extra{QStringLiteral("virtual-1"), QStringLiteral("Virtual-krdp-v1-1920x1080-s100"), QStringLiteral("c1"), QSize(1920, 1080), 1.0, QPoint(5120, 0), false};
        Layout resulting;
        resulting.monitors = {realMonitor(QStringLiteral("DP-1"), QPoint(0, 0), true),
                              realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false),
                              virtualMonitor(QStringLiteral("virtual-1"), QPoint(5120, 0), QSize(1920, 1080), 1.0, QStringLiteral("c1"))};
        const auto derived = derive(resulting, {standIn, extra}, QStringLiteral("c1"));
        QCOMPARE(derived.removing, QStringList{standIn.name});
        QVERIFY(derived.creating.isEmpty());
        QCOMPARE(derived.parkAnchor, QPoint(7040, 0));
        const auto parked = entryNamed(derived.arrangement, standIn.name);
        QVERIFY(parked.has_value());
        QVERIFY(parked->enabled);
        QCOMPARE(parked->position, QPoint(7040, 0));
        // virtual-1 keeps its place, DP-1 comes back on at its own.
        QCOMPARE(entryNamed(derived.arrangement, extra.name)->position, QPoint(5120, 0));
        QCOMPARE(entryNamed(derived.arrangement, QStringLiteral("DP-1"))->enabled, true);
        QCOMPARE(entryNamed(derived.arrangement, QStringLiteral("DP-1"))->position, QPoint(0, 0));
        QVERIFY(!derived.physicalDisabled);
        // The parked entry comes last, so it takes the lowest priority.
        QCOMPARE(derived.arrangement.last().name, standIn.name);
        // No two enabled entries share a position.
        for (qsizetype i = 0; i < derived.arrangement.size(); ++i) {
            for (qsizetype j = i + 1; j < derived.arrangement.size(); ++j) {
                if (derived.arrangement[i].enabled && derived.arrangement[j].enabled) {
                    QVERIFY(derived.arrangement[i].position != derived.arrangement[j].position);
                }
            }
        }
    }

    void deriveParksTheSecondOfTwoExtrasBeyondTheFirst()
    {
        // Review Important 2, example 2: virtual-1 at (5120,0) and virtual-2 at
        // (7040,0); the client keeps virtual-1 only. virtual-2 parks at 7040
        // (right of virtual-1), never at 5120 on top of it.
        const VirtualOutput first{QStringLiteral("virtual-1"), QStringLiteral("Virtual-krdp-v1-1920x1080-s100"), QStringLiteral("c1"), QSize(1920, 1080), 1.0, QPoint(5120, 0), false};
        const VirtualOutput second{QStringLiteral("virtual-2"), QStringLiteral("Virtual-krdp-v2-1920x1080-s125"), QStringLiteral("c1"), QSize(1920, 1080), 1.25, QPoint(7040, 0), false};
        Layout resulting;
        resulting.monitors = {realMonitor(QStringLiteral("DP-1"), QPoint(0, 0), true),
                              realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false),
                              virtualMonitor(QStringLiteral("virtual-1"), QPoint(5120, 0), QSize(1920, 1080), 1.0, QStringLiteral("c1"))};
        const auto derived = derive(resulting, {first, second}, QStringLiteral("c1"));
        QCOMPARE(derived.removing, QStringList{second.name});
        QCOMPARE(derived.parkAnchor, QPoint(7040, 0));
        QCOMPARE(entryNamed(derived.arrangement, second.name)->position, QPoint(7040, 0));
        QCOMPARE(entryNamed(derived.arrangement, first.name)->position, QPoint(5120, 0));
        // Two removals stack left to right by their logical width (1536 at 125 %).
        const VirtualOutput third{QStringLiteral("virtual-3"), QStringLiteral("Virtual-krdp-v3-1920x1080-s100"), QStringLiteral("c1"), QSize(1920, 1080), 1.0, QPoint(8576, 0), false};
        const auto both = derive(resulting, {first, second, third}, QStringLiteral("c1"));
        QCOMPARE(both.removing, (QStringList{second.name, third.name}));
        QCOMPARE(entryNamed(both.arrangement, second.name)->position, QPoint(7040, 0));
        QCOMPARE(entryNamed(both.arrangement, third.name)->position, QPoint(7040 + 1536, 0));
    }

    void deriveKeepsExistingOutputsAndResolvesNames()
    {
        const VirtualOutput extra{QStringLiteral("virtual-1"), QStringLiteral("Virtual-krdp-v1-1920x1080-s125"), QStringLiteral("c1"), QSize(1920, 1080), 1.25, QPoint(5120, 0), false};
        Layout resulting;
        resulting.monitors = {realMonitor(QStringLiteral("DP-1"), QPoint(0, 0), true),
                              realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false),
                              virtualMonitor(QStringLiteral("virtual-1"), QPoint(5120, 0), QSize(1920, 1080), 1.25, QStringLiteral("c1"))};
        const auto derived = derive(resulting, {extra}, QStringLiteral("c1"));
        QVERIFY(derived.creating.isEmpty());
        QVERIFY(derived.removing.isEmpty());
        QCOMPARE(derived.wanted, QList<VirtualOutput>{extra});
        QCOMPARE(derived.parkAnchor, QPoint(5120 + 1536, 0));
        QCOMPARE(outputNameFor(resulting, {extra}, QStringLiteral("DP-1")), QStringLiteral("DP-1"));
        QCOMPARE(outputNameFor(resulting, {extra}, QStringLiteral("virtual-1")), extra.name);
        QVERIFY(outputNameFor(resulting, {extra}, QStringLiteral("virtual-9")).isEmpty());
        auto dark = resulting;
        dark.monitors[0].lit = false;
        // A dark real monitor without its stand-in in the table resolves to nothing (not streamable yet).
        QVERIFY(outputNameFor(dark, {extra}, QStringLiteral("DP-1")).isEmpty());
        const VirtualOutput standIn{QStringLiteral("DP-1"), QStringLiteral("Virtual-krdp-si-DP-1-2560x1440-s100"), QStringLiteral("c1"), QSize(2560, 1440), 1.0, QPoint(0, 0), false};
        QCOMPARE(outputNameFor(dark, {extra, standIn}, QStringLiteral("DP-1")), standIn.name);
    }

    // The re-assert after a removal (hardware finding, 2026-09-19 step 4).
    void reassertTargetAfterARemovalDetectsKWinsReplay()
    {
        // Step 4, apply 2: the desk is Private (native stand-ins on both
        // monitors) and the client Fits DP-1 to 1920x1080. The arrangement
        // parks the old DP-1 stand-in at 5120,0 and the executor removes it;
        // KWin then re-queried its configuration for the four remaining
        // outputs and lit the desk: DP-1 and HDMI-A-1 back on at 0,0 and
        // 2560,0, the HDMI stand-in at 0,0, the new stand-in at 5120,0
        // (kscreen poll, 11:30:33).
        const Layout fitted = fittedPrivateLayout();
        const auto derived = derive(fitted, {oldDp1StandIn(), hdmiStandIn()}, QStringLiteral("c1"));
        QCOMPARE(derived.removing, QStringList{oldDp1StandIn().name});
        QCOMPARE(derived.creating.size(), 1);
        QCOMPARE(derived.creating.first().name, newDp1StandIn().name);
        QCOMPARE(derived.arrangement.size(), 5);
        QCOMPARE(entryNamed(derived.arrangement, oldDp1StandIn().name)->position, QPoint(5120, 0));

        // What is re-asserted once the parked output is gone: the target
        // without it, in the same order (priorities 1..N unchanged).
        const auto target = arrangementWithout(derived.arrangement, derived.removing);
        QCOMPARE(target.size(), 4);
        QVERIFY(!entryNamed(target, oldDp1StandIn().name).has_value());
        QCOMPARE(target[0], (Arrangement{newDp1StandIn().name, true, QPoint(0, 0)}));
        QCOMPARE(target[1], (Arrangement{hdmiStandIn().name, true, QPoint(2560, 0)}));
        QCOMPARE(target[2], (Arrangement{QStringLiteral("DP-1"), false, QPoint()}));
        QCOMPARE(target[3], (Arrangement{QStringLiteral("HDMI-A-1"), false, QPoint()}));
        // arrangementWithout() is by name only; nothing to remove is a copy.
        QCOMPARE(arrangementWithout(derived.arrangement, {}), derived.arrangement);
        QCOMPARE(arrangementWithout(derived.arrangement, {QStringLiteral("nothing-of-the-sort")}), derived.arrangement);

        // KWin's replay: detected as not matching.
        QVERIFY(!arrangementMatches(target, replayedReadBack()));
        // Only the physical outputs back on: not matching.
        QVector<Output> desertedDesk = heldReadBack();
        desertedDesk[0].enabled = true;
        desertedDesk[1].enabled = true;
        QVERIFY(!arrangementMatches(target, desertedDesk));
        // Only a stand-in moved: not matching.
        QVector<Output> standInMoved = heldReadBack();
        standInMoved[2].position = QPoint(5120, 0);
        QVERIFY(!arrangementMatches(target, standInMoved));
        // The target held (priorities renumbered by KWin are fine): matching,
        // and only against the target - the full arrangement can never match
        // again once the parked output has gone.
        QVERIFY(arrangementMatches(target, heldReadBack()));
        QVERIFY(!arrangementMatches(derived.arrangement, heldReadBack()));
        QVERIFY(!arrangementPresent(derived.arrangement, heldReadBack()));
        QVERIFY(arrangementPresent(target, heldReadBack()));
    }

    void appliedLayoutKeepsItsPositionsAndOnlyAFailedApplyReportsTheReadBack()
    {
        // The executor's current() is the applied layout as targeted: after
        // the replay above, DP-1 (dark, stood in) is still at 0,0 in it and
        // the next plan lights it there. Only an apply whose arrangement
        // could not be verified reports where KWin really put things.
        const Layout applied = fittedPrivateLayout();
        const QList<VirtualOutput> table{newDp1StandIn(), hdmiStandIn()};
        QCOMPARE(applied.monitors[0].position, QPoint(0, 0));
        QCOMPARE(applied.monitors[1].position, QPoint(2560, 0));

        const Layout reported = withReadBackPositions(applied, table, replayedReadBack());
        // DP-1 is carried by its (new) stand-in, which the replay put at 5120,0.
        QCOMPARE(reported.monitors[0].position, QPoint(5120, 0));
        // HDMI-A-1 by its stand-in, replayed to 0,0.
        QCOMPARE(reported.monitors[1].position, QPoint(0, 0));
        // Everything but the positions is the applied layout.
        Layout expected = applied;
        expected.monitors[0].position = QPoint(5120, 0);
        expected.monitors[1].position = QPoint(0, 0);
        QCOMPARE(reported, expected);
        // A read-back that holds the target changes nothing.
        QCOMPARE(withReadBackPositions(applied, table, heldReadBack()), applied);

        // A lit real monitor is carried by its connector; a disabled carrier
        // (its position from kscreen is stale) and a monitor with no carrier
        // in the table are left alone.
        Layout lit;
        lit.monitors = {realMonitor(QStringLiteral("DP-1"), QPoint(0, 0), true), realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false, false)};
        const QVector<Output> readBack{
            Output{QStringLiteral("DP-1"), true, QPoint(100, 0), 1, QSize(2560, 1440), 1.0},
            Output{QStringLiteral("HDMI-A-1"), false, QPoint(9999, 0), 0, QSize(2560, 1440), 1.0},
        };
        const Layout litReported = withReadBackPositions(lit, {}, readBack);
        QCOMPARE(litReported.monitors[0].position, QPoint(100, 0));
        QCOMPARE(litReported.monitors[1].position, QPoint(2560, 0));
    }

    void alignRealMonitorsToSnapshotMovesOnlyRealMonitors()
    {
        // A plan made from a read that disagrees with the guard's snapshot
        // (the desk rearranged in between) is aligned to the snapshot before
        // it is derived; virtual monitors keep their planned places.
        const QVector<Output> snapshot{
            Output{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(2560, 1440), 1.0},
            Output{QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0), 2, QSize(2560, 1440), 1.0},
        };
        Layout layout;
        layout.monitors = {realMonitor(QStringLiteral("DP-1"), QPoint(5120, 0), true),
                           realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false, false),
                           virtualMonitor(QStringLiteral("virtual-1"), QPoint(7040, 0), QSize(1920, 1080), 1.0, QStringLiteral("c1"))};
        QVERIFY(alignRealMonitorsToSnapshot(layout, snapshot));
        QCOMPARE(layout.monitors[0].position, QPoint(0, 0));
        QCOMPARE(layout.monitors[1].position, QPoint(2560, 0));
        QCOMPARE(layout.monitors[2].position, QPoint(7040, 0));
        // Idempotent, and a monitor the snapshot does not know is left alone.
        QVERIFY(!alignRealMonitorsToSnapshot(layout, snapshot));
        layout.monitors.push_back(realMonitor(QStringLiteral("DP-2"), QPoint(-1920, 0), false));
        QVERIFY(!alignRealMonitorsToSnapshot(layout, snapshot));
        QCOMPARE(layout.monitors[3].position, QPoint(-1920, 0));
    }

    void serialCreationStartsTheNextCreatorOnlyAfterTheLastResolved()
    {
        // OPT-047 mitigation 1: a three-output apply (Private + one extra,
        // the shape that crashed KWin on 2026-09-19) is created one output
        // at a time. The executor asks nextCreatorToStart() after each
        // resolve and arranges only on allCreatorsResolved().
        QList<CreatorState> states{{}, {}, {}};
        // Nothing started: the first is next, nothing is resolved.
        QCOMPARE(nextCreatorToStart(states), 0);
        QVERIFY(!allCreatorsResolved(states));
        // A started, still resolving: nobody may start.
        states[0].started = true;
        QCOMPARE(nextCreatorToStart(states), -1);
        QVERIFY(!allCreatorsResolved(states));
        // A resolved: B is next.
        states[0].resolved = true;
        QCOMPARE(nextCreatorToStart(states), 1);
        QVERIFY(!allCreatorsResolved(states));
        // B started and resolving: hold - even though C is unstarted.
        states[1].started = true;
        QCOMPARE(nextCreatorToStart(states), -1);
        // KWin takes A's screen away for a moment while B resolves: still
        // hold, and B resolving alone does not release it.
        states[0].resolved = false;
        states[1].resolved = true;
        QCOMPARE(nextCreatorToStart(states), -1);
        QVERIFY(!allCreatorsResolved(states));
        // A back: C is next.
        states[0].resolved = true;
        QCOMPARE(nextCreatorToStart(states), 2);
        // C started: all started, so no next, whether or not it has resolved.
        states[2].started = true;
        QCOMPARE(nextCreatorToStart(states), -1);
        QVERIFY(!allCreatorsResolved(states));
        states[2].resolved = true;
        QCOMPARE(nextCreatorToStart(states), -1);
        QVERIFY(allCreatorsResolved(states));
        // A restating apply creates nothing: it arranges at once.
        QCOMPARE(nextCreatorToStart({}), -1);
        QVERIFY(allCreatorsResolved({}));
        // A single output is started first and arranged as soon as it resolves.
        QList<CreatorState> one{{}};
        QCOMPARE(nextCreatorToStart(one), 0);
        one[0].started = true;
        QCOMPARE(nextCreatorToStart(one), -1);
        QVERIFY(!allCreatorsResolved(one));
        one[0].resolved = true;
        QVERIFY(allCreatorsResolved(one));
    }

private:
    // Step 4 of the 2026-09-19 hardware run, as data.
    static VirtualOutput oldDp1StandIn()
    {
        return VirtualOutput{QStringLiteral("DP-1"), QStringLiteral("Virtual-krdp-si-DP-1-2560x1440-s100"), QStringLiteral("c1"), QSize(2560, 1440), 1.0, QPoint(0, 0), false};
    }
    static VirtualOutput newDp1StandIn()
    {
        return VirtualOutput{QStringLiteral("DP-1"), QStringLiteral("Virtual-krdp-si-DP-1-1920x1080-s100"), QStringLiteral("c1"), QSize(1920, 1080), 1.0, QPoint(0, 0), true};
    }
    static VirtualOutput hdmiStandIn()
    {
        return VirtualOutput{QStringLiteral("HDMI-A-1"), QStringLiteral("Virtual-krdp-si-HDMI-A-1-2560x1440-s100"), QStringLiteral("c1"), QSize(2560, 1440), 1.0, QPoint(2560, 0), false};
    }
    /** The layout apply 2 results in: DP-1 stood in at 1920x1080, HDMI-A-1 dark at its native size. */
    static Layout fittedPrivateLayout()
    {
        HostMonitor dp1 = realMonitor(QStringLiteral("DP-1"), QPoint(0, 0), true, false);
        dp1.standIn = true;
        dp1.standInSize = QSize(1920, 1080);
        dp1.standInScale = 1.0;
        Layout layout;
        layout.monitors = {dp1, realMonitor(QStringLiteral("HDMI-A-1"), QPoint(2560, 0), false, false)};
        return layout;
    }
    /** kscreen poll 11:30:33: what KWin made of the four outputs left after the old stand-in went. */
    static QVector<Output> replayedReadBack()
    {
        return {
            Output{QStringLiteral("DP-1"), true, QPoint(0, 0), 2, QSize(2560, 1440), 1.0},
            Output{QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0), 3, QSize(2560, 1440), 1.0},
            Output{hdmiStandIn().name, true, QPoint(0, 0), 1, QSize(2560, 1440), 1.0},
            Output{newDp1StandIn().name, true, QPoint(5120, 0), 4, QSize(1920, 1080), 1.0},
        };
    }
    /**
     * The read-back the re-assert must produce: the arrangement apply 2 made
     * (kscreen poll 11:30:30 shows the outputs before it, with the new
     * stand-in still at 5120,0 prio 3) minus the parked, now removed,
     * old stand-in - KWin's priority numbering.
     */
    static QVector<Output> heldReadBack()
    {
        return {
            Output{QStringLiteral("DP-1"), false, QPoint(0, 0), 3, QSize(2560, 1440), 1.0},
            Output{QStringLiteral("HDMI-A-1"), false, QPoint(2560, 0), 4, QSize(2560, 1440), 1.0},
            Output{newDp1StandIn().name, true, QPoint(0, 0), 1, QSize(1920, 1080), 1.0},
            Output{hdmiStandIn().name, true, QPoint(2560, 0), 2, QSize(2560, 1440), 1.0},
        };
    }
};

QTEST_GUILESS_MAIN(OutputSnapshotTest)

#include "OutputSnapshotTest.moc"
