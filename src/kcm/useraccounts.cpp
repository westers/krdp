// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "useraccounts.h"

#include <KLocalizedString>
#include <QPointer>
#include <qt6keychain/keychain.h>

QString KeychainPasswordStore::serviceName()
{
    return QStringLiteral("Farside Server");
}

namespace
{
template<typename Job>
Job *startJob(QObject *parent, const QString &user, std::function<void(Job *)> onFinished, std::function<void(Job *)> setup = {})
{
    auto job = new Job(KeychainPasswordStore::serviceName(), parent);
    job->setAutoDelete(true);
    job->setKey(user);
    if (setup) {
        setup(job);
    }
    QObject::connect(job, &QKeychain::Job::finished, parent, [job, onFinished]() {
        onFinished(job);
    });
    job->start();
    return job;
}
}

void KeychainPasswordStore::writePassword(const QString &user, const QString &password, Done done)
{
    startJob<QKeychain::WritePasswordJob>(
        this,
        user,
        [done](QKeychain::WritePasswordJob *job) {
            done(job->error() == QKeychain::NoError, job->errorString());
        },
        [password](QKeychain::WritePasswordJob *job) {
            job->setTextData(password);
        });
}

void KeychainPasswordStore::deletePassword(const QString &user, Done done)
{
    startJob<QKeychain::DeletePasswordJob>(this, user, [done](QKeychain::DeletePasswordJob *job) {
        // Nothing to delete is what deleting wanted.
        const bool ok = job->error() == QKeychain::NoError || job->error() == QKeychain::EntryNotFound;
        done(ok, job->errorString());
    });
}

void KeychainPasswordStore::readPassword(const QString &user, ReadDone done)
{
    startJob<QKeychain::ReadPasswordJob>(this, user, [this, user, done](QKeychain::ReadPasswordJob *job) {
        if (job->error() == QKeychain::EntryNotFound) {
            auto *legacy = new QKeychain::ReadPasswordJob(QStringLiteral("KRDP"), this);
            legacy->setAutoDelete(true);
            legacy->setKey(user);
            connect(legacy, &QKeychain::Job::finished, this, [legacy, done]() {
                const bool ok = legacy->error() == QKeychain::NoError;
                done(ok, ok ? legacy->textData() : QString(), legacy->errorString());
            });
            legacy->start();
            return;
        }
        const bool ok = job->error() == QKeychain::NoError;
        done(ok, ok ? job->textData() : QString(), job->errorString());
    });
}

UserAccounts::UserAccounts(PasswordStore *store, std::function<QStringList()> users, std::function<void(const QStringList &)> commitUsers, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_users(std::move(users))
    , m_commitUsers(std::move(commitUsers))
{
}

void UserAccounts::begin()
{
    if (m_pending++ == 0) {
        Q_EMIT busyChanged(true);
    }
}

void UserAccounts::end()
{
    if (--m_pending == 0) {
        Q_EMIT busyChanged(false);
    }
}

void UserAccounts::addUser(const QString &user, const QString &password)
{
    if (user.isEmpty() || m_users().contains(user)) {
        return;
    }
    begin();
    QPointer self(this);
    m_store->writePassword(user, password, [self, user](bool ok, const QString &error) {
        if (!self) {
            return;
        }
        if (ok) {
            auto users = self->m_users();
            if (!users.contains(user)) {
                users.append(user);
                self->m_commitUsers(users);
            }
            Q_EMIT self->passwordStored(user);
        } else {
            Q_EMIT self->keychainError(i18nc("@info", "The password for “%1” could not be stored, so the user was not added: %2", user, error));
        }
        self->end();
    });
}

void UserAccounts::modifyUser(const QString &oldUser, const QString &newUser, const QString &password)
{
    if (oldUser.isEmpty()) {
        return;
    }
    const bool rename = !newUser.isEmpty() && newUser != oldUser;
    if (rename && m_users().contains(newUser)) {
        return;
    }
    const QString target = rename ? newUser : oldUser;
    begin();
    QPointer self(this);
    m_store->writePassword(target, password, [self, oldUser, target, rename](bool ok, const QString &error) {
        if (!self) {
            return;
        }
        if (!ok) {
            Q_EMIT self->keychainError(i18nc("@info", "The password for “%1” could not be stored; nothing was changed: %2", target, error));
            self->end();
            return;
        }
        Q_EMIT self->passwordStored(target);
        if (!rename) {
            self->end();
            return;
        }
        auto users = self->m_users();
        const auto index = users.indexOf(oldUser);
        if (index >= 0) {
            users[index] = target;
        } else {
            users.append(target);
        }
        self->m_commitUsers(users);
        self->m_store->deletePassword(oldUser, [self, oldUser](bool ok, const QString &error) {
            if (!self) {
                return;
            }
            if (!ok) {
                Q_EMIT self->keychainError(i18nc("@info", "The user was renamed, but the old password entry for “%1” could not be removed: %2", oldUser, error));
            }
            self->end();
        });
    });
}

void UserAccounts::deleteUser(const QString &user)
{
    if (user.isEmpty()) {
        return;
    }
    auto users = m_users();
    if (users.removeAll(user) > 0) {
        m_commitUsers(users);
    }
    begin();
    QPointer self(this);
    m_store->deletePassword(user, [self, user](bool ok, const QString &error) {
        if (!self) {
            return;
        }
        if (!ok) {
            Q_EMIT self->keychainError(i18nc("@info", "The user was removed, but its password entry for “%1” could not be deleted: %2", user, error));
        }
        self->end();
    });
}

void UserAccounts::readPassword(const QString &user)
{
    if (user.isEmpty()) {
        return;
    }
    begin();
    QPointer self(this);
    m_store->readPassword(user, [self, user](bool ok, const QString &password, const QString &error) {
        if (!self) {
            return;
        }
        if (ok) {
            Q_EMIT self->passwordLoaded(user, password);
        } else {
            Q_EMIT self->keychainError(i18nc("@info", "The password for “%1” could not be read: %2", user, error));
        }
        self->end();
    });
}
