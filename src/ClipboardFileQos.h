// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <optional>

#include <QString>

#include "ClipboardFilePacing.h"
#include "StatsReporter.h"

namespace KRdp::ClipboardFileQos
{
/**
 * OPT-078 (spec 6.3, 7): the decision table from the connection's evidence to the pacer's inputs,
 * pure. The mode x ceiling combination is ClipboardFilePacing::effectivePolicy(); this adds the
 * mapping of a Stats::Snapshot (and the previous one, for counter deltas) so the wiring in a later
 * phase is one call per tick.
 *
 *   sendQueueBytes     <- snapshot.sendQueueBytes
 *   capacityKbps       <- snapshot.capacityKbps (absent = unknown; an app-limited sample is not a capacity: the stats already leave it out)
 *   otherKbps          <- snapshot.sentKbps minus \a fileKbps (the stats see ALL bytes; the file's own share is not "other traffic")
 *   videoActive        <- framesSent grew since \a previous
 *   audioActive        <- audio packets grew since \a previous
 *   videoBacklog       <- the caller's FrameQueuePolicy link-clear verdict (\a videoBacklog), or a send queue over 64 KiB
 *   audioLate          <- late deliveries, overflow drops or producer gaps grew since \a previous
 *   slow / congested   <- the snapshot flags (LinkEvidence verdict slow or the congestion detector)
 *   limit == "client"  <- no pressure: a client slow to read is bounded by its request window
 */
inline ClipboardFilePacing::Inputs inputsFromStats(const Stats::Snapshot &now, const Stats::Snapshot *previous, quint32 fileKbps = 0, bool videoBacklog = false)
{
    ClipboardFilePacing::Inputs in;
    in.sendQueueBytes = now.sendQueueBytes;
    in.capacityKbps = now.capacityKbps;
    if (now.sentKbps) {
        in.otherKbps = *now.sentKbps > fileKbps ? *now.sentKbps - fileKbps : 0u;
    }
    in.videoBacklog = videoBacklog;
    in.slow = now.slow;
    in.congested = now.congested;
    if (previous) {
        in.videoActive = now.framesSent > previous->framesSent;
        if (now.audio && previous->audio) {
            in.audioActive = now.audio->packets > previous->audio->packets;
            in.audioLate = now.audio->late > previous->audio->late || now.audio->overflowEvents > previous->audio->overflowEvents
                || now.audio->producerGaps > previous->audio->producerGaps;
        }
    }
    return in;
}
}
