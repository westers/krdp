// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "OutputRestoreJournal.h"

#include <cerrno>
#include <cmath>
#include <csignal>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace KRdp
{
namespace
{
const QLatin1String Format("krdp-output-restore");

bool sameScale(double a, double b)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 0.000001;
}

QJsonObject fieldsToJson(const OutputRestoreJournal::Fields &fields)
{
    QJsonObject object;
    if (fields.enabled) object.insert(QLatin1String("enabled"), *fields.enabled);
    if (!fields.mode.isEmpty()) object.insert(QLatin1String("mode"), fields.mode);
    if (fields.scale) object.insert(QLatin1String("scale"), *fields.scale);
    if (fields.position) {
        object.insert(QLatin1String("x"), fields.position->x());
        object.insert(QLatin1String("y"), fields.position->y());
    }
    if (fields.priority) object.insert(QLatin1String("priority"), *fields.priority);
    return object;
}

bool safeName(const QString &value)
{
    if (value.isEmpty() || value.size() > 128) return false;
    for (const QChar c : value) {
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != u'-' && c != u'_' && c != u'.') return false;
    }
    return true;
}

std::optional<OutputRestoreJournal::Fields> fieldsFromJson(const QJsonValue &value)
{
    if (value.isUndefined()) return OutputRestoreJournal::Fields{};
    if (!value.isObject()) return std::nullopt;
    const auto object = value.toObject();
    OutputRestoreJournal::Fields fields;
    if (object.contains(QLatin1String("enabled"))) {
        if (!object.value(QLatin1String("enabled")).isBool()) return std::nullopt;
        fields.enabled = object.value(QLatin1String("enabled")).toBool();
    }
    if (object.contains(QLatin1String("mode"))) {
        fields.mode = object.value(QLatin1String("mode")).toString();
        if (!safeName(fields.mode)) return std::nullopt;
    }
    if (object.contains(QLatin1String("scale"))) {
        const double scale = object.value(QLatin1String("scale")).toDouble(-1);
        if (!std::isfinite(scale) || scale <= 0 || scale > 8) return std::nullopt;
        fields.scale = scale;
    }
    if (object.contains(QLatin1String("x")) != object.contains(QLatin1String("y"))) return std::nullopt;
    if (object.contains(QLatin1String("x"))) {
        fields.position = QPoint(object.value(QLatin1String("x")).toInt(), object.value(QLatin1String("y")).toInt());
    }
    if (object.contains(QLatin1String("priority"))) {
        const int priority = object.value(QLatin1String("priority")).toInt(-1);
        if (priority < 0 || priority > 64) return std::nullopt;
        fields.priority = priority;
    }
    return fields;
}

enum class Field { Enabled, Mode, Scale, Position, Priority };

/** What plan() restores: shared by the argument builder and verification. */
struct Restoration {
    QString output;
    Field field;
};

const OutputRestoreJournal::Current *findCurrent(const QVector<OutputRestoreJournal::Current> &current, const QString &name)
{
    for (const auto &output : current) {
        if (output.name == name) return &output;
    }
    return nullptr;
}

QVector<Restoration> restorations(const OutputRestoreJournal::Entry &entry, const QVector<OutputRestoreJournal::Current> &current, QStringList *missing)
{
    // KDE priorities are one ordering: put ours back only when every
    // priority we changed is still exactly as we left it.
    bool prioritiesOurs = true;
    for (const auto &output : entry.outputs) {
        const auto *now = findCurrent(current, output.name);
        if (now && !output.applied.empty() && output.applied.priority && output.original.priority && now->priority != *output.applied.priority) {
            prioritiesOurs = false;
        }
    }
    QVector<Restoration> result;
    for (const auto &output : entry.outputs) {
        const auto *now = findCurrent(current, output.name);
        if (!now) {
            if (missing) missing->append(output.name);
            continue;
        }
        const bool unconditional = output.applied.empty();
        const auto &original = output.original;
        const auto &applied = output.applied;
        if (original.enabled
            && (unconditional || (applied.enabled && now->enabled == *applied.enabled))
            && now->enabled != *original.enabled) {
            result.append({output.name, Field::Enabled});
        }
        if (original.enabled && !*original.enabled) {
            continue; // Staying (or going) off: nothing else to restore.
        }
        if (!original.mode.isEmpty() && (unconditional || applied.mode == now->mode) && now->mode != original.mode) {
            result.append({output.name, Field::Mode});
        }
        if (original.scale && (unconditional || (applied.scale && sameScale(*applied.scale, now->scale))) && !sameScale(*original.scale, now->scale)) {
            result.append({output.name, Field::Scale});
        }
        if (original.position && (unconditional || applied.position == now->position) && *original.position != now->position) {
            result.append({output.name, Field::Position});
        }
        if (original.priority && (unconditional || (applied.priority && prioritiesOurs)) && *original.priority != now->priority) {
            result.append({output.name, Field::Priority});
        }
    }
    return result;
}

