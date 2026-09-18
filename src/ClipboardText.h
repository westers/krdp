// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>

namespace KRdp
{

/**
 * Line-ending conversion between the cliprdr wire and the host clipboard.
 *
 * CF_UNICODETEXT carries Windows line endings (CRLF) on the wire, while the
 * host clipboard is a Linux desktop that expects LF. Both directions leave a
 * CR that is not part of a CRLF pair alone. Pure string functions with no
 * FreeRDP or Qt GUI dependency so autotests/ClipboardTextTest.cpp can state
 * them without a display or a connection.
 */
namespace ClipboardText
{

/** Text received from the client, CRLF turned into LF for the host clipboard. */
inline QString toHost(QString text)
{
    return text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

/**
 * Host clipboard text as it goes on the wire: every LF that is not already
 * preceded by a CR gets one, so an existing CRLF stays a single CRLF.
 */
inline QString toWire(const QString &text)
{
    QString result;
    result.reserve(text.size() + text.count(QLatin1Char('\n')));
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n') && (i == 0 || text.at(i - 1) != QLatin1Char('\r'))) {
            result.append(QLatin1Char('\r'));
        }
        result.append(c);
    }
    return result;
}

}
}
