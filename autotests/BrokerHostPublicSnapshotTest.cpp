// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// S1 (OPT-057): the world-readable public-metadata snapshot never carries
// verifiers, account names/aliases, certificate/key paths or key material, is
// mode 0644, and is read back only from a safe regular file.
#include "BrokerHostAdmin.h"
#include "BrokerHostPublicSnapshot.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace KRdp;
using Scope = BrokerHostSettings::Scope;
namespace Snapshot = KRdp::BrokerHostPublicSnapshot;

class BrokerHostPublicSnapshotTest : public QObject {
    Q_OBJECT
    // What the privileged helper holds, including everything that must not leak.
    static QJsonObject hostileFullView(Scope scope)
    {
        const QByteArray document = "FARSIDE_CONSOLE_PORT=4321\nFARSIDE_CONSOLE_CERTIFICATE=/etc/farside/tls-imports/console/x/certificate.crt\n"
                                    "FARSIDE_CONSOLE_CERTIFICATE_KEY=/etc/farside/tls-imports/console/x/private.key\n"
                                    "FARSIDE_VIRTUAL_CERTIFICATE=/etc/farside/tls-imports/virtual/x/certificate.crt\n"
                                    "FARSIDE_VIRTUAL_CERTIFICATE_KEY=/etc/farside/tls-imports/virtual/x/private.key\nUNRELATED=fixture-secret\n";
        auto view = BrokerHostAdmin::view(scope, true, document).value;
        if (scope == Scope::VirtualSession) {
            view.insert(u"renderDevices"_s, QJsonArray{QJsonObject{{u"pci"_s, u"0000:01:00.0"_s}, {u"driver"_s, u"nvidia"_s},
                {u"render"_s, u"/dev/dri/renderD128"_s}, {u"verifier"_s, u"leak-device"_s}}});
        } else {
            view.insert(u"tls"_s, QJsonObject{{u"state"_s, u"valid"_s}, {u"administratorManaged"_s, false}, {u"fingerprint"_s, u"AA:BB"_s},
                {u"algorithm"_s, u"ECDSA"_s}, {u"notBefore"_s, u"2026-01-01T00:00:00Z"_s}, {u"notAfter"_s, u"2036-01-01T00:00:00Z"_s},
                {u"privateKeyPem"_s, u"leak-key-bytes"_s}, {u"path"_s, u"/etc/farside/console.key"_s}});
            view.insert(u"cameraLoopback"_s, QJsonObject{{u"supported"_s, scope == Scope::Console}, {u"state"_s, u"disabled"_s}});
        }
        // Account/alias material an unprivileged reader must never see.
        view.insert(u"accounts"_s, QJsonArray{u"alice"_s});
        view.insert(u"credentials"_s, QJsonArray{QJsonObject{{u"alias"_s, u"guest"_s}, {u"owner"_s, u"alice"_s}, {u"verifier"_s, u"leak-verifier"_s}}});
        view.insert(u"runtimeVerified"_s, false);
        view.insert(u"application"_s, scope == Scope::VirtualSession ? u"new-desktops"_s : u"broker-restart"_s);
        return view;
    }
    static QByteArray slurp(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
private Q_SLOTS:
    void sanitizedSnapshotHasNoSecretsAccountsOrPaths_data()
    {
        QTest::addColumn<int>("scope");
        QTest::newRow("console") << int(Scope::Console);
        QTest::newRow("virtual") << int(Scope::Virtual);
        QTest::newRow("session") << int(Scope::VirtualSession);
    }
    void sanitizedSnapshotHasNoSecretsAccountsOrPaths()
    {
        QFETCH(int, scope);
        const auto host = Scope(scope);
        const auto full = hostileFullView(host);
        if (host != Scope::VirtualSession) QVERIFY(QJsonDocument(full).toJson().contains("/etc/farside")); // the fixture really is hostile
        const auto clean = Snapshot::sanitize(host, full);
        const auto text = QJsonDocument(clean).toJson();
        for (const auto &needle : {"/etc/farside", "alice", "guest", "verifier", "leak-", "private.key", "certificate.crt", ".key", ".crt",
                                   "fixture-secret", "accounts", "credentials", "privateKeyPem", "\"path\""})
            QVERIFY2(!text.contains(needle), needle);
        for (const auto &name : {u"values"_s, u"defaults"_s, u"effective"_s}) {
            QVERIFY(!clean[name].toObject().contains(u"Certificate"_s));
            QVERIFY(!clean[name].toObject().contains(u"CertificateKey"_s));
        }
        QCOMPARE(clean[u"revision"_s].toString(), full[u"revision"_s].toString());
        QVERIFY(clean[u"effective"_s].toObject().contains(host == Scope::VirtualSession ? u"VaapiDriver"_s : u"Quality"_s));
        if (host == Scope::VirtualSession) {
            QCOMPARE(clean[u"renderDevices"_s].toArray().size(), 1);
            QCOMPARE(clean[u"renderDevices"_s].toArray().at(0).toObject().size(), 3);
        } else {
            QCOMPARE(clean[u"tls"_s].toObject()[u"fingerprint"_s].toString(), u"AA:BB"_s);
            QVERIFY(!clean[u"tls"_s].toObject().contains(u"privateKeyPem"_s));
        }
        QCOMPARE(clean[u"effective"_s].toObject().size() + 2 * !(host == Scope::VirtualSession),
                 full[u"effective"_s].toObject().size()); // the two TLS path keys are the only effective keys removed
    }
    void writesMode0644AtomicallyAndReadsBack()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto directory = root.filePath(u"public"_s); // created by the writer
        QString error;
        QVERIFY2(Snapshot::write(directory, Scope::Console, hostileFullView(Scope::Console), &error), qPrintable(error));
        struct stat info{};
        const auto path = directory + u"/"_s + Snapshot::snapshotFileName(Scope::Console);
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &info), 0);
        QCOMPARE(info.st_mode & 07777, mode_t(0644));
        QCOMPARE(info.st_uid, ::geteuid());               // root in production
        QCOMPARE(info.st_nlink, nlink_t(1));
        QCOMPARE(::stat(QFile::encodeName(directory).constData(), &info), 0);
        QCOMPARE(info.st_mode & 07777, mode_t(0755));
        QVERIFY(!slurp(path).contains("/etc/farside"));
        QCOMPARE(QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{Snapshot::snapshotFileName(Scope::Console)}); // no temp left
        // Overwrite is atomic and idempotent.
        QVERIFY(Snapshot::write(directory, Scope::Console, hostileFullView(Scope::Console), &error));
        const auto read = Snapshot::read(directory, Scope::Console, true);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QCOMPARE(read.value, Snapshot::sanitize(Scope::Console, hostileFullView(Scope::Console)));
    }
    void helperUmaskDoesNotNarrowTheMode()
    {
        const auto previous = ::umask(0077); // the settings helper runs with 0077
        QTemporaryDir root;
        QString error;
        const bool wrote = Snapshot::write(root.filePath(u"public"_s), Scope::Virtual, hostileFullView(Scope::Virtual), &error);
        ::umask(previous);
        QVERIFY2(wrote, qPrintable(error));
        struct stat info{};
        QCOMPARE(::stat(QFile::encodeName(root.filePath(u"public/virtual.json"_s)).constData(), &info), 0);
        QCOMPARE(info.st_mode & 07777, mode_t(0644));
        QCOMPARE(::stat(QFile::encodeName(root.filePath(u"public"_s)).constData(), &info), 0);
        QCOMPARE(info.st_mode & 07777, mode_t(0755));
    }
    void readerRefusesUnsafeFiles()
    {
        QTemporaryDir root;
        QString error;
        QVERIFY(Snapshot::write(root.path(), Scope::Console, hostileFullView(Scope::Console), &error));
        const auto file = root.filePath(Snapshot::snapshotFileName(Scope::Console));
        // Owned by the current (non-root) user: only trusted when explicitly allowed.
        if (::geteuid()) QVERIFY(!Snapshot::read(root.path(), Scope::Console, false).error.isEmpty());
        QVERIFY(Snapshot::read(root.path(), Scope::Console, true).error.isEmpty());
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther));
        QVERIFY(!Snapshot::read(root.path(), Scope::Console, true).error.isEmpty());       // group-writable
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
        QVERIFY(Snapshot::read(root.path(), Scope::Console, true).error.isEmpty());
        QVERIFY(QFile::remove(file));
        QVERIFY(QFile::link(root.filePath(u"elsewhere"_s), file));                          // dangling or foreign symlink
        QVERIFY(!Snapshot::read(root.path(), Scope::Console, true).error.isEmpty());
        QVERIFY(QFile::remove(file));
        QFile bad(file); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("not json"); bad.close();
        QVERIFY(!Snapshot::read(root.path(), Scope::Console, true).error.isEmpty());
        QVERIFY(!Snapshot::read(root.filePath(u"missing"_s), Scope::Console, true).error.isEmpty());
    }
    void keepingTlsPreservesAdministratorPathOverridesWithoutThem()
    {
        // The snapshot hides the path values, so a KCM that keeps TLS cannot echo
        // them back. The save must keep the saved overrides, not drop them.
        const QByteArray bytes("FARSIDE_CONSOLE_PORT=4321\nFARSIDE_CONSOLE_CERTIFICATE=/etc/ssl/admin.crt\nFARSIDE_CONSOLE_CERTIFICATE_KEY=/etc/ssl/admin.key\n");
        const QJsonObject request{{u"version"_s, 1}, {u"operation"_s, u"save"_s}, {u"scope"_s, u"console"_s},
            {u"revision"_s, BrokerHostAdmin::revision(Scope::Console, true, bytes)},
            {u"values"_s, QJsonObject{{u"Port"_s, u"5432"_s}}}, {u"tls"_s, QJsonObject{{u"mode"_s, u"keep"_s}}}};
        const auto update = BrokerHostAdmin::prepare(Scope::Console, true, bytes, request);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        QCOMPARE(update.effective[u"Certificate"_s].toString(), u"/etc/ssl/admin.crt"_s);
        QCOMPARE(update.effective[u"CertificateKey"_s].toString(), u"/etc/ssl/admin.key"_s);
        QCOMPARE(update.effective[u"Port"_s].toString(), u"5432"_s);
    }
};
QTEST_APPLESS_MAIN(BrokerHostPublicSnapshotTest)
#include "BrokerHostPublicSnapshotTest.moc"
