// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualResizeExecutor.h"
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <memory>
#include <utility>

using namespace KRdp;
namespace V = KRdp::VirtualResize;

VirtualResizeExecutor::VirtualResizeExecutor(QObject *parent, Runner runner)
    : QObject(parent), m_runner(std::move(runner))
{
}

void VirtualResizeExecutor::run(const QStringList &arguments, Reply reply)
{
    const QPointer<VirtualResizeExecutor> alive(this);
    auto once = std::make_shared<bool>(false);
    auto guarded = [alive, once, reply = std::move(reply)](bool ok, QByteArray data) {
        if (alive && !std::exchange(*once, true)) reply(ok, std::move(data));
    };
    if (m_runner) {
        const auto runner = m_runner;
        runner(arguments, std::move(guarded));
        return;
    }
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    const auto output = std::make_shared<QByteArray>();
    const auto failed = std::make_shared<bool>(false);
    const auto collect = [process, output, failed] {
        const auto bytes = process->readAllStandardOutput();
        if (output->size() + bytes.size() > 1024 * 1024) {
            *failed = true;
            process->kill();
        } else if (!*failed) {
            output->append(bytes);
        }
    };
    const auto complete = [process, timer, output, failed, collect, guarded](bool ok) {
        collect();
        timer->stop();
        process->deleteLater();
        guarded(ok && !*failed, *output);
    };
    connect(process, &QProcess::readyReadStandardOutput, process, collect);
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::finished, this, [complete](int code, QProcess::ExitStatus status) {
        complete(code == 0 && status == QProcess::NormalExit);
    });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        // Crashed processes still emit finished; do not begin another command
        // before the previous process has actually exited.
        if (error == QProcess::FailedToStart) complete(false);
    });
    connect(timer, &QTimer::timeout, process, [process, failed] { *failed = true; process->kill(); });
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("kscreen-doctor"));
    timer->start(5000);
    process->start(executable.isEmpty() ? QStringLiteral("kscreen-doctor") : executable, arguments);
}

bool VirtualResizeExecutor::resize(QSize pixels, double scale)
{
    if (m_busy || !V::validRequest(pixels, scale)) return false;
    m_busy = true;
    m_cancelled = false;
    run({QStringLiteral("-j")}, [this, pixels, scale](bool ok, const QByteArray &json) {
        QString error;
        const auto state = ok ? V::snapshot(json, &error) : std::nullopt;
        if (!state) {
            finish({{}, {}, ok ? error : QStringLiteral("could not read virtual output"), false});
            return;
        }
        const V::Plan plan{*state, pixels, V::normalizedScale(scale)};
        if (m_cancelled) {
            finish({plan, state, QStringLiteral("virtual resize cancelled before apply"), false});
            return;
        }
        if (V::matchingMode(*state, pixels, state->current.refresh)) {
            apply(plan, *state);
            return;
        }
        if (state->modes.size() >= 64) {
            finish({plan, state, QStringLiteral("virtual output mode limit reached (64); select an existing size"), false});
            return;
        }
        run(V::add(plan), [this, plan](bool commandOk, const QByteArray &) {
            // Addition alone never selects a mode; discover the new ID even
            // after a nonzero helper exit, since its side effect may exist.
            run({QStringLiteral("-j")}, [this, plan, commandOk](bool ok, const QByteArray &json) {
                QString error;
                const auto state = ok ? V::snapshot(json, &error) : std::nullopt;
                if (!state || !V::sameOutput(*state, plan.before)
                    || state->current.pixels != plan.before.current.pixels || state->current.refresh != plan.before.current.refresh
                    || !V::sameScale(state->scale, plan.before.scale)) {
                    finish({plan, {}, QStringLiteral("virtual output changed or could not be verified after adding mode"), true});
                    return;
                }
                if (!commandOk || !V::matchingMode(*state, plan.pixels, plan.before.current.refresh)) {
                    finish({plan, state, QStringLiteral("custom mode addition failed or was not advertised"), false});
                    return;
                }
                apply(plan, *state);
            });
        });
    });
    return true;
}

