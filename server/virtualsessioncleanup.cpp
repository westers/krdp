// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// ExecStopPost only, never an RDP method or a generic root cleanup command.
#include "VirtualSessionJournal.h"
#include "VirtualSessionKeeperProcess.h"
#include "VirtualSessionServiceScope.h"
#include "VirtualSessionLoginRecovery.h"
#include "VirtualSessionCleanupBirth.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <sys/syscall.h>
#include <unistd.h>
namespace {
int refused(const char *stage) { qCritical("Virtual session cleanup unresolved: %s", stage); return 1; }
bool waitGone(const KRdp::VirtualSessionKeeperProcess &keeper, int milliseconds)
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) {
        const auto gone = keeper.gone();
        if (!gone) return false;
        if (*gone) return true;
        QThread::msleep(20);
    }
    return keeper.gone() == std::optional<bool>(true);
}
}
int main(int argc, char **argv)
{
    for (char **entry = environ; entry && *entry; ++entry) {
        const QByteArray value(*entry);
        if (value != "PATH=/usr/bin:/bin" && value != "LANG=C.UTF-8") return refused("unclean environment");
    }
    if (getuid() || geteuid()) return refused("explicit root service required");
    if (syscall(SYS_close_range, 3U, ~0U, 0U)) return refused("inherited descriptors");
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({QStringLiteral("session"), QStringLiteral("Trusted systemd desktop UUID"), QStringLiteral("uuid")});
    parser.process(app);
    if (!parser.positionalArguments().isEmpty()) return refused("arguments");
    const auto record = KRdp::VirtualSessionJournal::readLaunchIntent(parser.value(QStringLiteral("session")));
    if (!record) return refused("committed launch intent");
    QFile boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!boot.open(QIODevice::ReadOnly) || QString::fromLatin1(boot.read(128)).trimmed() != record->boot)
        return refused("current boot");
    auto scope = KRdp::VirtualSessionServiceScope::open(record->session);
    if (!scope) return refused("dedicated service scope");
    const auto evidence = KRdp::VirtualSessionCleanupBirth::inspect([&] {
        bool missing = false;
        const auto value = KRdp::VirtualSessionJournal::readKeeper(*record, &missing);
        return KRdp::VirtualSessionCleanupBirth{value, missing};
    }, [&] { return scope->descendantsGone(); });
    const auto birth = evidence.keeper;
    if (!birth) {
        // Registration cannot begin without this durable record. This inference
        // also requires no surviving pre-PAM keeper able to publish it later.
        if (!evidence.absent) return refused("keeper identity or surviving service processes");
        // inspect() read missing twice around empty-scope proof; the consumed
        // launch cannot replay, and no keeper remains able to register with PAM.
        return KRdp::VirtualSessionJournal::recordReconciled(*record) ? 0 : refused("durable reconciliation evidence");
    }
    auto keeper = KRdp::VirtualSessionKeeperProcess::pin(*birth);
    if (!keeper) return refused("original keeper identity");
    const auto gone = keeper->gone();
    if (!gone) return refused("original keeper liveness");
    const auto desktopsGone = *gone ? scope->descendantsGone() : scope->onlyKeeperRemains(birth->pid);
    if (desktopsGone != std::optional<bool>(true)) return refused("desktop extinction");
    // Give EOF/owner-watch cleanup a chance before forced keeper termination.
    if (!*gone && !waitGone(*keeper, 500)) {
        if (!keeper->kill() || !waitGone(*keeper, 2000)) return refused("keeper extinction");
    }
    if (scope->descendantsGone() != std::optional<bool>(true)) return refused("final desktop extinction");
    const auto closed = KRdp::VirtualSessionJournal::keeperClosed(*record, *birth);
    if (!closed) return refused("successful-close evidence");
    const auto result = KRdp::VirtualSessionLoginRecovery::reconcile(QDBusConnection::systemBus(), *record, *birth, 15000, *closed);
    if (result != KRdp::VirtualSessionLoginRecovery::Result::Removed && result != KRdp::VirtualSessionLoginRecovery::Result::AlreadyClosed)
        return refused("logind identity, disappearance, or registration still uncertain");
    if (!KRdp::VirtualSessionJournal::recordReconciled(*record)) return refused("durable reconciliation evidence");
    qInfo("Virtual session process/logind reconciliation completed; launch records preserved");
    return 0;
}
