// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerpreferences.h"
#include "UserConfiguration.h"
#include <KLocalizedString>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace KRdp;
namespace {
const QString filename = u"farsideserverrc"_s;
QString ownDirectory()
{
    const auto path = UserConfiguration::userPath(getuid());
    return path ? QFileInfo(*path).absolutePath() : QString();
}
}
BrokerPreferences::BrokerPreferences(QObject *parent) : BrokerPreferences(ownDirectory(), parent) {}
BrokerPreferences::BrokerPreferences(const QString &directory, QObject *parent) : QObject(parent), m_directory(directory) {}

bool BrokerPreferences::reject(const QString &error) { m_error = error; Q_EMIT changed(); return false; }
BrokerPreferences::Read BrokerPreferences::read() const
{
    if (m_directory.isEmpty() || !getuid() || getuid() != geteuid()) return {{}, false, u"User preference identity is unavailable"_s};
    const auto path = m_directory + u"/"_s + filename;
    struct stat state{};
    if (::lstat(QFile::encodeName(path).constData(), &state)) {
        if (errno == ENOENT) return {{}, true, {}};
        return {{}, false, u"User preferences cannot be read"_s};
    }
    if (!S_ISREG(state.st_mode) || state.st_uid != getuid() || state.st_nlink != 1 || (state.st_mode & 0022))
        return {{}, false, u"User preference file is unsafe"_s};
    const auto contents = UserConfiguration::readFile(path, getuid());
    if (!contents) return {{}, false, u"User preferences are unreadable or changed during reading"_s};
    const auto result = BrokerUserSettings::parse(*contents);
    return result.error.isEmpty() ? Read{*contents, false, {}} : Read{{}, false, result.error};
}
void BrokerPreferences::adopt(const Read &read)
{
    m_document = read.document; m_absent = read.absent; m_loaded = true;
    const auto fields = BrokerUserSettings::fields(m_document);
    const auto parsed = BrokerUserSettings::parse(m_document);
    m_snapshot = m_pending = BrokerUserSettings::publicValues(parsed.preferences, fields);
    m_locked.clear();
    for (const auto &key : BrokerUserSettings::preferenceKeys())
        if (fields.immutableGroup || fields.immutable.contains(key)) m_locked.append(key);
    m_error.clear(); Q_EMIT changed();
}
bool BrokerPreferences::reload()
{
    const auto result = read();
    if (!result.error.isEmpty()) return reject(result.error);
    adopt(result); return true;
}
bool BrokerPreferences::setValue(const QString &key, const QString &value)
{
    if (!loaded() || !BrokerUserSettings::preferenceKeys().contains(key) || m_locked.contains(key)
        || value.size() > 256 || value.contains(QChar::Null) || value.contains(u'\n') || value.contains(u'\r'))
        return reject(u"Invalid or locked preference"_s);
    m_pending.insert(key, value); m_error.clear(); Q_EMIT changed(); return true;
}
bool BrokerPreferences::inherit(const QString &key)
{
    if (!loaded() || !BrokerUserSettings::preferenceKeys().contains(key) || m_locked.contains(key)) return reject(u"Invalid or locked preference"_s);
    m_pending.remove(key); m_error.clear(); Q_EMIT changed(); return true;
}
void BrokerPreferences::defaults()
{
    if (!loaded()) return;
    for (const auto &key : BrokerUserSettings::preferenceKeys()) if (!m_locked.contains(key)) m_pending.remove(key);
    m_error.clear(); Q_EMIT changed();
}
void BrokerPreferences::discard()
{
    if (!loaded()) return;
    m_pending = m_snapshot; m_error.clear(); Q_EMIT changed();
}
QString BrokerPreferences::error() const
{
    if (!m_error.isEmpty()) return m_error;
    return loaded() ? BrokerUserSettings::edit(m_document, m_pending).error : QString();
}
bool BrokerPreferences::canSave() const { return modified() && BrokerUserSettings::edit(m_document, m_pending).error.isEmpty(); }