void VirtualResizeExecutor::apply(const V::Plan &plan, const V::Snapshot &state)
{
    if (m_cancelled) {
        finish({plan, state, QStringLiteral("virtual resize cancelled before apply"), false});
        return;
    }
    if (V::matches(state, plan)) {
        finish({plan, state, {}, false}); // Still requires a fresh keyframe in the lifecycle.
        return;
    }
    const auto mode = V::matchingMode(state, plan.pixels, plan.before.current.refresh);
    if (!mode) {
        finish({plan, state, QStringLiteral("requested virtual mode disappeared"), false});
        return;
    }
    const QPointer<VirtualResizeExecutor> alive(this);
    Q_EMIT mutationStarting();
    if (!alive) return;
    if (m_cancelled) {
        finish({plan, state, QStringLiteral("virtual resize cancelled before apply"), false});
        return;
    }
    run(V::select(state, *mode, plan.scale), [this, plan](bool commandOk, const QByteArray &) {
        run({QStringLiteral("-j")}, [this, plan, commandOk](bool ok, const QByteArray &json) {
            QString error;
            const auto state = ok ? V::snapshot(json, &error) : std::nullopt;
            const bool verified = commandOk && state && V::matches(*state, plan);
            finish({plan, state, verified ? QString() : QStringLiteral("virtual mode/scale apply was not verified"), true});
        });
    });
}

bool VirtualResizeExecutor::rollback(const V::Plan &plan)
{
    if (m_busy || !plan.valid()) return false;
    m_busy = true;
    run({QStringLiteral("-j")}, [this, plan](bool ok, const QByteArray &json) {
        QString error;
        const auto state = ok ? V::snapshot(json, &error) : std::nullopt;
        if (!state || !V::sameOutput(*state, plan.before)) {
            finish({plan, {}, QStringLiteral("cannot verify virtual output identity for rollback"), true});
            return;
        }
        auto expected = *state;
        QStringList args;
        const QString prefix = QStringLiteral("output.%1.").arg(state->name);
        if (state->current.pixels == plan.pixels && state->current.refresh == plan.before.current.refresh
            && state->current.pixels != plan.before.current.pixels) {
            const auto previous = V::matchingMode(*state, plan.before.current.pixels, plan.before.current.refresh);
            if (!previous) {
                finish({plan, {}, QStringLiteral("original virtual mode no longer available for rollback"), true});
                return;
            }
            args << prefix + QStringLiteral("mode.") + previous->id;
            expected.current = *previous;
        }
        if (V::sameScale(state->scale, plan.scale) && !V::sameScale(state->scale, plan.before.scale)) {
            args << prefix + QStringLiteral("scale.") + QString::number(plan.before.scale, 'g', 12);
            expected.scale = plan.before.scale;
        }
        if (args.isEmpty()) {
            finish({plan, state, {}, true}); // Independent changes remain authoritative.
            return;
        }
        const QPointer<VirtualResizeExecutor> alive(this);
        Q_EMIT mutationStarting();
        if (!alive) return;
        run(args, [this, plan, expected](bool, const QByteArray &) {
            run({QStringLiteral("-j")}, [this, plan, expected](bool ok, const QByteArray &json) {
                QString error;
                const auto state = ok ? V::snapshot(json, &error) : std::nullopt;
                const bool verified = state && V::sameOutput(*state, expected) && state->current.pixels == expected.current.pixels
                    && state->current.refresh == expected.current.refresh && V::sameScale(state->scale, expected.scale);
                finish({plan, verified ? state : std::nullopt, verified ? QString() : QStringLiteral("virtual rollback was not verified"), true});
            });
        });
    });
    return true;
}

void VirtualResizeExecutor::finish(Result result)
{
    m_busy = false;
    Q_EMIT finished(result); // No member access after an external callback.
}
