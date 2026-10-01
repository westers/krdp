// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "LegacySettingsMigration.h"
#include "BrokerHostSettings.h"
#include "BrokerUserSettings.h"
#include "UserConfiguration.h"
#include <KConfig>
#include <KConfigGroup>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;
static void initializeLegacySchema() { Q_INIT_RESOURCE(farside_legacy_schema); }

namespace KRdp::LegacySettingsMigration {
namespace {
struct Field { QString type, value; };
using Schema = QMap<QString, Field>;
Schema schema()
{
    static const Schema result = [] {
        initializeLegacySchema();
        QFile file(u":/farside/migration/krdpserversettings.kcfg"_s);
        if (!file.open(QIODevice::ReadOnly)) return Schema{};
        QXmlStreamReader xml(file.readAll()); Schema fields;
        QString key, type, value;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"entry") {
                key = xml.attributes().value(u"name").toString();
                type = xml.attributes().value(u"type").toString(); value.clear();
            } else if (xml.isStartElement() && xml.name() == u"default") value = xml.readElementText();
            else if (xml.isEndElement() && xml.name() == u"entry") {
                if (key.isEmpty() || fields.contains(key)) return Schema{};
                fields.insert(key, {type, value});
            }
        }
        return xml.hasError() ? Schema{} : fields;
    }();
    return result;
}
QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
bool nameValid(const QString &name)
{
    if (name.isEmpty() || name.size() > 256) return false;
    for (const auto character : name) if (!character.isPrint()) return false;
    return true;
}
struct Line { QByteArray bytes; QString key; bool general = false; };
struct Document { QList<Line> lines; QString error; };
Document document(const QByteArray &bytes, const Schema &known)
{
    if (bytes.size() > UserConfiguration::MaximumBytes || bytes.contains('\0') || bytes.startsWith("\xef\xbb\xbf"))
        return {{}, u"Legacy document is oversized or contains NUL/BOM"_s};
    QStringDecoder decode(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decode(bytes); Q_UNUSED(decoded);
    if (decode.hasError()) return {{}, u"Legacy document is not valid UTF-8"_s};
    Document result; bool general = false;
    const auto parts = bytes.split('\n');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto raw = parts[i], trimmed = raw.trimmed(); QString key;
        if (trimmed.startsWith('[')) {
            static const QRegularExpression groupPattern(u"^(?:\\[[^\\[\\]\\r\\n]+\\])+$"_s);
            if (!groupPattern.match(QString::fromUtf8(trimmed)).hasMatch())
                return {{}, u"Legacy group is malformed"_s};
            general = trimmed == "[General]" || trimmed == "[General][$i]";
            if (trimmed.startsWith("[General][$") && !general)
                return {{}, u"Legacy General group marker requires resolution"_s};
        } else if (general && !trimmed.isEmpty() && !trimmed.startsWith('#')) {
            const auto equals = trimmed.indexOf('=');
            if (equals < 1) return {{}, u"Legacy General entry is malformed"_s};
            auto name = QString::fromUtf8(trimmed.left(equals).trimmed());
            if (name.contains(u'\\')) return {{}, u"Escaped legacy key requires resolution"_s};
            key = name.section(u'[', 0, 0);
            if (known.contains(key) && name != key && name != key + u"[$i]")
                return {{}, u"Legacy key marker requires resolution: "_s + key};
        }
        result.lines.append({raw + (i + 1 < parts.size() ? QByteArrayLiteral("\n") : QByteArray()), key, general});
    }
    return result;
}
bool booleanText(const QString &value) { return QStringList{u"true"_s, u"false"_s, u"1"_s, u"0"_s}.contains(value.toLower()); }
QByteArray rewrite(const Document &original, const QVariantMap &values, const QSet<QString> &locks)
{
    const auto keys = BrokerUserSettings::preferenceKeys();
    QList<QByteArray> kept; qsizetype insert = -1; bool general = false;
    QByteArray newline("\n");
    for (const auto &line : original.lines) {
        if (line.bytes.endsWith("\r\n")) newline = "\r\n";
        general = line.general;
        if (!(general && keys.contains(line.key))) kept.append(line.bytes);
        if (general) insert = kept.size();
    }
    QByteArray block;
    for (const auto &key : keys)
        block += key.toUtf8() + (locks.contains(key) ? QByteArrayLiteral("[$i]") : QByteArray())
            + '=' + values.value(key).toString().toUtf8() + newline;
    if (insert < 0) { insert = kept.size(); block.prepend(QByteArrayLiteral("[General]") + newline); }
    if (insert > 0 && !kept[insert - 1].isEmpty() && !kept[insert - 1].endsWith('\n')) kept[insert - 1].append(newline);
    kept.insert(insert, block);
    QByteArray output; for (const auto &line : kept) output += line;
    return output;
}
}

