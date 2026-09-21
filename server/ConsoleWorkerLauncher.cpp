// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerLauncher.h"

#include <algorithm>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <QFile>
#include <QDebug>
#include <QProcess>

#include "ConsoleSeat.h"

namespace KRdp
{
namespace
{
QProcessEnvironment environmentFor(const ConsoleSeat::Session &session, QString *error)
{
    if (session.leader == 0) {
        *error = QStringLiteral("logind session has no leader process");
        return {};
    }
    QFile file(QStringLiteral("/proc/%1/environ").arg(session.leader));
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read session-leader environment");
        return {};
    }
    QProcessEnvironment environment;
    for (const auto &entry : file.readAll().split('\0')) {
        const int equals = entry.indexOf('=');
        if (equals > 0) {
            environment.insert(QString::fromLocal8Bit(entry.first(equals)), QString::fromLocal8Bit(entry.mid(equals + 1)));
        }
    }
    const QString runtime = environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
    const QString expectedRuntime = QStringLiteral("/run/user/%1").arg(session.uid);
    if (runtime != expectedRuntime || environment.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()
        || environment.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")).isEmpty()) {
        *error = QStringLiteral("session leader lacks a usable Wayland environment");
        return {};
    }
    return environment;
}
}

ConsoleWorkerLauncher::ConsoleWorkerLauncher(QString workerProgram, QObject *parent)
    : QObject(parent)
    , m_workerProgram(std::move(workerProgram))
{
}

ConsoleWorkerLauncher::~ConsoleWorkerLauncher() = default;

bool ConsoleWorkerLauncher::launch(const ConsoleHandoff::Target &target, const QString &socketName, const QByteArray &token, QString *error)
{
    if (error) {
        error->clear();
    }
    const auto sessions = ConsoleSeat::readLogindSessions(error);
    const auto found = std::find_if(sessions.cbegin(), sessions.cend(), [&target](const auto &session) {
        return session.id == target.sessionId && session.uid == target.uid;
    });
    if (found == sessions.cend()) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("selected logind session disappeared");
        }
        return false;
    }
    QString environmentError;
    auto environment = environmentFor(*found, &environmentError);
    if (environment.isEmpty()) {
        if (error) {
            *error = environmentError;
        }
        return false;
    }
    const passwd *account = getpwuid(target.uid);
    if (!account) {
        if (error) {
            *error = QStringLiteral("selected uid has no passwd entry");
        }
        return false;
    }
    auto process = std::make_unique<QProcess>();
    QProcess *raw = process.get();
    process->setProcessEnvironment(environment);
    process->setProgram(m_workerProgram);
    process->setArguments({QStringLiteral("--socket"), socketName, QStringLiteral("--session"), target.sessionId, QStringLiteral("--uid"), QString::number(target.uid), QStringLiteral("--token-hex"), QString::fromLatin1(token.toHex())});
    const gid_t gid = account->pw_gid;
    const QByteArray user = QByteArray(account->pw_name);
    process->setChildProcessModifier([raw, target, gid, user]() {
        if (initgroups(user.constData(), gid) != 0 || setgid(gid) != 0 || setuid(target.uid) != 0) {
            raw->failChildProcessModifier("cannot drop privileges into logind session");
        }
    });
    connect(raw, &QProcess::errorOccurred, this, [raw](QProcess::ProcessError) {
        qWarning().noquote() << "Console worker launch error:" << raw->errorString();
    });
    process->start();
    if (!process->waitForStarted(3000)) {
        if (error) {
            *error = process->errorString();
        }
        return false;
    }
    m_processes.push_back(std::move(process));
    return true;
}
}
