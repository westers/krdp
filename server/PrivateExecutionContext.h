// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Shared by the root settings helpers: no core dump (RLIMIT_CORE=0) and not
// dumpable (PR_SET_DUMPABLE=0), so verifiers, passwords and key material that
// pass through the helper never reach a core file or coredump store.
#pragma once
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>

namespace KRdp
{
inline bool enterPrivateExecutionContext()
{
    const rlimit noCore{0, 0};
    return ::setrlimit(RLIMIT_CORE, &noCore) == 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0;
}

// REVIEW-S2: the two root network brokers (farside-console-host, farside-virtual-host) hold the PAM passwords of connecting
// users and the TLS private key in memory before and after authentication. They become non-dumpable so no core file or
// coredump-store entry can contain them. RLIMIT_CORE is deliberately NOT touched: the console host starts the capture worker
// and the worker inherits rlimits (but not the dumpable flag, which execve resets), and worker cores stay wanted.
//
// An administrator who needs a broker core for a diagnosis creates this root-owned marker (and, for the virtual host, a
// `LimitCORE=infinity` drop-in); the brokers then stay dumpable and log a warning. /etc/farside is root-only (0700).
inline constexpr const char *BrokerCoreDumpMarker = "/etc/farside/allow-broker-core-dumps";

inline bool brokerCoreDumpsAllowed(const char *marker = BrokerCoreDumpMarker)
{
    struct stat st{};
    return ::lstat(marker, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

enum class BrokerContext { NotDumpable, DumpableByMarker, Failed };

inline BrokerContext enterBrokerExecutionContext(const char *marker = BrokerCoreDumpMarker)
{
    if (brokerCoreDumpsAllowed(marker)) {
        return BrokerContext::DumpableByMarker;
    }
    return ::prctl(PR_SET_DUMPABLE, 0) == 0 ? BrokerContext::NotDumpable : BrokerContext::Failed;
}
}
