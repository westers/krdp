// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QString>
#include <memory>

namespace KRdp
{
/** Prepare storage after dropping to the PAM user's real/effective UID.
 * Caller keeps the returned object alive for the desktop lifetime: it holds an
 * exclusive profile lock. Destruction releases FDs, never deletes user data.
 * A preexisting runtime is refused, not treated as a recoverable desktop.
 */
class VirtualSessionStorage
{
public:
    ~VirtualSessionStorage();
    VirtualSessionStorage(const VirtualSessionStorage &) = delete;
    VirtualSessionStorage &operator=(const VirtualSessionStorage &) = delete;
    static std::unique_ptr<VirtualSessionStorage> prepare(quint32 uid, const QString &accountHome,
        const QString &sessionId, const QString &launchId, const QByteArray &token, QString *error = nullptr);
    QString runtimeDirectory() const { return m_runtimePath; }
    QString profileDirectory() const { return m_profilePath; }

private:
    friend class VirtualSessionStorageTest;
    VirtualSessionStorage() = default;
    static std::unique_ptr<VirtualSessionStorage> prepareAt(quint32 uid, const QString &home,
        const QString &runtimeBase, const QString &sessionId, const QString &launchId,
        const QByteArray &token, QString *error);
    int m_lock = -1;
    QString m_runtimePath;
    QString m_profilePath;
};
}
