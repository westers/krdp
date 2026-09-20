// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>

struct pw_stream;
struct pw_thread_loop;

namespace KRdp
{
class PipeWireMicrophone
{
public:
    ~PipeWireMicrophone();
    bool start(const QString &id);
    void stop();
    void write(const QByteArray &pcm);
private:
    static void process(void *data);
    void process();
    QMutex m_mutex;
    QByteArray m_pending;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
};
}
