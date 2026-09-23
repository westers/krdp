// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "H264KeyframeSize.h"
#include <QScopeGuard>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace KRdp
{
std::optional<QSize> h264KeyframeSize(const QByteArray &packet)
{
    if (packet.isEmpty() || packet.size() > 16 * 1024 * 1024) return {};
    // Reject ambiguous parameter sets or inter pictures. The fixed AVC420
    // worker emits Annex-B access units with parameter sets on an IDR.
    int sps = 0, pps = 0, slices = 0, nals = 0, firstSlices = 0;
    qsizetype start = -1;
    for (qsizetype i = 0; i + 3 < packet.size(); ++i) {
        if (packet[i] != 0 || packet[i + 1] != 0 || packet[i + 2] != 1) continue;
        if (start < 0) {
            for (qsizetype j = 0; j < i; ++j) if (packet[j] != 0) return {};
        }
        start = i;
        if (++nals > 1024) return {};
        const auto header = static_cast<unsigned char>(packet[i + 3]);
        if (header & 0x80) return {};
        switch (header & 0x1f) {
        case 7: if (++sps != 1 || pps || slices) return {}; break;
        case 8: if (++pps != 1 || sps != 1 || slices) return {}; break;
        case 5:
            if (sps != 1 || pps != 1 || ++slices > 256 || i + 4 >= packet.size()) return {};
            // first_mb_in_slice is the first unsigned Exp-Golomb value. Its
            // first bit is one exactly for zero. Every picture starts at MB0;
            // permit later slices, never a second picture in this access unit.
            if (static_cast<unsigned char>(packet[i + 4]) & 0x80) ++firstSlices;
            if (firstSlices != 1) return {};
            break;
        case 6: case 9: case 12: break; // SEI, access-unit delimiter, filler.
        default: return {};
        }
        i += 3;
    }
    if (sps != 1 || pps != 1 || slices == 0) return {};

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return {};
    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    AVFrame *frame = av_frame_alloc();
    AVPacket *input = av_packet_alloc();
    const auto cleanup = qScopeGuard([&] {
        av_packet_free(&input); av_frame_free(&frame);
        if (parser) av_parser_close(parser);
        avcodec_free_context(&context);
    });
    if (!context || !parser || !frame || !input) return {};
    context->max_pixels = 4096LL * 4096;
    context->thread_count = 1;
    context->err_recognition = AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    // Keep FFmpeg's error-resilience bookkeeping enabled. Disabling it marks
    // valid sliced-thread x264 pictures with FF_DECODE_ERROR_DECODE_SLICES.
    // Repaired pictures are still rejected below via decode_error_flags.
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    QByteArray padded = packet;
    padded.append(QByteArray(AV_INPUT_BUFFER_PADDING_SIZE, '\0'));
    uint8_t *parsed = nullptr;
    int parsedSize = 0;
    const int consumed = av_parser_parse2(parser, context, &parsed, &parsedSize,
        reinterpret_cast<const uint8_t *>(padded.constData()), int(packet.size()), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (consumed != packet.size() || parsedSize != packet.size() || !parsed || parser->key_frame != 1
        || parser->width < 320 || parser->width > 4096 || parser->height < 200 || parser->height > 4096
        || parser->coded_width < parser->width || parser->coded_width > 4096
        || parser->coded_height < parser->height || parser->coded_height > 4096
        || (parser->format != AV_PIX_FMT_YUV420P && parser->format != AV_PIX_FMT_YUVJ420P)
        || parser->width % 2 || parser->height % 2 || parser->picture_structure != AV_PICTURE_STRUCTURE_FRAME) return {};
    const QSize dimensions(parser->width, parser->height);
    // The parser exposes dimensions even on some later slice errors. Decode
    // this one candidate and reject every reported decode/concealment error;
    // do not treat a plausible SPS alone as a complete, correctly-sized frame.
    if (avcodec_open2(context, codec, nullptr) < 0 || av_new_packet(input, int(packet.size())) < 0) return {};
    std::memcpy(input->data, packet.constData(), packet.size());
    if (avcodec_send_packet(context, input) < 0) return {};
    int received = avcodec_receive_frame(context, frame);
    bool drained = false;
    if (received == AVERROR(EAGAIN)) {
        if (avcodec_send_packet(context, nullptr) < 0) return {};
        drained = true;
        received = avcodec_receive_frame(context, frame);
    }
    if (received < 0 || frame->decode_error_flags || !(frame->flags & AV_FRAME_FLAG_KEY)
        || (frame->flags & AV_FRAME_FLAG_CORRUPT)
        || (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        || QSize(frame->width, frame->height) != dimensions) return {};
    if (!drained && avcodec_send_packet(context, nullptr) < 0) return {};
    if (avcodec_receive_frame(context, frame) != AVERROR_EOF) return {};
    return dimensions;
}
}
