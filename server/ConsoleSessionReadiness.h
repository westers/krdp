// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QDir>
#include <QProcessEnvironment>
#include <QString>

#include "ConsoleSeat.h"

namespace KRdp
{
/**
 * Whether a logind session can take a console worker yet (AUD-FIX F6).
 *
 * On Sol the worker was started right after SDDM handed over, before the new
 * Plasma session's Wayland socket existed; Qt found no platform plugin and
 * aborted, and the broker only recovered on its 30 s failure backoff. The
 * launcher now waits, bounded and with backoff, until the session is Active
 * (not opening or closing), a process in it has a Wayland environment, and
 * that environment's socket accepts a connection.
 *
 * Pure: the launcher gathers the observation, this decides.
 */
namespace ConsoleSessionReadiness
{
enum class Verdict {
    Ready,
    Wait,
    Gone, ///< the session disappeared or changed owner: stop waiting
};

struct Observation {
    std::optional<ConsoleSeat::Session> session;
    quint32 expectedUid = 0;
    /// Empty when no process in the session has a usable Wayland environment yet.
    QProcessEnvironment environment;
    QString environmentError;
    /// Result of trying to connect to waylandSocketPath(environment).
    bool socketConnectable = false;
};

struct Result {
    Verdict verdict = Verdict::Wait;
    QString reason;
};

/// Bounded wait: the backoff below reaches its cap after about 3 s, then
/// polls every 2 s until this budget is spent.
constexpr int WaitBudgetMs = 30000;
constexpr int FirstRetryMs = 100;
constexpr int MaxRetryMs = 2000;

inline int retryDelayMs(int attempt)
{
    int delay = FirstRetryMs;
    for (int i = 0; i < attempt && delay < MaxRetryMs; ++i) {
        delay *= 2;
    }
    return std::min(delay, MaxRetryMs);
}

/// `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`, or WAYLAND_DISPLAY itself when absolute (as libwayland resolves it).
inline QString waylandSocketPath(const QProcessEnvironment &environment)
{
    const QString display = environment.value(QStringLiteral("WAYLAND_DISPLAY"));
    if (display.isEmpty()) {
        return {};
    }
    if (QDir::isAbsolutePath(display)) {
        return display;
    }
    const QString runtime = environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
    return runtime.isEmpty() ? QString() : QDir(runtime).filePath(display);
}

inline Result evaluate(const Observation &observation)
{
    if (!observation.session || observation.session->uid != observation.expectedUid) {
        return {Verdict::Gone, QStringLiteral("selected logind session disappeared")};
    }
    const auto &session = *observation.session;
    if (session.state == QLatin1String("closing")) {
        return {Verdict::Gone, QStringLiteral("session %1 is closing").arg(session.id)};
    }
    if (!session.active || session.state != QLatin1String("active")) {
        return {Verdict::Wait, QStringLiteral("session %1 is not active yet (state %2)").arg(session.id, session.state.isEmpty() ? QStringLiteral("unknown") : session.state)};
    }
    if (observation.environment.isEmpty()) {
        return {Verdict::Wait, observation.environmentError.isEmpty() ? QStringLiteral("no Wayland environment in session %1 yet").arg(session.id)
                                                                      : observation.environmentError};
    }
    const QString socket = waylandSocketPath(observation.environment);
    if (socket.isEmpty()) {
        return {Verdict::Wait, QStringLiteral("session %1 has no WAYLAND_DISPLAY yet").arg(session.id)};
    }
    if (!observation.socketConnectable) {
        return {Verdict::Wait, QStringLiteral("Wayland socket %1 is not accepting connections yet").arg(socket)};
    }
    return {Verdict::Ready, {}};
}
}
}
