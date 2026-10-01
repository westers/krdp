// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionSupervisor.h"
#include "VirtualSessionJournal.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QPointer>
#include <deque>
#include <memory>

class VirtualSessionControlTest;

namespace KRdp
{
/** Server-side virtual-session command dispatch. The broker supplies a unique
 * transport ID and RdpConnection::authenticatedUserUid(), never JSON identity.
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
        // Limit: the per-user or host desktop limit (a stock client is told so).
        enum class Refusal { Ordinary, Maintenance, Limit };
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
    using InitialOutputs = QVector<VirtualSessionJournal::Record::InitialOutput>;
    void setSelectedCreateHandler(std::function<CreateResult(quint32, const InitialOutputs &)> create)
    { m_selectedCreate = std::move(create); ++m_initialRevision; }
    // Default unavailable; the production host enables only with explicit
    // experimental opt-in until native selected-layout acceptance is complete.
    struct InitialLayoutPreviewCapabilities {
        int maxOutputs = 0;
        int maxOutputDimension = 0;
        int maxAtlasDimension = 0;
        bool operator==(const InitialLayoutPreviewCapabilities &) const = default;
    };
    void setInitialLayoutPreviewCapabilities(InitialLayoutPreviewCapabilities caps)
    { m_initialCaps = caps; ++m_initialRevision; }
    /// Selected-screen creation is offered (KRDPCTL v2 `capabilities.virtualSessions.selectedCreate`).
    bool selectedCreateAvailable() const { return m_initialCaps.maxOutputs > 0 && bool(m_selectedCreate); }
    enum class DismissResult { Accepted, Unavailable, Uncertain };
    void setDismissHandlers(std::function<bool(quint32, const QString &)> eligible,
                            std::function<DismissResult(quint32, const QString &)> dismiss)
    { m_dismissible = std::move(eligible); m_dismiss = std::move(dismiss); }
    InitialLayoutPreviewCapabilities initialLayoutCapabilities() const { return m_initialCaps; }
    /**
     * AUD-D4: a stock RDP client that never sent a `virtual-session` record.
     * Attaches the user's most recently used desktop (retained, attached or
     * starting); one attached to another connection of the same user is taken
     * over (that connection is released first, then reported to the displaced
     * handler). Without any, creates one with \a outputs as its first layout.
     * Starting means: call attachStarted() until it is ready.
     */
    struct StockResult {
        enum class Kind { Attached, Starting, Refused };
        enum class Reason { Failed, Limit, Maintenance };
        Kind kind = Kind::Refused;
        Reason reason = Reason::Failed; // Refused only
        QString session;
        std::optional<Handle> handle; // Attached only
    };
    StockResult attachStockClient(std::optional<quint32> authenticatedUid, quint64 client, const InitialOutputs &outputs);
    StockResult attachStarted(std::optional<quint32> authenticatedUid, quint64 client, const QString &session);
    /** Mark \a session as just used (attach, detach, create; the host adds recovered desktops in journal order). */
    void noteUsed(const QString &session) { m_recency.insert(session, ++m_useClock); }
    /** Called for each transport a stock-client takeover released, after the new attach. */
    void setDisplacedHandler(std::function<void(quint64)> displaced) { m_displaced = std::move(displaced); }
    // Includes release callbacks; nested event loops must not retire registry
    // entries while a command or synchronous revocation is on the stack.
    bool dispatchActive() const { return m_dispatchDepth != 0; }

private:
    friend class ::VirtualSessionControlTest;
    struct Reply { QJsonObject request; QJsonObject response; bool pending = true; };
    struct Transport {
        struct InitialPreview {
            QString token;
            QString requestId;
            QJsonArray screens;
            QJsonArray outputs;
            InitialOutputs committed;
            quint64 revision = 0;
            QElapsedTimer age;
        };
        quint32 uid;
        std::optional<Handle> attached;
        std::deque<Reply> replies;
        std::optional<InitialPreview> initialPreview;
    };
    QJsonObject dispatch(quint32 uid, quint64 client, const std::shared_ptr<Transport> &transport, const QJsonObject &request);
    void release(quint64 client, const std::shared_ptr<Transport> &transport, std::function<void()> revoke = {});
    std::shared_ptr<Transport> stockTransport(std::optional<quint32> uid, quint64 client);
    StockResult takeOver(quint32 uid, quint64 client, const std::shared_ptr<Transport> &transport, const QString &session);
    QHash<QString, quint64> m_recency;
    quint64 m_useClock = 0;
    std::function<void(quint64)> m_displaced;
    QPointer<VirtualSessionSupervisor> m_supervisor;
    Release m_release;
    std::function<CreateResult(quint32)> m_create;
    std::function<CreateResult(quint32, const InitialOutputs &)> m_selectedCreate;
    InitialLayoutPreviewCapabilities m_initialCaps;
    quint64 m_initialRevision = 1;
    std::function<bool(quint32, const QString &)> m_dismissible;
    std::function<DismissResult(quint32, const QString &)> m_dismiss;
    QHash<quint64, std::shared_ptr<Transport>> m_transports;
    unsigned m_dispatchDepth = 0;
};
}
