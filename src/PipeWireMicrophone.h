// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <atomic>

struct pw_stream;
struct pw_thread_loop;

namespace KRdp
{
class PipeWireMicrophone
{
public:
    enum class State { Stopped, Starting, Ready, Failed };
    ~PipeWireMicrophone();
    bool start(const QString &id);
    void stop();
    State state() const { return m_state.load(); }
    void write(const QByteArray &pcm);
private:
    static void process(void *data);
    void process();
    QMutex m_mutex;
    QByteArray m_pending;
    std::atomic<State> m_state{State::Stopped};
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
};
}
