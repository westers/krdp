// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-C-3: console Fit resize, the physical lease and PhysicalOutputGuard
// share one output-restore journal.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "ConsoleResizeSession.h"
#include "ConsoleTopologyLease.h"
#include "OutputRestoreJournal.h"
#include "PhysicalOutputGuard.h"

using namespace KRdp;
using Journal = OutputRestoreJournal;

namespace
{
QByteArray resizeSnapshot(bool resized)
{
    return QStringLiteral(R"({"outputs":[{"name":"DP-3","connected":true,"enabled":true,"currentModeId":"%1","scale":1,"modes":[
        {"id":"1","refreshRate":60,"size":{"width":1920,"height":1080}},
        {"id":"2","refreshRate":60,"size":{"width":1280,"height":720}}]}]})")
        .arg(resized ? 2 : 1)
        .toUtf8();
}

/// A stand-in `kscreen-doctor` on PATH that keeps its layout in a JSON file.
const char FakeKScreen[] = R"(#!/usr/bin/env python3
import json, os, sys
path = os.environ["FAKE_KSCREEN_STATE"]
state = json.load(open(path))
args = sys.argv[1:]
if args == ["-j"]:
    print(json.dumps(state))
    sys.exit(0)
if args and args[0].startswith("--"):
    sys.exit(0)
for arg in args:
    parts = arg.split(".")
    output = next(o for o in state["outputs"] if o["name"] == parts[1])
    key, value = parts[2], ".".join(parts[3:])
    if key == "enable": output["enabled"] = True
    elif key == "disable": output["enabled"] = False
    elif key == "position":
        x, y = value.split(",")
        output["pos"] = {"x": int(x), "y": int(y)}
    elif key == "priority": output["priority"] = int(value)
    elif key == "mode": output["currentModeId"] = value
    elif key == "scale": output["scale"] = float(value)
json.dump(state, open(path, "w"))
)";

QJsonObject fakeOutput(const QString &name, bool enabled, QPoint position, int priority, const QString &mode = QStringLiteral("1"))
{
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("connected"), true},
                       {QStringLiteral("enabled"), enabled},
                       {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), position.x()}, {QStringLiteral("y"), position.y()}}},
                       {QStringLiteral("priority"), priority},
                       {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080}}},
                       {QStringLiteral("scale"), 1.0},
                       {QStringLiteral("currentModeId"), mode},
                       {QStringLiteral("modes"), QJsonArray{}}};
}
}

class ConsoleOutputJournalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fitIsJournaledBeforeTheModeChangesAndDroppedWhenRestored()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        ConsoleResizeExecutor::Reply pending;
        QStringList args;
        QVector<Journal::Entry> journalAtApply;
        ConsoleResizeSession session(nullptr, [&](auto arguments, auto reply) {
            args = arguments;
            if (arguments.first().startsWith(QStringLiteral("output."))) journalAtApply = journal.entries();
            pending = reply;
        });
        session.setJournal(&journal, QStringLiteral("3"));
        session.setControl({1, true});
        session.request({11, 1, QStringLiteral("DP-3"), QSize(1280, 720), 1});
        std::exchange(pending, {})(true, resizeSnapshot(false)); // Discovery.
        QCOMPARE(args.first(), QStringLiteral("output.DP-3.mode.2"));
        // Written before kscreen-doctor ran the mode change.
        QCOMPARE(journalAtApply.size(), 1);
        const auto entry = journalAtApply.first();
        QCOMPARE(entry.owner, QString::fromLatin1(Journal::ConsoleResizeOwner));
        QCOMPARE(entry.pid, qint64(getpid()));
        QCOMPARE(entry.outputs.size(), 1);
        QCOMPARE(entry.outputs.first().original.mode, QStringLiteral("1"));
        QCOMPARE(entry.outputs.first().applied.mode, QStringLiteral("2"));
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, resizeSnapshot(true)); // Verified apply.
        QCOMPARE(journal.entries().size(), 1); // Still owed while the Fit is in place.

        // A crash now would leave the entry for the next worker. Instead the
        // controller leaves: the session restores and releases it.
        session.setControl({2, false});
        std::exchange(pending, {})(true, resizeSnapshot(true));
        QCOMPARE(args.first(), QStringLiteral("output.DP-3.mode.1"));
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, resizeSnapshot(false));
        QVERIFY(journal.entries().isEmpty());
        QVERIFY(!QFile::exists(journal.path()));
    }

    void failedRestoreKeepsTheEntry()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        ConsoleResizeExecutor::Reply pending;
        ConsoleResizeSession session(nullptr, [&](auto, auto reply) { pending = reply; });
        session.setJournal(&journal, QStringLiteral("3"));
        session.setControl({1, true});
        session.request({11, 1, QStringLiteral("DP-3"), QSize(1280, 720), 1});
        std::exchange(pending, {})(true, resizeSnapshot(false));
        std::exchange(pending, {})(true, {});
        std::exchange(pending, {})(true, resizeSnapshot(true));
        session.stop();
        std::exchange(pending, {})(false, {}); // Cannot even read the outputs.
        QCOMPARE(journal.entries().size(), 1); // Left for the next worker's replay.
    }

    void leaseJournalsOnlyChangedFields()
    {
        ConsoleTopologyLease::State lease;
        ConsoleTopologyPlan::OutputState dp{QStringLiteral("DP-1"), QPoint(0, 0), 1.5, {QStringLiteral("1"), QSize(2560, 1440), 60000}, {}};
        ConsoleTopologyPlan::OutputState hdmi{QStringLiteral("HDMI-A-1"), QPoint(1707, 0), 1.0, {QStringLiteral("3"), QSize(1920, 1080), 60000}, {}};
        lease.original.states = {{dp.name, dp}, {hdmi.name, hdmi}};
        lease.original.priorities = {{dp.name, 1}, {hdmi.name, 2}};
        lease.cumulative.afterPriorities = {{dp.name, 2}, {hdmi.name, 1}};
        lease.cumulative.modes.insert(dp.name, {QSize(1920, 1080), 1.0});
        lease.selected.insert(dp.name, {QStringLiteral("5"), QSize(1920, 1080), 60000});
        lease.cumulative.positions.insert(hdmi.name, QPoint(-1920, 0));
        const auto outputs = ConsoleTopologyLease::journalOutputs(lease);
        QCOMPARE(outputs.size(), 2);
        const auto &first = outputs[0].name == dp.name ? outputs[0] : outputs[1];
        const auto &second = outputs[0].name == dp.name ? outputs[1] : outputs[0];
        QCOMPARE(first.original.mode, QStringLiteral("1"));
        QCOMPARE(first.applied.mode, QStringLiteral("5"));
        QCOMPARE(first.original.scale, std::optional<double>(1.5));
        QCOMPARE(first.applied.scale, std::optional<double>(1.0));
        QCOMPARE(first.original.priority, std::optional<int>(1));
        QCOMPARE(first.applied.priority, std::optional<int>(2));
        QVERIFY(!first.original.position);
        QCOMPARE(second.original.position, std::optional<QPoint>(QPoint(1707, 0)));
        QCOMPARE(second.applied.position, std::optional<QPoint>(QPoint(-1920, 0)));
        QVERIFY(second.original.mode.isEmpty());
    }

    void guardAndConsoleEntriesShareOneJournal()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()) {
            QSKIP("python3 is needed for the fake kscreen-doctor");
        }
        QTemporaryDir directory;
        const QString bin = directory.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(bin));
        QFile script(bin + QStringLiteral("/kscreen-doctor"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(FakeKScreen);
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString state = directory.filePath(QStringLiteral("kscreen.json"));
        {
            // A crashed virtual-monitor krdpserver left both panels off, and a
            // crashed console worker left its Fit mode on DP-1.
            QJsonObject root{{QStringLiteral("outputs"),
                              QJsonArray{fakeOutput(QStringLiteral("DP-1"), false, QPoint(0, 0), 2, QStringLiteral("7")),
                                         fakeOutput(QStringLiteral("HDMI-A-1"), false, QPoint(2560, 0), 1)}}};
            QFile file(state);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(root).toJson());
        }
        qputenv("PATH", (bin + QStringLiteral(":") + qEnvironmentVariable("PATH")).toUtf8());
        qputenv("FAKE_KSCREEN_STATE", state.toUtf8());
        const QString journalPath = directory.filePath(QStringLiteral("state/farside/output-restore.json"));
        qputenv("FARSIDE_OUTPUT_RESTORE_JOURNAL", journalPath.toUtf8());
        QCOMPARE(PhysicalOutputGuard::stateFilePath(), journalPath);

        Journal journal;
        Journal::Fields dp;
        dp.enabled = true;
        dp.position = QPoint(0, 0);
        dp.priority = 1;
        Journal::Fields hdmi;
        hdmi.enabled = true;
        hdmi.position = QPoint(2560, 0);
        hdmi.priority = 2;
        const qint64 deadPid = 0x3fffffff; // No such process.
        QVERIFY(journal.hold({QString::fromLatin1(Journal::OutputGuardOwner), deadPid, {}, {{QStringLiteral("DP-1"), dp, {}}, {QStringLiteral("HDMI-A-1"), hdmi, {}}}}));
        Journal::Fields fitOriginal;
        fitOriginal.mode = QStringLiteral("1");
        Journal::Fields fitApplied;
        fitApplied.mode = QStringLiteral("7");
        QVERIFY(journal.hold({QString::fromLatin1(Journal::ConsoleResizeOwner), deadPid, QStringLiteral("3"), {{QStringLiteral("DP-1"), fitOriginal, fitApplied}}}));
        QCOMPARE(journal.entries().size(), 2);
        QVERIFY(PhysicalOutputGuard::restoreFromStateFile());
        QVERIFY(journal.entries().isEmpty());
        QFile file(state);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto outputs = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("outputs")).toArray();
        QVERIFY(outputs[0].toObject().value(QStringLiteral("enabled")).toBool());
        QVERIFY(outputs[1].toObject().value(QStringLiteral("enabled")).toBool());
        QCOMPARE(outputs[0].toObject().value(QStringLiteral("priority")).toInt(), 1);
        QCOMPARE(outputs[0].toObject().value(QStringLiteral("currentModeId")).toString(), QStringLiteral("1")); // Console entry replayed too.
        qunsetenv("FARSIDE_OUTPUT_RESTORE_JOURNAL");
    }
};

QTEST_GUILESS_MAIN(ConsoleOutputJournalTest)

#include "ConsoleOutputJournalTest.moc"
