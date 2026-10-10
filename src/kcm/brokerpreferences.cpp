// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerpreferences.h"
#include "settingfielddefinition.h"
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
        return reject(i18nc("@info", "That preference cannot be changed."));
    m_pending.insert(key, value); m_error.clear(); Q_EMIT changed(); return true;
}
bool BrokerPreferences::inherit(const QString &key)
{
    if (!loaded() || !BrokerUserSettings::preferenceKeys().contains(key) || m_locked.contains(key)) return reject(i18nc("@info", "That preference cannot be changed."));
    m_pending.remove(key); m_error.clear(); Q_EMIT changed(); return true;
}
void BrokerPreferences::defaults()
{
    if (!loaded()) return;
    for (const auto &key : BrokerUserSettings::preferenceKeys()) if (!m_locked.contains(key)) m_pending.remove(key);
    m_error.clear(); Q_EMIT changed();
}
bool BrokerPreferences::representsDefaults() const
{
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) if (!m_locked.contains(it.key())) return false;
    return true;
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
        return reject(i18nc("@info", "Your preferences folder could not be created."));
    const int directory = ::open(QFile::encodeName(m_directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return reject(i18nc("@info", "Your preferences folder is not available."));
    const auto closeDirectory = qScopeGuard([directory] { ::close(directory); });
    struct stat parent{};
    if (::fstat(directory, &parent) || parent.st_uid != getuid() || (parent.st_mode & 0022)) return reject(i18nc("@info", "Your preferences folder has unsafe permissions."));
    const int lock = ::openat(directory, ".farsideserverrc.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (lock < 0) return reject(i18nc("@info", "Your preferences are locked and cannot be saved right now."));
    const auto closeLock = qScopeGuard([lock] { ::close(lock); });
    struct stat lockState{};
    if (::fstat(lock, &lockState) || !S_ISREG(lockState.st_mode) || lockState.st_uid != getuid() || lockState.st_nlink != 1
        || (lockState.st_mode & 0077) || ::flock(lock, LOCK_EX | LOCK_NB)) return reject(i18nc("@info", "Your preferences are in use by another program or their lock file is unsafe."));
    const auto current = read();
    if (!current.error.isEmpty()) return reject(current.error);
    if (current.absent != m_absent || current.document != m_document) return reject(i18nc("@info", "Your preferences changed elsewhere. Reset the page, then try again."));
    struct stat actualParent{};
    if (::stat(QFile::encodeName(m_directory).constData(), &actualParent) || actualParent.st_dev != parent.st_dev || actualParent.st_ino != parent.st_ino)
        return reject(i18nc("@info", "Your preferences folder changed. Reset the page, then try again."));
    // Anchor the atomic write to the inspected directory, rather than following
    // a changing parent symlink a second time.
    QSaveFile file(u"/proc/self/fd/"_s + QString::number(directory) + u"/"_s + filename);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(candidate.document) != candidate.document.size() || !file.flush() || ::fsync(file.handle()) || !file.commit())
        return reject(i18nc("@info", "Your preferences could not be saved."));
    m_reconnect = true;
    const bool durable = ::fsync(directory) == 0;
    const auto saved = read();
    if (!saved.error.isEmpty() || saved.document != candidate.document) return reject(i18nc("@info", "Your preferences were saved, but reading them back failed. Reset the page before reconnecting."));
    adopt(saved);
    if (!durable) return reject(i18nc("@info", "Your preferences were saved, but the folder could not be flushed to disk."));
    return true;
}

QString BrokerPreferences::sectionTitle(const QString &section) const
{
    if (section == u"video") return i18nc("@title:group", "Video");
    if (section == u"displays") return i18nc("@title:group", "Console Displays");
    if (section == u"sound") return i18nc("@title:group", "Sound and Session");
    if (section == u"advanced") return i18nc("@title:group", "Encoding and Compatibility");
    return {};
}

QVariantList BrokerPreferences::definitions() const
{
    using namespace KRdp::SettingFields;
    QVariantList result;
    const auto choice = [](const QString &value, const QString &text) { return QVariantMap{{u"value"_s, value}, {u"text"_s, text}}; };
    const auto add = [&](const QString &key, const QString &group, const QString &label, const QString &help, const QVariantList &options, const Spec &spec) {
        auto choices = options;
        if (!choices.isEmpty()) choices.prepend(choice({}, i18nc("@item:inlistbox", "Use host setting")));
        auto withScope = spec;
        withScope.inheritText = i18nc("@item:inlistbox", "Use host setting");
        result.append(makeFieldDefinition(key, group, label, help, choices, withScope));
    };
    const QVariantList boolean{choice(u"true"_s, i18nc("@item:inlistbox", "On")), choice(u"false"_s, i18nc("@item:inlistbox", "Off"))};
    const auto video = i18nc("@title:group", "Video");
    const auto displays = i18nc("@title:group", "Console Displays");
    const auto media = i18nc("@title:group", "Audio and Devices");
    const auto virtualDesktop = i18nc("@title:group", "Virtual Compatibility");
    const auto when = [](Spec spec, const QString &mode) { spec.showWhenKey = u"MonitorMode"_s; spec.showWhenValue = mode; return spec; };
    const auto interval = [](const QString &formLabel, const QString &seed) { return Spec{.control = u"spin"_s, .section = u"video"_s, .advanced = true, .formLabel = formLabel, .min = 16, .max = 5000, .unit = i18nc("@label", "ms"), .customSeed = seed}; };
    add(u"Quality"_s, video, i18nc("@label", "Video quality"), i18nc("@info", "From 0 to 100. Higher values look sharper and use more bandwidth."), {},
        {.control = u"slider"_s, .section = u"video"_s, .formLabel = i18nc("@label", "Image quality"), .min = 0, .max = 100, .customSeed = u"80"_s});
    add(u"AdaptiveQuality"_s, video, i18nc("@label", "Adapt quality to the connection"), i18nc("@info", "Lower the quality automatically when the connection cannot keep up."), boolean,
        {.section = u"video"_s, .formLabel = i18nc("@label", "Adjust to connection")});
    add(u"Codec"_s, video, i18nc("@label", "Video codec preference"), i18nc("@info", "Chooses how colors are sent. Automatic uses full color (AVC444) when the client and the connection allow it. The available encoders and the client still decide; shared Console viewers always get standard color (AVC420)."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"avc420"_s, i18nc("@item:inlistbox", "AVC420")), choice(u"avc444"_s, i18nc("@item:inlistbox", "AVC444 full color"))},
        {.section = u"video"_s, .formLabel = i18nc("@label", "Color detail"),
         .optionText = {{u"avc420"_s, i18nc("@item:inlistbox", "Standard color")}, {u"avc444"_s, i18nc("@item:inlistbox", "Full color")}}});
    add(u"SoftwareEncoding"_s, video, i18nc("@label", "Software encoding"), i18nc("@info", "Automatic weighs the connection speed against the processor load. “Prefer hardware” falls back to software only as a last resort. Limited by what the host allows: this can narrow the host's software encoding, never widen it."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"never"_s, i18nc("@item:inlistbox", "Prefer hardware")), choice(u"prefer"_s, i18nc("@item:inlistbox", "Allow the best codec in software"))},
        {.section = u"video"_s, .advanced = true, .formLabel = i18nc("@label", "Encoding policy")});
    add(u"Av1Tiles"_s, video, i18nc("@label", "AV1 tiles"), i18nc("@info", "Automatic uses what the client can decode. More tiles can help clients that decode in software."),
        {choice(u"auto"_s, i18nc("@item:inlistbox", "Automatic")), choice(u"1"_s, u"1"_s), choice(u"2"_s, u"2"_s), choice(u"4"_s, u"4"_s), choice(u"8"_s, u"8"_s), choice(u"16"_s, u"16"_s)},
        {.section = u"video"_s, .advanced = true});
    add(u"Avc444MotionGapMs"_s, video, i18nc("@label", "AVC444 color update interval during motion"), i18nc("@info", "From 16 to 5000 ms. Must not be longer than the rest interval."), {},
        interval(i18nc("@label", "AVC444 motion interval"), u"100"_s));
    add(u"Avc444RestMs"_s, video, i18nc("@label", "AVC444 rest interval"), i18nc("@info", "From 16 to 5000 ms. Must not be shorter than the motion interval."), {},
        interval(i18nc("@label", "AVC444 rest interval"), u"150"_s));
    add(u"Avc444MaxGapMs"_s, video, i18nc("@label", "AVC444 maximum color update gap"), i18nc("@info", "From 16 to 5000 ms. Must not be shorter than the rest interval."), {},
        interval(i18nc("@label", "AVC444 maximum interval"), u"1500"_s));
    add(u"PreferAudioQuality"_s, media, i18nc("@label", "Prefer audio quality"), i18nc("@info", "When the network is busy, favor sound over video."), boolean,
        {.section = u"sound"_s, .formLabel = i18nc("@label", "When network is busy"),
         .optionText = {{u"true"_s, i18nc("@item:inlistbox", "Keep sound smooth")}, {u"false"_s, i18nc("@item:inlistbox", "Keep video sharp")}}});
    add(u"StandardClientMedia"_s, media, i18nc("@label", "Media for other RDP apps"), i18nc("@info", "Lets other remote desktop apps send sound and devices. The host must allow it and each app must still ask. This does not give anyone access to your microphone or camera."), boolean,
        {.section = u"sound"_s, .formLabel = i18nc("@label", "Media for other RDP apps"),
         .optionText = {{u"true"_s, i18nc("@item:inlistbox", "Allow")}, {u"false"_s, i18nc("@item:inlistbox", "Block")}}});
    add(u"MonitorMode"_s, displays, i18nc("@label", "Console screens"), i18nc("@info", "Which of the Console computer’s screens you see. Client-created displays follow their own layout setting."),
        {choice(u"workspace"_s, i18nc("@item:inlistbox", "Whole workspace")), choice(u"primary"_s, i18nc("@item:inlistbox", "Primary display")), choice(u"specific"_s, i18nc("@item:inlistbox", "One display")),
         choice(u"multi"_s, i18nc("@item:inlistbox", "Displays as separate streams")), choice(u"virtual"_s, i18nc("@item:inlistbox", "Client-created displays"))},
        {.section = u"displays"_s, .formLabel = i18nc("@label", "Screens to share")});
    add(u"MonitorIndex"_s, displays, i18nc("@label", "Display index"), i18nc("@info", "A number from 0 to 65535, counting from zero. Used when sharing one display; that display must exist on the Console desktop."), {},
        when({.control = u"spin"_s, .section = u"displays"_s, .min = 0, .max = 65535, .customSeed = u"0"_s}, u"specific"_s));
    // OPT-060: one permission, always shown. `replace` (the default) means "when the connection asks"; `extend` is the legacy keep-on value.
    add(u"VirtualMonitorPolicy"_s, displays, i18nc("@label", "Let connections turn off this computer’s screens"),
        i18nc("@info", "When a connection asks to use its own monitors, this computer’s screens switch off while you are connected and come back when you disconnect or when someone uses this computer’s keyboard or mouse."),
        {choice(u"off"_s, i18nc("@item:inlistbox", "Never turn the computer’s screens off")), choice(u"replace"_s, i18nc("@item:inlistbox", "Turn the computer’s screens off when the connection asks")),
         choice(u"extend"_s, i18nc("@item:inlistbox", "Keep the computer’s screens on"))},
        {.section = u"displays"_s, .formLabel = i18nc("@label", "Let my Farside connections turn off this computer’s screens"),
         .optionText = {{u"off"_s, i18nc("@item:inlistbox", "Off")}, {u"replace"_s, i18nc("@item:inlistbox", "When the connection asks")},
                        {u"extend"_s, i18nc("@item:inlistbox", "Add monitors without turning the host’s screens off (advanced)")}},
         .advancedChoices = {u"extend"_s}});
    add(u"VirtualMonitorLayout"_s, displays, i18nc("@label", "Client-created display layout"), i18nc("@info", "“Client monitors” needs the client to ask for its monitors. “Physical display layout” copies the computer’s own screens, including their resolution and scale."),
        {choice(u"client"_s, i18nc("@item:inlistbox", "Client monitors")), choice(u"single"_s, i18nc("@item:inlistbox", "One display")), choice(u"physical"_s, i18nc("@item:inlistbox", "Physical display layout"))},
        when({.section = u"displays"_s, .formLabel = i18nc("@label", "Layout")}, u"virtual"_s));
    add(u"VirtualMonitorFallbackSize"_s, displays, i18nc("@label", "Fallback display size"), i18nc("@info", "Width and height in pixels, both even numbers, from 320x200 to 8192x8192. Used when the client does not report its monitors."), {},
        when({.control = u"size"_s, .section = u"displays"_s, .formLabel = i18nc("@label", "Fallback size"), .min = 320, .max = 8192, .heightMin = 200, .customSeed = u"1920x1080"_s}, u"virtual"_s));
    add(u"WakeDisplayOnConnect"_s, i18nc("@title:group", "Session"), i18nc("@label", "Wake and keep displays awake"), i18nc("@info", "Wakes the screens and keeps them awake while you are connected, in Console and Virtual. This stops when the last viewer leaves. It never unlocks the screen."), boolean,
        {.section = u"sound"_s, .formLabel = i18nc("@label", "Keep displays awake")});
    add(u"VirtualStockClientPolicy"_s, virtualDesktop, i18nc("@label", "Standard clients in Virtual"), i18nc("@info", "How apps that cannot choose a Farside session connect. The desktop always belongs to the account that signs in."),
        {choice(u"attach-or-create"_s, i18nc("@item:inlistbox", "Attach to or create a desktop")), choice(u"refuse"_s, i18nc("@item:inlistbox", "Require session selection"))},
        {.section = u"video"_s, .advanced = true, .formLabel = i18nc("@label", "Other RDP apps in Virtual")});
    return result;
}
