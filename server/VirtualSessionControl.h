// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionSupervisor.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QPointer>
#include <deque>
#include <memory>

namespace KRdp
{
/** Server-side virtual-session command dispatch. The broker supplies a unique
 * transport ID and RdpConnection::authenticatedPamUid(), never JSON identity.
 * Release must synchronously revoke that transport's input/media destination.
 * Not advertised until the broker wires capture, release and transport lifetime.
 */
class VirtualSessionControl : public QObject
{
public:
    using Handle = VirtualSessionRegistry::Handle;
    using Release = std::function<void(quint64, const Handle &)>;
    explicit VirtualSessionControl(VirtualSessionSupervisor &supervisor, Release release)
        : m_supervisor(&supervisor), m_release(std::move(release)) {}
    QJsonObject request(std::optional<quint32> authenticatedUid, quint64 client, const QJsonObject &record);
    void disconnected(quint64 client);
    // Protect transport revocation signals with the same dispatch barrier as
    // registry release. The callback runs even when no control record exists.
    void disconnected(quint64 client, std::function<void()> revoke);
    std::optional<Handle> attachment(quint64 client) const;
    struct CreateResult {
        enum class Refusal { Ordinary, Maintenance };
        std::optional<Handle> handle;
        Refusal refusal = Refusal::Ordinary; // Relevant only without a handle.
        CreateResult() = default;
        CreateResult(std::optional<Handle> accepted) : handle(std::move(accepted)) {}
        explicit CreateResult(Refusal reason) : refusal(reason) {}
        explicit operator bool() const { return handle.has_value(); }
        const Handle *operator->() const { return &handle.value(); }
        const Handle &operator*() const { return handle.value(); }
    };
    void setCreateHandler(std::function<CreateResult(quint32)> create) { m_create = std::move(create); }
    // Preview-only until the immutable launch intent and worker can consume a
    // selected layout. The production host does not set these capabilities.
    struct InitialLayoutPreviewCapabilities {
        int maxOutputs = 0;
        int maxOutputDimension = 0;
        int maxAtlasDimension = 0;
    };
    void setInitialLayoutPreviewCapabilities(InitialLayoutPreviewCapabilities caps) { m_initialCaps = caps; }
    enum class DismissResult { Accepted, Unavailable, Uncertain };
    void setDismissHandlers(std::function<bool(quint32, const QString &)> eligible,
                            std::function<DismissResult(quint32, const QString &)> dismiss)
    { m_dismissible = std::move(eligible); m_dismiss = std::move(dismiss); }
    // Includes release callbacks; nested event loops must not retire registry
    // entries while a command or synchronous revocation is on the stack.
    bool dispatchActive() const { return m_dispatchDepth != 0; }

private:
    struct Reply { QJsonObject request; QJsonObject response; bool pending = true; };
    struct Transport {
        struct InitialPreview {
            QString token;
            QString requestId;
            QJsonArray screens;
            QJsonArray outputs;
            QElapsedTimer age;
        };
        quint32 uid;
        std::optional<Handle> attached;
        std::deque<Reply> replies;
        std::optional<InitialPreview> initialPreview;
    };
    QJsonObject dispatch(quint32 uid, quint64 client, const std::shared_ptr<Transport> &transport, const QJsonObject &request);
    void release(quint64 client, const std::shared_ptr<Transport> &transport, std::function<void()> revoke = {});
    QPointer<VirtualSessionSupervisor> m_supervisor;
    Release m_release;
    std::function<CreateResult(quint32)> m_create;
    InitialLayoutPreviewCapabilities m_initialCaps;
    std::function<bool(quint32, const QString &)> m_dismissible;
    std::function<DismissResult(quint32, const QString &)> m_dismiss;
    QHash<quint64, std::shared_ptr<Transport>> m_transports;
    unsigned m_dispatchDepth = 0;
};
}
