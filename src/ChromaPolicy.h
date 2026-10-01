// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

namespace KRdp
{
/** AVC444 auxiliary stream timing, shared by the broker, worker and session.
 * Matches private KPipeWire's ChromaPolicy without requiring its API here. */
struct ChromaPolicy {
    int motionGapMs = 100;
    int restMs = 150;
    int maxGapMs = 1500;

    bool operator==(const ChromaPolicy &) const = default;

    bool isValid() const
    {
        const auto inRange = [](int value) { return value >= 16 && value <= 5000; };
        return inRange(motionGapMs) && inRange(restMs) && inRange(maxGapMs)
            && motionGapMs <= restMs && restMs <= maxGapMs;
    }
};
}