const OutputRestoreJournal::Output *findOutput(const OutputRestoreJournal::Entry &entry, const QString &name)
{
    for (const auto &output : entry.outputs) {
        if (output.name == name) return &output;
    }
    return nullptr;
}

bool fsyncDirectory(const QString &directory)
{
    const int fd = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

class LockFile
{
public:
    LockFile(const QString &path, int operation)
    {
        m_fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd >= 0 && ::flock(m_fd, operation) != 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }
    ~LockFile()
    {
        if (m_fd >= 0) ::close(m_fd); // Releases the flock.
    }
    bool locked() const { return m_fd >= 0; }

private:
    int m_fd = -1;
};

bool sameKey(const OutputRestoreJournal::Entry &a, const OutputRestoreJournal::Entry &b)
{
    return a.owner == b.owner && a.pid == b.pid;
}
}

OutputRestoreJournal::OutputRestoreJournal(QString path)
    : m_path(std::move(path))
{
}

QString OutputRestoreJournal::defaultPath()
{
    const QString overridden = qEnvironmentVariable("KRDP_OUTPUT_RESTORE_JOURNAL");
    if (!overridden.isEmpty()) return overridden;
    // One path for every krdp process of this user, whatever its app name.
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty() || !QDir::isAbsolutePath(state)) state = QDir::homePath() + QLatin1String("/.local/state");
    return state + QLatin1String("/krdp/output-restore.json");
}

QByteArray OutputRestoreJournal::serialize(const QVector<Entry> &entries)
{
    QJsonArray array;
    for (const auto &entry : entries) {
        QJsonArray outputs;
        for (const auto &output : entry.outputs) {
            QJsonObject object{{QLatin1String("name"), output.name}, {QLatin1String("original"), fieldsToJson(output.original)}};
            if (!output.applied.empty()) object.insert(QLatin1String("applied"), fieldsToJson(output.applied));
            outputs.append(object);
        }
        array.append(QJsonObject{{QLatin1String("owner"), entry.owner},
                                 {QLatin1String("pid"), entry.pid},
                                 {QLatin1String("session"), entry.session},
                                 {QLatin1String("outputs"), outputs}});
    }
    return QJsonDocument(QJsonObject{{QLatin1String("format"), Format}, {QLatin1String("version"), Version}, {QLatin1String("entries"), array}})
        .toJson(QJsonDocument::Indented);
}

std::optional<QVector<OutputRestoreJournal::Entry>> OutputRestoreJournal::parse(const QByteArray &json)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    const auto root = document.object();
    if (root.value(QLatin1String("format")).toString() != Format || root.value(QLatin1String("version")).toInt() != Version
        || !root.value(QLatin1String("entries")).isArray()) {
        return std::nullopt;
    }
    QVector<Entry> entries;
    for (const auto &value : root.value(QLatin1String("entries")).toArray()) {
        const auto object = value.toObject();
        Entry entry;
        entry.owner = object.value(QLatin1String("owner")).toString();
        entry.pid = object.value(QLatin1String("pid")).toInteger(0);
        entry.session = object.value(QLatin1String("session")).toString();
        if (!safeName(entry.owner) || !object.value(QLatin1String("outputs")).isArray()) return std::nullopt;
        for (const auto &outputValue : object.value(QLatin1String("outputs")).toArray()) {
            const auto outputObject = outputValue.toObject();
            Output output;
            output.name = outputObject.value(QLatin1String("name")).toString();
            const auto original = fieldsFromJson(outputObject.value(QLatin1String("original")));
            const auto applied = fieldsFromJson(outputObject.value(QLatin1String("applied")));
            if (!safeName(output.name) || !original || !applied || original->empty()) return std::nullopt;
            output.original = *original;
            output.applied = *applied;
            entry.outputs.append(output);
        }
        if (!entry.outputs.isEmpty()) entries.append(entry);
    }
    return entries;
}

