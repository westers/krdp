// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionGuardianClient.h"
#include <memory>
#include <optional>
#include <QVector>

namespace KRdp {
/** Root-service launch intent, persisted BEFORE spawn. Never proof of liveness.
 * Immutable per desktop: uncertain state cannot overwrite credentials or spawn
 * a replacement. Recovery must match boot and authenticate guardian + capture.
 * No client-supplied paths, commands, PIDs or environment are persisted.
 */
class VirtualSessionJournal {
public:
    struct Record {
        quint32 uid = 0;
        QString session, launch, incarnation, boot;
        QByteArray token;
        bool valid() const;
        VirtualSessionGuardianClient::Identity identity() const;
        QString workerSocket() const;
        bool operator==(const Record &) const = default;
    };
    ~VirtualSessionJournal();
    VirtualSessionJournal(const VirtualSessionJournal &) = delete;
    VirtualSessionJournal &operator=(const VirtualSessionJournal &) = delete;
    // Installer supplies /var/lib/krdp/virtual-sessions, root:root 0700.
    // Exclusive nonblocking directory flock lasts until destruction.
    static std::unique_ptr<VirtualSessionJournal> open(QString *error = nullptr);
    // Independent root-service entry reads only its committed immutable intent,
    // without taking the broker's exclusive lease. Not permission to relaunch.
    static std::optional<Record> readLaunchIntent(const QString &session, QString *error = nullptr);
    // Persist one-use launch consumption before any exec. Failure/uncertainty
    // leaves the marker; only explicit privileged reconciliation may remove it.
    static bool claimLaunch(const Record &expected, QString *error = nullptr);
    struct Keeper {
        pid_t pid = 0;
        quint64 startTicks = 0;
        quint64 pidInode = 0;
        bool operator==(const Keeper &) const = default;
    };
    // Keeper records its own birth identity durably BEFORE any PAM call.
    // Immutable, separate from broker recovery identity; never deleted here.
    static bool recordKeeper(const Record &expected, QString *error = nullptr);
    // Missing is distinct from malformed/uncertain. Neither supplies a PID.
    static std::optional<Keeper> readKeeper(const Record &expected, bool *missing, QString *error = nullptr);
    bool insert(const Record &record, QString *error = nullptr);
    std::optional<QVector<Record>> records(QString *error = nullptr) const;
private:
    friend class VirtualSessionJournalTest;
    friend class VirtualSessionHostControllerTest;
    VirtualSessionJournal(int directory, quint32 owner, bool writable) : m_directory(directory), m_owner(owner), m_writable(writable) {}
    static std::unique_ptr<VirtualSessionJournal> openAt(const QString &path, quint32 owner, QString *error, bool writable = true);
    std::optional<Record> readRecord(const QString &session, QString *error) const;
    bool claimRecord(const Record &expected, QString *error);
    bool hasClaim(const Record &expected) const;
    bool writeKeeper(const Record &expected, const Keeper &keeper, QString *error);
    std::optional<Keeper> readKeeperRecord(const Record &expected, bool *missing, QString *error) const;
    int m_directory;
    quint32 m_owner;
    bool m_writable;
};
}
