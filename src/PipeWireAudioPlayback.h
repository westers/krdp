// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>

#include "krdp_export.h"

struct pw_stream;
struct pw_thread_loop;

namespace KRdp
{
/** Capture the desktop's default PipeWire sink monitor as 44.1 kHz stereo PCM.
 *
 * PipeWire invokes process() on an RT thread. The RDP session thread takes
 * complete PCM blocks with take(); it is the only thread that writes RDPSND.
 */
class KRDP_EXPORT PipeWireAudioPlayback
{
public:
    ~PipeWireAudioPlayback();
    bool start(const QString &targetSink);
    /**
     * Create a session-private PipeWire sink and make it the session's
     * default sink.  Applications started after this succeeds play only into
     * the RDP capture, not the physical speakers.  The preceding defaults
     * are restored by stop(), provided nobody changed them in the meantime.
     */
    bool startIsolated(const QString &id);
    void stop();
    QByteArray take();

private:
    static void process(void *data);
    void process();

    QMutex m_mutex;
    QByteArray m_pending;
    QString m_isolatedSinkName;
    QString m_previousDefaultSink;
    QString m_previousConfiguredSink;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
};
}
