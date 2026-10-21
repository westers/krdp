// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "OutputSnapshot.h"

#include <algorithm>
#include <optional>

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace KRdp::ConsoleReplaceSnapshotGate
{
/**
 * OPT-082: the Console worker must not snapshot the host's outputs while KWin is still re-adding some of them.
 *
 * After a display wake KWin removes and re-adds the physical outputs for a few seconds, so a `kscreen-doctor -j` taken
 * at that instant can list only some of the screens the worker is itself capturing. A snapshot of that partial list made
 * "Replace my screens" plan (and silently apply) one stand-in for a two-screen host. The worker therefore polls kscreen
 * until every output it captures is connected and enabled and the set has stayed the same for one more poll, and fails
 * open (no Replace, normal capture of the real screens) when that does not happen in time.
 *
 * This header is the pure decision; the worker supplies the poll timer and the kscreen reads.
 */
inline constexpr int PollMs = 250;
inline constexpr int TimeoutMs = 6000;

/** The names that must be present: the captured physical outputs. Virtual outputs, empty and repeated names are dropped. */
inline QStringList expectedNames(const QStringList &captured)
{
    QStringList names;
    for (const auto &name : captured) {
        if (!name.isEmpty() && !OutputSnapshot::isVirtual(name) && !names.contains(name)) names.append(name);
    }
    names.sort();
    return names;
}

/** Expected names that are not a connected, enabled physical output of \a outputs. */
inline QStringList missingNames(const QStringList &expected, const QVector<OutputSnapshot::Output> &outputs)
{
    QSet<QString> present;
    for (const auto &output : outputs) {
        if (output.enabled && !OutputSnapshot::isVirtual(output.name)) present.insert(output.name);
    }
    QStringList missing;
    for (const auto &name : expected) {
        if (!present.contains(name)) missing.append(name);
    }
    return missing;
}

enum class State : quint8 {
    Wait, // not all there yet, or not stable yet: poll again
    Ready, // every expected output is there and the set did not change since the previous poll
    TimedOut, // out of time: fail open
};

struct Verdict {
    State state = State::Wait;
    QStringList missing; // expected, but not connected and enabled (empty on a timeout means "still changing")
    QStringList unexpected; // enabled physical outputs nobody captures (reported, never waited for)
};

/** One wait. observe() is fed one kscreen reading per poll (std::nullopt = kscreen could not be read). */
class Waiter
{
public:
    explicit Waiter(const QStringList &captured)
        : m_expected(expectedNames(captured))
    {
    }

    const QStringList &expected() const { return m_expected; }

    Verdict observe(const std::optional<QVector<OutputSnapshot::Output>> &reading, qint64 elapsedMs)
    {
        Verdict verdict;
        const QVector<OutputSnapshot::Output> physical = reading ? OutputSnapshot::physicalOnly(*reading) : QVector<OutputSnapshot::Output>{};
        verdict.missing = reading ? missingNames(m_expected, physical) : m_expected;
        QStringList signature;
        for (const auto &output : physical) {
            signature.append(output.name + (output.enabled ? QLatin1String("+") : QLatin1String("-")));
            if (output.enabled && !m_expected.contains(output.name)) verdict.unexpected.append(output.name);
        }
        signature.sort();
        verdict.unexpected.sort();

        const bool complete = reading && verdict.missing.isEmpty();
        const bool stable = complete && m_previous && *m_previous == signature;
        m_previous = complete ? std::optional<QStringList>(signature) : std::nullopt;
        if (stable) {
            verdict.state = State::Ready;
        } else {
            verdict.state = elapsedMs >= TimeoutMs ? State::TimedOut : State::Wait;
        }
        return verdict;
    }

private:
    QStringList m_expected;
    std::optional<QStringList> m_previous; // the last reading's physical set, kept only while it was complete
};

/** The log line for a timeout; the missing names are what tells Steve which screen KWin had not brought back. */
inline QString timeoutReason(const Verdict &verdict, bool unreadable)
{
    if (unreadable) return QStringLiteral("kscreen-doctor could not be read within %1 ms").arg(TimeoutMs);
    if (verdict.missing.isEmpty())
        return QStringLiteral("the host's outputs were still changing after %1 ms").arg(TimeoutMs);
    return QStringLiteral("output(s) %1 not connected and enabled in kscreen after %2 ms (the compositor is still re-adding them)")
        .arg(verdict.missing.join(QLatin1String(", ")))
        .arg(TimeoutMs);
}
/**
 * Broker side: does what the worker created match what the broker's own planner predicted for the same request?
 * \a predicted are the KWin names of the stand-ins the plan lists (Virtual- prefix included), \a created the names of the
 * outputs the worker reports capturing.
 */
enum class Agreement : quint8 {
    Consistent,
    SingleFallback, // the worker's own bounded fallback: one krdp-m* output standing in for every host screen
    CountDiffers, // the worker mapped a different number of screens (it planned from a partial snapshot): abort the Replace
    NamesDiffer, // same count, other names: unknown, only reported
};

inline Agreement agreement(const QStringList &predicted, const QStringList &created)
{
    if (predicted.size() != created.size()) {
        const QLatin1String fallback("krdp-m");
        const bool single = created.size() == 1 && created.first().startsWith(OutputSnapshot::VirtualPrefix + fallback);
        return single ? Agreement::SingleFallback : Agreement::CountDiffers;
    }
    for (qsizetype i = 0; i < predicted.size(); ++i) {
        if (created[i] != predicted[i] && !created[i].endsWith(predicted[i].mid(OutputSnapshot::VirtualPrefix.size()))) return Agreement::NamesDiffer;
    }
    return Agreement::Consistent;
}
}
