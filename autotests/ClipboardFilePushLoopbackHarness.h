// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: a real KRdp::Server on 127.0.0.1, a real Clipboard on its connection, the worker's real HostFileStager and staging file
// system behind it in-process (with a fake host clipboard and a temporary cache directory), and krdpctl-probe --clipboard push (a libfreerdp
// client that COPIES files) as the sending side. Shared by the loopback tests.
#pragma once

#include "ClipboardFileLoopbackHarness.h"

#include "ConsoleWorkerWire.h"
#include "HostFileStager.h"

namespace PushLoopback
{
using namespace LoopbackHarness;
using namespace KRdp;

struct PushRun {
    Server server;
    QPointer<RdpConnection> connection;
    QPointer<Clipboard> board;
    std::unique_ptr<HostFileStager> stager;
    QProcess probe;
    QByteArray log;
    QTemporaryDir cache;
    ConsoleWorkerWire::Deframer deframer;
    // what the host saw
    QList<QStringList> uriWrites;
    int clears = 0;
    QList<QPair<QString, QString>> notes;
    QList<ConsoleWorkerWire::FilePushReply> replies;
    QList<ConsoleWorkerWire::FilePushState> states;
    // settings of the run
    bool filesEnabled = true;
    bool clientAllowed = true; ///< the planner's files client-to-host flag
    quint64 reserve = 0;
    std::function<std::optional<HostFileStaging::SpaceInfo>(int)> space;
    quint64 nextSeq = 0;

    int count(const char *needle) const { return int(log.count(needle)); }
    void readProbe() { log += probe.readAllStandardError(); }
    template<typename F>
    bool waitFor(std::chrono::milliseconds limit, F done)
    {
        QDeadlineTimer deadline(limit);
        while (!done() && !deadline.hasExpired()) {
            QTest::qWait(20);
            readProbe();
        }
        return done();
    }
    void wait(std::chrono::milliseconds duration) { waitFor(duration, [] { return false; }); }
    QList<QByteArray> lines(const char *prefix) const
    {
        QList<QByteArray> out;
        for (const QByteArray &l : log.split('\n')) {
            const int at = l.indexOf(prefix);
            if (at >= 0) out.append(l.mid(at));
        }
        return out;
    }
    static QByteArray field(const QByteArray &lineText, const char *key)
    {
        const QByteArray needle = QByteArray(key) + '=';
        const int at = lineText.indexOf(needle);
        if (at < 0) return {};
        const int start = at + needle.size();
        int end = lineText.indexOf(' ', start);
        if (end < 0) end = lineText.size();
        return lineText.mid(start, end - start);
    }
    QString staged(const QString &relative) const
    {
        const QDir root(cache.filePath(QStringLiteral("krdp-clip")));
        const auto instances = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (instances.isEmpty()) return {};
        const QDir instance(root.filePath(instances.first()));
        const auto entries = instance.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        QStringList transfers; // (not the `<seq>.parts` directories of the temporary files)
        for (const QString &name : entries) {
            if (!name.endsWith(QLatin1String(".parts"))) transfers.append(name);
        }
        return transfers.isEmpty() ? QString() : instance.filePath(transfers.last()) + QLatin1Char('/') + relative;
    }
    bool anyStaging() const
    {
        const QDir root(cache.filePath(QStringLiteral("krdp-clip")));
        for (const auto &instance : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!QDir(root.filePath(instance)).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) return true;
        }
        return false;
    }

