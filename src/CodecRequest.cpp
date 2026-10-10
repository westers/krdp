// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "CodecRequest.h"

#include "LayoutControl.h"
#include "VideoStream.h"

#include <QJsonArray>

namespace KRdp::CodecRequest
{
namespace
{
constexpr qsizetype MaxFamilies = 3;

std::optional<CodecPolicy::Family> familyFromName(const QString &name)
{
    if (name == QLatin1String("avc")) return CodecPolicy::Family::Avc;
    if (name == QLatin1String("hevc")) return CodecPolicy::Family::Hevc;
    if (name == QLatin1String("av1")) return CodecPolicy::Family::Av1;
    return std::nullopt;
}

CodecPolicy::DecodePath pathFromName(const QString &value)
{
    return value == QLatin1String("hw") ? CodecPolicy::DecodePath::Hardware
        : value == QLatin1String("sw")  ? CodecPolicy::DecodePath::Software
                                        : CodecPolicy::DecodePath::Unknown;
}

void invalid(QString *error, const QString &message)
{
    if (error) *error = message;
}
}

std::optional<Request> parse(const QJsonObject &record, QString *error)
{
    const QJsonValue codecs = record.value(QLatin1String("codecs"));
    Request request;
    request.adaptive = record.value(QLatin1String("adaptive")).toBool(true);
    if (const QJsonValue decode = record.value(QLatin1String("decode")); decode.isObject()) {
        const QJsonObject paths = decode.toObject();
        const auto path = [&paths](const char *codec) {
            return pathFromName(paths.value(QLatin1String(codec)).toString().trimmed().toLower());
        };
        request.decode = {path("avc"), path("hevc"), path("av1")};
    }
    if (!codecs.isUndefined()) {
        if (!codecs.isArray()) {
            invalid(error, QStringLiteral("codec codecs must be an array of hevc and/or av1"));
            return std::nullopt;
        }
        for (const QJsonValue &value : codecs.toArray()) {
            if (!value.isString()) {
                invalid(error, QStringLiteral("codec codecs must be an array of hevc and/or av1"));
                return std::nullopt;
            }
            const QString name = value.toString().trimmed().toLower();
            if (name == QLatin1String("hevc")) {
                request.codecs.append(VideoCodec::Hevc);
            } else if (name == QLatin1String("av1")) {
                request.codecs.append(VideoCodec::Av1);
            } else {
                invalid(error, QStringLiteral("codec codecs must be an array of hevc and/or av1"));
                return std::nullopt;
            }
            request.names.append(name);
        }
    }

    // OPT-062 S2: the per-connection fields. All optional; any of them makes the request an explicit one.
    const QJsonValue order = record.value(QLatin1String("order"));
    const QJsonValue encode = record.value(QLatin1String("encode"));
    const QJsonValue decodeMode = record.value(QLatin1String("decodeMode"));
    const QJsonValue decoders = record.value(QLatin1String("decoders"));
    if (order.isUndefined() && encode.isUndefined() && decodeMode.isUndefined() && decoders.isUndefined()) {
        return request;
    }

    QList<CodecPolicy::Family> families;
    if (!order.isUndefined()) {
        if (!order.isArray() || order.toArray().size() > MaxFamilies) {
            invalid(error, QStringLiteral("codec order must be an array of at most three distinct codecs (avc, hevc, av1)"));
            return std::nullopt;
        }
        for (const QJsonValue &value : order.toArray()) {
            const auto family = value.isString() ? familyFromName(value.toString().trimmed().toLower()) : std::nullopt;
            if (!family || families.contains(*family)) {
                invalid(error, QStringLiteral("codec order must be an array of at most three distinct codecs (avc, hevc, av1)"));
                return std::nullopt;
            }
            families.append(*family);
        }
    } else {
        for (const VideoCodec codec : std::as_const(request.codecs)) {
            families.append(codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : CodecPolicy::Family::Av1);
        }
    }
    const auto mode = [&](const QJsonValue &value, const char *field) -> std::optional<CodecPolicy::Mode> {
        if (value.isUndefined()) return CodecPolicy::Mode::Any;
        if (value.isString()) {
            if (const auto parsed = CodecPolicy::parseMode(value.toString())) return parsed;
        }
        invalid(error, QStringLiteral("codec %1 must be hardware, software or any").arg(QLatin1String(field)));
        return std::nullopt;
    };
    const auto encodeMode = mode(encode, "encode");
    if (!encodeMode) return std::nullopt;
    const auto decodeModeValue = mode(decodeMode, "decodeMode");
    if (!decodeModeValue) return std::nullopt;

    CodecPolicy::Request selection = CodecPolicy::requestFromRecord(families, request.decode, request.adaptive);
    selection.encode = *encodeMode;
    selection.decode = *decodeModeValue;
    if (!decoders.isUndefined()) {
        if (!decoders.isObject()) {
            invalid(error, QStringLiteral("codec decoders must be an object with avc, hevc and av1 arrays of hw and sw"));
            return std::nullopt;
        }
        const QJsonObject map = decoders.toObject();
        for (auto it = map.begin(); it != map.end(); ++it) {
            const auto family = familyFromName(it.key());
            if (!family || !it.value().isArray() || it.value().toArray().size() > 2) {
                invalid(error, QStringLiteral("codec decoders must be an object with avc, hevc and av1 arrays of hw and sw"));
                return std::nullopt;
            }
            CodecPolicy::DecoderPaths paths;
            CodecPolicy::DecodePath first = CodecPolicy::DecodePath::Unknown;
            for (const QJsonValue &value : it.value().toArray()) {
                const auto path = value.isString() ? pathFromName(value.toString().trimmed().toLower()) : CodecPolicy::DecodePath::Unknown;
                const bool duplicate = (path == CodecPolicy::DecodePath::Hardware && paths.hardware) || (path == CodecPolicy::DecodePath::Software && paths.software);
                if (path == CodecPolicy::DecodePath::Unknown || duplicate) {
                    invalid(error, QStringLiteral("codec decoders must be an object with avc, hevc and av1 arrays of hw and sw"));
                    return std::nullopt;
                }
                (path == CodecPolicy::DecodePath::Hardware ? paths.hardware : paths.software) = true;
                if (first == CodecPolicy::DecodePath::Unknown) first = path;
            }
            selection.decoders[size_t(*family)] = paths;
            // `decode` (the path the client will use) is the first entry; [] = it cannot decode the codec.
            (*family == CodecPolicy::Family::Hevc ? request.decode.hevc : *family == CodecPolicy::Family::Av1 ? request.decode.av1 : request.decode.avc) = first;
        }
    }
    request.selection = selection;
    request.orderText = QStringLiteral("order [%1] encode %2 decode %3")
                            .arg(CodecPolicy::orderText(families),
                                 QLatin1String(CodecPolicy::modeName(selection.encode)),
                                 QLatin1String(CodecPolicy::modeName(selection.decode)));
    return request;
}

QJsonObject invalidRecord(const QString &message)
{
    return LayoutControl::errorRecord({QStringLiteral("invalid"), message.isEmpty() ? QStringLiteral("codec codecs must be an array of hevc and/or av1") : message});
}

QJsonObject apply(VideoStream &stream, const Request &request, QString *log)
{
    // The client's order is always honoured (OPT-062): its `order` when it sent the S2 fields, else its `codecs` in
    // the order it sent them. The host's software ceiling and the encoders this host really has decide what is
    // possible; the reply says what was skipped and why. `adaptive: false` pins the first choice.
    // AV1-Q: the decode paths first, so the first encoder settings already carry the AV1 tiles.
    stream.setClientDecode(request.decode);
    const auto decision = request.selection ? stream.setCodecRequest(*request.selection) : stream.setPrivateCodecPolicy(request.codecs, request.adaptive);
    const QString selected = QString::fromLatin1(CodecPolicy::familyName(decision.choice.family));
    QString reason = decision.reason.isEmpty() ? QStringLiteral("initial choice") : decision.reason;
    const auto skipped = stream.skippedBeforeChoice();
    if (decision.baseline) {
        reason = QStringLiteral("nothing in the codec list can be used (%1): standard AVC").arg(CodecPolicy::skippedText(skipped));
    } else if (!skipped.isEmpty()) {
        reason += QStringLiteral("; skipped %1").arg(CodecPolicy::skippedText(skipped));
    }
    if (log) {
        *log = QStringLiteral("KRDPCTL: codec asked [%1]%5 (decode avc=%6 hevc=%7 av1=%8), selected %2 (%3)%4")
                   .arg(request.names.join(QLatin1Char(',')),
                        selected,
                        decision.choice.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                        QStringLiteral(": ") + reason,
                        request.orderText.isEmpty() ? QString() : QStringLiteral(", ") + request.orderText,
                        QLatin1String(CodecPolicy::decodePathName(request.decode.avc)),
                        QLatin1String(CodecPolicy::decodePathName(request.decode.hevc)),
                        QLatin1String(CodecPolicy::decodePathName(request.decode.av1)));
        if (decision.choice.family == CodecPolicy::Family::Av1) {
            *log += QStringLiteral("; AV1 tiles %1 (setting %2)")
                        .arg(stream.av1Tiles() ? QString::number(stream.av1Tiles()) : QStringLiteral("per resolution"),
                             CodecPolicy::av1TilesName(stream.av1TilesSetting()));
        }
    }
    return LayoutControl::codecRecord(selected, decision.choice.hardware, reason, stream.codecDetail());
}
}
