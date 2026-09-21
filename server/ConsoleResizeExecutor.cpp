// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleResizeExecutor.h"

#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <memory>
#include <utility>

using namespace KRdp;

ConsoleResizeExecutor::ConsoleResizeExecutor(QObject *parent, Runner runner)
    : QObject(parent), m_runner(std::move(runner))
{
}

void ConsoleResizeExecutor::run(const QStringList &arguments, Reply reply)
{
    // Test runners and process callbacks may outlive the executor.
    const QPointer<ConsoleResizeExecutor> guard(this);
    auto guardedReply = [guard, reply = std::move(reply)](bool ok, QByteArray output) {
        if (guard) {
            reply(ok, std::move(output));
        }
    };
    if (m_runner) {
        m_runner(arguments, std::move(guardedReply));
        return;
    }
    auto *process = new QProcess(this);
    auto *deadline = new QTimer(process);
    deadline->setSingleShot(true);
    auto delivered = std::make_shared<bool>(false);
    const auto complete = [process, deadline, delivered, reply = std::move(guardedReply)](bool ok) {
        if (std::exchange(*delivered, true)) {
            return;
        }
        deadline->stop();
        const QByteArray output = process->readAllStandardOutput();
        process->deleteLater();
        reply(ok, output);
    };
    connect(process, &QProcess::finished, this, [complete](int code, QProcess::ExitStatus status) {
        complete(code == 0 && status == QProcess::NormalExit);
    });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError) { complete(false); });
    connect(deadline, &QTimer::timeout, process, [process]() { process->kill(); });
    // Drain stderr, which is diagnostic only, to keep a broken helper bounded.
    connect(process, &QProcess::readyReadStandardError, process, [process]() { process->readAllStandardError(); });
    connect(process, &QProcess::readyReadStandardOutput, process, [process]() {
        if (process->bytesAvailable() > 1024 * 1024) {
            process->kill();
        }
    });
    deadline->start(5000);
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("kscreen-doctor"));
    process->start(executable.isEmpty() ? QStringLiteral("kscreen-doctor") : executable, arguments);
}

bool ConsoleResizeExecutor::resize(const QString &output, QSize pixels, double scale)
{
    if (m_busy) {
        return false;
    }
    m_busy = true;
    run({QStringLiteral("-j")}, [this, output, pixels, scale](bool ok, const QByteArray &snapshot) {
        const auto plan = ConsoleResize::plan(snapshot, output, pixels, scale);
        if (!ok || !plan.valid()) {
            finish(plan, ok ? plan.error : QStringLiteral("could not read physical output modes"));
            return;
        }
        if (ConsoleResize::matches(snapshot, plan)) {
            finish(plan, {}); // Avoid a needless modeset/cursor warp for an exact no-op.
            return;
        }
        apply(plan, false);
    });
    return true;
}

bool ConsoleResizeExecutor::restore(const ConsoleResize::Plan &plan)
{
    if (m_busy || !plan.valid()) {
        return false;
    }
    m_busy = true;
    run({QStringLiteral("-j")}, [this, plan](bool ok, const QByteArray &snapshot) {
        if (!ok) {
            finish(plan, QStringLiteral("could not read physical outputs before restoration"));
        } else if (!ConsoleResize::matches(snapshot, plan)) {
            // Local changes supersede our temporary mode. Do not undo them.
            finish(plan, {});
        } else {
            apply(plan, true);
        }
    });
    return true;
}

void ConsoleResizeExecutor::apply(ConsoleResize::Plan plan, bool restoring)
{
    Q_EMIT changing();
    run(restoring ? plan.restore : plan.apply, [this, plan, restoring](bool commandOk, const QByteArray &) {
        run({QStringLiteral("-j")}, [this, plan, restoring, commandOk](bool readOk, const QByteArray &snapshot) {
            if (readOk && ConsoleResize::matches(snapshot, plan, restoring)) {
                finish(plan, {}); // Readback is authoritative even if the helper exit was nonzero.
                return;
            }
            const QString error = !readOk ? QStringLiteral("could not verify physical output mode")
                : commandOk ? QStringLiteral("physical output did not adopt the requested mode and scale")
                            : QStringLiteral("physical mode command failed");
            const auto rollback = !restoring && readOk ? ConsoleResize::rollbackArgs(snapshot, plan) : QStringList{};
            if (rollback.isEmpty()) {
                finish(plan, error);
                return;
            }
            // Undo only the pieces of a partial apply still matching our
            // requested state; never overwrite an independently changed field.
            run(rollback, [this, plan, error](bool, const QByteArray &) {
                run({QStringLiteral("-j")}, [this, plan, error](bool ok, const QByteArray &restored) {
                    finish(plan, error + (ok && ConsoleResize::matches(restored, plan, true)
                                             ? QStringLiteral("; original mode restored")
                                             : QStringLiteral("; rollback not fully verified")));
                });
            });
        });
    });
}

void ConsoleResizeExecutor::finish(const ConsoleResize::Plan &plan, const QString &error)
{
    m_busy = false;
    Q_EMIT finished(plan, error);
}
