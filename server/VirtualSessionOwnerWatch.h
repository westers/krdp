// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <thread>
#include <unistd.h>
namespace KRdp {
// Caller owns a pidfd until after this object is destroyed. Only signals self.
// Unlike PDEATHSIG, monitoring is not cleared by PAM credential changes.
class VirtualSessionOwnerWatch {
public:
    explicit VirtualSessionOwnerWatch(int parent, int startupMs = 30000)
        : m_deadline(nowMs() + startupMs), m_thread([this, parent](std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd fd{parent, POLLIN, 0};
            const int result = poll(&fd, 1, 20);
            const auto deadline = m_deadline.load();
            if ((result < 0 && errno != EINTR) || (result > 0 && fd.revents) || (deadline && nowMs() >= deadline)) {
                kill(getpid(), SIGKILL);
                _exit(1);
            }
        }
    }) {}
    void retained() { m_deadline.store(0); }
    void closing(int timeoutMs = 10000) { m_deadline.store(nowMs() + timeoutMs); }
private:
    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::atomic<long long> m_deadline;
    std::jthread m_thread;
};
}