bool OutputRestoreJournal::load(QVector<Entry> *entries, QString *error, bool repair) const
{
    entries->clear();
    QFile main(m_path);
    if (main.exists()) {
        if (!main.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("cannot read %1: %2").arg(m_path, main.errorString());
            return false;
        }
        const auto parsed = parse(main.readAll());
        main.close();
        if (!parsed) {
            const QString corrupt = m_path + QStringLiteral(".corrupt-%1").arg(QDateTime::currentMSecsSinceEpoch());
            if (!repair) {
                if (error) *error = QStringLiteral("%1 is not a valid output-restore journal").arg(m_path);
                return false;
            }
            // Never forget that something may need restoring: keep the
            // evidence under another name and say how to recover by hand.
            QFile::rename(m_path, corrupt);
            qCritical().noquote() << "Output-restore journal" << m_path << "is unreadable; kept as" << corrupt
                                  << "- check the monitors with kscreen-doctor -o";
        } else {
            *entries = *parsed;
        }
    }
    const QString temporary = m_path + QStringLiteral(".tmp");
    QFile leftover(temporary);
    if (leftover.exists() && leftover.open(QIODevice::ReadOnly)) {
        // A crash between writing the temporary file and renaming it. If it
        // is complete, it holds the newest intent: merge what it adds. An
        // incomplete one is a torn write that never became the journal.
        const auto parsed = parse(leftover.readAll());
        leftover.close();
        if (parsed) {
            for (const auto &entry : *parsed) {
                if (std::none_of(entries->cbegin(), entries->cend(), [&entry](const Entry &existing) { return sameKey(existing, entry); })) {
                    entries->append(entry);
                }
            }
            qWarning().noquote() << "Output-restore journal: merged an interrupted write from" << temporary;
        } else {
            qWarning().noquote() << "Output-restore journal: discarding torn write" << temporary;
        }
        if (repair) QFile::remove(temporary);
    }
    return true;
}

bool OutputRestoreJournal::writeAtomically(const QByteArray &data, QString *error) const
{
    const QString directory = QFileInfo(m_path).absolutePath();
    const QString temporary = m_path + QStringLiteral(".tmp");
    if (data.isEmpty()) {
        if (QFile::exists(m_path) && !QFile::remove(m_path)) {
            if (error) *error = QStringLiteral("cannot remove %1").arg(m_path);
            return false;
        }
        fsyncDirectory(directory);
        return true;
    }
    const int fd = ::open(QFile::encodeName(temporary).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error) *error = QStringLiteral("cannot create %1: %2").arg(temporary, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    qsizetype written = 0;
    while (written < data.size()) {
        const ssize_t chunk = ::write(fd, data.constData() + written, size_t(data.size() - written));
        if (chunk < 0 && errno == EINTR) continue;
        if (chunk <= 0) break;
        written += chunk;
    }
    const bool synced = written == data.size() && ::fsync(fd) == 0;
    ::close(fd);
    if (!synced || ::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(m_path).constData()) != 0) {
        if (error) *error = QStringLiteral("cannot write %1 atomically: %2").arg(m_path, QString::fromLocal8Bit(strerror(errno)));
        ::unlink(QFile::encodeName(temporary).constData());
        return false;
    }
    fsyncDirectory(directory);
    return true;
}

