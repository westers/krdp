// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QMap>

namespace KRdp
{
/** Seat-wide admission and audio policy. Only admit authenticated clients. */
class ConsoleControl
{
public:
    using Id = quint64;
    struct Media {
        bool playback = false;
        bool silenceHost = false;
        bool operator==(const Media &) const = default;
    };

    void admit(Id id)
    {
        if (!id || m_clients.contains(id)) {
            return;
        }
        m_clients.insert(id, {});
        if (!m_owner) {
            m_owner = id;
        }
    }

    bool ownsControl(Id id) const { return id && id == m_owner; }

    bool setMedia(Id id, Media media)
    {
        auto client = m_clients.find(id);
        if (client == m_clients.end() || (media.playback && media.silenceHost && !ownsControl(id))) {
            return false;
        }
        media.silenceHost = media.playback && media.silenceHost;
        *client = media;
        return true;
    }

    void remove(Id id)
    {
        m_clients.remove(id);
        if (ownsControl(id)) {
            // Viewers never silently gain input when the controller leaves.
            m_owner = 0;
        }
    }

    Media media() const
    {
        Media result;
        for (auto client = m_clients.cbegin(); client != m_clients.cend(); ++client) {
            result.playback |= client->playback;
            result.silenceHost |= ownsControl(client.key()) && client->silenceHost;
        }
        return result;
    }

private:
    QMap<Id, Media> m_clients;
    Id m_owner = 0;
};
}
