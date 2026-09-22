// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionMaintenanceRecord.h"

namespace KRdp {
class VirtualSessionRuntimeProfile;
class VirtualSessionProvisioningReceiptTest;
/** Trusted first-provisioning evidence reader, not bootstrap authority.
 * The trusted installer must durably create this receipt at the explicit
 * supported-writer boundary. Ownership and fsync cannot prove that history.
 * Caller must hold package SH/frontend, SH/backend, gate EX, read EXISTING
 * InitialBlocked state, and durably consume its claim before validation.
 * Missing/deleted state or receipt never means first installation. This reader
 * does not create state, approve a profile, establish quiescence or publish clean.
 *
 * Canonical ASCII: KRDP-FIRST-PROVISIONING-1 LF, then installation, transaction,
 * generation, boot, profile SHA256, attestation, each followed by LF. No other
 * fields, whitespace or records. UUIDs/digest use MaintenanceRecord's grammar.
 */
class VirtualSessionProvisioningReceipt {
public:
    // Real/effective/saved-root only, fixed
    // /var/lib/krdp/maintenance/first-provisioning-receipt.
    // Establishes file+directory durability on every invocation; failures refuse
    // untouched. Profile digest matching is not profile filesystem validation.
    static bool validate(const VirtualSessionMaintenanceRecord &expected,
        const VirtualSessionRuntimeProfile &approvedProfile, QString *error = nullptr);
private:
    friend class VirtualSessionProvisioningReceiptTest;
    static bool validateAt(const QString &directory, unsigned owner, bool fixture,
        const QString &currentBoot, const QString &profileDigest,
        const VirtualSessionMaintenanceRecord &expected, QString *error);
};
}
