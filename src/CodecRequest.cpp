// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "CodecRequest.h"

#include "LayoutControl.h"
#include "VideoStream.h"

#include <QJsonArray>

namespace KRdp::CodecRequest
{
std::optional<Request> parse(const QJsonObject &record)
{
    const QJsonValue codecs = record.value(QLatin1String("codecs"));
    Request request;
    request.adaptive = record.value(QLatin1String("adaptive")).toBool(true);
    if (const QJsonValue decode = record.value(QLatin1String("decode")); decode.isObject()) {
        const QJsonObject paths = decode.toObject();
        const auto path = [&paths](const char *codec) {
            const QString value = paths.value(QLatin1String(codec)).toString().trimmed().toLower();
            return value == QLatin1String("hw") ? CodecPolicy::DecodePath::Hardware
                : value == QLatin1String("sw")  ? CodecPolicy::DecodePath::Software
                                                : CodecPolicy::DecodePath::Unknown;
        };
        request.decode = {path("avc"), path("hevc"), path("av1")};
    }
    if (codecs.isUndefined()) {
        return request; // no list: AVC only (krdpserver has always read it so)
    }
    if (!codecs.isArray()) {
        return std::nullopt;
    }
    for (const QJsonValue &value : codecs.toArray()) {
        if (!value.isString()) {
            return std::nullopt;
        }
        const QString name = value.toString().trimmed().toLower();
        if (name == QLatin1String("hevc")) {
            request.codecs.append(VideoCodec::Hevc);
        } else if (name == QLatin1String("av1")) {
            request.codecs.append(VideoCodec::Av1);
        } else {
            return std::nullopt;
        }
        request.names.append(name);
    }
    return request;
}

QJsonObject invalidRecord()
{
    return LayoutControl::errorRecord({QStringLiteral("invalid"), QStringLiteral("codec codecs must be an array of hevc and/or av1")});
}

QJsonObject apply(VideoStream &stream, const Request &request, QString *log)
{
    // The server picks among the client's codecs and AVC by its SoftwareEncoding policy and the
    // encoders this host really has (AUD-FIX2 F1: Sol's KPipeWire had no HEVC encoder, and a HEVC
    // answer there labelled H.264 bytes 0x8001). `adaptive: false` pins the first choice.
    // AV1-Q: the decode paths first, so the first encoder settings already carry the AV1 tiles.
    stream.setClientDecode(request.decode);
    const auto decision = stream.setPrivateCodecPolicy(request.codecs, request.adaptive);
    const QString selected = QString::fromLatin1(CodecPolicy::familyName(decision.choice.family));
    QString reason = decision.reason;
    if (decision.choice.family == CodecPolicy::Family::Avc && !request.codecs.isEmpty()) {
        // AUD-FIX4 D5: say why, accurately: no encoder at all, or software ones the policy did
        // not pick (a normal link under SoftwareEncoding=auto).
        QList<CodecPolicy::Family> families;
        for (const auto codec : request.codecs) {
            families.append(codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : CodecPolicy::Family::Av1);
        }
        reason = CodecPolicy::avcChoiceReason(stream.encoderPolicy(), stream.softwareEncoding(), request.adaptive, families);
    }
    if (log) {
        *log = QStringLiteral("KRDPCTL: codec asked [%1] (decode avc=%5 hevc=%6 av1=%7), selected %2 (%3)%4")
                   .arg(request.names.join(QLatin1Char(',')),
                        selected,
                        decision.choice.hardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                        reason.isEmpty() ? QString() : QStringLiteral(": ") + reason,
                        QLatin1String(CodecPolicy::decodePathName(request.decode.avc)),
                        QLatin1String(CodecPolicy::decodePathName(request.decode.hevc)),
                        QLatin1String(CodecPolicy::decodePathName(request.decode.av1)));
        if (decision.choice.family == CodecPolicy::Family::Av1) {
            *log += QStringLiteral("; AV1 tiles %1 (setting %2)")
                        .arg(stream.av1Tiles() ? QString::number(stream.av1Tiles()) : QStringLiteral("per resolution"),
                             CodecPolicy::av1TilesName(stream.av1TilesSetting()));
        }
    }
    return LayoutControl::codecRecord(selected, decision.choice.hardware, reason);
}
}
