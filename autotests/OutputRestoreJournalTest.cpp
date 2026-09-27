// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-C-3: the shared, atomically written output-restore journal and its
// replay against a fake kscreen-doctor.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include "OutputRestoreJournal.h"

using namespace KRdp;
using Journal = OutputRestoreJournal;

namespace
{
/// A tiny KScreen: `kscreen-doctor -j` and the mutation arguments replay uses.
struct FakeScreen {
    QMap<QString, Journal::Current> outputs;
    QList<QStringList> commands;
    bool ignoreCommands = false;

    QByteArray json() const
    {
        QJsonArray array;
        for (const auto &output : outputs) {
            array.append(QJsonObject{{QStringLiteral("name"), output.name},
                                     {QStringLiteral("connected"), true},
                                     {QStringLiteral("enabled"), output.enabled},
                                     {QStringLiteral("currentModeId"), output.mode},
                                     {QStringLiteral("scale"), output.scale},
                                     {QStringLiteral("priority"), output.priority},
                                     {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), output.position.x()}, {QStringLiteral("y"), output.position.y()}}}});
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("outputs"), array}}).toJson();
    }

    bool apply(const QStringList &arguments)
    {
        commands.append(arguments);
        if (ignoreCommands) return true; // Helper claims success, nothing changes.
        for (const auto &argument : arguments) {
            const auto parts = argument.split(u'.');
            if (parts.size() < 3 || parts[0] != u"output" || !outputs.contains(parts[1])) return false;
            auto &output = outputs[parts[1]];
            const QString value = parts.mid(3).join(u'.');
            if (parts[2] == u"enable") output.enabled = true;
            else if (parts[2] == u"disable") output.enabled = false;
            else if (parts[2] == u"mode") output.mode = value;
            else if (parts[2] == u"scale") output.scale = value.toDouble();
            else if (parts[2] == u"priority") output.priority = value.toInt();
            else if (parts[2] == u"position") {
                const auto xy = value.split(u',');
                output.position = QPoint(xy.value(0).toInt(), xy.value(1).toInt());
            }
        }
        return true;
    }

    Journal::Read reader() { return [this] { return std::optional<QByteArray>(json()); }; }
    Journal::Apply applier() { return [this](const QStringList &arguments) { return apply(arguments); }; }
};

FakeScreen twoMonitors()
{
    FakeScreen screen;
    screen.outputs.insert(QStringLiteral("DP-1"), {QStringLiteral("DP-1"), true, QStringLiteral("1"), 1.5, QPoint(0, 0), 1});
    screen.outputs.insert(QStringLiteral("HDMI-A-1"), {QStringLiteral("HDMI-A-1"), true, QStringLiteral("3"), 1.0, QPoint(1707, 0), 2});
    return screen;
}

Journal::Entry resizeEntry(qint64 pid)
{
    Journal::Fields original;
    original.mode = QStringLiteral("1");
    original.scale = 1.5;
    Journal::Fields applied;
    applied.mode = QStringLiteral("7");
    applied.scale = 1.0;
    return {QString::fromLatin1(Journal::ConsoleResizeOwner), pid, QStringLiteral("3"), {{QStringLiteral("DP-1"), original, applied}}};
}

const auto dead = [](qint64) { return false; };
}

class OutputRestoreJournalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void roundTripAndAtomicFile()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("state/krdp/output-restore.json")));
        auto entry = resizeEntry(4242);
        QVERIFY(journal.hold(entry));
        QVERIFY(QFile::exists(journal.path()));
        QVERIFY(!QFile::exists(journal.path() + QStringLiteral(".tmp"))); // Renamed into place.
        struct stat info{};
        QCOMPARE(::stat(QFile::encodeName(journal.path()).constData(), &info), 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
        QCOMPARE(journal.entries(), QVector<Journal::Entry>{entry});

        Journal::Fields guardOriginal;
        guardOriginal.enabled = true;
        guardOriginal.position = QPoint(1707, 0);
        guardOriginal.priority = 2;
        const Journal::Entry guard{QString::fromLatin1(Journal::OutputGuardOwner), 77, {}, {{QStringLiteral("HDMI-A-1"), guardOriginal, {}}}};
        QVERIFY(journal.hold(guard));
        QCOMPARE(journal.entries().size(), 2);
        entry.outputs[0].applied.mode = QStringLiteral("9"); // Re-hold replaces, never duplicates.
        QVERIFY(journal.hold(entry));
        QCOMPARE(journal.entries().size(), 2);
        QVERIFY(journal.release(entry.owner, entry.pid));
        QCOMPARE(journal.entries(), QVector<Journal::Entry>{guard});
        QVERIFY(journal.release(guard.owner, guard.pid));
        QVERIFY(!QFile::exists(journal.path())); // Nothing held: no file.
        QVERIFY(journal.entries().isEmpty());
        QVERIFY(!Journal::parse("{\"format\":\"krdp-output-restore\",\"version\":2,\"entries\":[]}")); // Only version 1.
    }

    void replayRestoresOnlyWhatIsStillOurs()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        auto screen = twoMonitors();
        screen.outputs[QStringLiteral("DP-1")].mode = QStringLiteral("7"); // Our Fit is still applied...
        screen.outputs[QStringLiteral("DP-1")].scale = 1.25; // ... but someone changed the scale since.
        QVERIFY(journal.hold(resizeEntry(4242)));
        const auto result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.restored, 1);
        QCOMPARE(screen.commands, (QList<QStringList>{{QStringLiteral("output.DP-1.mode.1")}}));
        QCOMPARE(screen.outputs[QStringLiteral("DP-1")].mode, QStringLiteral("1"));
        QCOMPARE(screen.outputs[QStringLiteral("DP-1")].scale, 1.25); // Local change survives.
        QVERIFY(journal.entries().isEmpty());
    }

    void replaySkipsLiveOwnersAndKeepsUnverified()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        auto screen = twoMonitors();
        screen.outputs[QStringLiteral("DP-1")].mode = QStringLiteral("7");
        screen.outputs[QStringLiteral("DP-1")].scale = 1.0;
        QVERIFY(journal.hold(resizeEntry(4242)));
        auto result = journal.replay(screen.reader(), screen.applier(), [](qint64 pid) { return pid == 4242; });
        QCOMPARE(result.skippedLive, 1);
        QVERIFY(screen.commands.isEmpty());

        screen.ignoreCommands = true; // kscreen-doctor exits 0 but nothing changes.
        result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.kept, 1);
        QCOMPARE(journal.entries().size(), 1); // Retried at the next replay.

        screen.ignoreCommands = false;
        result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.restored, 1);
        QCOMPARE(screen.outputs[QStringLiteral("DP-1")].mode, QStringLiteral("1"));
        QCOMPARE(screen.outputs[QStringLiteral("DP-1")].scale, 1.5);
    }

    void unconditionalGuardEntryAndMissingOutputs()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        auto screen = twoMonitors();
        screen.outputs[QStringLiteral("DP-1")].enabled = false; // Replaced by a virtual monitor.
        screen.outputs[QStringLiteral("HDMI-A-1")].enabled = false;
        Journal::Fields dp;
        dp.enabled = true;
        dp.position = QPoint(0, 0);
        dp.priority = 1;
        Journal::Fields hdmi;
        hdmi.enabled = true;
        hdmi.position = QPoint(1707, 0);
        hdmi.priority = 2;
        QVERIFY(journal.hold({QString::fromLatin1(Journal::OutputGuardOwner), 55, {}, {{QStringLiteral("DP-1"), dp, {}}, {QStringLiteral("HDMI-A-1"), hdmi, {}}}}));
        auto result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.restored, 1);
        QVERIFY(screen.outputs[QStringLiteral("DP-1")].enabled);
        QVERIFY(screen.outputs[QStringLiteral("HDMI-A-1")].enabled);

        // A conditional entry for an unplugged monitor waits for it ...
        screen.outputs.remove(QStringLiteral("DP-1"));
        QVERIFY(journal.hold(resizeEntry(4243)));
        result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.kept, 1);
        QCOMPARE(journal.entries().size(), 1);
        // ... an unconditional snapshot is set aside instead of replayed forever.
        QVERIFY(journal.hold({QString::fromLatin1(Journal::OutputGuardOwner), 56, {}, {{QStringLiteral("DP-1"), dp, {}}, {QStringLiteral("HDMI-A-1"), hdmi, {}}}}));
        result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.restored, 1);
        QCOMPARE(journal.entries().size(), 1); // Only the conditional one remains.
        QVERIFY(QFile::exists(journal.path() + QStringLiteral(".stale")));
    }

    void prioritiesAreRestoredAsOneOrdering()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        auto screen = twoMonitors();
        // Our lease swapped the primary; then the user moved HDMI themselves.
        screen.outputs[QStringLiteral("DP-1")].priority = 2;
        screen.outputs[QStringLiteral("HDMI-A-1")].priority = 1;
        Journal::Fields dpOriginal, dpApplied, hdmiOriginal, hdmiApplied;
        dpOriginal.priority = 1;
        dpApplied.priority = 2;
        hdmiOriginal.priority = 2;
        hdmiApplied.priority = 1;
        hdmiOriginal.position = QPoint(1707, 0);
        hdmiApplied.position = QPoint(-1920, 0);
        screen.outputs[QStringLiteral("HDMI-A-1")].position = QPoint(-1000, 0);
        const Journal::Entry lease{QString::fromLatin1(Journal::ConsoleLeaseOwner), 9, QStringLiteral("3"),
                                   {{QStringLiteral("DP-1"), dpOriginal, dpApplied}, {QStringLiteral("HDMI-A-1"), hdmiOriginal, hdmiApplied}}};
        QVERIFY(journal.hold(lease));
        QCOMPARE(Journal::plan(lease, Journal::parseCurrent(screen.json())).arguments,
                 (QStringList{QStringLiteral("output.DP-1.priority.1"), QStringLiteral("output.HDMI-A-1.priority.2")}));
        screen.outputs[QStringLiteral("DP-1")].priority = 3; // Someone else reordered: leave the order alone.
        QVERIFY(Journal::plan(lease, Journal::parseCurrent(screen.json())).arguments.isEmpty());
    }

    void replaysAJournalWrittenWithoutTheAtomicRename()
    {
        // Crash after the temporary file was written and fsynced but before
        // rename(): the journal proper does not exist yet.
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        QFile temporary(journal.path() + QStringLiteral(".tmp"));
        QVERIFY(temporary.open(QIODevice::WriteOnly));
        temporary.write(Journal::serialize({resizeEntry(4242)}));
        temporary.close();
        QCOMPARE(journal.entries().size(), 1); // Visible to readers, not lost.
        auto screen = twoMonitors();
        screen.outputs[QStringLiteral("DP-1")].mode = QStringLiteral("7");
        screen.outputs[QStringLiteral("DP-1")].scale = 1.0;
        const auto result = journal.replay(screen.reader(), screen.applier(), dead);
        QCOMPARE(result.restored, 1);
        QCOMPARE(screen.outputs[QStringLiteral("DP-1")].mode, QStringLiteral("1"));
        QVERIFY(!QFile::exists(temporary.fileName()));
        QVERIFY(journal.entries().isEmpty());
    }

    void tornTemporaryAndCorruptJournal()
    {
        QTemporaryDir directory;
        Journal journal(directory.filePath(QStringLiteral("j.json")));
        QVERIFY(journal.hold(resizeEntry(4242)));
        // A second writer died half way through its temporary file.
        QFile temporary(journal.path() + QStringLiteral(".tmp"));
        QVERIFY(temporary.open(QIODevice::WriteOnly));
        temporary.write(Journal::serialize({resizeEntry(5000)}).left(40));
        temporary.close();
        QCOMPARE(journal.entries(), QVector<Journal::Entry>{resizeEntry(4242)}); // Torn write ignored.
        QVERIFY(journal.hold(resizeEntry(4243)));
        QVERIFY(!QFile::exists(temporary.fileName()));
        QCOMPARE(journal.entries().size(), 2);

        // A journal that is not even JSON is kept for a human, never trusted.
        QFile main(journal.path());
        QVERIFY(main.open(QIODevice::WriteOnly | QIODevice::Truncate));
        main.write("{\"format\": \"krdp-output-restore\", \"version\": 1, \"entries\": [{\"owner\"");
        main.close();
        QVERIFY(journal.hold(resizeEntry(4244)));
        QCOMPARE(journal.entries(), QVector<Journal::Entry>{resizeEntry(4244)});
        const auto corrupt = QDir(directory.path()).entryList({QStringLiteral("j.json.corrupt-*")});
        QCOMPARE(corrupt.size(), 1);
    }
};

QTEST_GUILESS_MAIN(OutputRestoreJournalTest)

#include "OutputRestoreJournalTest.moc"
