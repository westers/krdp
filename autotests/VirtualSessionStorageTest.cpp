// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <sys/stat.h>
#include <unistd.h>
#include "VirtualSessionStorage.h"
#include "VirtualSessionGuardian.h"
using namespace Qt::StringLiterals;
namespace KRdp
{
class VirtualSessionStorageTest : public QObject
{
    Q_OBJECT
    QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
private Q_SLOTS:
    void initTestCase() { QVERIFY(getuid() != 0); }
    void privateTokenAndExclusivePersistentProfile()
    {
        QTemporaryDir home, runtime;
        QVERIFY(home.isValid() && runtime.isValid());
        const auto session = id(), launch = id();
        const QByteArray token(32, 'x');
        QString error;
        auto storage = VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, launch, token, &error);
        QVERIFY2(storage, qPrintable(error));
        QFile secret(storage->runtimeDirectory() + u"/worker-token"_s);
        QVERIFY(secret.open(QIODevice::ReadOnly));
        QCOMPARE(secret.readAll(), token);
        struct stat info{};
        QVERIFY(!stat(QFile::encodeName(secret.fileName()).constData(), &info));
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
        QVERIFY(!stat(QFile::encodeName(storage->runtimeDirectory()).constData(), &info));
        QCOMPARE(info.st_mode & 0777, mode_t(0700));
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), token, &error));
        QVERIFY(error.contains(u"already in use"_s));
        const auto profile = storage->profileDirectory();
        QFile saved(profile + u"/state/retained-document"_s);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("unsaved fixture"), qint64(15));
        saved.close();
        storage.reset();
        QVERIFY(QFile::exists(secret.fileName())); // destruction is not deletion
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, launch, token, &error));
        QVERIFY(error.contains(u"new private directory"_s));
        storage = VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), token, &error);
        QVERIFY2(storage, qPrintable(error));
        QCOMPARE(storage->profileDirectory(), profile);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("unsaved fixture"));
    }
    void refusesSymlinksAndSharedPermissions()
    {
        QTemporaryDir home, runtime, outside;
        QVERIFY(QFile::link(outside.path(), home.path() + u"/.local"_s));
        QString error;
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), id(), id(), QByteArray(32, 'x'), &error));
        QVERIFY(QDir(outside.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
        QTemporaryDir cleanHome;
        QVERIFY(!chmod(QFile::encodeName(runtime.path()).constData(), 0755));
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), cleanHome.path(), runtime.path(), id(), id(), QByteArray(32, 'x'), &error));
    }
    void refusesIdentityAndMalformedInputWithoutWrites()
    {
        QTemporaryDir home, runtime;
        QString error;
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid() + 1, home.path(), runtime.path(), id(), id(), QByteArray(32, 'x'), &error));
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), u"../other"_s, id(), QByteArray(32, 'x'), &error));
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), id(), id(), QByteArray(31, 'x'), &error));
        QVERIFY(QDir(home.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
        QVERIFY(QDir(runtime.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
    }
    void rejectsHardlinkedProfileLock()
    {
        QTemporaryDir home, runtime;
        QString error;
        const auto session = id();
        auto storage = VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), QByteArray(32, 'x'), &error);
        QVERIFY(storage);
        const auto lockPath = storage->profileDirectory() + u"/profile.lock"_s;
        storage.reset();
        QVERIFY(!::link(QFile::encodeName(lockPath).constData(), QFile::encodeName(home.path() + u"/lock-alias"_s).constData()));
        QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), QByteArray(32, 'x'), &error));
        QVERIFY(error.contains(u"unsafe"_s));
    }
    void guardianOwnsProfileAcrossChildExec()
    {
        QTemporaryDir home, runtime;
        QString error;
        const auto session = id();
        const QByteArray token(32, 'x');
        auto storage = VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), token, &error);
        QVERIFY(storage);
        const auto profile = storage->profileDirectory();
        {
            VirtualSessionGuardian guardian;
            QVERIFY2(guardian.startPrepared(getuid(), session, token, std::move(storage),
                {u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}}, &error), qPrintable(error));
            QTRY_COMPARE(guardian.phase(), u"running"_s);
            QVERIFY(!storage);
            QVERIFY(!VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), token, &error));
            QVERIFY(error.contains(u"already in use"_s));
        }
        auto next = VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), token, &error);
        QVERIFY2(next, qPrintable(error));
        QCOMPARE(next->profileDirectory(), profile);
    }
    void guardianRejectsWrongProfileIdentity()
    {
        QTemporaryDir home, runtime;
        QString error;
        const auto session = id();
        const QByteArray token(32, 'x');
        auto storage = VirtualSessionStorage::prepareAt(getuid(), home.path(), runtime.path(), session, id(), token, &error);
        QVERIFY(storage);
        VirtualSessionGuardian guardian;
        QVERIFY(!guardian.startPrepared(getuid(), id(), token, std::move(storage),
            {u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}}, &error));
        QCOMPARE(guardian.phase(), u"absent"_s);
        QCOMPARE(guardian.processId(), qint64(0));
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionStorageTest)
#include "VirtualSessionStorageTest.moc"
