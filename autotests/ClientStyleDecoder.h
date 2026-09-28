// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

// AUD-FIX11 R6: decode a delivered video stream the way krdp-client does, so a test fails
// where the client would. krdp-client 0.5.5 (GraphicsPipeline::decodePrivateSurface) keeps one
// FFmpeg decoder per RDPGFX surface, opened on that surface's first private packet with
// avcodec_find_decoder() (AV1: libdav1d when FFmpeg has it) and no options, no extradata and no
// parser; every surface command's payload is one avcodec_send_packet(), then every frame is
// drained with avcodec_receive_frame(). Any error from either is fatal for the client (it drops
// the codec and asks the host for AVC). AVC420 goes through FreeRDP's own H.264 decoder, which
// is also FFmpeg's h264 fed whole access units; the same loop stands in for it.

#include <QByteArray>
#include <QHash>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "VideoCodec.h"

namespace KRdp::ClientStyle
{
/// The OBU (AV1) or NAL unit (H.264/HEVC, Annex B) types in one packet, e.g. "TD SEQ FRAME".
inline QString unitTypes(VideoCodec codec, const QByteArray &packet)
{
    QStringList types;
    const auto *data = reinterpret_cast<const uint8_t *>(packet.constData());
    const qsizetype size = packet.size();
    if (codec == VideoCodec::Av1) {
        static const char *const names[] = {"RESERVED0", "SEQ", "TD", "FRAME_HDR", "TILE_GROUP", "METADATA", "FRAME", "REDUNDANT", "TILE_LIST"};
        for (qsizetype i = 0; i < size;) {
            const uint8_t header = data[i];
            const int type = (header >> 3) & 0xf;
            types << (type < 9 ? QString::fromLatin1(names[type]) : (type == 15 ? QStringLiteral("PADDING") : QStringLiteral("OBU%1").arg(type)));
            qsizetype at = i + 1 + ((header & 0x4) ? 1 : 0);
            if (!(header & 0x2)) {
                types << QStringLiteral("(no size field)");
                break;
            }
            uint64_t obuSize = 0;
            int n = 0;
            for (; n < 8 && at + n < size; ++n) {
                obuSize |= uint64_t(data[at + n] & 0x7f) << (7 * n);
                if (!(data[at + n] & 0x80)) break;
            }
            at += n + 1;
            if (obuSize > uint64_t(size - std::min(at, size))) {
                types << QStringLiteral("(truncated)");
                break;
            }
            i = at + qsizetype(obuSize);
        }
        return types.join(QLatin1Char(' '));
    }
    const bool hevc = codec == VideoCodec::Hevc;
    for (qsizetype i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const uint8_t header = data[i + 3];
            const int type = hevc ? (header >> 1) & 0x3f : header & 0x1f;
            QString name;
            if (hevc) {
                name = type == 32 ? QStringLiteral("VPS") : type == 33 ? QStringLiteral("SPS") : type == 34 ? QStringLiteral("PPS") : type == 35 ? QStringLiteral("AUD")
                    : (type == 19 || type == 20) ? QStringLiteral("IDR") : type == 21 ? QStringLiteral("CRA") : type < 16 ? QStringLiteral("SLICE")
                    : (type == 39 || type == 40) ? QStringLiteral("SEI") : QStringLiteral("NAL%1").arg(type);
            } else {
                name = type == 7 ? QStringLiteral("SPS") : type == 8 ? QStringLiteral("PPS") : type == 9 ? QStringLiteral("AUD") : type == 5 ? QStringLiteral("IDR")
                    : type == 1 ? QStringLiteral("SLICE") : type == 6 ? QStringLiteral("SEI") : QStringLiteral("NAL%1").arg(type);
            }
            types << name;
            i += 2;
        }
    }
    if (types.isEmpty()) types << QStringLiteral("(no start code)");
    return types.join(QLatin1Char(' '));
}

/// One client: a decoder per surface, fed the surface commands in the order they arrived.
class Decoder
{
public:
    struct Surface {
        std::shared_ptr<AVCodecContext> context;
        std::shared_ptr<AVFrame> frame;
        int packets = 0;
        int pictures = 0;
        QSize lastPicture;
    };

    /// Feeds one surface command's payload. False (and error()) as soon as the client would give up.
    bool feed(int surface, VideoCodec codec, const QByteArray &payload)
    {
        if (!m_error.isEmpty()) return false;
        const int number = m_packets++;
        auto &s = m_surfaces[surface];
        if (!s.context) {
            const AVCodecID id = codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : codec == VideoCodec::Av1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_H264;
            const AVCodec *decoder = avcodec_find_decoder(id);
            if (!decoder) return fail(number, surface, codec, payload, QStringLiteral("no decoder"));
            s.context = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(decoder), [](AVCodecContext *c) {
                avcodec_free_context(&c);
            });
            s.frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame *f) {
                av_frame_free(&f);
            });
            if (!s.context || !s.frame || avcodec_open2(s.context.get(), decoder, nullptr) < 0) {
                return fail(number, surface, codec, payload, QStringLiteral("could not open %1").arg(QLatin1String(decoder->name)));
            }
            m_decoderNames[surface] = QString::fromLatin1(decoder->name);
        }
        ++s.packets;
        AVPacket packet{};
        packet.data = reinterpret_cast<uint8_t *>(const_cast<char *>(payload.constData()));
        packet.size = int(payload.size());
        int result = avcodec_send_packet(s.context.get(), &packet);
        if (result < 0 && result != AVERROR(EAGAIN)) return fail(number, surface, codec, payload, QStringLiteral("packet rejected: %1").arg(errorString(result)));
        while ((result = avcodec_receive_frame(s.context.get(), s.frame.get())) >= 0) {
            ++s.pictures;
            s.lastPicture = QSize(s.frame->width, s.frame->height);
            av_frame_unref(s.frame.get());
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) return fail(number, surface, codec, payload, QStringLiteral("decode failed: %1").arg(errorString(result)));
        return true;
    }

    /// Surfaces that decoded fewer pictures than they were given packets (a decoder that drops
    /// a frame whose reference it never saw, as FFmpeg's HEVC decoder does, errs silently).
    QString missingPictures() const
    {
        QStringList missing;
        for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
            if (it->pictures != it->packets) missing << QStringLiteral("surface %1: %2 pictures from %3 packets").arg(it.key()).arg(it->pictures).arg(it->packets);
        }
        return missing.join(QStringLiteral("; "));
    }
    QString error() const { return m_error; }
    int packets() const { return m_packets; }
    const QHash<int, Surface> &surfaces() const { return m_surfaces; }
    QString decoderName(int surface) const { return m_decoderNames.value(surface); }

private:
    static QString errorString(int error)
    {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(error, buffer, sizeof buffer);
        return QString::fromUtf8(buffer);
    }
    bool fail(int number, int surface, VideoCodec codec, const QByteArray &payload, const QString &why)
    {
        m_error = QStringLiteral("packet %1 (surface %2's #%3, %4 bytes: %5) - %6")
                      .arg(number)
                      .arg(surface)
                      .arg(m_surfaces.value(surface).packets)
                      .arg(payload.size())
                      .arg(unitTypes(codec, payload), why);
        return false;
    }

    QHash<int, Surface> m_surfaces;
    QHash<int, QString> m_decoderNames;
    QString m_error;
    int m_packets = 0;
};
}