Snapshot inspect(const QByteArray &source, const Owner &owner, const Resolver &resolve)
{
    const auto fail = [](const QString &reason) { Snapshot result; result.error = reason; return result; };
    if (!owner.uid || owner.uid == quint32(-1) || !nameValid(owner.account) || !resolve || resolve(owner.account) != owner.uid)
        return fail(u"Original daemon owner is unresolved or inconsistent"_s);
    const auto known = schema();
    const QStringList special{u"ListenPort"_s, u"ListenAddress"_s, u"AutogenerateCertificates"_s, u"Certificate"_s,
        u"CertificateKey"_s, u"VaapiDriverMode"_s, u"CameraLoopbackDevice"_s, u"Users"_s, u"SystemUserEnabled"_s};
    const auto preferences = BrokerUserSettings::preferenceKeys();
    auto expected = preferences; expected.append(special);
    if (known.size() != expected.size()) return fail(u"Legacy schema coverage changed"_s);
    for (const auto &key : expected) if (!known.contains(key)) return fail(u"Legacy schema coverage changed"_s);
    QByteArray profile;
    for (auto it = known.cbegin(); it != known.cend(); ++it)
        profile += it.key().toUtf8() + '\0' + it.value().type.toUtf8() + '\0' + it.value().value.toUtf8() + '\n';
    // Old omissions must never inherit a future schema's changed defaults.
    if (digest(profile) != "7cc20fc66baf2de64cbe4bdaf0e9d42019a61dae9323759056e912eecd948d86")
        return fail(u"Legacy default profile changed; retain its historical schema"_s);
    const auto lexical = document(source, known);
    if (!lexical.error.isEmpty()) return fail(lexical.error);
    // Use actual KConfig decoding, isolated from globals, HOME and locale.
    // This temporary snapshot is private; never write the source/destination.
    QTemporaryDir temporary; if (!temporary.isValid()) return fail(u"Private legacy snapshot is unavailable"_s);
    QFile file(temporary.filePath(u"legacyrc"_s));
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(source) != source.size()) return fail(u"Private legacy snapshot cannot be prepared"_s);
    file.close();
    KConfig config(file.fileName(), KConfig::SimpleConfig); config.setLocale(u"C"_s);
    const KConfigGroup group(&config, u"General"_s);
    Snapshot result; result.owner = owner; result.revision = digest(source);
    QVariantMap desired;
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        const auto key = it.key(); QString raw = group.readEntry(key, it.value().value);
        if (it.value().type == u"Bool") {
            if (!booleanText(raw)) return fail(u"Invalid legacy boolean: "_s + key);
            // Preserve the generated legacy class's typed interpretation, not
            // an independently invented Boolean conversion.
            raw = group.readEntry(key, it.value().value == u"true") ? u"true"_s : u"false"_s;
        }
        if (preferences.contains(key)) {
            desired.insert(key, raw);
            if (!group.hasKey(key)) result.defaultedKeys.append(key);
            if (group.isEntryImmutable(key)) result.lockedPreferences.insert(key);
        } else if (key == u"Users") {
            if (group.isEntryImmutable(key)) result.lockedAdmissionKeys.insert(key);
            result.aliases = group.readEntry(key, QStringList{});
            QSet<QString> unique;
            if (result.aliases.size() > 128) return fail(u"Legacy alias limit exceeded"_s);
            for (const auto &alias : result.aliases) {
                if (!nameValid(alias) || unique.contains(alias)) return fail(u"Legacy aliases are invalid or duplicated"_s);
                unique.insert(alias);
            }
        } else if (key == u"SystemUserEnabled") {
            if (group.isEntryImmutable(key)) result.lockedAdmissionKeys.insert(key);
            result.ownerPam = raw.toLower() == u"true" || raw == u"1";
        }
        else {
            if (group.isEntryImmutable(key)) result.lockedHostFacts.insert(key);
            QString value = raw;
            QString hostKey;
            if (key == u"ListenPort") hostKey = u"Port"_s;
            else if (key == u"ListenAddress") { hostKey = u"Address"_s; if (value.isEmpty()) value = u"0.0.0.0"_s; }
            else if (key == u"VaapiDriverMode") hostKey = u"VaapiDriver"_s;
            else if (key == u"CameraLoopbackDevice") { hostKey = key; if (value.isEmpty()) value = u"none"_s; }
            if (!hostKey.isEmpty()) {
                const auto normalized = BrokerHostSettings::normalize(BrokerHostSettings::Scope::Console, hostKey, value);
                if (!normalized) return fail(u"Legacy host field requires resolution: "_s + key);
                value = *normalized;
            } else if (key == u"AutogenerateCertificates") value = (raw.toLower() == u"true" || raw == u"1") ? u"true"_s : u"false"_s;
            else if (value.size() > 4096 || value.contains(u'\n') || value.contains(u'\r')) return fail(u"Invalid legacy TLS path"_s);
            result.hostFacts.insert(key, value);
        }
    }
    for (const auto &key : group.keyList()) if (!known.contains(key)) ++result.unknownKeys;
    const auto canonical = BrokerUserSettings::edit({}, desired);
    if (!canonical.error.isEmpty()) return fail(canonical.error);
    const auto parsed = BrokerUserSettings::parse(canonical.document);
    result.preferences = BrokerUserSettings::publicValues(parsed.preferences, BrokerUserSettings::fields(canonical.document));
    if (result.preferences.size() != preferences.size()) return fail(u"Legacy preference coverage changed"_s);
    return result;
}

