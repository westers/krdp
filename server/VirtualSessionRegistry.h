// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "VirtualSessionState.h"
#include <QList>
#include <QString>
#include <QUuid>
#include <map>
#include <optional>

namespace KRdp
{
/** Live registry for a supervisor, not an authentication or process-liveness
 * oracle. Caller UIDs come from authenticated transports. Internal Handles
 * belong to the supervisor and must never be constructed from client JSON.
 * A manager restart starts empty: recovery must validate the surviving runtime
 * before adoption; a file or a recycled PID is not proof of a live desktop.
 */
class VirtualSessionRegistry
{
public:
    using Phase = VirtualSessionState::Phase;
    struct Handle {
        QString id;
        QUuid manager;
        quint64 generation = 0;
    };
    struct Summary {
        QString id;
        Phase phase;
    };

    explicit VirtualSessionRegistry(size_t perUserLimit = 4, size_t totalLimit = 64)
        : m_perUserLimit(perUserLimit), m_totalLimit(totalLimit) {}
    VirtualSessionRegistry(const VirtualSessionRegistry &) = delete;
    VirtualSessionRegistry &operator=(const VirtualSessionRegistry &) = delete;

    std::optional<Handle> create(quint32 authenticatedUid)
    {
        QString id;
        do {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        } while (m_sessions.contains(id));
        return reserveRetained(authenticatedUid, id);
    }

    // Trusted recovery identity only, never a client-supplied creation ID.
    // Reserving does NOT prove the desktop exists or make it attachable.
    std::optional<Handle> reserveRetained(quint32 authenticatedUid, const QString &id)
    {
        if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id || m_sessions.contains(id)) return {};
        if (!authenticatedUid || m_sessions.size() >= m_totalLimit) {
            return {};
        }
        size_t owned = 0;
        for (const auto &[id, entry] : m_sessions) {
            if (entry.uid == authenticatedUid) {
                ++owned;
            }
        }
        if (owned >= m_perUserLimit) {
            return {};
        }
        auto [it, inserted] = m_sessions.try_emplace(id, authenticatedUid);
        Q_ASSERT(inserted);
        const auto generation = it->second.state.create(authenticatedUid);
        Q_ASSERT(generation);
        return Handle{id, m_manager, generation};
    }

    QList<Summary> list(quint32 authenticatedUid) const
    {
        QList<Summary> result;
        if (authenticatedUid) {
            for (const auto &[id, entry] : m_sessions) {
                if (entry.uid == authenticatedUid) {
                    result.append({id, entry.state.phase()});
                }
            }
        }
        return result;
    }

    // Only actual authenticated capture readiness may produce this event.
    bool ready(const Handle &handle)
    {
        auto *entry = current(handle);
        return entry && entry->state.ready(handle.generation);
    }

    std::optional<Handle> attach(quint32 authenticatedUid, const QString &id, quint64 client)
    {
        auto *entry = owned(authenticatedUid, id);
        if (!entry || !client) {
            return {};
        }
        // A transport cannot own two input/media destinations simultaneously.
        for (const auto &[otherId, other] : m_sessions) {
            if (other.state.client() == client) {
                return {};
            }
        }
        const auto generation = entry->state.generation();
        if (!entry->state.attach(authenticatedUid, generation, client)) {
            return {};
        }
        return Handle{id, m_manager, generation};
    }

    bool disconnect(const Handle &handle, quint64 client)
    {
        auto *entry = current(handle);
        return entry && entry->state.disconnect(handle.generation, client);
    }

    std::optional<Handle> stop(quint32 authenticatedUid, const QString &id)
    {
        auto *entry = owned(authenticatedUid, id);
        if (!entry || !entry->state.stop(authenticatedUid, entry->state.generation())) {
            return {};
        }
        return Handle{id, m_manager, entry->state.generation()};
    }

    bool exited(const Handle &handle)
    {
        auto *entry = current(handle);
        return entry && entry->state.exited(handle.generation);
    }

    bool unavailable(const Handle &handle)
    {
        auto *entry = current(handle);
        return entry && entry->state.unavailable(handle.generation);
    }

    std::optional<Handle> stopUnavailable(quint32 uid, const QString &id)
    {
        auto *entry = owned(uid, id);
        if (!entry || !entry->state.stopUnavailable(uid, entry->state.generation())) return {};
        return Handle{id, m_manager, entry->state.generation()};
    }

    // A crash is visible as Failed. Never silently replace lost applications.
    std::optional<Handle> recreate(quint32 authenticatedUid, const QString &id)
    {
        auto *entry = owned(authenticatedUid, id);
        if (!entry) {
            return {};
        }
        const auto generation = entry->state.create(authenticatedUid);
        if (!generation) {
            return {};
        }
        return Handle{id, m_manager, generation};
    }

    // Retained/starting/stopping entries continue counting against limits;
    // removing metadata is not a substitute for terminating a runtime.
    bool forget(quint32 authenticatedUid, const QString &id)
    {
        const auto *entry = owned(authenticatedUid, id);
        if (!entry || (entry->state.phase() != Phase::Absent && entry->state.phase() != Phase::Failed)) {
            return false;
        }
        m_sessions.erase(id);
        return true;
    }

private:
    struct Entry {
        explicit Entry(quint32 owner) : uid(owner), state(owner) {}
        quint32 uid;
        VirtualSessionState state;
    };
    Entry *owned(quint32 uid, const QString &id)
    {
        const auto it = m_sessions.find(id);
        return uid && it != m_sessions.end() && it->second.uid == uid ? &it->second : nullptr;
    }
    Entry *current(const Handle &handle)
    {
        if (handle.manager != m_manager || !handle.generation) {
            return nullptr;
        }
        const auto it = m_sessions.find(handle.id);
        return it != m_sessions.end() && it->second.state.generation() == handle.generation ? &it->second : nullptr;
    }
    const QUuid m_manager = QUuid::createUuid();
    const size_t m_perUserLimit;
    const size_t m_totalLimit;
    std::map<QString, Entry> m_sessions;
};
}
