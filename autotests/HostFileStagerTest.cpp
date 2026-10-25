// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: the worker's client-to-host file service (policy gate, plan, background staging, acks, progress, the offer to the host
// clipboard, notifications, cleanup) against a fake host and a real temporary cache directory. No compositor, no daemon.

#include <functional>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

#include "HostFileStager.h"

using namespace KRdp;
using namespace KRdp::ConsoleWorkerWire;
using namespace Qt::StringLiterals;
using ClipboardFileFetch::AckStatus;

namespace
{
const QString Canary = u"CANARY-e41c7b-do-not-log"_s;
QStringList g_logs;
void captureLog(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_logs.append(message);
}

QByteArray descriptorOf(const QList<ClipboardFileDescriptor::EncodeItem> &items)
{
    return ClipboardFileDescriptor::encode(items).data;
}
QByteArray pattern(quint64 size, int salt = 0)
{
    QByteArray data(int(size), '\0');
    for (int i = 0; i < data.size(); ++i) data[i] = char((i * 17 + salt) & 0xff);
    return data;
}
QByteArray slurp(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
}

QStringList entriesOf(const QString &dirPath)
{
    return QDir(dirPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

struct Rig {
    QTemporaryDir cache;
    Deframer deframer;
    QList<Record> records;
    QList<QStringList> uriWrites;
    QList<bool> secretWrites;
    int clears = 0;
    QList<QPair<QString, QString>> notes;
    bool writeOk = true;
    /// Runs inside every record the stager sends, i.e. while the stager is in the middle of one of its own functions (what the broker does in
    /// the loopback harness: it reacts to an ack or a state by ending the transfer, which re-enters the stager).
    std::function<void(const Record &)> onRecord;
    std::function<void()> onUriWrite; ///< same, inside the host clipboard write
    std::function<void()> onNotify; ///< same, inside a notification
    ControlState control{42, true};
    ClipboardPolicy policy;
    std::unique_ptr<HostFileStager> stager;
    HostFileStaging::Config config;
    HostFileStager::Tuning tuning;

    explicit Rig(bool desktop = true, bool startNow = true)
        : desktop(desktop)
    {
        policy.revision = 1;
        policy.controlGeneration = 42;
        policy.hostToClient = true;
        policy.clientToHost = true;
        policy.filesHostToClient = true;
        policy.filesClientToHost = true;
        policy.greeter = !desktop;
        if (!desktop) policy.filesHostToClient = policy.filesClientToHost = policy.hostToClient = false;
        config.cacheDir = cache.path();
        if (startNow) start();
    }
    void start()
    {
        HostFileStager::Backend backend;
        backend.writeUriList = [this](const QStringList &paths, bool secret) {
            uriWrites.append(paths);
            secretWrites.append(secret);
            if (onUriWrite) onUriWrite();
            return writeOk;
        };
        backend.clearUriList = [this] { ++clears; };
        backend.notify = [this](const QString &summary, const QString &body) {
            notes.append({summary, body});
            if (onNotify) onNotify();
        };
        stager = std::make_unique<HostFileStager>([this](const QByteArray &record) {
                                                      deframer.feed(record);
                                                      while (const auto r = deframer.next()) {
                                                          records.append(*r);
                                                          if (onRecord) onRecord(*r);
                                                      }
                                                  },
                                                  backend, desktop, config, tuning);
        stager->setControl(control);
        stager->setPolicy(policy);
    }
    bool desktop;

    template<typename T, typename F>
    QList<T> collect(F parse) const
    {
        QList<T> out;
        for (const auto &record : records) {
            if (const auto parsed = parse(record)) out.append(*parsed);
        }
        return out;
    }
    QList<FilePushReply> replies() const { return collect<FilePushReply>([](const Record &r) { return filePushReply(r); }); }
    QList<FilePushAck> acks() const { return collect<FilePushAck>([](const Record &r) { return filePushAck(r); }); }
    QList<FilePushState> states() const { return collect<FilePushState>([](const Record &r) { return filePushState(r); }); }
    FilePushState lastState() const { return states().isEmpty() ? FilePushState{} : states().last(); }

    QString instanceDir() const
    {
        const QDir root(cache.filePath(u"krdp-clip"_s));
        const auto names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        return names.isEmpty() ? QString() : root.filePath(names.first());
    }
    QString transferDir(quint64 seq) const { return instanceDir() + u"/"_s + QString::number(seq); }

    void offer(quint64 seq, const QByteArray &descriptor, bool huge = true, quint64 reserve = 0)
    {
        stager->offer(FilePushOffer{seq, huge, reserve, descriptor});
    }
    void data(quint64 seq, quint32 index, quint64 offset, const QByteArray &bytes)
    {
        stager->data(FilePushData{seq, index, offset, bytes});
    }
};
}

class HostFileStagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        g_logs.clear();
    }

