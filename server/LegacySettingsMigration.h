// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <functional>
#include <optional>

namespace KRdp::LegacySettingsMigration {
struct Owner { QString account; quint32 uid = 0; };
using Resolver = std::function<std::optional<quint32>(const QString &)>;
// All document/host/admission data below is private. No password, wallet, policy
// writer, authorization or path selection is part of this planning component.
struct Snapshot {
    Owner owner;
    QVariantMap preferences; // All17 legacy effective values, including omissions.
    QVariantMap hostFacts; // Legacy facts, NOT an authorized broker configuration.
    bool ownerPam = false; // true -> allow-list{owner}, false -> disabled; never any.
    QStringList aliases; // Every alias belongs to owner.uid, never its name's UID.
    QSet<QString> lockedPreferences;
    QSet<QString> lockedHostFacts, lockedAdmissionKeys;
    QStringList defaultedKeys;
    int unknownKeys = 0;
    QByteArray revision;
    QString error; // Fixed structural reason/key only.
};
Snapshot inspect(const QByteArray &source, const Owner &trustedOwner, const Resolver &resolve);
struct Plan {
    Snapshot legacy;
    QByteArray document; // Preserving prepared destination; never a public property.
    QByteArray revision;
    bool destinationPresent = false;
    bool changed = false;
    QStringList conflicts; // Recognized keys only, no raw values.
    QString error;
};
enum class DestinationMode { Separate, SameLegacySnapshot };
// nullopt means absent, QByteArray{} means existing-empty. No destination write.
// SameLegacySnapshot is explicit trusted transaction input, never inferred from
// equal bytes. It requires the destination to still equal the source snapshot.
Plan prepare(const QByteArray &source, const std::optional<QByteArray> &destination,
             const Owner &trustedOwner, const Resolver &resolve, DestinationMode mode = DestinationMode::Separate);
// Counts and fixed pending actions only; preparation never authorizes cutover.
QJsonObject manifest(const Plan &plan);
}
