// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QMetaType>
#include <QString>

namespace KRdp
{
/// Per-second AVC444 encoder costs, mirrored from private KPipeWire (microseconds).
struct ChromaTimingReport {
    int frames = 0, auxSent = 0, auxSkippedMotion = 0, auxRestRefresh = 0, rewriteFailures = 0;
    QString splitVariant;
    qint64 downloadAvg = 0, downloadMax = 0, splitAvg = 0, splitMax = 0, uploadAvg = 0, uploadMax = 0;
    qint64 encodeMainAvg = 0, encodeMainMax = 0, encodeAuxAvg = 0, encodeAuxMax = 0;
    int auxMaxGap = 0;
    qint64 downloadMin = 0, splitMin = 0, uploadMin = 0, encodeMainMin = 0, encodeAuxMin = 0;
    bool operator==(const ChromaTimingReport &) const = default;

    QString costSummary() const
    {
        return QStringLiteral("AVC444 cost: frames %1 aux sent %2 skipped-motion %3 rest-refresh %4 aux max-gap %5 rewrite-failures %6 split=%7 download avg %8 max %9 us split avg %10 max %11 us upload avg %12 max %13 us main queue->packet avg %14 max %15 aux avg %16 max %17 us min download %18 split %19 upload %20 main %21 aux %22 us")
            .arg(frames).arg(auxSent).arg(auxSkippedMotion).arg(auxRestRefresh).arg(auxMaxGap)
            .arg(rewriteFailures).arg(splitVariant).arg(downloadAvg).arg(downloadMax)
            .arg(splitAvg).arg(splitMax).arg(uploadAvg).arg(uploadMax)
            .arg(encodeMainAvg).arg(encodeMainMax).arg(encodeAuxAvg).arg(encodeAuxMax)
            .arg(downloadMin).arg(splitMin).arg(uploadMin).arg(encodeMainMin).arg(encodeAuxMin);
    }
};
}
Q_DECLARE_METATYPE(KRdp::ChromaTimingReport)
