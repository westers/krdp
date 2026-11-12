// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"
#include "WorkerSocketBinding.h"

using namespace KRdp;
using Binding = WorkerSocketBinding::Directory;

class WorkerSocketBindingTest : public QObject
{
    Q_OBJECT

    static bool isSocket(const QString &path)
    {
        struct stat info{};
        return ::lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISSOCK(info.st_mode);
    }

private Q_SLOTS:
    void directoryAcceptanceFollowsWhoIsRunning()
    {
        struct stat dir{};
        dir.st_mode = S_IFDIR | 0700;
        dir.st_uid = 1000;
        QVERIFY(WorkerSocketBinding::directoryAcceptable(dir, 0, 1000)); // root, the worker's private directory
        QVERIFY(!WorkerSocketBinding::directoryAcceptable(dir, 0, 1001)); // root, somebody else's directory
        dir.st_uid = 0;
        QVERIFY(!WorkerSocketBinding::directoryAcceptable(dir, 0, 1000)); // root-owned (e.g. /etc via a planted link): refused
        dir.st_uid = 1000;
        dir.st_mode = S_IFDIR | 0750;
        QVERIFY(!WorkerSocketBinding::directoryAcceptable(dir, 0, 1000)); // open to a group
        dir.st_mode = S_IFDIR | 0702;
        QVERIFY(!WorkerSocketBinding::directoryAcceptable(dir, 0, 1000)); // writable by everyone
        dir.st_mode = S_IFREG | 0700;
        QVERIFY(!WorkerSocketBinding::directoryAcceptable(dir, 0, 1000)); // not a directory
        QVERIFY(!WorkerSocketBinding::directoryAcceptable(dir, 1000, 1000));
        dir.st_mode = S_IFDIR | 0777;
        QVERIFY(WorkerSocketBinding::directoryAcceptable(dir, 1000, 1000)); // unprivileged broker: nothing to escalate
    }

    void tokenComparisonIsExactAndLengthChecked()
    {
        using WorkerSocketBinding::constantTimeEqual;
        const QByteArray token(32, 'a');
        QVERIFY(constantTimeEqual(token, QByteArray(32, 'a')));
        QVERIFY(!constantTimeEqual(token, QByteArray(31, 'a')));
        QVERIFY(!constantTimeEqual(token, QByteArray(33, 'a')));
        QVERIFY(!constantTimeEqual(token, QByteArray()));
        QVERIFY(constantTimeEqual(QByteArray(), QByteArray()));
        for (int i = 0; i < token.size(); ++i) {
            QByteArray other = token;
            other[i] = 'b';
            QVERIFY2(!constantTimeEqual(token, other), qPrintable(QStringLiteral("byte %1").arg(i)));
        }
    }

