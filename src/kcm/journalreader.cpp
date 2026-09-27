// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "journalreader.h"

#include "krdpkcm_logging.h"

#include <QScopeGuard>

#include <algorithm>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

QStringList newestLinesInOrder(const QStringList &newestFirst, int maxLines)
{
    QStringList lines = newestFirst.mid(0, std::max(0, maxLines));
    std::reverse(lines.begin(), lines.end());
    return lines;
}

#ifdef HAVE_SYSTEMD
namespace
{
QString journalValue(sd_journal *journal, const char *field)
{
    size_t length = 0;
    const void *data = nullptr;
    if (sd_journal_get_data(journal, field, &data, &length) != 0 || length > size_t(std::numeric_limits<qsizetype>::max())) {
        return {};
    }
    return QString::fromUtf8(static_cast<const char *>(data), qsizetype(length)).section(u'=', 1);
}

bool addMatch(sd_journal *journal, const QString &match)
{
    const auto bytes = match.toUtf8();
    const int rc = sd_journal_add_match(journal, bytes.constData(), 0);
    if (rc != 0) {
        qCWarning(KRDPKCM) << "Failed to add journal match" << match << rc;
    }
    return rc == 0;
}
}

QStringList readServiceJournal(const QString &unit, const QString &invocationId, int maxLines)
{
    sd_journal *journal = nullptr;
    // Captures by reference: the pointer is only set by sd_journal_open() below.
    auto cleanup = qScopeGuard([&journal] {
        if (journal) {
            sd_journal_close(journal);
        }
    });

    if (sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_CURRENT_USER) != 0) {
        qCWarning(KRDPKCM) << "Failed to open the journal to read the error messages.";
        return {};
    }

    // (USER_UNIT=unit OR _SYSTEMD_USER_UNIT=unit) AND _SYSTEMD_INVOCATION_ID AND SYSLOG_IDENTIFIER
    if (!addMatch(journal, QStringLiteral("USER_UNIT=%1").arg(unit)) || sd_journal_add_disjunction(journal) != 0
        || !addMatch(journal, QStringLiteral("_SYSTEMD_USER_UNIT=%1").arg(unit)) || sd_journal_add_conjunction(journal) != 0
        || !addMatch(journal, QStringLiteral("_SYSTEMD_INVOCATION_ID=%1").arg(invocationId)) || sd_journal_add_conjunction(journal) != 0
        || !addMatch(journal, QStringLiteral("SYSLOG_IDENTIFIER=krdpserver"))) {
        return {};
    }

    if (sd_journal_seek_tail(journal) != 0) {
        qCWarning(KRDPKCM) << "Could not seek the tail of the journal";
        return {};
    }

    QStringList newestFirst;
    while (newestFirst.size() < maxLines && sd_journal_previous(journal) > 0) {
        newestFirst << journalValue(journal, "MESSAGE");
    }
    return newestLinesInOrder(newestFirst, maxLines);
}
#else
QStringList readServiceJournal(const QString &unit, const QString &invocationId, int maxLines)
{
    Q_UNUSED(unit);
    Q_UNUSED(invocationId);
    Q_UNUSED(maxLines);
    return {};
}
#endif
