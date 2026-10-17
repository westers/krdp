// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QClipboard>
#include <QMimeData>
#include <QPointer>

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
    // OPT-077: the text+html offer this process last wrote. Reading our own selection through SelectionReader would ask this very
    // thread (blocked in the read) to serve the data and stall until the reader's idle timeout, so the echo check reads the
    // QMimeData we hold instead. A QPointer: KSystemClipboard owns it and deletes it when another copy replaces it.
    static QPointer<QMimeData> ownRich;
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
        },
        // OPT-077: formatted text. Offered when text/html is listed (nothing is read); read on paste only, with the same
        // byte cap as the picture reader (twice the document cap: some sources offer UTF-16, which is turned into UTF-8 later).
        [] {
            const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
            return data && data->hasFormat(QStringLiteral("text/html"));
        },
        [] {
            const QString mime = QStringLiteral("text/html");
            if (const auto *own = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard); own && own == ownRich.data()) {
                const QByteArray bytes = own->data(mime); // our own write: in-process, never a stall
                return HostClipboard::ImageRead{bytes, false, qint64(bytes.size())};
            }
            const auto read = SelectionReader::readSelection({mime}, [](const QString &) { return ClipboardLimits::MaxHtmlBytes * 2; });
            switch (read.status) {
            case SelectionReader::Status::Ok:
                return HostClipboard::ImageRead{read.data, false, read.bytes};
            case SelectionReader::Status::TooLarge:
                return HostClipboard::ImageRead{{}, true, read.bytes};
            case SelectionReader::Status::Unavailable: {
                const auto *data = KSystemClipboard::instance()->mimeData(QClipboard::Clipboard);
                if (!data || !data->hasFormat(mime)) return HostClipboard::ImageRead{};
                return HostClipboard::ImageRead{data->data(mime), false, 0};
            }
            default:
                return HostClipboard::ImageRead{};
            }
        },
        [](const QString &text, const QByteArray &html, bool secret) {
            // KSystemClipboard takes ownership: plain text and HTML in one offer, so a reader takes the richest it understands.
            auto *mime = new QMimeData();
            if (!text.isEmpty()) mime->setText(text);
            mime->setData(QStringLiteral("text/html"), html);
            if (secret) mime->setData(QStringLiteral("x-kde-passwordManagerHint"), QByteArrayLiteral("secret"));
            ownRich = mime;
            KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
            return true;
        }};
}
}
