// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionMaintenanceGuard.h"

namespace KRdp {
class VirtualUnattendedUpgradeGuardTest;
/** Invalidate durably, release the gate, then replace this process with the fixed
 * unattended-upgrade backend. Never rearms, even if the backend succeeds.
 * Deployment/diversion, supported loader environment and actual launcher path
 * selection are separate prerequisites; this wrapper does not establish them.
 */
class VirtualUnattendedUpgradeGuard {
public:
    // Requires real/effective/saved root. Preserves argv[1..] and environment;
    // sets argv[0] to /usr/libexec/krdp/unattended-upgrade.real. No shell/PATH
    // lookup/fallback. Returns nonzero on refusal/failed exec; success never
    // returns. No Qt application argument parsing or environment rewriting.
    static int run(int argc, char *const argv[], QString *error = nullptr);
private:
    friend class VirtualUnattendedUpgradeGuardTest;
    static int runWithLease(int argc, char *const argv[],
        std::optional<VirtualSessionMaintenanceGuard::Lease> lease,
        const QByteArray &backend, QString *error);
};
}
