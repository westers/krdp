// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QtGlobal>
#include <limits>

namespace KRdp
{
/** Broker-owned lifecycle for one user's retained desktop, not a seat0 output.
 * UID arguments must come from successful authentication, never client JSON.
 * The supervisor supplies readiness/exit events for its actual process handle.
 * This model does not launch processes or establish authentication itself.
 */
class VirtualSessionState
{
public:
    enum class Phase { Absent, Starting, Retained, Attached, Stopping, Failed };
    explicit VirtualSessionState(quint32 ownerUid) : m_ownerUid(ownerUid) {}

    Phase phase() const { return m_phase; }
    quint64 generation() const { return m_generation; }
    quint64 client() const { return m_client; }

    // Zero means refused. Failed sessions require explicit creation, never an
    // automatic replacement that could falsely promise retained applications.
    quint64 create(quint32 authenticatedUid)
    {
        if (!owns(authenticatedUid) || (m_phase != Phase::Absent && m_phase != Phase::Failed)
            || m_generation == std::numeric_limits<quint64>::max()) {
            return 0;
        }
        ++m_generation;
        m_phase = Phase::Starting;
        return m_generation;
    }

    // Call only after authenticated worker/capture readiness, not process spawn.
    bool ready(quint64 generation)
    {
        if (!current(generation) || m_phase != Phase::Starting) {
            return false;
        }
        m_phase = Phase::Retained;
        return true;
    }

    bool attach(quint32 authenticatedUid, quint64 generation, quint64 client)
    {
        if (!owns(authenticatedUid) || !current(generation) || !client || m_phase != Phase::Retained) {
            return false;
        }
        m_client = client;
        m_phase = Phase::Attached;
        return true;
    }

    // The caller releases held input and transport audio. No process stop is
    // requested: the compositor and all applications outlive the connection.
    bool disconnect(quint64 generation, quint64 client)
    {
        if (!current(generation) || m_phase != Phase::Attached || !client || m_client != client) {
            return false;
        }
        m_client = 0;
        m_phase = Phase::Retained;
        return true;
    }

    // Explicit owner action. Drain/revoke input before the supervisor stops the
    // process group; no new attachment or creation until exit is confirmed.
    bool stop(quint32 authenticatedUid, quint64 generation)
    {
        if (!owns(authenticatedUid) || !current(generation)
            || (m_phase != Phase::Starting && m_phase != Phase::Retained && m_phase != Phase::Attached)) {
            return false;
        }
        m_client = 0;
        m_phase = Phase::Stopping;
        return true;
    }

    bool exited(quint64 generation)
    {
        if (!current(generation) || m_phase == Phase::Absent || m_phase == Phase::Failed) {
            return false;
        }
        m_client = 0;
        m_phase = m_phase == Phase::Stopping ? Phase::Absent : Phase::Failed;
        return true;
    }

private:
    bool owns(quint32 uid) const { return m_ownerUid != 0 && uid == m_ownerUid; }
    bool current(quint64 generation) const { return generation != 0 && generation == m_generation; }
    const quint32 m_ownerUid;
    Phase m_phase = Phase::Absent;
    quint64 m_generation = 0;
    quint64 m_client = 0;
};
}
