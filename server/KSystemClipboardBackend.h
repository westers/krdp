// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QClipboard>
#include <QMimeData>

#include <KSystemClipboard>

#include "ClipboardImage.h"
#include "HostClipboard.h"
#include "ClipboardLimits.h"
#include "PngMimeData.h"
#include "SelectionReader.h"

namespace KRdp
{
/**
 * OPT-067 / OPT-072: the worker's HostClipboard backend over KSystemClipboard (the data-control protocol, falling back to
 * QClipboard where the compositor has none). It lives in a header so the worker and the Wayland probe use the same code.
 */
inline HostClipboard::Backend ksystemClipboardBackend()
{
    return HostClipboard::Backend{
        [] {
            const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
            return data && data->hasText();
        },
        [] {
            const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
            return data ? data->text() : QString();
        },
        [](const QString &text, bool secret) {
            auto *clipboard = KSystemClipboard::instance();
            if (text.isEmpty()) {
                clipboard->clear(QClipboard::Clipboard);
                return true;
            }
            // KSystemClipboard takes ownership. A write made on the login screen or a locked session asks
            // Klipper to keep it out of its history.
            auto *mime = new QMimeData();
            mime->setText(text);
            if (secret) mime->setData(QStringLiteral("x-kde-passwordManagerHint"), QByteArrayLiteral("secret"));
            clipboard->setMimeData(mime, QClipboard::Clipboard);
            return true;
        },
        // OPT-072: pictures. Offered when any image format is listed (nothing is read); read on paste only.
        [] {
            const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
            return data && !ClipboardImage::preferredMime(data->formats()).isEmpty();
        },
        [] {
            const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
            if (!data) return QByteArray();
            const QString mimeType = ClipboardImage::preferredMime(data->formats());
            return mimeType.isEmpty() ? QByteArray() : data->data(mimeType);
        },
        [](const QByteArray &png, bool secret) {
            // KSystemClipboard takes ownership. PngMimeData, not a plain QMimeData with setData("image/png"): see its comment
            // (KSystemClipboard 6.24 sends an empty image to every other process for the plain form).
            KSystemClipboard::instance()->setMimeData(new PngMimeData(png, secret ? QByteArrayLiteral("secret") : QByteArray()), QClipboard::Clipboard);
            return true;
        },
        // OPT-072: the same read with a byte cap (16 MiB for a PNG, 64 MiB for the other formats, the bridge's own limits).
        // KSystemClipboard reads an offer to its end whatever its size, so on KWin (ext_data_control) the worker reads the
        // selection itself and abandons a source that sends more; elsewhere it keeps the unbounded read above.
        [] {
            const auto read = SelectionReader::readSelection(ClipboardImage::mimeOrder(), [](const QString &mime) {
                return mime == QLatin1String("image/png") ? ClipboardLimits::MaxImageBytes : ClipboardLimits::MaxImageWireBytes;
            });
            switch (read.status) {
            case SelectionReader::Status::Ok:
                return HostClipboard::ImageRead{read.data, false, read.bytes};
            case SelectionReader::Status::TooLarge:
                return HostClipboard::ImageRead{{}, true, read.bytes};
            case SelectionReader::Status::Unavailable: {
                const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
                if (!data) return HostClipboard::ImageRead{};
                const QString mimeType = ClipboardImage::preferredMime(data->formats());
                return HostClipboard::ImageRead{mimeType.isEmpty() ? QByteArray() : data->data(mimeType), false, 0};
            }
            default:
                return HostClipboard::ImageRead{}; // nothing there, no readable format, a stalled source: as "gone"
            }
        }};
}
}
