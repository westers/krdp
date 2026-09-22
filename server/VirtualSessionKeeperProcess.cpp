// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionKeeperProcess.h"
#include "VirtualSessionProcessIdentity.h"
#include <csignal>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace KRdp {
std::unique_ptr<VirtualSessionKeeperProcess> VirtualSessionKeeperProcess::pin(const VirtualSessionJournal::Keeper &record)
{
    if (record.pid <= 1 || record.pid == getpid() || !record.startTicks || record.pidInode < 2) return {};
    const int fd = int(syscall(SYS_pidfd_open, record.pid, 0));
    if (fd < 0 && errno != ESRCH) return {};
    auto process = std::unique_ptr<VirtualSessionKeeperProcess>(new VirtualSessionKeeperProcess(fd));
    if (fd < 0) return process; // No process can still own the absent numeric PID.
    const auto identity = virtualPidfdIdentity(fd);
    if (!identity) return {};
    if (*identity != record.pidInode) {
        close(fd); process->m_fd = -1;
        return process; // Original died; replacement remains completely untouched.
    }
    const auto exited = process->gone();
    if (!exited) return {};
    if (*exited) return process;
    const auto ticks = virtualProcessStartTime(record.pid);
    // Numeric procfs lookup can race exit/reuse. The pinned pidfd decides
    // whether its original process died; never transfer authority to a PID.
    const auto after = process->gone();
    if (!after) return {};
    if (*after) return process;
    if (!ticks || *ticks != record.startTicks) return {};
    return process;
}
VirtualSessionKeeperProcess::~VirtualSessionKeeperProcess() { if (m_fd >= 0) close(m_fd); }
std::optional<bool> VirtualSessionKeeperProcess::gone() const
{
    if (m_fd < 0) return true;
    pollfd fd{m_fd, POLLIN, 0};
    int result;
    do { result = poll(&fd, 1, 0); } while (result < 0 && errno == EINTR);
    if (result < 0 || (fd.revents & (POLLERR | POLLNVAL))) return {};
    return bool(fd.revents & (POLLIN | POLLHUP));
}
bool VirtualSessionKeeperProcess::kill() const
{
    if (m_fd < 0) return true;
    return !syscall(SYS_pidfd_send_signal, m_fd, SIGKILL, nullptr, 0) || errno == ESRCH;
}
}
