// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleTopologyLease.h"

#include <functional>

namespace KRdp::ConsoleTopologyRelease
{
using Read = std::function<std::optional<QByteArray>()>;
using Apply = std::function<bool(const QStringList &)>;

struct Result {
    bool verified = false;
    bool commandAttempted = false;
    bool helperReportedSuccess = false;
    QStringList arguments;
    QString error;
};

// Runs exactly one conditional compensating batch. The helper's exit code is
// diagnostic only: libkscreen Doctor can report an apply failure yet exit 0,
// or a timed-out helper may have changed KDE before it was killed. Only the
// complete second KScreen readback can verify what actually happened. Capture
// must be re-established separately before any next controller sees video.
inline Result reconcile(const ConsoleTopologyLease::State &lease, const Read &read, const Apply &apply)
{
    Result result;
    const auto current = read();
    if (!current) {
        result.error = QStringLiteral("cannot read current physical layout");
        return result;
    }
    const auto commands = ConsoleTopologyPlan::recoveryArguments(lease.cumulative,
        lease.original, lease.selected, *current);
    if (!commands) {
        result.error = QStringLiteral("conditional physical recovery is unsafe");
        return result;
    }
    result.arguments = *commands;
    if (!commands->isEmpty()) {
        result.commandAttempted = true;
        result.helperReportedSuccess = apply(*commands);
    }
    const auto after = read();
    if (!after) {
        result.error = QStringLiteral("cannot verify restored physical layout");
        return result;
    }
    result.verified = ConsoleTopologyPlan::recoveryVerified(lease.cumulative,
        lease.original, lease.selected, *current, *after);
    if (!result.verified) result.error = QStringLiteral("restored physical layout differs from expected readback");
    return result;
}
}
