// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>

namespace KRdp {
// Serializes consent with external PCM delivery. A sink must not reenter the
// queue. Disabling waits for an in-flight send, then discards buffered audio.
class ExternalAudioQueue
{
public:
    void setEnabled(bool enabled)
    {
        QMutexLocker lock(&m_mutex);
        m_enabled = enabled;
        if (!enabled) m_pcm.clear();
    }
    void submit(const QByteArray &pcm)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_enabled || pcm.isEmpty() || pcm.size() % 4) return;
        constexpr int maximum = 44100 * 4 / 2;
        m_pcm.append(pcm);
        if (m_pcm.size() > maximum) m_pcm.remove(0, m_pcm.size() - maximum);
    }
    template<typename Sink> bool deliver(Sink &&sink)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_enabled || m_pcm.isEmpty()) return false;
        const auto bytes = qMin(m_pcm.size(), qsizetype(44100 * 4 / 50));
        const auto pcm = m_pcm.left(bytes);
        m_pcm.remove(0, bytes);
        sink(pcm);
        return true;
    }
private:
    QMutex m_mutex;
    QByteArray m_pcm;
    bool m_enabled = false;
};
}
