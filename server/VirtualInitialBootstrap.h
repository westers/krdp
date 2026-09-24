// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "VirtualSessionJournal.h"
#include "RetainedKScreenReadback.h"

#include <algorithm>

namespace KRdp::VirtualInitialBootstrap
{
using Output = VirtualSessionJournal::Record::InitialOutput;

inline QString name(qsizetype index)
{
    return index == 0 ? QStringLiteral("Virtual-0") : QStringLiteral("Virtual-krdp-initial-%1").arg(index);
}

inline std::optional<QStringList> applyArguments(const QVector<Output> &expected,
    const RetainedKScreenReadback::Snapshot &before)
{
    if (expected.isEmpty() || expected.size() != before.outputs.size()) return {};
    QStringList arguments;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        const auto key = name(i);
        const auto found = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&key](const auto &entry) {
            return entry.backendKey == key;
        });
        if (found == before.outputs.cend() || found->nativePixels != expected[i].pixels) return {};
        const QString prefix = QStringLiteral("output.%1.").arg(key);
        arguments << prefix + QStringLiteral("scale.") + QString::number(expected[i].scale, 'g', 17)
            << prefix + QStringLiteral("position.%1,%2").arg(expected[i].position.x()).arg(expected[i].position.y())
            << prefix + QStringLiteral("priority.%1").arg(i + 1);
    }
    return arguments;
}

inline bool matches(const QVector<Output> &expected, const RetainedKScreenReadback::Snapshot &readback)
{
    if (expected.isEmpty() || expected.size() != readback.outputs.size()) return false;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        const auto key = name(i);
        const auto found = std::find_if(readback.outputs.cbegin(), readback.outputs.cend(), [&key](const auto &entry) {
            return entry.backendKey == key;
        });
        if (found == readback.outputs.cend() || found->nativePixels != expected[i].pixels
            || found->scale != expected[i].scale || found->logicalGeometry.topLeft() != expected[i].position
            || found->logicalGeometry.size() != RemoteMonitorGeometry::logicalSize(expected[i].pixels, expected[i].scale)
            || found->primary != expected[i].primary) return false;
    }
    return true;
}
}
