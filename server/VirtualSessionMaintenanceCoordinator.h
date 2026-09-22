// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>

namespace KRdp {
/** Fixed first-bootstrap coordinator, not a generic maintenance wrapper.
 * Requires an existing installer-owned InitialBlocked record and receipt.
 * No arguments can supply a profile, path, invocation, predicate or clean state.
 * A consumed/interrupted/unknown attempt cannot be retried as first bootstrap.
 */
class VirtualSessionMaintenanceCoordinator {
public:
    enum class Result { Clean, Refused, Uncertain };
    static Result bootstrap(QString *error = nullptr);
};
}
