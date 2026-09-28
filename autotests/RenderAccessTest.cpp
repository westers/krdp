// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX8 B3: inside a virtual desktop the worker's encoder probe found no render node (ace,
// cray), so it reported no hardware encoder while KPipeWire encoded in hardware. The sandbox's
// /dev is a tmpfs with the granted node bind-mounted onto an empty regular file: readdir() says
// DT_REG, QDir::System dropped it. These tests reproduce that sandbox with bwrap (skipped when
// user namespaces are unavailable), and cover the group and permission handling.

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>
#include <pwd.h>
#include <unistd.h>

#include "EncoderSupport.h"
#include "RenderAccess.h"
#include "RenderNodes.h"
#include "VaapiDriverMode.h"
#include "VirtualSessionLaunchPlan.h"

using namespace KRdp;

namespace
{
QString bwrap()
{
    return QStandardPaths::findExecutable(QStringLiteral("bwrap"));
}

/// Runs this test binary in \a mode inside a bwrap sandbox shaped like the virtual desktop's
/// (fresh /dev, one node bind-mounted); nullopt when bwrap cannot run here.
std::optional<QByteArray> inSandbox(const QString &mode, const QStringList &bindings)
{
    const QString program = bwrap();
    if (program.isEmpty()) return std::nullopt;
    QStringList arguments{QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"), QStringLiteral("--proc"), QStringLiteral("/proc"),
                          QStringLiteral("--dev"), QStringLiteral("/dev")};
    arguments += bindings;
    arguments += {QCoreApplication::applicationFilePath(), mode};
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("KRDP_RENDER_NODE"));
    environment.insert(QStringLiteral("LIBVA_MESSAGING_LEVEL"), QStringLiteral("1")); // libva prints info on stdout
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qInfo().noquote() << "bwrap unavailable here:" << process.readAllStandardError().left(300);
        return std::nullopt;
    }
    return process.readAllStandardOutput().trimmed();
}

QByteArray lastLine(const QByteArray &output)
{
    return output.split('\n').last().trimmed();
}
}

class RenderAccessTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void listUsesStatNotTheEntryType();
    void bindMountedNodeIsListedInSandbox();
    void probeInSandboxFindsTheHostsHardware();
    void preferredNodeComesFirst();
    void accessThroughOwnerGroupOrOther();
    void warningNamesTheFix();
    void userGroupsMatchTheSystem();
    void vaapiDriverModes();
    void launchPlanCarriesTheVaapiDriver();
};

void RenderAccessTest::listUsesStatNotTheEntryType()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile regular(directory.filePath(QStringLiteral("renderD128")));
    QVERIFY(regular.open(QIODevice::WriteOnly));
    regular.close();
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("renderD129")));
    // A symlink to a character device stats as one: the same rule as a bind mount.
    QVERIFY(QFile::link(QStringLiteral("/dev/null"), directory.filePath(QStringLiteral("renderD130"))));
    QVERIFY(QFile::link(QStringLiteral("/dev/null"), directory.filePath(QStringLiteral("card0"))));
    QCOMPARE(RenderNodes::list(directory.path()), QStringList{directory.filePath(QStringLiteral("renderD130"))});
    QVERIFY(RenderNodes::list(directory.filePath(QStringLiteral("missing"))).isEmpty());
}

void RenderAccessTest::bindMountedNodeIsListedInSandbox()
{
    const auto listed = inSandbox(QStringLiteral("--list"),
                                  {QStringLiteral("--dev-bind"), QStringLiteral("/dev/null"), QStringLiteral("/dev/dri/renderD200")});
    if (!listed) QSKIP("bwrap cannot create a sandbox here");
    // The line after the stat() list is what QDir's QDir::System filter returned (the old code).
    const auto lines = listed->split('\n');
    QCOMPARE(lines.value(0), QByteArray("stat: /dev/dri/renderD200"));
    qInfo() << "QDir::System in the same sandbox:" << lines.value(1);
}

