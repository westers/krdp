// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Disposable nonprivileged protocol/process fixture. Never opens PAM.
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
volatile sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
bool log(const QString &directory, const char *event, bool valid = true)
{
    QFile file(directory + QStringLiteral("/events"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("event"), QString::fromLatin1(event)},
        {QStringLiteral("pid"), qint64(getpid())}, {QStringLiteral("valid"), valid}}).toJson(QJsonDocument::Compact) + '\n';
    return file.write(bytes) == bytes.size();
}
}
int main(int argc, char **argv)
{
    alarm(10);
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &mask)) return 1;
    bool clean = !sigismember(&mask, SIGTERM) && !sigismember(&mask, SIGINT) && fcntl(200, F_GETFD) < 0;
    for (char **env = environ; env && *env; ++env) {
        const QByteArray value(*env);
        clean &= value == "PATH=/usr/bin:/bin" || value == "LANG=C.UTF-8";
    }
    QStringList args;
    for (int i = 0; i < argc; ++i) args.append(QString::fromLocal8Bit(argv[i]));
    const bool desktop = args.value(1) == QStringLiteral("desktop");
    const QString directory = desktop ? args.value(2) : QFileInfo(args[0]).path();
    const QString mode = desktop ? args.value(3) : QFileInfo(args[0]).fileName();
    if (desktop) {
        const int flags = fcntl(0, F_GETFL);
        if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK)) return 1;
        char token[33];
        clean &= read(0, token, sizeof(token)) == 32 && QByteArray(token, 32) == QByteArray(32, 's');
        close(0);
        struct sigaction action{}; sigemptyset(&action.sa_mask);
        action.sa_handler = mode == QStringLiteral("ignore") ? SIG_IGN : stop;
        if (sigaction(SIGTERM, &action, nullptr)) return 1;
        if (!log(directory, "desktop-start", clean)) return 1;
        if (mode == QStringLiteral("exit")) return 0;
        while (!stopped) poll(nullptr, 0, 10);
        return log(directory, "desktop-exit") ? 0 : 1;
    }
    struct stat input{}, output{};
    clean &= !fstat(0, &input) && !fstat(1, &output) && S_ISFIFO(input.st_mode) && S_ISFIFO(output.st_mode);
    clean &= args.value(args.indexOf(QStringLiteral("--parent")) + 1) == QString::number(getppid());
    if (!log(directory, "keeper-start", clean)) return 1;
    if (mode == QStringLiteral("hang-open")) while (true) pause();
    const auto value = [&](const QString &key) { return args.value(args.indexOf(key) + 1); };
    QJsonObject record{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("ready")},
        {QStringLiteral("session"), value(QStringLiteral("--session"))},
        {QStringLiteral("launch"), value(QStringLiteral("--launch"))},
        {QStringLiteral("uid"), 1000}, {QStringLiteral("leader"), qint64(getpid())},
        {QStringLiteral("login"), QStringLiteral("fixture1")}};
    if (mode == QStringLiteral("bad-launch")) record[QStringLiteral("launch")] = QStringLiteral("different");
    if (mode == QStringLiteral("bad-uid")) record[QStringLiteral("uid")] = QStringLiteral("1000");
    if (mode == QStringLiteral("bad-leader")) record[QStringLiteral("leader")] = qint64(getppid());
    QByteArray ready = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
    if (mode == QStringLiteral("oversized")) ready = QByteArray(1100, 'x');
    if (mode == QStringLiteral("malformed")) ready = "not json\n";
    if (mode == QStringLiteral("fragment")) {
        if (write(1, ready.constData(), 10) != 10) return 1;
        poll(nullptr, 0, 30);
        ready.remove(0, 10);
    }
    if (!log(directory, "keeper-ready")) return 1;
    // Marker must precede protocol publication: the receiver may launch its
    // sibling before this process gets another scheduling timeslice.
    if (write(1, ready.constData(), ready.size()) != ready.size()) return 1;
    if (mode == QStringLiteral("exit-retained")) { poll(nullptr, 0, 100); return 1; }
    QByteArray command;
    while (!command.contains('\n')) {
        char data[16]; const auto count = read(0, data, sizeof(data));
        if (count <= 0) return 1;
        command.append(data, count);
    }
    if (!log(directory, "keeper-stop", command == "stop\n")) return 1;
    if (mode == QStringLiteral("hang-close")) while (true) pause();
    return command == "stop\n" ? 0 : 1;
}