bool OutputRestoreJournal::update(const std::function<void(QVector<Entry> &)> &mutate, QString *error)
{
    const QString directory = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(directory);
        return false;
    }
    LockFile lock(m_path + QStringLiteral(".lock"), LOCK_EX);
    if (!lock.locked()) {
        if (error) *error = QStringLiteral("cannot lock %1").arg(m_path);
        return false;
    }
    QVector<Entry> entries;
    if (!load(&entries, error, true)) return false;
    mutate(entries);
    return writeAtomically(entries.isEmpty() ? QByteArray() : serialize(entries), error);
}

bool OutputRestoreJournal::hold(const Entry &entry, QString *error)
{
    if (entry.outputs.isEmpty()) return release(entry.owner, entry.pid, error);
    return update([&entry](QVector<Entry> &entries) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&entry](const Entry &existing) { return sameKey(existing, entry); });
        if (found != entries.end()) *found = entry;
        else entries.append(entry);
    }, error);
}

bool OutputRestoreJournal::release(const QString &owner, qint64 pid, QString *error)
{
    if (!QFile::exists(m_path) && !QFile::exists(m_path + QStringLiteral(".tmp"))) return true;
    return update([&owner, pid](QVector<Entry> &entries) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&owner, pid](const Entry &entry) {
            return entry.owner == owner && entry.pid == pid;
        }), entries.end());
    }, error);
}

QVector<OutputRestoreJournal::Entry> OutputRestoreJournal::entries(QString *error) const
{
    QVector<Entry> result;
    if (!QFile::exists(m_path) && !QFile::exists(m_path + QStringLiteral(".tmp"))) return result;
    LockFile lock(m_path + QStringLiteral(".lock"), LOCK_SH);
    load(&result, error, false);
    return result;
}

QVector<OutputRestoreJournal::Current> OutputRestoreJournal::parseCurrent(const QByteArray &kscreenJson, bool *ok)
{
    const auto document = QJsonDocument::fromJson(kscreenJson);
    const bool valid = document.isObject() && document.object().value(QLatin1String("outputs")).isArray();
    if (ok) *ok = valid;
    QVector<Current> outputs;
    if (!valid) return outputs;
    for (const auto &value : document.object().value(QLatin1String("outputs")).toArray()) {
        const auto object = value.toObject();
        if (!object.value(QLatin1String("connected")).toBool()) continue;
        const auto position = object.value(QLatin1String("pos")).toObject();
        outputs.append({object.value(QLatin1String("name")).toString(),
                        object.value(QLatin1String("enabled")).toBool(),
                        object.value(QLatin1String("currentModeId")).toString(),
                        object.value(QLatin1String("scale")).toDouble(1),
                        QPoint(position.value(QLatin1String("x")).toInt(), position.value(QLatin1String("y")).toInt()),
                        object.value(QLatin1String("priority")).toInt()});
    }
    return outputs;
}

OutputRestoreJournal::Plan OutputRestoreJournal::plan(const Entry &entry, const QVector<Current> &current)
{
    Plan result;
    for (const auto &restoration : restorations(entry, current, &result.missing)) {
        const auto *output = findOutput(entry, restoration.output);
        const auto &original = output->original;
        const QString prefix = QStringLiteral("output.%1.").arg(restoration.output);
        switch (restoration.field) {
        case Field::Enabled:
            result.arguments.append(prefix + (*original.enabled ? QStringLiteral("enable") : QStringLiteral("disable")));
            break;
        case Field::Mode:
            result.arguments.append(prefix + QStringLiteral("mode.") + original.mode);
            break;
        case Field::Scale:
            result.arguments.append(prefix + QStringLiteral("scale.") + QString::number(*original.scale, 'g', 12));
            break;
        case Field::Position:
            result.arguments.append(prefix + QStringLiteral("position.%1,%2").arg(original.position->x()).arg(original.position->y()));
            break;
        case Field::Priority:
            result.arguments.append(prefix + QStringLiteral("priority.%1").arg(*original.priority));
            break;
        }
    }
    return result;
}

