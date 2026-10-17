// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QMimeData>
#include <QString>

namespace KRdp
{
/**
 * OPT-077 fix: the QMimeData a client's rich copy becomes on the host: the sender's own plain text as text/plain (never derived
 * from the HTML) next to the HTML document as text/html. KSystemClipboard offers the charset variant of text/plain itself
 * (measured on KWin 6.6 with LibreOffice 26.2: the source lists text/plain, text/html, text/plain;charset=utf-8, and
 * Paste Special -> Unformatted text asks for text/plain;charset=utf-8 and gets exactly the sender's text). With no text the
 * offer is the HTML alone; with no HTML, the text alone. A write made on the login screen or a locked session asks Klipper to
 * keep it out of its history. The caller owns the result (KSystemClipboard::setMimeData takes ownership).
 */
inline QMimeData *buildClipboardMime(const QString &text, const QByteArray &html, bool secret)
{
    auto *mime = new QMimeData();
    if (!text.isEmpty()) {
        mime->setText(text);
    }
    if (!html.isEmpty()) {
        mime->setData(QStringLiteral("text/html"), html);
    }
    if (secret) {
        mime->setData(QStringLiteral("x-kde-passwordManagerHint"), QByteArrayLiteral("secret"));
    }
    return mime;
}
}
