// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

namespace KRdp
{
/**
 * The one on-disk record of physical output changes that have to be undone
 * (AUD-C-3). Console Fit resize, the physical Console topology lease and
 * krdpserver's PhysicalOutputGuard all write here *before* they mutate a
 * physical output, and remove their entry once the original state is back.
 * Whatever process finds an entry whose owner has died replays it: the next
 * console worker before it reports Ready (so broker start and worker exit
 * both lead to a replay), and krdpserver at start or `--restore-outputs`.
 *
 * File: `$XDG_STATE_HOME/krdp/output-restore.json` of the desktop user
 * (`~/.local/state/krdp/output-restore.json`), overridable with
 * `KRDP_OUTPUT_RESTORE_JOURNAL`. Format (version 1):
 *
 *     {"format": "krdp-output-restore", "version": 1, "entries": [
 *       {"owner": "console-resize", "pid": 4242, "session": "3",
 *        "outputs": [{"name": "DP-1",
 *                     "original": {"mode": "1", "scale": 1.5},
 *                     "applied":  {"mode": "7", "scale": 1}}]}]}
 *
 * Fields (all optional): `enabled` (bool), `mode` (KScreen mode id), `scale`,
 * `x`/`y` (logical position), `priority`. An output with an `applied` object
 * is restored conditionally: only a field that still reads back as the
 * applied value is put back, so a later local change is never overwritten.
 * Without `applied` every original field is restored (PhysicalOutputGuard's
 * replace, which switched the outputs off).
 *
 * Every write is atomic: temporary file, fsync, rename, fsync of the
 * directory, under an exclusive flock on `<file>.lock`. A temporary file left
 * by a crash between write and rename is merged on the next update (never
 * silently dropped); an unparsable journal is kept as `<file>.corrupt-<time>`.
 */
class OutputRestoreJournal
{
public:
    static constexpr int Version = 1;
    static constexpr const char *ConsoleResizeOwner = "console-resize";
    static constexpr const char *ConsoleLeaseOwner = "console-lease";
    static constexpr const char *ConsoleVirtualOwner = "console-virtual-outputs";
    static constexpr const char *OutputGuardOwner = "physical-output-guard";

    struct Fields {
        std::optional<bool> enabled;
        QString mode;
        std::optional<double> scale;
        std::optional<QPoint> position;
        std::optional<int> priority;
        bool empty() const { return !enabled && mode.isEmpty() && !scale && !position && !priority; }
        bool operator==(const Fields &) const = default;
    };
    struct Output {
        QString name;
        Fields original;
        Fields applied; ///< Empty: restore `original` unconditionally.
        bool operator==(const Output &) const = default;
    };
    struct Entry {
        QString owner;
        qint64 pid = 0;
        QString session;
        QVector<Output> outputs;
        bool operator==(const Entry &) const = default;
    };
    /** One output as `kscreen-doctor -j` reports it now. */
    struct Current {
        QString name;
        bool enabled = false;
        QString mode;
        double scale = 1;
        QPoint position;
        int priority = 0;
    };

    using Read = std::function<std::optional<QByteArray>()>;
    using Apply = std::function<bool(const QStringList &arguments)>;
    using Alive = std::function<bool(qint64 pid)>;

    struct Plan {
        QStringList arguments; ///< One kscreen-doctor batch; empty when nothing of ours is left.
        QStringList missing; ///< Journaled outputs that are not connected now.
    };
    struct ReplayResult {
        int restored = 0; ///< Entries whose outputs are back (or were changed by someone else since).
        int kept = 0; ///< Entries left for a later replay.
        int skippedLive = 0; ///< Entries of a still-running owner.
        QStringList errors;
    };

    explicit OutputRestoreJournal(QString path = defaultPath());
    static QString defaultPath();
    QString path() const { return m_path; }

    /** Add or replace the entry of (`entry.owner`, `entry.pid`); an entry without outputs removes it. */
    bool hold(const Entry &entry, QString *error = nullptr);
    /** Remove the entry of (`owner`, `pid`). */
    bool release(const QString &owner, qint64 pid, QString *error = nullptr);
    /** Current entries, including any left in an interrupted temporary file. */
    QVector<Entry> entries(QString *error = nullptr) const;
    /**
     * Restore every entry accepted by `filter` whose owner is not alive,
     * one entry at a time: read, apply the conditional batch, read back.
     * Verified (or superseded) entries are removed; an unconditional entry
     * whose outputs are not all connected is set aside in `<file>.stale`,
     * a conditional one is kept for the next replay.
     */
    ReplayResult replay(const Read &read, const Apply &apply, const Alive &alive, const std::function<bool(const Entry &)> &filter = {});

    static QVector<Current> parseCurrent(const QByteArray &kscreenJson, bool *ok = nullptr);
    static Plan plan(const Entry &entry, const QVector<Current> &current);
    /** Every field `plan()` chose to restore now reads back as original. */
    static bool verified(const Entry &entry, const QVector<Current> &before, const QVector<Current> &after);
    /** A live process other than this one, named krdp* when /proc can tell (pid reuse). */
    static bool ownerAlive(qint64 pid);

    static QByteArray serialize(const QVector<Entry> &entries);
    static std::optional<QVector<Entry>> parse(const QByteArray &json);

private:
    bool update(const std::function<void(QVector<Entry> &)> &mutate, QString *error);
    bool load(QVector<Entry> *entries, QString *error, bool repair) const;
    bool writeAtomically(const QByteArray &data, QString *error) const;

    QString m_path;
};
}
