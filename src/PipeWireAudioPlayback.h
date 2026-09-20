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
/** Capture the desktop's default PipeWire sink monitor as 44.1 kHz stereo PCM.
 *
 * PipeWire invokes process() on an RT thread. The RDP session thread takes
 * complete PCM blocks with take(); it is the only thread that writes RDPSND.
 */
class PipeWireAudioPlayback
{
public:
    ~PipeWireAudioPlayback();
    bool start(const QString &targetSink);
    void stop();
    QByteArray take();

private:
    static void process(void *data);
    void process();

    QMutex m_mutex;
    QByteArray m_pending;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
};
}
