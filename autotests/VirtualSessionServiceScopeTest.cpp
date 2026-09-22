// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionServiceScope.h"
#include <QTest>
#include <QUuid>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QScopeGuard>
#include <fcntl.h>
using namespace Qt::StringLiterals;
namespace KRdp {
class VirtualSessionServiceScopeTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void exactServiceOnly() {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(VirtualSessionServiceScope::expectedMembership(id), QByteArray("0::/system.slice/krdp-virtual-session@") + id.toLatin1() + ".service\n");
        for (const auto &invalid : {QString(), u"../system.slice"_s, u"00000000-0000-0000-0000-000000000000"_s,
                u"{12345678-1234-1234-1234-123456789abc}"_s, u"12345678-1234-1234-1234-123456789ABC"_s})
            QVERIFY(VirtualSessionServiceScope::expectedMembership(invalid).isEmpty());
        // Even a valid random UUID cannot select some other process's cgroup.
        QVERIFY(!VirtualSessionServiceScope::open(id));
    }
    void excludesParentAndDeduplicates() {
        const auto empty = VirtualSessionServiceScope::parseMembers("123\n", 123);
        QVERIFY(empty); QVERIFY(empty->isEmpty());
        const auto members = VirtualSessionServiceScope::parseMembers("456\n123\n789\n456\n", 123);
        QVERIFY(members); QCOMPARE(*members, QList<pid_t>({456, 789}));
    }
    void uncertainMembershipNeverMeansEmpty_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("missing parent") << QByteArray("456\n");
        QTest::newRow("truncated") << QByteArray("123");
        QTest::newRow("init") << QByteArray("123\n1\n");
        QTest::newRow("zero") << QByteArray("123\n0\n");
        QTest::newRow("negative") << QByteArray("123\n-45\n");
        QTest::newRow("whitespace") << QByteArray("123\n 456\n");
        QTest::newRow("leading zero") << QByteArray("123\n0456\n");
        QTest::newRow("empty line") << QByteArray("123\n\n");
        QTest::newRow("overflow") << QByteArray("123\n2147483648\n");
        QTest::newRow("huge") << QByteArray(65537, '1');
        QTest::newRow("too many") << QByteArray("123\n").repeated(4097);
    }
    void uncertainMembershipNeverMeansEmpty() {
        QFETCH(QByteArray, bytes);
        QVERIFY(!VirtualSessionServiceScope::parseMembers(bytes, 123));
    }
    void filesystemUncertaintyRefusesExtinction() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const int fd = ::open(QFile::encodeName(dir.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(fd >= 0); const auto cleanup = qScopeGuard([&] { close(fd); });
        QVERIFY(!VirtualSessionServiceScope::readMembers(fd, 123));
        QFile procs(dir.filePath(u"cgroup.procs"_s));
        QVERIFY(procs.open(QIODevice::WriteOnly)); QCOMPARE(procs.write("123\n"), qint64(4)); procs.close();
        auto members = VirtualSessionServiceScope::readMembers(fd, 123); QVERIFY(members); QVERIFY(members->isEmpty());
        // A nested group can hold processes even if this group's procs is empty
        // except for the owning parent. Do not treat it as extinction.
        QVERIFY(QDir(dir.path()).mkdir(u"nested"_s));
        QVERIFY(!VirtualSessionServiceScope::readMembers(fd, 123));
        QVERIFY(QDir(dir.path()).rmdir(u"nested"_s));
        QVERIFY(QFile::link(procs.fileName(), dir.filePath(u"alias"_s)));
        QVERIFY(!VirtualSessionServiceScope::readMembers(fd, 123));
        QVERIFY(QFile::remove(dir.filePath(u"alias"_s)));
        QVERIFY(procs.open(QIODevice::WriteOnly | QIODevice::Truncate)); procs.close();
        QVERIFY(!VirtualSessionServiceScope::readMembers(fd, 123));
        QVERIFY(!VirtualSessionServiceScope::readMembers(-1, 123));
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionServiceScopeTest)
#include "VirtualSessionServiceScopeTest.moc"