bool OutputRestoreJournal::verified(const Entry &entry, const QVector<Current> &before, const QVector<Current> &after)
{
    for (const auto &restoration : restorations(entry, before, nullptr)) {
        const auto *output = findOutput(entry, restoration.output);
        const auto *now = findCurrent(after, restoration.output);
        if (!now) return false;
        const auto &original = output->original;
        switch (restoration.field) {
        case Field::Enabled:
            if (now->enabled != *original.enabled) return false;
            break;
        case Field::Mode:
            if (now->mode != original.mode) return false;
            break;
        case Field::Scale:
            if (!sameScale(now->scale, *original.scale)) return false;
            break;
        case Field::Position:
            if (now->position != *original.position) return false;
            break;
        case Field::Priority:
            if (now->priority != *original.priority) return false;
            break;
        }
    }
    return true;
}

bool OutputRestoreJournal::ownerAlive(qint64 pid)
{
    if (pid <= 0 || pid == qint64(getpid())) return false;
    if (::kill(pid_t(pid), 0) != 0 && errno != EPERM) return false;
    // The PID may have been reused since the crash; when /proc can name the
    // process, only a krdp process counts.
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    if (comm.open(QIODevice::ReadOnly)) {
        const QByteArray name = comm.readAll().trimmed();
        return name.isEmpty() || name.startsWith("krdp");
    }
    return true;
}

OutputRestoreJournal::ReplayResult OutputRestoreJournal::replay(const Read &read, const Apply &apply, const Alive &alive,
                                                                const std::function<bool(const Entry &)> &filter)
{
    ReplayResult result;
    QString error;
    if (!QFile::exists(m_path) && !QFile::exists(m_path + QStringLiteral(".tmp"))) {
        return result; // The common case: nothing was ever left behind.
    }
    // Repair (merge a torn write, set aside a corrupt file) under the lock
    // first; the restore commands themselves run without holding it.
    if (!update([](QVector<Entry> &) { }, &error) && !error.isEmpty()) result.errors.append(error);
    for (const auto &entry : entries(&error)) {
        if (filter && !filter(entry)) continue;
        if (alive && alive(entry.pid)) {
            ++result.skippedLive;
            continue;
        }
        const auto beforeJson = read();
        bool parsed = false;
        const auto before = beforeJson ? parseCurrent(*beforeJson, &parsed) : QVector<Current>{};
        if (!parsed) {
            result.errors.append(QStringLiteral("cannot read the current outputs"));
            ++result.kept;
            continue;
        }
        const auto restore = plan(entry, before);
        bool ok = true;
        if (!restore.arguments.isEmpty()) {
            qWarning().noquote() << "Restoring outputs left by" << entry.owner << "PID" << entry.pid << ":" << restore.arguments.join(u' ');
            apply(restore.arguments); // Exit status is diagnostic only; the readback decides.
            const auto afterJson = read();
            bool afterParsed = false;
            const auto after = afterJson ? parseCurrent(*afterJson, &afterParsed) : QVector<Current>{};
            ok = afterParsed && verified(entry, before, after);
        }
        const bool unconditional = std::any_of(entry.outputs.cbegin(), entry.outputs.cend(), [](const Output &output) { return output.applied.empty(); });
        if (!ok) {
            result.errors.append(QStringLiteral("%1 PID %2: restore not verified").arg(entry.owner).arg(entry.pid));
            ++result.kept;
            continue;
        }
        if (!restore.missing.isEmpty() && !unconditional) {
            ++result.kept; // Conditional: harmless to retry when the monitor is back.
            continue;
        }
        if (!restore.missing.isEmpty()) {
            // An unconditional snapshot cannot be finished without the missing
            // outputs; do not replay it at every start. Keep it for a human.
            const QString stale = m_path + QStringLiteral(".stale");
            QVector<Entry> kept;
            if (QFile::exists(stale)) {
                QFile file(stale);
                if (file.open(QIODevice::ReadOnly)) kept = parse(file.readAll()).value_or(QVector<Entry>{});
            }
            kept.append(entry);
            OutputRestoreJournal(stale).writeAtomically(serialize(kept), nullptr);
            qCritical().noquote() << "Outputs" << restore.missing.join(QStringLiteral(", ")) << "of" << entry.owner << "are not connected; entry set aside in" << stale;
        }
        if (release(entry.owner, entry.pid, &error)) ++result.restored;
        else result.errors.append(error);
    }
    return result;
}
}
