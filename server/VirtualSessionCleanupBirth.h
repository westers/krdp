// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionJournal.h"
#include <functional>
namespace KRdp {
struct VirtualSessionCleanupBirth {
    std::optional<VirtualSessionJournal::Keeper> keeper;
    bool absent = false; // false with no keeper means uncertain, not absence.
    static VirtualSessionCleanupBirth inspect(const std::function<VirtualSessionCleanupBirth()> &read,
        const std::function<std::optional<bool>()> &serviceEmpty)
    {
        auto evidence = read();
        if (evidence.keeper || !evidence.absent) return evidence;
        if (serviceEmpty() != std::optional<bool>(true)) return {};
        // The keeper may have published and migrated after our first read but
        // BEFORE the service became empty. Only this post-extinction read can
        // establish that no registered/migrated keeper was overlooked.
        return read();
    }
};
}
