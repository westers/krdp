// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "SafeTreeRemoval.h"

using namespace KRdp;

class SafeTreeRemovalTest : public QObject
{
    Q_OBJECT

    static void write(const QString &path, const QByteArray &data = QByteArrayLiteral("x"))
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
    }

private Q_SLOTS:
    void cleanup() { SafeTreeRemoval::raceHookForTest() = {}; }

    void removesANestedTree()
    {
        QTemporaryDir root;
        const QString top = root.filePath(QStringLiteral("seat0-1-abc"));
        QVERIFY(QDir().mkpath(top + QStringLiteral("/a/b/c")));
        write(top + QStringLiteral("/worker.sock"));
        write(top + QStringLiteral("/a/b/c/file"));
        write(top + QStringLiteral("/a/other"));
        QVERIFY(SafeTreeRemoval::removeTree(top));
        QVERIFY(!QFileInfo::exists(top));
        QVERIFY(QDir(root.path()).exists()); // Siblings and the parent are untouched.
    }

    void missingPathIsAlreadyGone()
    {
        QTemporaryDir root;
        QVERIFY(SafeTreeRemoval::removeTree(root.filePath(QStringLiteral("nothing"))));
        QVERIFY(SafeTreeRemoval::removeTree(root.filePath(QStringLiteral("no/such/parent"))));
    }

    void refusesDegeneratePaths()
    {
        QVERIFY(!SafeTreeRemoval::removeTree(QString()));
        QVERIFY(!SafeTreeRemoval::removeTree(QStringLiteral("relative")));
        QVERIFY(!SafeTreeRemoval::removeTree(QStringLiteral("/")));
        QVERIFY(!SafeTreeRemoval::removeTree(QStringLiteral("/tmp/")));
        QVERIFY(!SafeTreeRemoval::removeTree(QStringLiteral("/tmp/..")));
    }

    void aSymlinkInsideTheTreeIsUnlinkedNotFollowed()
    {
        QTemporaryDir root;
        QTemporaryDir victim;
        write(victim.filePath(QStringLiteral("precious")), "keep");
        QVERIFY(QDir().mkpath(victim.filePath(QStringLiteral("sub"))));
        write(victim.filePath(QStringLiteral("sub/deep")), "keep");
        const QString top = root.filePath(QStringLiteral("dir"));
        QVERIFY(QDir().mkpath(top));
        QVERIFY(QFile::link(victim.path(), top + QStringLiteral("/to-dir"))); // QFile::link makes a symlink on Unix.
        QCOMPARE(::symlink(QFile::encodeName(victim.filePath(QStringLiteral("precious"))).constData(),
                     QFile::encodeName(top + QStringLiteral("/to-file")).constData()),
            0);
        QVERIFY(SafeTreeRemoval::removeTree(top));
        QVERIFY(!QFileInfo::exists(top));
        QVERIFY(QFileInfo::exists(victim.filePath(QStringLiteral("precious"))));
        QVERIFY(QFileInfo::exists(victim.filePath(QStringLiteral("sub/deep"))));
    }

    void aSymlinkAsTheTopPathIsNotFollowed()
    {
        QTemporaryDir root;
        QTemporaryDir victim;
        write(victim.filePath(QStringLiteral("precious")), "keep");
        const QString link = root.filePath(QStringLiteral("worker-dir"));
        QVERIFY(QFile::link(victim.path(), link));
        QVERIFY(SafeTreeRemoval::removeTree(link));
        QVERIFY(!QFileInfo(link).isSymLink());
        QVERIFY(QFileInfo::exists(victim.filePath(QStringLiteral("precious"))));
    }

    void aDirectorySwappedForALinkAfterTheCheckIsNotEntered()
    {
        // The account that owns the tree swaps a subdirectory for a symlink between root's
        // examination and its open: the old path-walking removal would then empty the target.
        QTemporaryDir root;
        QTemporaryDir victim;
        write(victim.filePath(QStringLiteral("precious")), "keep");
        const QString top = root.filePath(QStringLiteral("dir"));
        QVERIFY(QDir().mkpath(top + QStringLiteral("/sub")));
        write(top + QStringLiteral("/sub/file"));
        int swaps = 0;
        SafeTreeRemoval::raceHookForTest() = [&](const QByteArray &name) {
            if (name != "sub") return;
            ++swaps;
            QVERIFY(QDir(top + QStringLiteral("/sub")).removeRecursively());
            QVERIFY(QFile::link(victim.path(), top + QStringLiteral("/sub")));
        };
        QVERIFY(!SafeTreeRemoval::removeTree(top)); // Fails closed: something stayed.
        QCOMPARE(swaps, 1);
        QVERIFY(QFileInfo::exists(victim.filePath(QStringLiteral("precious"))));
    }

    void tooDeepATreeIsNotWalkedForever()
    {
        QTemporaryDir root;
        // Long paths exceed PATH_MAX; build level by level with relative mkdir through a descriptor.
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("d"))));
        SafeTreeRemoval::Fd dir(::open(QFile::encodeName(root.filePath(QStringLiteral("d"))).constData(), SafeTreeRemoval::DirFlags));
        QVERIFY(dir.valid());
        int current = ::dup(dir);
        for (int i = 0; i < SafeTreeRemoval::MaxDepth + 5; ++i) {
            QCOMPARE(::mkdirat(current, "n", 0700), 0);
            const int next = ::openat(current, "n", SafeTreeRemoval::DirFlags);
            QVERIFY(next >= 0);
            ::close(current);
            current = next;
        }
        ::close(current);
        QVERIFY(!SafeTreeRemoval::removeTree(root.filePath(QStringLiteral("d")))); // Refuses past the depth cap; no hang, no crash.
    }
};

QTEST_GUILESS_MAIN(SafeTreeRemovalTest)

#include "SafeTreeRemovalTest.moc"
