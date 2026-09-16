// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "DisplayWakeGuard.h"
#include "RdpConnection.h"
#include <AbstractSession.h>
#include <KStatusNotifierItem>
#include <vector>

#include <QObject>

namespace KRdp
{
class AbstractSession;
class Server;
class RdpConnection;
}

class SessionWrapper;

class SessionController : public QObject
{
    Q_OBJECT
public:
    enum class SessionType {
        Portal,
        Plasma,
    };

    SessionController(KRdp::Server *server, SessionType sessionType);
    ~SessionController() override;

    void setVirtualMonitor(const KRdp::VirtualMonitor &vm);
    void setMonitorIndex(const std::optional<int> &index);
    void setQuality(const std::optional<int> &quality);
    void setAdaptiveQuality(bool enabled);
    void setWakeDisplayOnConnect(bool enabled);
    void refreshDisplayConfiguration();
    void setSNIStatus(const KRdp::RdpConnection::State state);
    void stopFromSNI();

private:
    void onNewConnection(KRdp::RdpConnection *newConnection);
    std::unique_ptr<KRdp::AbstractSession> makeSession();

    KRdp::Server *m_server = nullptr;
    SessionType m_sessionType;
    std::optional<int> m_monitorIndex;
    std::optional<int> m_quality;
    bool m_adaptiveQuality = true;
    std::optional<KRdp::VirtualMonitor> m_virtualMonitor;

    std::unique_ptr<KRdp::AbstractSession> m_initializationSession;

    // Declared before m_wrappers so it outlives the wrappers that release into it.
    DisplayWakeGuard m_displayWakeGuard;

    std::vector<std::unique_ptr<SessionWrapper>> m_wrappers;

    KStatusNotifierItem *m_sni;
};