void RenderAccessTest::probeInSandboxFindsTheHostsHardware()
{
    if (bwrap().isEmpty()) QSKIP("no bwrap");
    const auto host = EncoderSupport::probeUncached();
    if (host.renderNode.isEmpty()) QSKIP("no hardware encoder on this host");
    const auto sandboxed = inSandbox(QStringLiteral("--probe"), {QStringLiteral("--dev-bind"), host.renderNode, host.renderNode});
    if (!sandboxed) QSKIP("bwrap cannot create a sandbox here");
    // Before AUD-FIX8 this was "avc sw, hevc sw, av1 sw, avc444 none" with a hardware host.
    QCOMPARE(QString::fromUtf8(lastLine(*sandboxed)), EncoderSupport::describe(host));
}

void RenderAccessTest::preferredNodeComesFirst()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (const auto *name : {"renderD128", "renderD129", "renderD130"}) {
        QVERIFY(QFile::link(QStringLiteral("/dev/null"), directory.filePath(QString::fromLatin1(name))));
    }
    const auto preferred = directory.filePath(QStringLiteral("renderD129"));
    QCOMPARE(RenderNodes::ordered(preferred, directory.path()),
             (QStringList{preferred, directory.filePath(QStringLiteral("renderD128")), directory.filePath(QStringLiteral("renderD130"))}));
    QCOMPARE(RenderNodes::ordered(QStringLiteral("/dev/dri/renderD999"), directory.path()).first(), directory.filePath(QStringLiteral("renderD128")));
}

void RenderAccessTest::accessThroughOwnerGroupOrOther()
{
    const RenderAccess::Node render{QStringLiteral("/dev/dri/renderD128"), 0, 990, 0660};
    QVERIFY(!RenderAccess::canUse(render, 1000, {1000, 4, 27}));
    QVERIFY(RenderAccess::canUse(render, 1000, {1000, 990}));
    QVERIFY(RenderAccess::canUse(render, 0, {})); // root
    QVERIFY(RenderAccess::canUse({render.path, 1000, 0, 0600}, 1000, {1000})); // the virtual desktop's private node
    QVERIFY(!RenderAccess::canUse({render.path, 1000, 0, 0400}, 1000, {1000}));
    QVERIFY(RenderAccess::canUse({render.path, 0, 990, 0666}, 1000, {1000}));
    QVERIFY(!RenderAccess::canUse({render.path, 0, 990, 0640}, 1000, {990})); // read-only for the group
}

void RenderAccessTest::warningNamesTheFix()
{
    const QList<RenderAccess::Node> nodes{{QStringLiteral("/dev/dri/renderD128"), 0, 990, 0660}, {QStringLiteral("/dev/dri/renderD129"), 0, 990, 0660}};
    const auto name = [](gid_t gid) {
        return gid == 990 ? QStringLiteral("render") : QString::number(gid);
    };
    const auto warning = RenderAccess::missingGroupWarning(QStringLiteral("westers"), 1000, {1000, 27}, nodes, QStringLiteral("no hardware"), name);
    QVERIFY2(warning.contains(QStringLiteral("usermod -aG render westers")), qPrintable(warning));
    QVERIFY(warning.contains(QStringLiteral("/dev/dri/renderD128, /dev/dri/renderD129")));
    QVERIFY(warning.contains(QStringLiteral("no hardware")));
    QVERIFY(RenderAccess::missingGroupWarning(QStringLiteral("westers"), 1000, {1000, 990}, nodes, {}, name).isEmpty());
    QVERIFY(RenderAccess::missingGroupWarning(QStringLiteral("westers"), 1000, {1000}, {}, {}, name).isEmpty());
}

void RenderAccessTest::userGroupsMatchTheSystem()
{
    const passwd *account = getpwuid(getuid());
    QVERIFY(account && account->pw_name);
    const auto groups = RenderAccess::userGroups(account->pw_name, account->pw_gid);
    QVERIFY(groups);
    QVERIFY(std::find(groups->begin(), groups->end(), account->pw_gid) != groups->end());
    QVERIFY(!RenderAccess::userGroups(nullptr, 0));
    // `id -G <user>` reads the group database, not this process's groups (a user added to
    // render today is in it before logging in again - ace).
    QProcess id;
    id.start(QStringLiteral("id"), {QStringLiteral("-G"), QString::fromLocal8Bit(account->pw_name)});
    if (!id.waitForFinished(5000) || id.exitCode() != 0) QSKIP("no id(1)");
    QSet<gid_t> expected;
    for (const auto &value : id.readAllStandardOutput().trimmed().split(' ')) {
        expected.insert(gid_t(value.toUInt()));
    }
    QCOMPARE(QSet<gid_t>(groups->begin(), groups->end()), expected);
}

