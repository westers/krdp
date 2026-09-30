// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerUserSettings.h"
#include "UserConfiguration.h"
#include <QMap>
#include <QRegularExpression>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace KRdp::BrokerUserSettings
{
Result parse(const QByteArray &contents)
{
    if (contents.size() > UserConfiguration::MaximumBytes || contents.contains('\0'))
        return {{}, QStringLiteral("configuration is oversized or contains NUL")};
    QMap<QString, QString> fields;
    bool general = false;
    for (const auto &raw : contents.split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[')) {
            general = line == "[General]" || line == "[General][$i]";
            continue;
        }
        if (!general) continue;
        const auto equals = line.indexOf('=');
        if (equals < 1) continue;
        auto key = line.left(equals).trimmed();
        // Only nonlocalized values and KConfig's immutable marker participate.
        // Do not expand [$e] expressions using a privileged broker environment.
        if (key.endsWith("[$i]")) key.chop(4);
        if (key.contains('[')) continue;
        fields.insert(QString::fromUtf8(key), QString::fromUtf8(line.mid(equals + 1)).trimmed());
    }
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
    choice(QStringLiteral("VirtualMonitorPolicy"), {u"replace"_s, u"extend"_s}, p.virtualMonitorPolicy);
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
}
