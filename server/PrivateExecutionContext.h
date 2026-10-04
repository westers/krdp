// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Shared by the root settings helpers: no core dump (RLIMIT_CORE=0) and not
// dumpable (PR_SET_DUMPABLE=0), so verifiers, passwords and key material that
// pass through the helper never reach a core file or coredump store.
#pragma once
#include <sys/prctl.h>
#include <sys/resource.h>

namespace KRdp
{
inline bool enterPrivateExecutionContext()
{
    const rlimit noCore{0, 0};
    return ::setrlimit(RLIMIT_CORE, &noCore) == 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0;
}
}
