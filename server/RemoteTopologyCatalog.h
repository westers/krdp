// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cmath>
#include <limits>
#include <optional>

#include <QMap>
#include <QRect>
#include <QSize>
#include <QString>
#include <QUuid>
#include <QVector>

namespace KRdp
{
// A readback cache, not an authority for changing the compositor. Backend
// keys identify outputs only while continuously present; a departed key gets
// a new public ID if it reappears, even in the same compositor generation.
class RemoteTopologyCatalog
{
public:
    struct Output {
        QString backendKey;
        QString name;
        QSize nativePixels;
        QRect logicalGeometry;
        qreal scale = 1.0;
        bool enabled = true;
        bool primary = false;
        bool physical = true;
        QString owner;

        bool operator==(const Output &) const = default;
    };

    struct Entry {
        QString id;
        Output output;

        bool operator==(const Entry &) const = default;
    };

    struct Snapshot {
        QString generation;
        quint64 revision = 0;
        QVector<Entry> outputs;

        bool operator==(const Snapshot &) const = default;
    };

    RemoteTopologyCatalog() { resetGeneration(); }

    void resetGeneration()
    {
        m_snapshot = {QUuid::createUuid().toString(QUuid::WithoutBraces), 0, {}};
        m_current.clear();
        m_nextId = 0;
        m_observed = false;
    }

    // Feed *fresh* compositor readback. Invalid/ambiguous readback leaves the
    // last good snapshot untouched; callers must report failure, not success.
    std::optional<Snapshot> observe(const QVector<Output> &readback)
    {
        QMap<QString, Output> sorted;
        for (const auto &output : readback) {
            if (output.backendKey.isEmpty() || sorted.contains(output.backendKey)
                || !std::isfinite(output.scale) || output.scale <= 0.0
                || (output.enabled && (output.nativePixels.isEmpty() || output.logicalGeometry.isEmpty()))) return {};
            sorted.insert(output.backendKey, output);
        }
        if (m_snapshot.revision >= MaxExactJsonInteger || m_nextId > MaxExactJsonInteger - quint64(sorted.size())) return {};

        QMap<QString, QString> nextCurrent;
        QVector<Entry> entries;
        entries.reserve(sorted.size());
        quint64 nextId = m_nextId;
        for (auto it = sorted.cbegin(); it != sorted.cend(); ++it) {
            auto id = m_current.value(it.key());
            if (id.isEmpty()) id = QStringLiteral("o-%1").arg(++nextId);
            nextCurrent.insert(it.key(), id);
            entries.append({id, it.value()});
        }
        if (!m_observed || entries != m_snapshot.outputs) ++m_snapshot.revision;
        m_snapshot.outputs = std::move(entries);
        m_current = std::move(nextCurrent);
        m_nextId = nextId;
        m_observed = true;
        return m_snapshot;
    }

    const Snapshot &snapshot() const { return m_snapshot; }
    bool observed() const { return m_observed; }

private:
    static constexpr quint64 MaxExactJsonInteger = (quint64(1) << 53) - 1;
    Snapshot m_snapshot;
    QMap<QString, QString> m_current;
    quint64 m_nextId = 0;
    bool m_observed = false;
};
}
