// SPDX-FileCopyrightText: 2025 David Edmundson <davidedmundson@kde.org>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "krdpserversettings.h"
#include <QAbstractListModel>
#include <QStringList>

#include <KUser>

/*
 * A model representing a list of users that can be used to configure the KRDPServerSettings.
 * The first entry always represents the system user.
 *
 */

class UsersModel : public QAbstractListModel
{
    Q_OBJECT
    // AUD-K10: rowCount() always includes the system-user row, so views must
    // not use `count` to ask "can anybody log in?".
    Q_PROPERTY(int additionalUserCount READ additionalUserCount NOTIFY loginMethodsChanged)
    Q_PROPERTY(int loginMethodCount READ loginMethodCount NOTIFY loginMethodsChanged)
public:
    explicit UsersModel(KRDPServerSettings *settings, QObject *parent = nullptr);

    enum Roles {
        /// both a human name but also an ID for additional users
        DisplayRole = Qt::DisplayRole,
        /// whether this entry represents the first entry for system login
        SystemUserRole = Qt::UserRole,
        // whether system user login is enabled, only applicable for SystemUserRole
        SystemUserEnabledRole = Qt::UserRole + 1
    };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QHash<int, QByteArray> roleNames() const override;


    QStringList users() const;
    /**
     * Updates both the model and the underlying configuration object
     */
    void setUsers(const QStringList &users);

    /// Users with a stored password (the rows after the system-user row).
    int additionalUserCount() const;
    /// additionalUserCount() plus one when system-password login is enabled.
    int loginMethodCount() const;

Q_SIGNALS:
    void loginMethodsChanged();

private:
    KRDPServerSettings *m_settings;
    KUser m_currentUser;
};

