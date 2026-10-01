// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "LayoutControl.h"
#include "VideoCodecSupport.h"

#include <optional>

/**
 * A connection's AVC444 chroma timing (OPT-045b): starts as the server's configured default
 * (krdpserverrc, SessionController::setChromaPolicyDefaults()) and changes only when that
 * connection's client sends a `chroma` record - which, KRDPCTL having no pre-login records,
 * can only happen after it authenticated, typically while it is already streaming. Whether
 * AVC444 is used at all is the standard RDPGFX caps negotiation; only this timing is ours, so
 * a stock client (no KRDPCTL) keeps the configured default for its whole session.
 */
namespace KRdp::ChromaMerge
{
enum class Outcome {
    Applied,    ///< the merged policy is in force: stored, and pushed to every running session
    Malformed,  ///< the record did not parse (chromaFromJson()); nothing changed
    OutOfRange, ///< the merged policy is not ChromaPolicy::isValid(); nothing changed
};

struct Result {
    Outcome outcome = Outcome::Malformed;
    ChromaPolicy policy; ///< the merged policy (also when OutOfRange, for the log)
};

/** Present fields override \a current, absent ones keep it. */
inline Result merge(const ChromaPolicy &current, const std::optional<LayoutControl::ChromaRequest> &request)
{
    Result result{Outcome::Malformed, current};
    if (!request) {
        return result;
    }
    if (request->motionGapMs) {
        result.policy.motionGapMs = *request->motionGapMs;
    }
    if (request->restMs) {
        result.policy.restMs = *request->restMs;
    }
    if (request->maxGapMs) {
        result.policy.maxGapMs = *request->maxGapMs;
    }
    result.outcome = result.policy.isValid() ? Outcome::Applied : Outcome::OutOfRange;
    return result;
}

/** Broker acknowledgement; this accepts timing policy, not codec capability. */
inline QJsonObject brokerReply(const Result &result)
{
    if (result.outcome != Outcome::Applied)
        return LayoutControl::errorRecord({QStringLiteral("invalid"), QStringLiteral("invalid AVC444 chroma timing policy")});
    return {{QStringLiteral("type"), QStringLiteral("chroma")}, {QStringLiteral("v"), 1}, {QStringLiteral("ok"), true},
            {QStringLiteral("motionGapMs"), result.policy.motionGapMs}, {QStringLiteral("restMs"), result.policy.restMs},
            {QStringLiteral("maxGapMs"), result.policy.maxGapMs}};
}

/**
 * merge(), and when it applies: \a current (the connection's policy, what sessions built later
 * start with) takes it, and every session in \a sessions (already running - the encoder applies
 * a timing change from its next frame, no keyframe or reconnect needed) gets setChromaPolicy().
 */
template<typename Sessions>
Result apply(ChromaPolicy &current, const std::optional<LayoutControl::ChromaRequest> &request, Sessions &sessions)
{
    const Result result = merge(current, request);
    if (result.outcome == Outcome::Applied) {
        current = result.policy;
        for (auto &session : sessions) {
            session->setChromaPolicy(current);
        }
    }
    return result;
}
}