Plan prepare(const QByteArray &source, const std::optional<QByteArray> &destination, const Owner &owner, const Resolver &resolve, DestinationMode mode)
{
    Plan result; result.legacy = inspect(source, owner, resolve);
    if (!result.legacy.error.isEmpty()) { result.error = result.legacy.error; return result; }
    result.destinationPresent = destination.has_value();
    if (mode == DestinationMode::SameLegacySnapshot && (!destination || *destination != source)) {
        result.error = u"Same-file legacy snapshot changed or is absent"_s; return result;
    }
    result.revision = digest(QByteArrayLiteral("legacy-settings-v1\n") + QByteArray::number(owner.uid) + '\n'
        + owner.account.toUtf8() + '\n' + digest(source) + '\n' + (destination ? QByteArrayLiteral("present:") + digest(*destination) : QByteArrayLiteral("absent")));
    result.revision = digest(result.revision + (mode == DestinationMode::SameLegacySnapshot ? QByteArrayLiteral("same-legacy-snapshot") : QByteArrayLiteral("separate")));
    const auto original = destination.value_or(QByteArray{});
    const auto lexical = document(original, schema());
    if (!lexical.error.isEmpty()) { result.error = lexical.error; return result; }
    const auto oldFields = BrokerUserSettings::fields(original);
    auto locks = result.legacy.lockedPreferences;
    // Same-file upgrade preserves effective legacy values and its locks even
    // when KConfig syntax/defaults differ from the broker preference parser.
    if (mode == DestinationMode::Separate) {
        const auto parsed = BrokerUserSettings::parse(original);
        if (!parsed.error.isEmpty()) { result.error = parsed.error; return result; }
        const auto values = BrokerUserSettings::publicValues(parsed.preferences, oldFields);
        for (const auto &key : BrokerUserSettings::preferenceKeys()) {
            if ((values.contains(key) && values.value(key) != result.legacy.preferences.value(key))
                || (oldFields.immutableGroup && !values.contains(key))) result.conflicts.append(key);
        }
        if (!result.conflicts.isEmpty()) { result.error = u"Destination preferences conflict; resolve named keys before migration"_s; return result; }
    }
    locks.unite(oldFields.immutable);
    result.document = rewrite(lexical, result.legacy.preferences, locks);
    const auto valid = BrokerUserSettings::parse(result.document);
    const auto fields = BrokerUserSettings::fields(result.document);
    if (!valid.error.isEmpty() || BrokerUserSettings::publicValues(valid.preferences, fields) != result.legacy.preferences) {
        result.document.clear(); result.error = u"Prepared legacy preferences failed independent readback"_s; return result;
    }
    for (const auto &key : locks) {
        if (BrokerUserSettings::preferenceKeys().contains(key) && !fields.immutableGroup && !fields.immutable.contains(key)) {
            result.document.clear(); result.error = u"Prepared legacy preference lock was lost"_s; return result;
        }
    }
    result.changed = !destination || result.document != *destination;
    return result;
}
QJsonObject manifest(const Plan &plan)
{
    return {{u"version"_s, 1}, {u"defaultProfile"_s, u"legacy-f255ee2-4b308bd"_s}, {u"prepared"_s, plan.error.isEmpty() && !plan.document.isEmpty()},
        {u"revision"_s, QString::fromLatin1(plan.revision)}, {u"sourceRevision"_s, QString::fromLatin1(plan.legacy.revision)},
        {u"destinationPresent"_s, plan.destinationPresent}, {u"changed"_s, plan.changed},
        {u"preferenceCount"_s, plan.legacy.preferences.size()}, {u"materializedDefaultCount"_s, plan.legacy.defaultedKeys.size()},
        {u"lockedPreferenceCount"_s, plan.legacy.lockedPreferences.size()}, {u"aliasCount"_s, plan.legacy.aliases.size()},
        {u"lockedHostFactCount"_s, plan.legacy.lockedHostFacts.size()}, {u"lockedAdmissionKeyCount"_s, plan.legacy.lockedAdmissionKeys.size()},
        {u"unknownKeyCount"_s, plan.legacy.unknownKeys}, {u"conflictCount"_s, plan.conflicts.size()},
        {u"pamScope"_s, !plan.legacy.error.isEmpty() ? u"unavailable"_s : plan.legacy.ownerPam ? u"original-owner-only"_s : u"disabled"_s},
        {u"pendingActions"_s, QJsonArray{u"verify-console-parity"_s, u"backup-and-publish"_s,
            u"authorize-host-admission-device-policy"_s, u"verify-destination-certificate"_s,
            u"verify-credential-migration"_s, u"verify-client-retained-migration"_s, u"verify-rollback"_s}}};
}
}
