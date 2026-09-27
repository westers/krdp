// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

// AUD-K2: every keychain operation reports its result when its job finishes
// (never right after start(), when nothing has happened yet), and the user
// list only changes once the password it depends on is safely stored.

class PasswordStore : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(bool ok, const QString &error)>;
    using ReadDone = std::function<void(bool ok, const QString &password, const QString &error)>;

    using QObject::QObject;

    virtual void writePassword(const QString &user, const QString &password, Done done) = 0;
    virtual void deletePassword(const QString &user, Done done) = 0;
    virtual void readPassword(const QString &user, ReadDone done) = 0;
};

// The "KRDP" service in the user's keychain (KWallet), keyed by the UTF-8
// user name; krdpserver reads the same entries at startup.
class KeychainPasswordStore : public PasswordStore
{
    Q_OBJECT
public:
    using PasswordStore::PasswordStore;

    static QString serviceName();

    void writePassword(const QString &user, const QString &password, Done done) override;
    void deletePassword(const QString &user, Done done) override;
    void readPassword(const QString &user, ReadDone done) override;
};

class UserAccounts : public QObject
{
    Q_OBJECT
public:
    // `users` reads the current list; `commitUsers` replaces it (the KCM
    // updates the model and saves).
    UserAccounts(PasswordStore *store, std::function<QStringList()> users, std::function<void(const QStringList &)> commitUsers, QObject *parent = nullptr);

    // Stores the password, then adds the user.
    void addUser(const QString &user, const QString &password);
    // Stores the new password under the (possibly new) name. On a rename the
    // list changes, then the old entry is deleted, only after that write
    // succeeded.
    void modifyUser(const QString &oldUser, const QString &newUser, const QString &password);
    // Removes the user, then its keychain entry.
    void deleteUser(const QString &user);
    void readPassword(const QString &user);

Q_SIGNALS:
    void passwordStored(const QString &user);
    void passwordLoaded(const QString &user, const QString &password);
    void keychainError(const QString &errorText);
    void busyChanged(bool busy);

public:
    int pendingOperations() const
    {
        return m_pending;
    }

private:
    void begin();
    void end();

    PasswordStore *m_store;
    std::function<QStringList()> m_users;
    std::function<void(const QStringList &)> m_commitUsers;
    int m_pending = 0;
};
