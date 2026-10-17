// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 2: a real KRdp::Server on 127.0.0.1, a real Clipboard on its connection, the worker's file source (HostFiles) behind it
// in-process, and krdpctl-probe --clipboard files (a libfreerdp client) as the receiving side. Shared by the loopback tests.
#pragma once

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include "Clipboard.h"
#include "ClipboardFileDescriptor.h"
#include "HostFiles.h"
#include "RdpConnection.h"
#include "Server.h"

namespace LoopbackHarness
{
using namespace KRdp;
using namespace std::chrono_literals;

inline const QString TestUser = QStringLiteral("alice");
inline const QString Password = QStringLiteral("correct horse");

inline bool sparseSupported(const QString &directory)
{
    const QByteArray path = QFile::encodeName(directory + QStringLiteral("/sparse-probe"));
    const int fd = ::open(path.constData(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return false;
    struct stat st {};
    const bool grown = ::ftruncate(fd, qint64(5) << 30) == 0 && ::fstat(fd, &st) == 0;
    ::close(fd);
    ::unlink(path.constData());
    return grown && st.st_blocks < 1024;
}

inline QString makeCertificate(const QString &dir, QString *key)
{
    const QString certificate = dir + QStringLiteral("/server.crt");
    *key = dir + QStringLiteral("/server.key");
    QProcess p;
    p.start(QStandardPaths::findExecutable(QStringLiteral("openssl")),
            {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-keyout"), *key,
             QStringLiteral("-out"), certificate, QStringLiteral("-days"), QStringLiteral("1"), QStringLiteral("-subj"), QStringLiteral("/CN=krdp-test")});
    p.waitForFinished(30000);
    return p.exitCode() == 0 ? certificate : QString();
}

/// One run: the server, the host side of the files (a selection of paths), the probe.
struct Run {
    Server server;
    QPointer<RdpConnection> connection;
    QPointer<Clipboard> board;
    QProcess probe;
    QByteArray log;
    QList<QByteArray> selection; ///< what the "host user" copied
    std::shared_ptr<HostFiles::Snapshot> snapshot;
    HostFiles::WalkResult walked;
    // what the backend saw
    struct Read {
        quint64 seq;
        quint32 index;
        quint64 offset;
        quint32 length;
    };
    QList<Read> reads;
    QList<quint32> locks, unlocks;
    QList<quint64> cancels;
    int ended = 0;
    int lists = 0;
    bool announce = true;
    bool filesEnabled = true;
    bool hugeAtWalk = true;
    bool failReads = false; ///< the worker "dies": reads are never answered
    int readDelayMs = 0;
    quint64 offerSeq = 1;
    quint64 bytesRead = 0;

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
    QByteArray line(const char *prefix) const
    {
        for (const QByteArray &l : log.split('\n')) {
            const int at = l.indexOf(prefix);
            if (at >= 0) return l.mid(at);
        }
        return {};
    }
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

    bool start(QObject *parent, const QString &certificate, const QString &key, const QString &runtimeDir, const QStringList &probeArguments)
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
            board->setFileDirections(true, false);
            Clipboard::HostFileBackend backend;
            backend.read = [this](quint64 seq, quint32 index, quint64 offset, quint32 length, quint32 streamId) {
                reads.append({seq, index, offset, length});
                if (failReads) return true; // sent, never answered
                const auto result = snapshot ? snapshot->read(index, offset, length) : HostFiles::ReadResult{};
                QPointer<Clipboard> guarded(board);
                QTimer::singleShot(readDelayMs, board.data(), [this, guarded, streamId, result] {
                    if (!guarded) return;
                    Clipboard::ReadResult status = Clipboard::ReadResult::IoError;
                    switch (result.status) {
                    case HostFiles::ReadStatus::Ok: status = Clipboard::ReadResult::Ok; bytesRead += quint64(result.data.size()); break;
                    case HostFiles::ReadStatus::Changed: status = Clipboard::ReadResult::Changed; break;
                    case HostFiles::ReadStatus::Gone: status = Clipboard::ReadResult::Gone; break;
                    case HostFiles::ReadStatus::Denied: status = Clipboard::ReadResult::Denied; break;
                    case HostFiles::ReadStatus::IoError: status = Clipboard::ReadResult::IoError; break;
                    }
                    guarded->provideHostFileRead(streamId, status, result.data);
                });
                return true;
            };
            backend.lock = [this](quint64, quint32 id) { locks.append(id); };
            backend.unlock = [this](quint32 id) { unlocks.append(id); };
            backend.cancel = [this](quint64 seq) { cancels.append(seq); };
            board->setHostFileBackend(backend);
            QObject::connect(board, &Clipboard::hostFilesRequested, board, [this](quint64 requestId, bool huge) {
                HostFiles::WalkOptions options;
                options.hugeFiles = huge && hugeAtWalk;
                options.budgetMs = 600000;
                walked = HostFiles::walk(selection, options);
                ++lists;
                snapshot = walked.snapshot;
                board->provideHostFiles(requestId, offerSeq, walked.descriptor, walked.skipped, walked.status == HostFiles::WalkStatus::Ok);
            });
            QObject::connect(board, &Clipboard::hostFileTransferEnded, board, [this](quint64, int, int) { ++ended; });
        });
        if (!server.start()) return false;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
        environment.insert(QStringLiteral("HOME"), runtimeDir);
        probe.setProcessEnvironment(environment);
        QStringList arguments{QStringLiteral("127.0.0.1"), QString::number(server.serverPort()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                              QStringLiteral("--no-krdpctl"), QStringLiteral("--clipboard"), QStringLiteral("files"), QStringLiteral("--timeout"), QStringLiteral("120")};
        arguments += probeArguments;
        probe.start(QStringLiteral(KRDPCTL_PROBE), arguments);
        if (!probe.waitForStarted(5000)) return false;
        probe.write(Password.toUtf8() + '\n'); // the password goes over stdin, never on the command line
        probe.closeWriteChannel();
        return true;
    }

    /// Once the client has sent its capabilities, the host "copies" its selection.
    bool announceFiles()
    {
        if (!waitFor(30s, [this] { return log.contains("FILES caps"); })) return false;
        wait(300ms);
        if (!board) return false;
        board->setGraphicsDelivered();
        board->announceHost(false, false, false, true);
        return true;
    }
    ~Run()
    {
        if (probe.state() != QProcess::NotRunning) {
            probe.terminate();
            if (!probe.waitForFinished(5000)) {
                probe.kill();
                probe.waitForFinished(5000);
            }
        }
    }
};

inline QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

inline QByteArray readAt(const QString &path, quint64 offset, quint32 length)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY);
    QByteArray data(int(length), '\0');
    const ssize_t n = fd >= 0 ? ::pread(fd, data.data(), length, off_t(offset)) : -1;
    if (fd >= 0) ::close(fd);
    data.resize(n < 0 ? 0 : int(n));
    return data;
}

inline void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(data);
}

/// A reproducible, non-repeating pattern of \a size bytes.
inline QByteArray pattern(qsizetype size, quint32 seed)
{
    QByteArray data(size, Qt::Uninitialized);
    quint32 x = seed * 2654435761u + 1;
    for (qsizetype i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        data[i] = char(x >> 24);
    }
    return data;
}
}
