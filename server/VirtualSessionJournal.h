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
    };
    ~VirtualSessionJournal();
    VirtualSessionJournal(const VirtualSessionJournal &) = delete;
    VirtualSessionJournal &operator=(const VirtualSessionJournal &) = delete;
    // Installer supplies /var/lib/krdp/virtual-sessions, root:root 0700.
    // Exclusive nonblocking directory flock lasts until destruction.
    static std::unique_ptr<VirtualSessionJournal> open(QString *error = nullptr);
    bool insert(const Record &record, QString *error = nullptr);
    std::optional<QVector<Record>> records(QString *error = nullptr) const;
private:
    friend class VirtualSessionJournalTest;
    VirtualSessionJournal(int directory, quint32 owner) : m_directory(directory), m_owner(owner) {}
    static std::unique_ptr<VirtualSessionJournal> openAt(const QString &path, quint32 owner, QString *error);
    int m_directory;
    quint32 m_owner;
};
}
