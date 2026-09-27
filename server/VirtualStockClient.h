// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ClientDisplayInfo.h"
#include "VirtualSessionJournal.h"

#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <optional>

/**
 * AUD-D4: a stock RDP client (mstsc, FreeRDP, Remmina) on the virtual-desktop
 * broker. It has no KRDPCTL, or never sends a `virtual-session` record, so the
 * broker picks the user's desktop itself (VirtualStockClientPolicy):
 * attach the most recently used retained desktop, or create one sized from
 * the client's standard monitor data. Everything here is pure except
 * readUserPolicy().
 */
namespace KRdp::VirtualStockClient
{
enum class Policy { AttachOrCreate, Refuse };

/** `attach-or-create` | `refuse` (krdpserverrc VirtualStockClientPolicy); anything else is nullopt. */
std::optional<Policy> parsePolicy(const QString &value);
/** VirtualStockClientPolicy from krdpserverrc contents ([General] group); the default when absent or invalid. */
Policy policyFromConfig(const QByteArray &contents);
/**
 * The policy in \a path, which must be a regular file owned by \a uid (a
 * symlink, FIFO, device, oversized or foreign file gives the default). Opened
 * non-blocking so a hostile file can never stall the broker.
 */
Policy readPolicyFile(const QString &path, quint32 uid);
/** The authenticated user's own ~/.config/krdpserverrc, read with the user's file-system identity. */
Policy readUserPolicy(quint32 uid);

/** Why a stock client gets no desktop; each maps to one standard Set Error Info code. */
enum class Refusal {
    Policy, ///< VirtualStockClientPolicy=refuse
    NoFreeSlot, ///< the per-user or host desktop limit
    StartFailed, ///< creation or the desktop's start failed
    StartTimeout, ///< the new desktop did not become ready in time
    Displaced, ///< another connection of the same user took the desktop over
};
/** The MS-RDPBCGR `ERRINFO_*` code a stock client shows for \a refusal. */
quint32 errorInfo(Refusal refusal);

struct Limits {
    int maxOutputs = 0;
    int maxOutputDimension = 4096;
    int maxAtlasDimension = 8192;
};
/**
 * The first layout for a desktop created for a stock client, from its
 * standard monitor data (TS_UD_CS_MONITOR, else TS_UD_CS_CORE desktop size),
 * clamped to the backend's limits. Several outputs only when the client sent
 * a usable multi-monitor list and \a limits allows that many; otherwise one
 * output at the desktop size (clamped to 320..4096 x 200..4096, even),
 * \a fallback when the client gave none. Never empty.
 */
QVector<VirtualSessionJournal::Record::InitialOutput> initialOutputs(const ClientDisplay::Info &info, const Limits &limits,
                                                                    const QSize &fallback = QSize(1920, 1080));
/**
 * The single-output size an MS-RDPEDISP layout asks for, or nullopt when it
 * cannot drive a one-output desktop (no monitors, or more than one). Rounded
 * down to even and clamped to the virtual resize limits.
 */
std::optional<QSize> displayControlSize(const QList<VideoMonitor> &monitors);
}