    void bindsInTheVerifiedDirectoryEvenAfterItsPathIsSwapped()
    {
        // The account that owns the directory renames it away and plants a symlink to a directory of
        // its choosing at the old path after the broker opened and checked it.
        QTemporaryDir root;
        QTemporaryDir victim;
        const QString dir = root.filePath(QStringLiteral("launch"));
        QVERIFY(QDir().mkpath(dir));
        Binding binding;
        QString error;
        QVERIFY2(binding.open(dir + QStringLiteral("/worker.sock"), ::getuid(), &error), qPrintable(error));
        QVERIFY(QFile::rename(dir, dir + QStringLiteral(".moved")));
        QVERIFY(QFile::link(victim.path(), dir));
        const int listener = binding.bindListening(&error);
        QVERIFY2(listener >= 0, qPrintable(error));
        ::close(listener);
        QVERIFY(isSocket(dir + QStringLiteral(".moved/worker.sock"))); // Created in the directory that was verified...
        QVERIFY(!QFileInfo::exists(victim.filePath(QStringLiteral("worker.sock")))); // ...never where the link points.
        binding.close();
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral(".moved/worker.sock"))); // Cleaned up through the same descriptor.
    }

    void aLinkPlantedAtTheSocketNameIsReplacedNotFollowed()
    {
        QTemporaryDir root;
        QTemporaryDir victim;
        const QString victimFile = victim.filePath(QStringLiteral("precious"));
        QFile file(victimFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep");
        file.close();
        const QString dir = root.path();
        QVERIFY(QFile::link(victimFile, dir + QStringLiteral("/worker.sock")));
        Binding binding;
        QString error;
        QVERIFY2(binding.open(dir + QStringLiteral("/worker.sock"), ::getuid(), &error), qPrintable(error));
        const int listener = binding.bindListening(&error);
        QVERIFY2(listener >= 0, qPrintable(error));
        ::close(listener);
        QVERIFY(isSocket(dir + QStringLiteral("/worker.sock")));
        struct stat info{};
        QCOMPARE(::stat(QFile::encodeName(victimFile).constData(), &info), 0);
        QCOMPARE(info.st_size, off_t(4)); // The victim file was neither chmod-ed nor removed nor replaced.
        QVERIFY((info.st_mode & 0777) != 0777);
    }

    void refusesAPathWithoutADirectoryAndANonDirectory()
    {
        QTemporaryDir root;
        Binding binding;
        QString error;
        QVERIFY(!binding.open(QStringLiteral("worker.sock"), ::getuid(), &error));
        QVERIFY(!binding.open(root.filePath(QStringLiteral("missing/worker.sock")), ::getuid(), &error));
        QVERIFY(!binding.open(root.path() + QStringLiteral("/"), ::getuid(), &error));
        QFile plain(root.filePath(QStringLiteral("plain")));
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.close();
        QVERIFY(!binding.open(root.filePath(QStringLiteral("plain/worker.sock")), ::getuid(), &error));
        // The directory itself being a symlink is not followed.
        QTemporaryDir other;
        QVERIFY(QFile::link(other.path(), root.filePath(QStringLiteral("linked"))));
        QVERIFY(!binding.open(root.filePath(QStringLiteral("linked/worker.sock")), ::getuid(), &error));
    }

    void endpointListensAtTheRequestedPathAndCleansUp()
    {
        QTemporaryDir directory;
        const QString socket = directory.filePath(QStringLiteral("worker.sock"));
        // A stale socket file from a crashed run is replaced.
        {
            Binding stale;
            QString error;
            QVERIFY(stale.open(socket, ::getuid(), &error));
            ::close(stale.bindListening(&error));
            stale.close(); // Removes it; recreate a plain leftover instead.
            QFile leftover(socket);
            QVERIFY(leftover.open(QIODevice::WriteOnly));
        }
        ConsoleWorkerEndpoint endpoint;
        const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(::getuid())};
        QString error;
        QVERIFY2(endpoint.listen(socket, target, QByteArray(32, 't'), &error), qPrintable(error));
        QCOMPARE(endpoint.socketName(), socket); // The path the worker connects to is unchanged.
        QVERIFY(isSocket(socket));
        struct stat info{};
        QCOMPARE(::stat(QFile::encodeName(socket).constData(), &info), 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0777)); // World access, as QLocalServer::WorldAccessOption gave.
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY2(worker.waitForConnected(2000), qPrintable(worker.errorString()));
        worker.abort();
        endpoint.close();
        QVERIFY(!QFileInfo::exists(socket));
        QVERIFY(endpoint.socketName().isEmpty());
        // Listening again after close works, and an invalid directory is refused with a reason.
        QVERIFY2(endpoint.listen(socket, target, QByteArray(32, 't'), &error), qPrintable(error));
        endpoint.close();
        QVERIFY(!endpoint.listen(directory.filePath(QStringLiteral("nodir/worker.sock")), target, QByteArray(32, 't'), &error));
        QVERIFY(!error.isEmpty());
    }

    void endpointDoesNotFollowADirectorySwapBetweenListens()
    {
        // The socket is created where the verified directory is, at the path the caller gave.
        QTemporaryDir root;
        QTemporaryDir victim;
        const QString dir = root.filePath(QStringLiteral("launch"));
        QVERIFY(QDir().mkpath(dir));
        ConsoleWorkerEndpoint endpoint;
        const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(::getuid())};
        QVERIFY(endpoint.listen(dir + QStringLiteral("/worker.sock"), target, QByteArray(32, 't')));
        QVERIFY(QFile::rename(dir, dir + QStringLiteral(".moved")));
        QVERIFY(QFile::link(victim.path(), dir));
        endpoint.close(); // Removes its own socket through the held descriptor, not through the swapped path.
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral(".moved/worker.sock")));
        QVERIFY(QDir(victim.path()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(WorkerSocketBindingTest)

#include "WorkerSocketBindingTest.moc"
