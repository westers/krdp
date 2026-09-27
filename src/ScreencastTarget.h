// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QRect>
#include <QRegion>
#include <QString>
#include <QVector>

#include "SurfaceLayout.h"

namespace KRdp
{

/**
 * Which output a physical PlasmaScreencastV1Session captures, and the logical
 * rect its pointer input is mapped through.
 *
 * AUD-P1: the capture screen and the input rect used to come from different
 * sources (the screen from a remembered name, the rect from the current
 * index), so after an index or primary change the stream showed one monitor
 * while input landed on another. resolve() derives both from the one screen
 * it picks, and is pure (no QGuiApplication) so the agreement is unit tested.
 */
namespace ScreencastTarget
{

struct Screen {
    QString name;
    QRect geometry;
    bool primary = false;
};

enum class Kind {
    /** One output: screens.at(screenIndex). */
    Output,
    /** The whole workspace (index -1 or out of range, or the named screen is gone and fallback is allowed). */
    Workspace,
    /** Recovery only: the remembered screen is not back yet and workspace fallback is not allowed. */
    Wait,
};

struct Resolution {
    Kind kind = Kind::Workspace;
    /** Index into the screens passed in, for Kind::Output; -1 otherwise. */
    int screenIndex = -1;
    /** The captured screen's name, for Kind::Output; what a later recovery looks up. */
    QString name;
    /** KWin-global logical rect of the capture: input is mapped through this. */
    QRect logicalRect;
    /** Monitor layout local to logicalRect, for the RDPGFX reset. */
    QVector<VideoMonitor> monitors;
};

/**
 * The remembered screen name is honoured only for a recovery (a closed stream
 * being re-created while KWin re-adds its outputs, where the list order is
 * transiently wrong) and only while it was resolved for the same stream index.
 * An explicit start or refresh resolves from the index, the configured truth,
 * which the controller re-derives on every topology or primary change.
 */
inline Resolution resolve(const QList<Screen> &screens, int streamIndex, const QString &rememberedName, int rememberedIndex, bool recovery, bool allowWorkspaceFallback)
{
    Resolution result;
    if (screens.isEmpty()) {
        return result;
    }

    int picked = -1;
    const bool useName = recovery && !rememberedName.isEmpty() && rememberedIndex == streamIndex && streamIndex >= 0;
    if (useName) {
        for (int i = 0; i < screens.size(); ++i) {
            if (screens.at(i).name == rememberedName) {
                picked = i;
                break;
            }
        }
        if (picked < 0 && !allowWorkspaceFallback) {
            result.kind = Kind::Wait;
            result.name = rememberedName;
            return result;
        }
    } else if (streamIndex >= 0 && streamIndex < screens.size()) {
        picked = streamIndex;
    }

    if (picked >= 0) {
        const auto &screen = screens.at(picked);
        result.kind = Kind::Output;
        result.screenIndex = picked;
        result.name = screen.name;
        result.logicalRect = screen.geometry;
        result.monitors = {VideoMonitor{.geometry = QRect(QPoint(0, 0), screen.geometry.size()), .primary = true}};
        return result;
    }

    QRegion region;
    for (const auto &screen : screens) {
        region += screen.geometry;
    }
    result.kind = Kind::Workspace;
    result.logicalRect = region.boundingRect();
    bool anyPrimary = false;
    for (const auto &screen : screens) {
        result.monitors.push_back(VideoMonitor{.geometry = screen.geometry.translated(-result.logicalRect.topLeft()), .primary = screen.primary});
        anyPrimary = anyPrimary || screen.primary;
    }
    if (!anyPrimary && !result.monitors.isEmpty()) {
        result.monitors.first().primary = true;
    }
    return result;
}

} // namespace ScreencastTarget
} // namespace KRdp
