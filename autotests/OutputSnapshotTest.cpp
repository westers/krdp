// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "OutputSnapshot.h"

using namespace KRdp::OutputSnapshot;

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
};

QTEST_GUILESS_MAIN(OutputSnapshotTest)

#include "OutputSnapshotTest.moc"