    bool start(QObject *parent, const QString &certificate, const QString &key, const QString &runtimeDir, const QString &pushFrom, const QStringList &probeArguments)
    {
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(certificate.toStdString());
        server.setTlsCertificateKey(key.toStdString());
        server.setUsers({{TestUser, Password}});
        QObject::connect(&server, &Server::newConnectionCreated, parent, [this](RdpConnection *c) {
            connection = c;
            board = c->clipboard();
            board->setFilesEnabled(filesEnabled);
            board->setDirections(true, true);
            board->setFileDirections(false, clientAllowed);
            board->setFileTransferRequest({ClipboardFilePacing::RequestedMode::Unlimited, std::nullopt});
            board->setStockClientForTest(false);
            HostFileStaging::Config config;
            config.cacheDir = cache.path();
            if (space) config.space = space;
            HostFileStager::Backend backend;
            backend.writeUriList = [this](const QStringList &paths, bool) {
                uriWrites.append(paths);
                return true;
            };
            backend.clearUriList = [this] { ++clears; };
            backend.notify = [this](const QString &summary, const QString &body) { notes.append({summary, body}); };
            stager = std::make_unique<HostFileStager>(
                [this](const QByteArray &frame) {
                    deframer.feed(frame);
                    while (const auto record = deframer.next()) route(*record);
                },
                backend, true, config);
            stager->setControl({1, true});
            ConsoleWorkerWire::ClipboardPolicy policy;
            policy.revision = 1;
            policy.controlGeneration = 1;
            policy.hostToClient = true;
            policy.clientToHost = true;
            policy.filesClientToHost = clientAllowed;
            stager->setPolicy(policy);
            Clipboard::ClientFileBackend files;
            files.offer = [this](const QByteArray &descriptor, bool huge) -> quint64 {
                if (!stager) return 0;
                const quint64 seq = ++nextSeq;
                stager->offer({seq, huge, reserve, descriptor});
                return seq;
            };
            files.data = [this](quint64 seq, quint32 index, quint64 offset, const QByteArray &data) {
                if (!stager) return false;
                stager->data({seq, index, offset, data});
                return true;
            };
            files.cancel = [this](quint64 seq, int reason) {
                if (stager) stager->cancel({seq, ConsoleWorkerWire::FilePushCancelReason(reason)});
            };
            board->setClientFileBackend(files);
        });
        if (!server.start()) return false;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
        environment.insert(QStringLiteral("HOME"), runtimeDir);
        probe.setProcessEnvironment(environment);
        QStringList arguments{QStringLiteral("127.0.0.1"), QString::number(server.serverPort()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                              QStringLiteral("--no-krdpctl"), QStringLiteral("--clipboard"), QStringLiteral("push"), QStringLiteral("--push-from"), pushFrom,
                              QStringLiteral("--timeout"), QStringLiteral("120")};
        arguments += probeArguments;
        probe.start(QStringLiteral(KRDPCTL_PROBE), arguments);
        if (!probe.waitForStarted(5000)) return false;
        probe.write(Password.toUtf8() + '\n'); // the password goes over stdin, never on the command line
        probe.closeWriteChannel();
        return true;
    }

    void route(const ConsoleWorkerWire::Record &record)
    {
        if (!board) return;
        if (const auto reply = ConsoleWorkerWire::filePushReply(record)) {
            replies.append(*reply);
            Clipboard::ClientFilesReply out;
            out.seq = reply->seq;
            out.accepted = reply->accepted;
            out.reason = QString::fromLatin1(ConsoleWorkerWire::filePushReasonName(reply->reason));
            out.files = reply->files;
            out.directories = reply->directories;
            out.skipped = reply->skipped;
            out.totalBytes = reply->totalBytes;
            out.freeBytes = reply->freeBytes;
            out.neededBytes = reply->neededBytes;
            board->clientFilesReplied(out);
        } else if (const auto ack = ConsoleWorkerWire::filePushAck(record)) {
            board->clientFilesAcked(ack->seq, ack->index, ack->offset, ack->length, int(ack->status));
        } else if (const auto state = ConsoleWorkerWire::filePushState(record)) {
            states.append(*state);
            using Phase = ConsoleWorkerWire::FilePushPhase;
            board->clientFilesState(state->seq,
                                    state->phase == Phase::Staging  ? Clipboard::ClientFilesPhase::Staging
                                        : state->phase == Phase::Offered ? Clipboard::ClientFilesPhase::Offered
                                        : state->phase == Phase::Done    ? Clipboard::ClientFilesPhase::Done
                                                                         : Clipboard::ClientFilesPhase::Failed,
                                    QString::fromLatin1(ConsoleWorkerWire::filePushReasonName(state->reason)), state->doneBytes, state->totalBytes);
        }
    }

    /// Once the client has sent its capabilities and announced its files, the (fake) graphics acknowledgement lets the clipboard ask for them.
    bool letTheClipboardGo()
    {
        if (!waitFor(30s, [this] { return log.contains("PUSH announced"); })) return false;
        wait(300ms);
        if (!board) return false;
        board->setGraphicsDelivered();
        return true;
    }
    ~PushRun()
    {
        if (probe.state() != QProcess::NotRunning) {
            probe.terminate();
            if (!probe.waitForFinished(5000)) {
                probe.kill();
                probe.waitForFinished(5000);
            }
        }
        stager.reset(); // before the cache directory goes
    }
};
}
