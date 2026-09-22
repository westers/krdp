// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Disposable direct-child fixture, never a desktop or namespace launcher.
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <csignal>
#include <unistd.h>
namespace { volatile sig_atomic_t stopped = 0; void stop(int) { stopped = 1; } }
int main(int argc, char **argv)
{
    if (argc != 4) return 1;
    alarm(15); // Bound lifetime even if a failed test kills only its guardian.
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &mask)) return 1;
    const bool blocked = sigismember(&mask, SIGTERM) || sigismember(&mask, SIGINT);
    struct sigaction action{};
    action.sa_handler = QByteArray(argv[3]) == "ignore" ? SIG_IGN : stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, nullptr)) return 1;
    sigset_t term, oldMask;
    sigemptyset(&term); sigaddset(&term, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &term, &oldMask)) return 1;
    QFile ready(QString::fromLocal8Bit(argv[1]));
    if (!ready.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 1;
    ready.write(QJsonDocument(QJsonObject{{QStringLiteral("pid"), qint64(getpid())},
        {QStringLiteral("blocked"), blocked}}).toJson(QJsonDocument::Compact));
    ready.close();
    while (!stopped) sigsuspend(&oldMask);
    QFile exited(QString::fromLocal8Bit(argv[2]));
    if (!exited.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 1;
    return exited.write("graceful") == 8 ? 0 : 1;
}