void RenderAccessTest::vaapiDriverModes()
{
    QCOMPARE(VaapiDriverMode::normalize({}), std::optional(QStringLiteral("auto")));
    QCOMPARE(VaapiDriverMode::normalize(QStringLiteral("IHD")), std::optional(QStringLiteral("iHD")));
    QCOMPARE(VaapiDriverMode::normalize(QStringLiteral("disabled")), std::optional(QStringLiteral("off")));
    QVERIFY(!VaapiDriverMode::normalize(QStringLiteral("nvidia")));

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("LIBVA_DRIVER_NAME"), QStringLiteral("radeonsi")); // from the desktop session
    environment.insert(QStringLiteral("KRDP_AUTO_VAAPI_DRIVER"), QStringLiteral("0"));
    VaapiDriverMode::apply(environment, QStringLiteral("auto"));
    QCOMPARE(environment.value(QStringLiteral("LIBVA_DRIVER_NAME")), QStringLiteral("radeonsi"));
    QVERIFY(!environment.contains(QStringLiteral("KRDP_AUTO_VAAPI_DRIVER")));
    VaapiDriverMode::apply(environment, QStringLiteral("iHD"));
    QCOMPARE(environment.value(QStringLiteral("KRDP_FORCE_VAAPI_DRIVER")), QStringLiteral("iHD"));
    QCOMPARE(environment.value(QStringLiteral("LIBVA_DRIVER_NAME")), QStringLiteral("iHD"));
    VaapiDriverMode::apply(environment, QStringLiteral("off"));
    QVERIFY(!environment.contains(QStringLiteral("KRDP_FORCE_VAAPI_DRIVER")));
    QCOMPARE(environment.value(QStringLiteral("KRDP_AUTO_VAAPI_DRIVER")), QStringLiteral("0"));
}

void RenderAccessTest::launchPlanCarriesTheVaapiDriver()
{
    const VirtualSessionLaunchPlan::Account account{1000, QStringLiteral("westers"), QStringLiteral("/home/westers")};
    const QString session = QStringLiteral("0a1b2c3d-0000-4000-8000-000000000003");
    VirtualSessionLaunchPlan::Configuration config{QStringLiteral("/usr/share/krdp/virtual-session/launch-virtual-session.sh"),
                                                   QStringLiteral("/usr/bin/krdp-console-worker"), QStringLiteral("/usr/share/krdp/virtual-session/support"),
                                                   {QStringLiteral("0000:00:02.0")}};
    const auto automatic = VirtualSessionLaunchPlan::build(1000, account, session, config);
    QVERIFY(automatic);
    QVERIFY(!automatic->arguments.contains(QStringLiteral("--vaapi-driver"))); // auto: the launcher's default
    config.vaapiDriver = QStringLiteral("iHD");
    const auto forced = VirtualSessionLaunchPlan::build(1000, account, session, config);
    QVERIFY(forced);
    const auto at = forced->arguments.indexOf(QStringLiteral("--vaapi-driver"));
    QVERIFY(at > 0);
    QCOMPARE(forced->arguments.value(at + 1), QStringLiteral("iHD"));
    config.vaapiDriver = QStringLiteral("nvidia");
    QString error;
    QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, session, config, &error));
    QVERIFY(error.contains(QStringLiteral("VaapiDriverMode")));
}

int main(int argc, char **argv)
{
    // Sandboxed helper modes (see inSandbox()).
    if (argc == 2 && qstrcmp(argv[1], "--list") == 0) {
        QCoreApplication app(argc, argv);
        std::printf("stat: %s\n", qPrintable(RenderNodes::list().join(QLatin1Char(' '))));
        std::printf("QDir::System: %s\n",
                    qPrintable(QDir(QStringLiteral("/dev/dri")).entryList({QStringLiteral("renderD*")}, QDir::System, QDir::Name).join(QLatin1Char(' '))));
        return 0;
    }
    if (argc == 2 && qstrcmp(argv[1], "--probe") == 0) {
        QCoreApplication app(argc, argv);
        std::printf("%s\n", qPrintable(EncoderSupport::describe(EncoderSupport::probeUncached())));
        return 0;
    }
    QCoreApplication app(argc, argv);
    RenderAccessTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "RenderAccessTest.moc"
