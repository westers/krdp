// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionMaintenanceGuard.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace KRdp {
// Diagnostic output only: a clean historical record is not admission proof.
inline QByteArray maintenanceStatusJson(const VirtualSessionMaintenanceGuard::Diagnostic &status) {
    using Phase = VirtualSessionMaintenanceRecord::Phase;
    const auto phase = status.record.phase;
    const QString name = phase == Phase::InitialBlocked ? QStringLiteral("initial-blocked")
        : phase == Phase::Validating ? QStringLiteral("validating")
        : phase == Phase::Clean ? QStringLiteral("clean") : QStringLiteral("external-unknown");
    return QJsonDocument(QJsonObject{
        {QStringLiteral("v"), 1}, {QStringLiteral("phase"), name},
        {QStringLiteral("currentBoot"), status.currentBoot},
        {QStringLiteral("boot"), status.record.boot},
        {QStringLiteral("generation"), status.record.generation},
        {QStringLiteral("profile"), status.record.profile},
        {QStringLiteral("admission"), QStringLiteral("not-evaluated")}
    }).toJson(QJsonDocument::Compact) + '\n';
}
}