    void aSmallCopyIsStagedAckedOfferedAndTold()
    {
        Rig rig;
        const QByteArray a = pattern(5000), b = pattern(300, 3);
        rig.offer(1, descriptorOf({{u"docs"_s, true, 0, std::nullopt}, {u"docs\\a.txt"_s, false, quint64(a.size()), std::nullopt}, {u"b.bin"_s, false, quint64(b.size()), std::nullopt}, {u"empty"_s, false, 0, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        const auto reply = rig.replies().first();
        QVERIFY(reply.accepted);
        QCOMPARE(reply.seq, quint64(1));
        QCOMPARE(reply.files, 3u);
        QCOMPARE(reply.directories, 1u);
        QCOMPARE(reply.skipped, 0u);
        QCOMPARE(reply.totalBytes, quint64(5300));
        QCOMPARE(rig.notes.size(), 1); // told at the start, once
        QVERIFY(rig.uriWrites.isEmpty()); // nothing is offered before the bytes are in
        rig.data(1, 1, 0, a.left(2000));
        rig.data(1, 1, 2000, a.mid(2000));
        QTRY_COMPARE(rig.acks().size(), 2);
        QCOMPARE(rig.acks().at(1).offset, quint64(2000));
        QCOMPARE(int(rig.acks().at(1).status), int(AckStatus::Ok));
        QVERIFY(rig.uriWrites.isEmpty());
        rig.data(1, 2, 0, b);
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        QCOMPARE(rig.lastState().doneBytes, quint64(5300));
        QCOMPARE(rig.lastState().totalBytes, quint64(5300));
        QCOMPARE(rig.uriWrites.size(), 1);
        const QString base = rig.transferDir(1);
        QCOMPARE(rig.uriWrites.first(), QStringList({base + u"/docs"_s, base + u"/b.bin"_s, base + u"/empty"_s}));
        QCOMPARE(slurp(base + u"/docs/a.txt"_s), a);
        QCOMPARE(slurp(base + u"/b.bin"_s), b);
        QCOMPARE(QFileInfo(base + u"/empty"_s).size(), qint64(0));
        QVERIFY(rig.stager->waitForIdle(5000));
        QCOMPARE(rig.notes.size(), 1);
    }

    void policyGateRefusesWithoutTheFlag()
    {
        Rig rig;
        rig.policy.revision = 2;
        rig.policy.filesClientToHost = false; // locked desktop, new transfer
        rig.policy.locked = true;
        rig.stager->setPolicy(rig.policy);
        rig.offer(1, descriptorOf({{u"a"_s, false, 3, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QVERIFY(!rig.replies().first().accepted);
        QCOMPARE(rig.replies().first().reason, FilePushReason::Policy);
        QVERIFY(rig.instanceDir().isEmpty()); // nothing was created
        QVERIFY(rig.notes.isEmpty());
        // another generation, an inactive controller, the greeter: all refused
        Rig other;
        other.stager->setControl({43, true});
        other.offer(1, descriptorOf({{u"a"_s, false, 3, std::nullopt}}));
        QTRY_COMPARE(other.replies().size(), 1);
        QCOMPARE(other.replies().first().reason, FilePushReason::Policy);
        Rig greeter(false);
        greeter.offer(1, descriptorOf({{u"a"_s, false, 3, std::nullopt}}));
        QTRY_COMPARE(greeter.replies().size(), 1);
        QCOMPARE(greeter.replies().first().reason, FilePushReason::Policy);
        QVERIFY(greeter.instanceDir().isEmpty());
    }

    // spec 12.1: locking the screen denies NEW transfers; one that started goes on, and what it staged stays pasteable.
    void aTransferStartedBeforeTheLockCompletesAndSurvivesTheFlagGoing()
    {
        Rig rig;
        const QByteArray a = pattern(1000);
        rig.offer(1, descriptorOf({{u"a.txt"_s, false, 1000, std::nullopt}, {u"b.txt"_s, false, 1000, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, a);
        QTRY_COMPARE(rig.acks().size(), 1);
        // the screen locks; the broker keeps the flag because the transfer started
        rig.policy.revision = 2;
        rig.policy.locked = true;
        rig.stager->setPolicy(rig.policy);
        rig.data(1, 1, 0, a);
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        QCOMPARE(rig.uriWrites.size(), 1);
        QVERIFY(rig.secretWrites.last()); // written while locked: Klipper keeps it out of its history
        // now the transfer is over: the broker drops the flag; the staged files stay for the paste
        rig.policy.revision = 3;
        rig.policy.filesClientToHost = false;
        rig.stager->setPolicy(rig.policy);
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(QFileInfo::exists(rig.transferDir(1) + u"/a.txt"_s));
        QCOMPARE(rig.clears, 0);
        // but a NEW transfer is refused
        rig.offer(2, descriptorOf({{u"c"_s, false, 3, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 2);
        QCOMPARE(rig.replies().last().reason, FilePushReason::Policy);
    }
    void theFlagGoingMidTransferStopsItAndRemovesTheStaging()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a.txt"_s, false, 2000, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(1000));
        QTRY_COMPARE(rig.acks().size(), 1);
        rig.policy.revision = 2;
        rig.policy.filesClientToHost = false; // e.g. the host owner's setting changed
        rig.stager->setPolicy(rig.policy);
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Failed);
        QCOMPARE(rig.lastState().reason, FilePushReason::Policy);
        rig.data(1, 0, 1000, pattern(1000)); // more bytes after the stop are refused
        QTRY_COMPARE(rig.acks().size(), 2);
        QCOMPARE(int(rig.acks().last().status), int(AckStatus::Denied));
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
    }
    void aControllerChangeRemovesEverythingIncludingWhatWasOffered()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a.txt"_s, false, 4, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(4));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        rig.stager->setControl({44, true});
        QVERIFY(rig.stager->waitForIdle(5000));
        QTRY_COMPARE(rig.clears, 1);
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
    }

    // ---- refusals ----
    void hostileNamesAreRefusedBeforeAnythingIsCreated()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"fine.txt"_s, false, 3, std::nullopt}, {u"..\\"_s + Canary, false, 3, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QVERIFY(!rig.replies().first().accepted);
        QCOMPARE(rig.replies().first().reason, FilePushReason::Names);
        QVERIFY(rig.instanceDir().isEmpty());
        QCOMPARE(rig.notes.size(), 1); // the user is told, with a reason and no name
        for (const auto &note : std::as_const(rig.notes)) {
            QVERIFY(!note.first.contains(Canary) && !note.second.contains(Canary) && !note.second.contains(u"fine"_s));
        }
        // a torn descriptor and an empty selection
        rig.offer(2, descriptorOf({{u"link"_s, false, 3, std::nullopt}}).left(100));
        QTRY_COMPARE(rig.replies().size(), 2);
        // (a record like that never gets here: the wire parser refuses it; the stager still answers if it does)
        QCOMPARE(rig.replies().last().reason, FilePushReason::Descriptor);
    }
    void theDiskGuardRefusesUpFrontAndTheUserIsToldHowMuch()
    {
        Rig rig(true, false);
        rig.config.space = [](int) { return std::optional(HostFileStaging::SpaceInfo{3ULL << 30, 100ULL << 30}); };
        rig.start();
        rig.offer(1, descriptorOf({{u"big.iso"_s, false, 2ULL << 30, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        const auto reply = rig.replies().first();
        QVERIFY(!reply.accepted);
        QCOMPARE(reply.reason, FilePushReason::NoSpace);
        QCOMPARE(reply.freeBytes, quint64(3ULL << 30));
        QCOMPARE(reply.neededBytes, quint64(7ULL << 30));
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        QCOMPARE(rig.notes.size(), 1);
        QVERIFY2(rig.notes.first().second.contains(QLocale().formattedDataSize(qint64(7ULL << 30))), qPrintable(rig.notes.first().second));
        QVERIFY(rig.notes.first().second.contains(QLocale().formattedDataSize(qint64(3ULL << 30))));
        QVERIFY(!rig.notes.first().second.contains(u"big.iso"_s));
        // the offer's own reserve (ClipboardFileMinFreeBytes) is what counts
        rig.offer(2, descriptorOf({{u"big.iso"_s, false, 2ULL << 30, std::nullopt}}), true, 500ULL << 20);
        QTRY_COMPARE(rig.replies().size(), 2);
        QVERIFY(rig.replies().last().accepted);
    }
    // The real space probe (never injected here) on a cache directory that does not exist yet, parents included: the first copy on a fresh account.
    void theFirstCopyWorksOnAnAccountWithNoCacheDirectoryYet()
    {
        Rig rig(true, false);
        rig.config.cacheDir = rig.cache.filePath(u"home/user/.cache"_s);
        rig.start();
        QVERIFY(!QFileInfo::exists(rig.cache.filePath(u"home"_s)));
        rig.offer(1, descriptorOf({{u"a.txt"_s, false, 5, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QVERIFY2(rig.replies().first().accepted, qPrintable(QString::number(int(rig.replies().first().reason))));
        QVERIFY(QFileInfo(rig.cache.filePath(u"home/user/.cache/krdp-clip"_s)).isDir());
    }
    // A staging folder that cannot be made is said as such (the notification names the folder's job, never a path or a file name), and no byte is asked for.
    void aStagingFolderThatCannotBeMadeIsToldAsSuch_data()
    {
        QTest::addColumn<bool>("emptyCacheDir");
        QTest::newRow("unwritable parent") << false;
        QTest::newRow("no cache directory at all (no HOME, no passwd entry)") << true;
    }
    void aStagingFolderThatCannotBeMadeIsToldAsSuch()
    {
        QFETCH(bool, emptyCacheDir);
        if (!emptyCacheDir && ::getuid() == 0) QSKIP("root can create anything");
        Rig rig(true, false);
        const QString locked = rig.cache.filePath(u"locked"_s);
        QVERIFY(QDir().mkpath(locked));
        rig.config.cacheDir = emptyCacheDir ? QString() : locked + u"/.cache"_s;
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0500), 0);
        rig.start();
        rig.offer(1, descriptorOf({{u"secret-name.txt"_s, false, 5, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QVERIFY(rig.stager->waitForIdle(5000));
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0700), 0);
        QVERIFY(!rig.replies().first().accepted);
        QCOMPARE(rig.replies().first().reason, FilePushReason::StagingFailed);
        QCOMPARE(rig.notes.size(), 1);
        const QString body = rig.notes.first().second;
        QVERIFY2(body.contains(u"folder"_s) && body.contains(u"could not be prepared"_s), qPrintable(body));
        QVERIFY(!body.contains(u"secret-name"_s) && !body.contains(rig.cache.path()));
        QVERIFY(rig.acks().isEmpty());
        QVERIFY(entriesOf(locked).isEmpty());
    }
    void aProbeThatFailsRefusesInsteadOfGuessing()
    {
        Rig rig(true, false);
        rig.config.space = [](int) { return std::optional<HostFileStaging::SpaceInfo>(); };
        rig.start();
        rig.offer(1, descriptorOf({{u"a"_s, false, 5, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QCOMPARE(rig.replies().first().reason, FilePushReason::SpaceUnknown);
    }
    void aFullDiskMidWayAbortsDeletesThePartialAndReports()
    {
        Rig rig(true, false);
        quint64 freeNow = 100ULL << 30;
        rig.config.space = [&freeNow](int) { return std::optional(HostFileStaging::SpaceInfo{freeNow, 200ULL << 30}); };
        rig.config.spaceCheckIntervalBytes = 1000;
        rig.config.reserveBytes = 1;
        rig.start();
        rig.offer(1, descriptorOf({{u"a.bin"_s, false, 10'000, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(600));
        rig.data(1, 0, 600, pattern(600));
        QTRY_COMPARE(rig.acks().size(), 2);
        freeNow = 100;
        rig.data(1, 0, 1200, pattern(600));
        QTRY_COMPARE(rig.acks().size(), 3);
        QCOMPARE(int(rig.acks().last().status), int(AckStatus::NoSpace));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Failed);
        QCOMPARE(rig.lastState().reason, FilePushReason::NoSpace);
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        QVERIFY(rig.uriWrites.isEmpty());
        QCOMPARE(rig.notes.size(), 2); // started, failed
        QVERIFY(rig.notes.last().second.contains(u"space"_s, Qt::CaseInsensitive));
    }
    void hostileChunksFailTheTransferWithoutANameInTheReason()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a.bin"_s, false, 100, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 50, pattern(10)); // not the next offset
        QTRY_COMPARE(rig.acks().size(), 1);
        QCOMPARE(int(rig.acks().last().status), int(AckStatus::OutOfOrder));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Failed);
        QCOMPARE(rig.lastState().reason, FilePushReason::Hostile);
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        // data for a transfer that does not exist, or ended
        rig.data(9, 0, 0, pattern(10));
        rig.data(1, 0, 0, pattern(10));
        QTRY_COMPARE(rig.acks().size(), 3);
        QCOMPARE(int(rig.acks().at(1).status), int(AckStatus::Gone));
        QCOMPARE(int(rig.acks().at(2).status), int(AckStatus::Gone));
    }

    // ---- the offer rule (spec 3.2) ----
    void aLargeSelectionIsOfferedAfterItsFirstFinishedUnitAndRefreshedAtTheEnd()
    {
        Rig rig(true, false);
        rig.tuning.offerWhenCompleteBytes = 100; // the real threshold is 256 MiB
        rig.start();
        rig.offer(1, descriptorOf({{u"first.bin"_s, false, 200, std::nullopt}, {u"second.bin"_s, false, 200, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(100));
        QTRY_COMPARE(rig.acks().size(), 1);
        QVERIFY(rig.uriWrites.isEmpty()); // the first file is not finished
        rig.data(1, 0, 100, pattern(100));
        QTRY_COMPARE(rig.uriWrites.size(), 1);
        const QString base = rig.transferDir(1);
        QCOMPARE(rig.uriWrites.first(), QStringList({base + u"/first.bin"_s}));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Offered);
        rig.data(1, 1, 0, pattern(200));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        QCOMPARE(rig.uriWrites.size(), 2);
        QCOMPARE(rig.uriWrites.last(), QStringList({base + u"/first.bin"_s, base + u"/second.bin"_s}));
    }
    void aSmallSelectionIsOfferedOnlyWhenItIsAllThere()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a"_s, false, 10, std::nullopt}, {u"b"_s, false, 10, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(10));
        QTRY_COMPARE(rig.acks().size(), 1);
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(rig.uriWrites.isEmpty());
        rig.data(1, 1, 0, pattern(10));
        QTRY_COMPARE(rig.uriWrites.size(), 1);
    }
    void aSelectionWithNoBytesIsOfferedAtOnce()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"folder"_s, true, 0, std::nullopt}, {u"empty.txt"_s, false, 0, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        QCOMPARE(rig.uriWrites.size(), 1);
    }
    void aHostClipboardThatRefusesTheOfferFailsTheTransfer()
    {
        Rig rig;
        rig.writeOk = false;
        rig.offer(1, descriptorOf({{u"a"_s, false, 3, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(3));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Failed);
        QCOMPARE(rig.lastState().reason, FilePushReason::WriteFailed);
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
    }


    // ---- the broker ends a transfer from inside the stager's own send (re-entrancy) ----
    // In the loopback harness m_send reaches the broker synchronously and the broker answers an ack or a state by cancelling the transfer,
    // which erases the entry the stager is still working on. Nothing may be touched through an iterator, pointer or reference taken before
    // such a call. (Sol, a real full disk: written() -> fail(it) erased a map node that cancel() had already erased: double free.)
    void aFullDiskAckThatEndsTheTransferInsideTheSendDoesNotTouchTheErasedEntry()
    {
        Rig rig(true, false);
        quint64 freeNow = 100ULL << 30;
        rig.config.space = [&freeNow](int) { return std::optional(HostFileStaging::SpaceInfo{freeNow, 200ULL << 30}); };
        rig.config.spaceCheckIntervalBytes = 1000;
        rig.config.reserveBytes = 1;
        rig.start();
        int cancels = 0;
        rig.onRecord = [&](const Record &record) {
            if (const auto ack = filePushAck(record); ack && ack->status == AckStatus::NoSpace) {
                ++cancels;
                rig.stager->cancel({1, FilePushCancelReason::Failed}); // what the broker does on a NoSpace ack
            }
        };
        rig.offer(1, descriptorOf({{u"a.bin"_s, false, 10'000, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(600));
        rig.data(1, 0, 600, pattern(600));
        QTRY_COMPARE(rig.acks().size(), 2);
        freeNow = 100;
        rig.data(1, 0, 1200, pattern(600));
        QTRY_COMPARE(rig.acks().size(), 3);
        QCOMPARE(int(rig.acks().last().status), int(AckStatus::NoSpace));
        QCOMPARE(cancels, 1);
        QCOMPARE(rig.stager->transferCount(), 0);
        // the broker ended it: the worker says nothing more about it (no second end, no failure notification of its own)
        for (const auto &state : rig.states()) QVERIFY(state.phase != FilePushPhase::Failed);
        QCOMPARE(rig.notes.size(), 1); // started only
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        // and the stager still works
        freeNow = 100ULL << 30;
        rig.offer(2, descriptorOf({{u"b.bin"_s, false, 4, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 2);
        QVERIFY(rig.replies().last().accepted);
        rig.data(2, 0, 0, pattern(4));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
    }
    void anAckThatEndsTheTransferInsideTheSendStopsTheStagerFromGoingOn()
    {
        for (const bool lastChunk : {false, true}) {
            Rig rig; // each case on a fresh rig
            rig.offer(1, descriptorOf({{u"a.bin"_s, false, 20, std::nullopt}}));
            QTRY_COMPARE(rig.replies().size(), 1);
            const int statesBefore = rig.states().size();
            rig.onRecord = [&](const Record &record) {
                if (filePushAck(record)) rig.stager->cancel({1, FilePushCancelReason::Failed});
            };
            rig.data(1, 0, 0, pattern(lastChunk ? 20 : 10));
            QTRY_COMPARE(rig.acks().size(), 1);
            QCOMPARE(rig.stager->transferCount(), 0);
            QVERIFY(rig.stager->waitForIdle(5000));
            QTest::qWait(20);
            QCOMPARE(rig.states().size(), statesBefore); // no progress, no Done for a transfer that is gone
            QVERIFY(rig.uriWrites.isEmpty()); // nothing is offered for it
            QCOMPARE(rig.clears, 0);
            QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        }
    }
    void aLargeSelectionAckThatEndsTheTransferStopsTheOfferToTheHost()
    {
        Rig rig(true, false);
        rig.tuning.offerWhenCompleteBytes = 100;
        rig.start();
        rig.offer(1, descriptorOf({{u"first.bin"_s, false, 200, std::nullopt}, {u"second.bin"_s, false, 200, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        const int statesBefore = rig.states().size();
        rig.onRecord = [&](const Record &record) {
            if (filePushAck(record)) rig.stager->cancel({1, FilePushCancelReason::Failed});
        };
        rig.data(1, 0, 0, pattern(200)); // finishes the first unit: the stager would offer it now
        QTRY_COMPARE(rig.acks().size(), 1);
        QCOMPARE(rig.stager->transferCount(), 0);
        QVERIFY(rig.stager->waitForIdle(5000));
        QTest::qWait(20);
        QVERIFY(rig.uriWrites.isEmpty());
        QCOMPARE(rig.states().size(), statesBefore);
    }
    void aReplyThatEndsTheTransferInsideTheSendStopsTheStagerFromGoingOn()
    {
        Rig rig;
        rig.onRecord = [&](const Record &record) {
            if (const auto reply = filePushReply(record); reply && reply->accepted) rig.stager->cancel({1, FilePushCancelReason::Failed});
        };
        // only folders and empty files: nothing to wait for, so created() would finish the transfer right after sending the reply
        rig.offer(1, descriptorOf({{u"folder"_s, true, 0, std::nullopt}, {u"empty.txt"_s, false, 0, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QCOMPARE(rig.stager->transferCount(), 0);
        QVERIFY(rig.stager->waitForIdle(5000));
        QTest::qWait(20);
        QVERIFY(rig.uriWrites.isEmpty());
        for (const auto &state : rig.states()) QVERIFY(state.phase != FilePushPhase::Done);
    }
    void aNotificationThatEndsTheTransferInsideTheCallStopsTheStagerFromGoingOn()
    {
        Rig rig;
        rig.onNotify = [&] { rig.stager->cancel({1, FilePushCancelReason::Failed}); };
        rig.offer(1, descriptorOf({{u"folder"_s, true, 0, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        QCOMPARE(rig.stager->transferCount(), 0);
        QVERIFY(rig.stager->waitForIdle(5000));
        QTest::qWait(20);
        QVERIFY(rig.uriWrites.isEmpty());
        for (const auto &state : rig.states()) QVERIFY(state.phase != FilePushPhase::Done);
    }
    void aHostClipboardWriteThatEndsTheTransferInsideTheCallDoesNotTouchTheErasedEntry()
    {
        for (const bool accepted : {true, false}) {
            Rig rig;
            rig.writeOk = accepted;
            rig.onUriWrite = [&] { rig.stager->cancel({1, FilePushCancelReason::Failed}); };
            rig.offer(1, descriptorOf({{u"a.bin"_s, false, 4, std::nullopt}}));
            QTRY_COMPARE(rig.replies().size(), 1);
            rig.data(1, 0, 0, pattern(4));
            QTRY_COMPARE(rig.uriWrites.size(), 1);
            QCOMPARE(rig.stager->transferCount(), 0);
            QVERIFY(rig.stager->waitForIdle(5000));
            QTest::qWait(20);
            for (const auto &state : rig.states()) QVERIFY(state.phase != FilePushPhase::Done && state.phase != FilePushPhase::Failed);
            QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
            QCOMPARE(rig.clears, accepted ? 1 : 0); // a list written for a transfer that is gone must not stay on the host clipboard
        }
    }

    // ---- replace, cancel, end ----
    void aNewerOfferReplacesTheOlderAndRemovesItsStaging()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"old.txt"_s, false, 4, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(4));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        rig.offer(2, descriptorOf({{u"new.txt"_s, false, 4, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 2);
        QVERIFY(rig.replies().last().accepted);
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        QVERIFY(QFileInfo::exists(rig.transferDir(2)));
        QCOMPARE(rig.stager->transferCount(), 1);
    }
    void cancelRemovesOneOrAllAndWithdrawsTheOffer()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a"_s, false, 4, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(4));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        rig.stager->cancel({1, FilePushCancelReason::Replaced});
        QVERIFY(rig.stager->waitForIdle(5000));
        QTRY_COMPARE(rig.clears, 1);
        QVERIFY(!QFileInfo::exists(rig.transferDir(1)));
        // a cancel in the middle: later chunks find nothing
        rig.offer(2, descriptorOf({{u"b"_s, false, 8, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 2);
        rig.data(2, 0, 0, pattern(4));
        rig.stager->cancel({0, FilePushCancelReason::ConnectionEnded});
        rig.data(2, 0, 4, pattern(4));
        QVERIFY(rig.stager->waitForIdle(5000));
        QTRY_VERIFY(rig.acks().last().seq == 2 && rig.acks().last().status == AckStatus::Gone);
        QVERIFY(!QFileInfo::exists(rig.transferDir(2)));
        QCOMPARE(rig.stager->transferCount(), 0);
    }
    void destroyingTheStagerLeavesNoStagingBehind()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a"_s, false, 100, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        rig.data(1, 0, 0, pattern(50));
        QTRY_COMPARE(rig.acks().size(), 1);
        const QString instance = rig.instanceDir();
        QVERIFY(!instance.isEmpty());
        rig.stager.reset(); // the worker is going away cleanly
        QVERIFY(!QFileInfo::exists(instance));
        QVERIFY(QDir(rig.cache.filePath(u"krdp-clip"_s)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void startingUpSweepsWhatACrashLeftAndCreatesNothingYet()
    {
        Rig rig(true, false);
        const QString stale = rig.cache.filePath(u"krdp-clip/c-0000000000000dead/7/x.part"_s);
        QDir().mkpath(QFileInfo(stale).absolutePath());
        QFile f(stale);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const struct timespec old[2] = {{time(nullptr) - 600, 0}, {time(nullptr) - 600, 0}};
        ::utimensat(AT_FDCWD, QFile::encodeName(rig.cache.filePath(u"krdp-clip/c-0000000000000dead"_s)).constData(), old, 0);
        rig.start();
        rig.stager->sweepAtStart();
        QVERIFY(rig.stager->waitForIdle(5000));
        QVERIFY(!QFileInfo::exists(rig.cache.filePath(u"krdp-clip/c-0000000000000dead"_s)));
        QVERIFY(rig.instanceDir().isEmpty()); // no instance until a transfer needs one
    }

    // ---- progress and logs ----
    void progressIsThrottledButEveryPhaseChangeIsSent()
    {
        Rig rig;
        rig.offer(1, descriptorOf({{u"a.bin"_s, false, 200'000, std::nullopt}}));
        QTRY_COMPARE(rig.replies().size(), 1);
        for (int i = 0; i < 200; ++i) rig.data(1, 0, quint64(i) * 1000, pattern(1000, i));
        QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
        QCOMPARE(rig.acks().size(), 200);
        const auto states = rig.states();
        int staging = 0;
        for (const auto &state : states) staging += state.phase == FilePushPhase::Staging;
        QVERIFY2(staging <= 3 + 1, qPrintable(QString::number(staging))); // 4 a second at most; this runs well under a second
        QCOMPARE(states.last().doneBytes, quint64(200'000));
    }
    void namesAndContentsNeverReachALogOrANotification()
    {
        qInstallMessageHandler(captureLog);
        {
            Rig rig;
            const QByteArray secret = Canary.toUtf8() + pattern(50);
            rig.offer(1, descriptorOf({{u"dir-"_s + Canary, true, 0, std::nullopt}, {u"dir-"_s + Canary + u"\\file-"_s + Canary + u".txt"_s, false, quint64(secret.size()), std::nullopt}}));
            QTRY_COMPARE(rig.replies().size(), 1);
            rig.data(1, 1, 0, secret);
            QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Done);
            // and a failing one with the name inside
            rig.offer(2, descriptorOf({{u"x\\..\\"_s + Canary, false, 3, std::nullopt}}));
            QTRY_COMPARE(rig.replies().size(), 2);
            rig.offer(3, descriptorOf({{u"file-"_s + Canary, false, 100, std::nullopt}}));
            QTRY_COMPARE(rig.replies().size(), 3);
            rig.data(3, 0, 7, pattern(3));
            QTRY_COMPARE(rig.lastState().phase, FilePushPhase::Failed);
            QVERIFY(rig.stager->waitForIdle(5000));
            for (const auto &note : std::as_const(rig.notes)) {
                QVERIFY2(!note.first.contains(Canary) && !note.second.contains(Canary), "a notification carries a name");
            }
            // the only record that may carry a name is the broker's own descriptor going in; nothing the worker sends does
            for (const auto &record : std::as_const(rig.records)) QVERIFY(!record.payload.contains(Canary.toUtf8()));
        }
        qInstallMessageHandler(nullptr);
        QVERIFY(!g_logs.isEmpty()); // something was logged (counts and reasons)
        for (const auto &line : std::as_const(g_logs)) QVERIFY2(!line.contains(Canary), qPrintable(line));
    }
};

QTEST_GUILESS_MAIN(HostFileStagerTest)
#include "HostFileStagerTest.moc"
