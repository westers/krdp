// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "PipeWireRuntime.h"

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <atomic>
#include <memory>

struct pw_stream;
struct pw_thread_loop;

namespace KRdp
{
class PipeWireMicrophone
{
public:
    enum class State { Stopped, Starting, Ready, Failed };
    PipeWireMicrophone();
    ~PipeWireMicrophone();
    bool start(const QString &id);
    void stop();
    State state() const { return m_state.load(); }
    /**
     * An application on the host is recording from this source: at least one link from this
     * node to a consumer that is not a level meter (a peak-detect/monitor stream such as a
     * volume applet's). Clears as soon as the last such link goes. Any thread.
     *
     * Not the stream's STREAMING state: a linked meter, or a driver that keeps the node
     * running, keeps the source STREAMING with nobody recording (N1).
     */
    bool consumerActive() const;
    void write(const QByteArray &pcm);
private:
    static void process(void *data);
    void process();
    QMutex m_mutex;
    QByteArray m_pending;
    std::atomic<State> m_state{State::Stopped};
    std::atomic<bool> m_streaming{false};
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
    PipeWireRuntime::Reference m_runtime;
    // The links from this node and whether their consumer is a meter, from the registry.
    struct Graph;
    std::unique_ptr<Graph> m_graph;
};
}
