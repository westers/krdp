// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionJournal.h"
#include <memory>
namespace KRdp {
// Caller must first validate journal/current boot and establish desktop
// extinction. This object pins only the recorded keeper; it does not prove
// that PAM callbacks ran or that a logind session has disappeared.
class VirtualSessionKeeperProcess {
public:
    // nullptr = uncertain. A nonnull result may already be gone, including a
    // reused numeric PID with a DIFFERENT pidfs identity (never signal that PID).
    static std::unique_ptr<VirtualSessionKeeperProcess> pin(const VirtualSessionJournal::Keeper &record);
    ~VirtualSessionKeeperProcess();
    VirtualSessionKeeperProcess(const VirtualSessionKeeperProcess &) = delete;
    VirtualSessionKeeperProcess &operator=(const VirtualSessionKeeperProcess &) = delete;
    std::optional<bool> gone() const;
    // Submission only. Wait for gone() before any login-session reconciliation.
    bool kill() const;
private:
    explicit VirtualSessionKeeperProcess(int fd) : m_fd(fd) {}
    int m_fd;
};
}