bool BrokerPreferences::save()
{
    if (!loaded() || !modified()) return false;
    const auto candidate = BrokerUserSettings::edit(m_document, m_pending);
    if (!candidate.error.isEmpty()) return reject(candidate.error);
    // Resolve the account's actual config directory, including an existing
    // safe user-owned .config symlink supported by the production reader.
    if (!QDir(m_directory).exists() && ::mkdir(QFile::encodeName(m_directory).constData(), 0700) && errno != EEXIST)
        return reject(u"User preference directory cannot be created"_s);
    const int directory = ::open(QFile::encodeName(m_directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return reject(u"User preference directory is unavailable"_s);
    const auto closeDirectory = qScopeGuard([directory] { ::close(directory); });
    struct stat parent{};
    if (::fstat(directory, &parent) || parent.st_uid != getuid() || (parent.st_mode & 0022)) return reject(u"User preference directory is unsafe"_s);
    const int lock = ::openat(directory, ".farsideserverrc.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (lock < 0) return reject(u"User preference lock is unavailable"_s);
    const auto closeLock = qScopeGuard([lock] { ::close(lock); });
    struct stat lockState{};
    if (::fstat(lock, &lockState) || !S_ISREG(lockState.st_mode) || lockState.st_uid != getuid() || lockState.st_nlink != 1
        || (lockState.st_mode & 0077) || ::flock(lock, LOCK_EX | LOCK_NB)) return reject(u"User preferences are busy or their lock is unsafe"_s);
    const auto current = read();
    if (!current.error.isEmpty()) return reject(current.error);
    if (current.absent != m_absent || current.document != m_document) return reject(u"User preferences changed; reload before saving"_s);
    struct stat actualParent{};
    if (::stat(QFile::encodeName(m_directory).constData(), &actualParent) || actualParent.st_dev != parent.st_dev || actualParent.st_ino != parent.st_ino)
        return reject(u"User preference directory changed; reload before saving"_s);
    // Anchor the atomic write to the inspected directory, rather than following
    // a changing parent symlink a second time.
    QSaveFile file(u"/proc/self/fd/"_s + QString::number(directory) + u"/"_s + filename);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(candidate.document) != candidate.document.size() || !file.flush() || ::fsync(file.handle()) || !file.commit())
        return reject(u"User preferences could not be saved atomically"_s);
    m_reconnect = true;
    const bool durable = ::fsync(directory) == 0;
    const auto saved = read();
    if (!saved.error.isEmpty() || saved.document != candidate.document) return reject(u"Preferences saved but readback changed or failed; reload before reconnecting"_s);
    adopt(saved);
    if (!durable) return reject(u"Preferences saved but directory synchronization failed"_s);
    return true;
}

QVariantList BrokerPreferences::definitions() const
{
    QVariantList result;
    const auto choice = [](const QString &value, const QString &text) { return QVariantMap{{u"value"_s, value}, {u"text"_s, text}}; };
    const auto add = [&](const QString &key, const QString &group, const QString &label, const QString &help, const QVariantList &options = {}) {
        auto choices = options;
        if (!choices.isEmpty()) choices.prepend(choice({}, i18nc("@item:inlistbox", "Use host setting")));
        result.append(QVariantMap{{u"key"_s, key}, {u"group"_s, group}, {u"label"_s, label}, {u"help"_s, help}, {u"choices"_s, choices}});
    };
    const QVariantList boolean{choice(u"true"_s, i18nc("@item:inlistbox", "On")), choice(u"false"_s, i18nc("@item:inlistbox", "Off"))};
    const auto video = i18nc("@title:group", "Video");
    const auto displays = i18nc("@title:group", "Console Displays");
    const auto media = i18nc("@title:group", "Audio and Devices");
    const auto virtualDesktop = i18nc("@title:group", "Virtual Compatibility");
    add(u"Quality"_s, video, i18nc("@label", "Video quality"), i18nc("@info", "0–100. Higher values use more bandwidth."));
    add(u"AdaptiveQuality"_s, video, i18nc("@label", "Adapt quality to the connection"), i18nc("@info", "Allow quality to adjust to measured link capacity."), boolean);
    add(u"Codec"_s, video, i18nc("@label", "Video codec preference"), i18nc("@info", "Available encoders and client support determine the actual codec. Shared Console viewers use AVC420."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"avc420"_s, i18nc("@item:inlistbox", "AVC420")), choice(u"avc444"_s, i18nc("@item:inlistbox", "AVC444 full color"))});
    add(u"SoftwareEncoding"_s, video, i18nc("@label", "Software encoding"), i18nc("@info", "Automatic considers link capacity and CPU cost. Hardware preference can still use software AVC as a last resort."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"never"_s, i18nc("@item:inlistbox", "Prefer hardware")), choice(u"prefer"_s, i18nc("@item:inlistbox", "Allow the best codec in software"))});
    add(u"Av1Tiles"_s, video, i18nc("@label", "AV1 tiles"), i18nc("@info", "Automatic uses client decode capability. More tiles can help software decoding."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"1"_s, u"1"_s), choice(u"2"_s, u"2"_s), choice(u"4"_s, u"4"_s), choice(u"8"_s, u"8"_s), choice(u"16"_s, u"16"_s)});
    add(u"Avc444MotionGapMs"_s, video, i18nc("@label", "AVC444 color update interval during motion"), i18nc("@info", "16–5000 ms; must not exceed the rest interval."));
    add(u"Avc444RestMs"_s, video, i18nc("@label", "AVC444 rest interval"), i18nc("@info", "16–5000 ms; must be at least the motion interval."));
    add(u"Avc444MaxGapMs"_s, video, i18nc("@label", "AVC444 maximum color update gap"), i18nc("@info", "16–5000 ms; must be at least the rest interval."));
    add(u"PreferAudioQuality"_s, media, i18nc("@label", "Prefer audio quality"), i18nc("@info", "Prioritize audio quality when media is enabled."), boolean);
    add(u"StandardClientMedia"_s, media, i18nc("@label", "Standard client media"), i18nc("@info", "Requires host permission and channel consent. This does not grant microphone or camera access."), boolean);
    add(u"MonitorMode"_s, displays, i18nc("@label", "Share"), i18nc("@info", "Console capture selection. Client-created displays follow the separate layout and physical-display policy."),
        {choice(u"workspace"_s, i18nc("@item:inlistbox", "Whole workspace")), choice(u"primary"_s, i18nc("@item:inlistbox", "Primary display")), choice(u"specific"_s, i18nc("@item:inlistbox", "One display")),
         choice(u"multi"_s, i18nc("@item:inlistbox", "Displays as separate streams")), choice(u"virtual"_s, i18nc("@item:inlistbox", "Client-created displays"))});
    add(u"MonitorIndex"_s, displays, i18nc("@label", "Display index"), i18nc("@info", "0–65535, starting at zero. Used when sharing one display; it must exist in the current Console desktop."));
    add(u"VirtualMonitorPolicy"_s, displays, i18nc("@label", "Physical displays with client-created displays"), i18nc("@info", "Replace turns off physical displays during the connection; local reclaim restores them."),
        {choice(u"replace"_s, i18nc("@item:inlistbox", "Replace physical displays")), choice(u"extend"_s, i18nc("@item:inlistbox", "Keep physical displays"))});
    add(u"VirtualMonitorLayout"_s, displays, i18nc("@label", "Client-created display layout"), i18nc("@info", "Client layout needs the client's explicit monitor request. Physical layout mirrors native display pixels and scale."),
        {choice(u"client"_s, i18nc("@item:inlistbox", "Client monitors")), choice(u"single"_s, i18nc("@item:inlistbox", "One display")), choice(u"physical"_s, i18nc("@item:inlistbox", "Physical display layout"))});
    add(u"VirtualMonitorFallbackSize"_s, displays, i18nc("@label", "Fallback display size"), i18nc("@info", "Even WIDTHxHEIGHT, from 320x200 through 8192x8192. Used when client monitor data is unavailable."));
    add(u"WakeDisplayOnConnect"_s, i18nc("@title:group", "Session"), i18nc("@label", "Wake and keep displays awake"), i18nc("@info", "Applies to the streaming desktop in Console and Virtual, and releases when the final viewer leaves. It never unlocks the screen."), boolean);
    add(u"VirtualStockClientPolicy"_s, virtualDesktop, i18nc("@label", "Standard clients in Virtual"), i18nc("@info", "Controls clients without Farside session selection. Desktop ownership always follows the authenticated account."),
        {choice(u"attach-or-create"_s, i18nc("@item:inlistbox", "Attach to or create a desktop")), choice(u"refuse"_s, i18nc("@item:inlistbox", "Require session selection"))});
    return result;
}
