// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerUserSettings.h"
#include "UserConfiguration.h"
#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <type_traits>

using namespace Qt::StringLiterals;

namespace KRdp::BrokerUserSettings
{
Fields fields(const QByteArray &contents)
{
    Fields result;
    bool general = false;
    for (const auto &raw : contents.split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[')) {
            general = line == "[General]" || line == "[General][$i]";
            if (line == "[General][$i]") result.immutableGroup = true;
            continue;
        }
        if (!general) continue;
        const auto equals = line.indexOf('=');
        if (equals < 1) continue;
        auto key = line.left(equals).trimmed();
        // Only nonlocalized values and KConfig's immutable marker participate.
        // Do not expand [$e] expressions using a privileged broker environment.
        const bool immutable = key.endsWith("[$i]");
        if (immutable) key.chop(4);
        if (key.contains('[')) continue;
        const auto name = QString::fromUtf8(key);
        result.values.insert(name, QString::fromUtf8(line.mid(equals + 1)).trimmed());
        if (immutable) result.immutable.insert(name);
    }
    return result;
}

Result parse(const QByteArray &contents)
{
    if (contents.size() > UserConfiguration::MaximumBytes || contents.contains('\0'))
        return {{}, QStringLiteral("configuration is oversized or contains NUL")};
    const auto fields = BrokerUserSettings::fields(contents).values;
    Preferences p;
    QString error;
    const auto integer = [&](const QString &key, int low, int high, auto &target) {
        if (!fields.contains(key)) return;
        const QString v = fields.value(key);
        static const QRegularExpression decimal(QStringLiteral("^[0-9]+$"));
        bool valid = false;
        const int n = v.toInt(&valid);
        if (!valid || !decimal.match(v).hasMatch() || n < low || n > high) error = key;
        else target = n;
    };
    const auto boolean = [&](const QString &key, std::optional<bool> &target) {
        if (!fields.contains(key)) return;
        const auto v = fields.value(key).toLower();
        if (v == QLatin1String("true") || v == QLatin1String("1")) target = true;
        else if (v == QLatin1String("false") || v == QLatin1String("0")) target = false;
        else error = key;
    };
    const auto choice = [&](const QString &key, const QStringList &allowed, std::optional<QString> &target) {
        if (!fields.contains(key)) return;
        const auto v = fields.value(key).toLower();
        if (!allowed.contains(v)) error = key;
        else target = v;
    };
    integer(QStringLiteral("Quality"), 0, 100, p.quality);
    integer(QStringLiteral("MonitorIndex"), 0, 65535, p.monitorIndex);
    boolean(QStringLiteral("AdaptiveQuality"), p.adaptiveQuality);
    boolean(QStringLiteral("PreferAudioQuality"), p.preferAudioQuality);
    boolean(QStringLiteral("WakeDisplayOnConnect"), p.wakeDisplayOnConnect);
    boolean(QStringLiteral("StandardClientMedia"), p.standardClientMedia);
    choice(QStringLiteral("MonitorMode"), {u"workspace"_s, u"primary"_s, u"specific"_s, u"multi"_s, u"virtual"_s}, p.monitorMode);
    // OPT-060: `off` = never let this user's connections turn the screens off; `replace` = when the
    // connection asks (default); `extend` = keep them on.
    choice(QStringLiteral("VirtualMonitorPolicy"), {u"replace"_s, u"extend"_s, u"off"_s}, p.virtualMonitorPolicy);
    choice(QStringLiteral("VirtualMonitorLayout"), {u"client"_s, u"single"_s, u"physical"_s}, p.virtualMonitorLayout);
    choice(QStringLiteral("VirtualStockClientPolicy"), {u"attach-or-create"_s, u"refuse"_s}, p.virtualStockClientPolicy);
    if (fields.contains(QStringLiteral("Codec"))) {
        p.codec = VideoCodecSupport::parseCodecPreference(fields.value(QStringLiteral("Codec")));
        if (!p.codec) error = QStringLiteral("Codec");
    }
    if (fields.contains(QStringLiteral("SoftwareEncoding"))) {
        p.softwareEncoding = CodecPolicy::parseSoftwareEncoding(fields.value(QStringLiteral("SoftwareEncoding")));
        if (!p.softwareEncoding) error = QStringLiteral("SoftwareEncoding");
    }
    if (fields.contains(QStringLiteral("Av1Tiles"))) {
        p.av1Tiles = CodecPolicy::parseAv1Tiles(fields.value(QStringLiteral("Av1Tiles")));
        if (!p.av1Tiles) error = QStringLiteral("Av1Tiles");
    }
    if (fields.contains(QStringLiteral("VirtualMonitorFallbackSize"))) {
        static const QRegularExpression sizePattern(QStringLiteral("^([0-9]{1,4})x([0-9]{1,4})$"));
        const auto match = sizePattern.match(fields.value(QStringLiteral("VirtualMonitorFallbackSize")));
        const QSize size(match.captured(1).toInt(), match.captured(2).toInt());
        if (!match.hasMatch() || size.width() < 320 || size.width() > 8192 || size.height() < 200 || size.height() > 8192
            || size.width() % 2 || size.height() % 2) error = QStringLiteral("VirtualMonitorFallbackSize");
        else p.virtualMonitorFallbackSize = size;
    }
    const QString motion = QStringLiteral("Avc444MotionGapMs"), rest = QStringLiteral("Avc444RestMs"), gap = QStringLiteral("Avc444MaxGapMs");
    if (fields.contains(motion) || fields.contains(rest) || fields.contains(gap)) {
        std::optional<int> m, r, g;
        integer(motion, 16, 5000, m);
        integer(rest, 16, 5000, r);
        integer(gap, 16, 5000, g);
        const ChromaPolicy defaults;
        const ChromaPolicy chroma{m.value_or(defaults.motionGapMs), r.value_or(defaults.restMs), g.value_or(defaults.maxGapMs)};
        if (!chroma.isValid()) error = QStringLiteral("AVC444 chroma timing");
        else p.chroma = chroma;
    }
    if (!error.isEmpty()) return {{}, QStringLiteral("invalid preference: ") + error};
    return {p, {}};
}

Result readUser(quint32 uid)
{
    const auto data = UserConfiguration::readUser(uid);
    return data ? parse(*data) : Result{};
}

QStringList preferenceKeys()
{
    return {u"Quality"_s, u"AdaptiveQuality"_s, u"PreferAudioQuality"_s, u"Codec"_s, u"SoftwareEncoding"_s, u"Av1Tiles"_s,
        u"Avc444MotionGapMs"_s, u"Avc444RestMs"_s, u"Avc444MaxGapMs"_s, u"MonitorMode"_s, u"MonitorIndex"_s,
        u"VirtualMonitorPolicy"_s, u"VirtualMonitorLayout"_s, u"VirtualMonitorFallbackSize"_s,
        u"WakeDisplayOnConnect"_s, u"StandardClientMedia"_s, u"VirtualStockClientPolicy"_s};
}

QVariantMap publicValues(const Preferences &p, const Fields &fields)
{
    QVariantMap result;
    const auto put = [&](const QString &key, const auto &value) {
        if (value && fields.values.contains(key)) {
            if constexpr (std::is_same_v<typename std::decay_t<decltype(value)>::value_type, bool>)
                result.insert(key, *value ? u"true"_s : u"false"_s);
            else result.insert(key, QString::number(*value));
        }
    };
    put(u"Quality"_s, p.quality); put(u"AdaptiveQuality"_s, p.adaptiveQuality); put(u"PreferAudioQuality"_s, p.preferAudioQuality);
    put(u"MonitorIndex"_s, p.monitorIndex); put(u"WakeDisplayOnConnect"_s, p.wakeDisplayOnConnect); put(u"StandardClientMedia"_s, p.standardClientMedia);
    if (p.codec) result.insert(u"Codec"_s, *p.codec == CodecPreference::Auto ? u"auto"_s : *p.codec == CodecPreference::Avc420 ? u"avc420"_s : u"avc444"_s);
    if (p.softwareEncoding) result.insert(u"SoftwareEncoding"_s, QString::fromLatin1(CodecPolicy::softwareEncodingName(*p.softwareEncoding)));
    if (p.av1Tiles) result.insert(u"Av1Tiles"_s, CodecPolicy::av1TilesName(*p.av1Tiles));
    for (const auto &[key, value] : {std::pair{u"MonitorMode"_s, p.monitorMode}, {u"VirtualMonitorPolicy"_s, p.virtualMonitorPolicy},
            {u"VirtualMonitorLayout"_s, p.virtualMonitorLayout}, {u"VirtualStockClientPolicy"_s, p.virtualStockClientPolicy}})
        if (value) result.insert(key, *value);
    if (p.virtualMonitorFallbackSize) {
        const QString size = QString::number(p.virtualMonitorFallbackSize->width()) + u"x"_s + QString::number(p.virtualMonitorFallbackSize->height());
        result.insert(u"VirtualMonitorFallbackSize"_s, size);
    }
    if (p.chroma) {
        for (const auto &[key, value] : {std::pair{u"Avc444MotionGapMs"_s, p.chroma->motionGapMs}, {u"Avc444RestMs"_s, p.chroma->restMs}, {u"Avc444MaxGapMs"_s, p.chroma->maxGapMs}})
            if (fields.values.contains(key)) result.insert(key, QString::number(value));
    }
    return result;
}

Edit edit(const QByteArray &original, const QVariantMap &desired)
{
    const auto parsed = parse(original);
    if (!parsed.error.isEmpty()) return {{}, parsed.error};
    const auto oldFields = fields(original);
    const auto oldValues = publicValues(parsed.preferences, oldFields);
    const auto keys = preferenceKeys();
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const auto value = it.value().toString();
        if (!keys.contains(it.key()) || it.value().metaType() != QMetaType::fromType<QString>() || value.size() > 256
            || value.contains(QChar::Null) || value.contains(u'\n') || value.contains(u'\r')) return {{}, u"Invalid preference update"_s};
    }
    QByteArray submitted("[General]\n");
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) submitted += it.key().toUtf8() + '=' + it.value().toString().toUtf8() + '\n';
    const auto submittedResult = parse(submitted);
    if (!submittedResult.error.isEmpty()) return {{}, submittedResult.error};
    const auto canonical = publicValues(submittedResult.preferences, fields(submitted));
    for (const auto &key : keys) {
        if ((oldFields.immutableGroup || oldFields.immutable.contains(key))
            && (canonical.contains(key) != oldValues.contains(key) || canonical.value(key) != oldValues.value(key)))
            return {{}, u"Preference is locked: "_s + key};
    }
    if (canonical == oldValues) return {original, {}};
    // Preserve every unedited line exactly, including comments, host/legacy
    // data, localized/expanded keys and repeated sections. Insert the new
    // whitelist once into the last General section.
    const auto lines = original.split('\n');
    QList<QByteArray> kept;
    bool general = false;
    qsizetype insertion = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto raw = lines[i]; const auto line = raw.trimmed();
        if (line.startsWith('[')) {
            general = line == "[General]" || line == "[General][$i]";
            if (general) insertion = kept.size() + 1;
        }
        const auto equals = line.indexOf('=');
        const auto name = QString::fromUtf8(line.left(equals).trimmed());
        const bool editable = general && equals > 0 && keys.contains(name) && !oldFields.immutableGroup && !oldFields.immutable.contains(name);
        if (!editable) kept.append(raw + (i + 1 < lines.size() ? QByteArrayLiteral("\n") : QByteArray()));
        if (general) insertion = kept.size();
    }
    QByteArray block;
    const QByteArray newline = original.contains("\r\n") ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n");
    for (const auto &key : keys) {
        if (canonical.contains(key) && !oldFields.immutableGroup && !oldFields.immutable.contains(key))
            block += key.toUtf8() + '=' + canonical.value(key).toString().toUtf8() + newline;
    }
    if (insertion < 0 && !block.isEmpty()) { insertion = kept.size(); block.prepend(QByteArrayLiteral("[General]") + newline); }
    if (!block.isEmpty()) {
        if (insertion > 0 && !kept[insertion - 1].isEmpty() && !kept[insertion - 1].endsWith('\n')) kept[insertion - 1].append(newline);
        kept.insert(insertion, block);
    }
    QByteArray document;
    for (const auto &line : kept) document.append(line);
    const auto valid = parse(document);
    if (!valid.error.isEmpty()) return {{}, valid.error};
    const auto applied = publicValues(valid.preferences, fields(document));
    if (applied != canonical) return {{}, u"Preference update did not match readback"_s};
    return {document, {}};
}
}
