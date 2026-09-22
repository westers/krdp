// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUuid>
#include <optional>

namespace KRdp {
/** V2 coordinator record. Transition construction alone grants no authority:
 * callers must hold the ordered leases, durably publish claims before validating,
 * authenticate the coordinator invocation, and validate the approved profile.
 * This type neither bootstraps missing storage nor authorizes clean publication.
 */
struct VirtualSessionMaintenanceRecord {
    enum class Phase { InitialBlocked, Validating, Clean, ExternalUnknown };
    Phase phase = Phase::ExternalUnknown;
    QString installation, transaction, generation, boot, profile, invocation, attestation, epoch;
    bool operator==(const VirtualSessionMaintenanceRecord &) const = default;

    static bool uuid(const QString &value) {
        return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value;
    }
    static bool digest(const QString &value) {
        if (value.size() != 64) return false;
        for (const QChar c : value)
            if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
        return true;
    }
    bool valid() const {
        if (!uuid(generation) || !uuid(boot)) return false;
        if (phase == Phase::ExternalUnknown)
            return (installation.isEmpty() || uuid(installation)) && transaction.isEmpty()
                && profile.isEmpty() && invocation.isEmpty() && attestation.isEmpty() && epoch.isEmpty();
        if (!uuid(installation) || !uuid(transaction) || !digest(profile) || !uuid(attestation)) return false;
        if (phase == Phase::InitialBlocked) return invocation.isEmpty() && epoch.isEmpty();
        if (!uuid(invocation)) return false;
        if (phase == Phase::Validating) return epoch.isEmpty();
        return phase == Phase::Clean && uuid(epoch);
    }
    QByteArray encode() const {
        if (!valid()) return {};
        const char *name = phase == Phase::InitialBlocked ? "initial-blocked" : phase == Phase::Validating ? "validating"
            : phase == Phase::Clean ? "clean" : "external-unknown";
        QByteArray result = QByteArray("KRDP-MAINTENANCE-2\n") + name + '\n';
        // Fixed order, ASCII UUID/digest or empty line, one final LF. No
        // optional keys, escaping, whitespace normalization or extra records.
        for (const auto &field : {installation, transaction, generation, boot, profile, invocation, attestation, epoch})
            result += field.toLatin1() + '\n';
        return result;
    }
    static std::optional<VirtualSessionMaintenanceRecord> decode(const QByteArray &bytes) {
        if (bytes.isEmpty() || bytes.size() > 512) return {};
        const auto fields = bytes.split('\n');
        if (fields.size() != 11 || fields[0] != "KRDP-MAINTENANCE-2" || !fields[10].isEmpty()) return {};
        VirtualSessionMaintenanceRecord r;
        if (fields[1] == "initial-blocked") r.phase = Phase::InitialBlocked;
        else if (fields[1] == "validating") r.phase = Phase::Validating;
        else if (fields[1] == "clean") r.phase = Phase::Clean;
        else if (fields[1] != "external-unknown") return {};
        r.installation = QString::fromLatin1(fields[2]); r.transaction = QString::fromLatin1(fields[3]);
        r.generation = QString::fromLatin1(fields[4]); r.boot = QString::fromLatin1(fields[5]);
        r.profile = QString::fromLatin1(fields[6]); r.invocation = QString::fromLatin1(fields[7]);
        r.attestation = QString::fromLatin1(fields[8]); r.epoch = QString::fromLatin1(fields[9]);
        return r.valid() && r.encode() == bytes ? std::optional(r) : std::nullopt;
    }
    std::optional<VirtualSessionMaintenanceRecord> claim(const QString &currentBoot, const QString &coordinator) const {
        if (!valid() || phase != Phase::InitialBlocked || boot != currentBoot || !uuid(coordinator)) return {};
        auto next = *this;
        next.phase = Phase::Validating;
        next.invocation = coordinator;
        return next;
    }
    std::optional<VirtualSessionMaintenanceRecord> invalidate(const QString &currentBoot) const {
        if (!valid() || !uuid(currentBoot)) return {};
        VirtualSessionMaintenanceRecord next;
        next.installation = installation;
        next.boot = currentBoot;
        next.generation = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return next;
    }
};
}
