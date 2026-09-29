// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "H264KeyframeSize.h"
#include <algorithm>
#include <QScopeGuard>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace KRdp
{
namespace
{
std::optional<EncodedKeyframe> h264Keyframe(const QByteArray &packet)
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
    const QSize coded(parser->coded_width, parser->coded_height);
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
    // SPS frame cropping is the display size (FFmpeg's parser and decoder apply it).
    return EncodedKeyframe{coded, dimensions, true};
}

/// MSB-first reader for AV1 headers (spec 4.10 f(n), uvlc(), leb128()).
class Av1Bits
{
public:
    Av1Bits(const uint8_t *data, qsizetype size)
        : m_data(data)
        , m_bits(size * 8)
    {
    }
    bool ok() const
    {
        return !m_overrun;
    }
    uint32_t f(int n)
    {
        uint32_t value = 0;
        for (int i = 0; i < n; ++i) {
            if (m_position >= m_bits) {
                m_overrun = true;
                return 0;
            }
            value = (value << 1) | ((m_data[m_position / 8] >> (7 - m_position % 8)) & 1);
            ++m_position;
        }
        return value;
    }
    uint32_t uvlc()
    {
        int zeros = 0;
        while (!f(1)) {
            if (!ok() || ++zeros >= 32) {
                m_overrun = true;
                return 0;
            }
        }
        return f(zeros) + (uint32_t(1) << zeros) - 1;
    }
    /// ns(n), spec 4.10.7: a value in 0..n-1.
    uint32_t ns(uint32_t n)
    {
        if (n <= 1) return 0;
        int w = 0;
        for (uint32_t x = n; x; x >>= 1) ++w; // FloorLog2(n) + 1
        const uint32_t m = (uint32_t(1) << w) - n;
        const uint32_t v = f(w - 1);
        if (v < m) return v;
        return (v << 1) - m + f(1);
    }

private:
    const uint8_t *m_data;
    qsizetype m_bits;
    qsizetype m_position = 0;
    bool m_overrun = false;
};

struct Av1Sequence {
    bool reduced = false;
    bool decoderModelInfo = false;
    bool equalPictureInterval = false;
    int bufferRemovalTimeLength = 0;
    int framePresentationTimeLength = 0;
    int operatingPoints = 0;
    uint32_t operatingPointIdc[32] = {};
    bool decoderModelPresent[32] = {};
    int frameWidthBits = 0;
    int frameHeightBits = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    bool frameIdNumbers = false;
    int frameIdLength = 0;
    int forceScreenContentTools = 0;
    int forceIntegerMv = 0;
    int orderHintBits = 0;
    bool superres = false;
    bool use128 = false;
};

// AV1 spec 5.5.1 sequence_header_obu(), up to enable_superres.
std::optional<Av1Sequence> av1Sequence(Av1Bits bits)
{
    Av1Sequence seq;
    if (bits.f(3) > 2) return {}; // seq_profile
    bits.f(1); // still_picture
    seq.reduced = bits.f(1);
    if (seq.reduced) {
        seq.operatingPoints = 1;
        bits.f(5); // seq_level_idx[0]
    } else {
        int bufferDelayLength = 0;
        if (bits.f(1)) { // timing_info_present_flag
            bits.f(32); // num_units_in_display_tick
            bits.f(32); // time_scale
            seq.equalPictureInterval = bits.f(1);
            if (seq.equalPictureInterval) bits.uvlc(); // num_ticks_per_picture_minus_1
            seq.decoderModelInfo = bits.f(1);
            if (seq.decoderModelInfo) {
                bufferDelayLength = int(bits.f(5)) + 1;
                bits.f(32); // num_units_in_decoding_tick
                seq.bufferRemovalTimeLength = int(bits.f(5)) + 1;
                seq.framePresentationTimeLength = int(bits.f(5)) + 1;
            }
        }
        const bool initialDisplayDelay = bits.f(1);
        seq.operatingPoints = int(bits.f(5)) + 1;
        for (int i = 0; i < seq.operatingPoints; ++i) {
            seq.operatingPointIdc[i] = bits.f(12);
            if (bits.f(5) > 7) bits.f(1); // seq_level_idx, seq_tier
            if (seq.decoderModelInfo) {
                seq.decoderModelPresent[i] = bits.f(1);
                if (seq.decoderModelPresent[i]) {
                    bits.f(bufferDelayLength); // decoder_buffer_delay
                    bits.f(bufferDelayLength); // encoder_buffer_delay
                    bits.f(1); // low_delay_mode_flag
                }
            }
            if (initialDisplayDelay && bits.f(1)) bits.f(4); // initial_display_delay_minus_1
        }
    }
    seq.frameWidthBits = int(bits.f(4)) + 1;
    seq.frameHeightBits = int(bits.f(4)) + 1;
    seq.maxWidth = int(bits.f(seq.frameWidthBits)) + 1;
    seq.maxHeight = int(bits.f(seq.frameHeightBits)) + 1;
    seq.frameIdNumbers = !seq.reduced && bits.f(1);
    if (seq.frameIdNumbers) {
        const int delta = int(bits.f(4)) + 2;
        seq.frameIdLength = int(bits.f(3)) + 1 + delta;
    }
    seq.use128 = bits.f(1); // use_128x128_superblock
    bits.f(1); // enable_filter_intra
    bits.f(1); // enable_intra_edge_filter
    seq.forceScreenContentTools = 2; // SELECT_SCREEN_CONTENT_TOOLS
    seq.forceIntegerMv = 2; // SELECT_INTEGER_MV
    if (!seq.reduced) {
        bits.f(4); // interintra, masked compound, warped motion, dual filter
        const bool orderHint = bits.f(1);
        if (orderHint) bits.f(2); // jnt_comp, ref_frame_mvs
        seq.forceScreenContentTools = bits.f(1) ? 2 : int(bits.f(1));
        if (seq.forceScreenContentTools > 0) seq.forceIntegerMv = bits.f(1) ? 2 : int(bits.f(1));
        if (orderHint) seq.orderHintBits = int(bits.f(3)) + 1;
    }
    seq.superres = bits.f(1);
    if (!bits.ok()) return {};
    return seq;
}

struct Av1FrameSize {
    QSize frame; // UpscaledWidth x FrameHeight: what a decoder outputs
    QSize render;
    bool renderSignalled = false;
    QSize tiles; // AV1-Q: TileCols x TileRows (tile_info()); empty when the header stops earlier
};

int tileLog2(int blockSize, int target)
{
    int k = 0;
    while ((blockSize << k) < target) ++k;
    return k;
}

// AV1 spec 5.9.15 tile_info(): TileCols x TileRows.
QSize av1TileInfo(Av1Bits &bits, bool use128, int frameWidth, int frameHeight)
{
    const int miCols = 2 * ((frameWidth + 7) >> 3);
    const int miRows = 2 * ((frameHeight + 7) >> 3);
    const int sbCols = use128 ? (miCols + 31) >> 5 : (miCols + 15) >> 4;
    const int sbRows = use128 ? (miRows + 31) >> 5 : (miRows + 15) >> 4;
    const int sbShift = use128 ? 5 : 4;
    const int sbSize = sbShift + 2;
    const int maxTileWidthSb = 4096 >> sbSize;
    int maxTileAreaSb = (4096 * 2304) >> (2 * sbSize);
    const int minLog2TileCols = tileLog2(maxTileWidthSb, sbCols);
    const int maxLog2TileCols = tileLog2(1, std::min(sbCols, 64));
    const int maxLog2TileRows = tileLog2(1, std::min(sbRows, 64));
    const int minLog2Tiles = std::max(minLog2TileCols, tileLog2(maxTileAreaSb, sbRows * sbCols));
    int tileCols = 0;
    int tileRows = 0;
    if (bits.f(1)) { // uniform_tile_spacing_flag
        int colsLog2 = minLog2TileCols;
        while (colsLog2 < maxLog2TileCols && bits.f(1)) ++colsLog2; // increment_tile_cols_log2
        const int tileWidthSb = (sbCols + (1 << colsLog2) - 1) >> colsLog2;
        for (int start = 0; start < sbCols; start += tileWidthSb) ++tileCols;
        int rowsLog2 = std::max(minLog2Tiles - colsLog2, 0);
        while (rowsLog2 < maxLog2TileRows && bits.f(1)) ++rowsLog2; // increment_tile_rows_log2
        const int tileHeightSb = (sbRows + (1 << rowsLog2) - 1) >> rowsLog2;
        for (int start = 0; start < sbRows; start += tileHeightSb) ++tileRows;
    } else {
        int widestTileSb = 0;
        for (int start = 0; start < sbCols && bits.ok(); ++tileCols) {
            const int sizeSb = int(bits.ns(uint32_t(std::min(sbCols - start, maxTileWidthSb)))) + 1; // width_in_sbs_minus_1
            widestTileSb = std::max(sizeSb, widestTileSb);
            start += sizeSb;
        }
        maxTileAreaSb = minLog2Tiles > 0 ? (sbRows * sbCols) >> (minLog2Tiles + 1) : sbRows * sbCols;
        const int maxTileHeightSb = std::max(maxTileAreaSb / std::max(widestTileSb, 1), 1);
        for (int start = 0; start < sbRows && bits.ok(); ++tileRows) {
            start += int(bits.ns(uint32_t(std::min(sbRows - start, maxTileHeightSb)))) + 1; // height_in_sbs_minus_1
        }
    }
    return bits.ok() ? QSize(tileCols, tileRows) : QSize();
}

// AV1 spec 5.9.2 uncompressed_header() of a shown KEY_FRAME, up to render_size().
std::optional<Av1FrameSize> av1KeyFrameSize(Av1Bits bits, const Av1Sequence &seq, int temporalId, int spatialId)
{
    int frameType = 0; // KEY_FRAME
    if (!seq.reduced) {
        if (bits.f(1)) return {}; // show_existing_frame
        frameType = int(bits.f(2));
        if (frameType != 0 || !bits.f(1)) return {}; // a key frame, shown
        if (seq.decoderModelInfo && !seq.equalPictureInterval) bits.f(seq.framePresentationTimeLength);
        // showable_frame is implied; error_resilient_mode = 1 for a shown key frame.
    }
    const bool disableCdfUpdate = bits.f(1);
    const int screenContentTools = seq.forceScreenContentTools == 2 ? int(bits.f(1)) : seq.forceScreenContentTools;
    if (screenContentTools && seq.forceIntegerMv == 2) bits.f(1); // force_integer_mv
    if (seq.frameIdNumbers) bits.f(seq.frameIdLength); // current_frame_id
    const bool sizeOverride = seq.reduced ? false : bool(bits.f(1));
    bits.f(seq.orderHintBits); // order_hint
    // primary_ref_frame is PRIMARY_REF_NONE for an intra frame.
    if (seq.decoderModelInfo && bits.f(1)) { // buffer_removal_time_present_flag
        for (int i = 0; i < seq.operatingPoints; ++i) {
            if (!seq.decoderModelPresent[i]) continue;
            const uint32_t idc = seq.operatingPointIdc[i];
            if (!idc || (((idc >> temporalId) & 1) && ((idc >> (spatialId + 8)) & 1))) bits.f(seq.bufferRemovalTimeLength);
        }
    }
    // refresh_frame_flags = allFrames for a shown key frame; no ref_order_hint.
    Av1FrameSize size;
    int width = seq.maxWidth;
    int height = seq.maxHeight;
    if (sizeOverride) {
        width = int(bits.f(seq.frameWidthBits)) + 1;
        height = int(bits.f(seq.frameHeightBits)) + 1;
    }
    // superres_params(): the decoder outputs UpscaledWidth, the size before the downscale.
    int codedWidth = width; // FrameWidth
    if (seq.superres && bits.f(1)) {
        const int denominator = int(bits.f(3)) + 9;
        codedWidth = (width * 8 + denominator / 2) / denominator;
    }
    size.frame = QSize(width, height);
    size.renderSignalled = bits.f(1); // render_and_frame_size_different
    size.render = size.frame;
    if (size.renderSignalled) {
        const int renderWidth = int(bits.f(16)) + 1;
        size.render = QSize(renderWidth, int(bits.f(16)) + 1);
    }
    if (!bits.ok()) return {};
    // AV1-Q: on to tile_info(). An intra frame has no reference setup in between.
    if (screenContentTools && codedWidth == width) bits.f(1); // allow_intrabc
    if (!seq.reduced && !disableCdfUpdate) bits.f(1); // disable_frame_end_update_cdf
    Av1Bits tileBits = bits;
    const QSize tiles = av1TileInfo(tileBits, seq.use128, codedWidth, height);
    if (tileBits.ok()) size.tiles = tiles;
    return size;
}

/// The one sequence header and the first frame header of a low-overhead AV1 temporal unit.
std::optional<Av1FrameSize> av1TemporalUnitSize(const QByteArray &packet)
{
    const auto *data = reinterpret_cast<const uint8_t *>(packet.constData());
    const qsizetype size = packet.size();
    std::optional<Av1Sequence> sequence;
    int sequences = 0;
    for (qsizetype i = 0; i < size;) {
        const uint8_t header = data[i];
        if (header & 0x80) return {}; // obu_forbidden_bit
        const int type = (header >> 3) & 0xf;
        const bool extension = header & 0x4;
        if (!(header & 0x2)) return {}; // obu_has_size_field: KPipeWire and FFmpeg always write it
        qsizetype at = i + 1;
        int temporalId = 0;
        int spatialId = 0;
        if (extension) {
            if (at >= size) return {};
            temporalId = data[at] >> 5;
            spatialId = (data[at] >> 3) & 3;
            ++at;
        }
        uint64_t obuSize = 0;
        int n = 0;
        for (;; ++n) {
            if (n == 8 || at + n >= size) return {};
            obuSize |= uint64_t(data[at + n] & 0x7f) << (7 * n);
            if (!(data[at + n] & 0x80)) break;
        }
        at += n + 1;
        if (obuSize > uint64_t(size - at)) return {};
        const Av1Bits payload(data + at, qsizetype(obuSize));
        if (type == 1) { // OBU_SEQUENCE_HEADER
            if (++sequences > 1) return {};
            sequence = av1Sequence(payload);
            if (!sequence) return {};
        } else if (type == 3 || type == 6) { // OBU_FRAME_HEADER, OBU_FRAME
            return sequence ? av1KeyFrameSize(payload, *sequence, temporalId, spatialId) : std::nullopt;
        } else if (type != 2 && type != 5 && type != 15) { // temporal delimiter, metadata, padding
            return {};
        }
        i = at + qsizetype(obuSize);
    }
    return {};
}
}

std::optional<QSize> av1KeyframeTiles(const QByteArray &packet)
{
    const auto frame = av1TemporalUnitSize(packet);
    if (!frame || frame->tiles.isEmpty()) return {};
    return frame->tiles;
}

std::optional<QSize> h264KeyframeSize(const QByteArray &packet)
{
    const auto keyframe = h264Keyframe(packet);
    return keyframe ? std::optional(keyframe->display) : std::nullopt;
}

std::optional<EncodedKeyframe> encodedKeyframe(VideoCodec codec, const QByteArray &packet)
{
    if (codec != VideoCodec::Hevc && codec != VideoCodec::Av1) return h264Keyframe(packet);
    if (packet.isEmpty() || packet.size() > 16 * 1024 * 1024) return {};
    const AVCodecID id = codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_AV1;
    AVCodecParserContext *parser = av_parser_init(id);
    const AVCodec *decoder = avcodec_find_decoder(id);
    AVCodecContext *context = avcodec_alloc_context3(decoder); // a null codec gives a parser-only context
    AVFrame *frame = av_frame_alloc();
    AVPacket *input = av_packet_alloc();
    const auto cleanup = qScopeGuard([&] {
        av_packet_free(&input); av_frame_free(&frame);
        if (parser) av_parser_close(parser);
        avcodec_free_context(&context);
    });
    if (!parser || !context || !frame || !input) return {};
    context->max_pixels = 4096LL * 4096;
    context->thread_count = 1;
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    QByteArray padded = packet;
    padded.append(QByteArray(AV_INPUT_BUFFER_PADDING_SIZE, '\0'));
    uint8_t *parsed = nullptr;
    int parsedSize = 0;
    const int consumed = av_parser_parse2(parser, context, &parsed, &parsedSize,
        reinterpret_cast<const uint8_t *>(padded.constData()), int(packet.size()), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    // Some parsers only fill the codec context's dimensions.
    const int width = parser->width > 0 ? parser->width : context->width;
    const int height = parser->height > 0 ? parser->height : context->height;
    if (consumed != packet.size() || !parsed || parsedSize <= 0 || parser->key_frame != 1
        || width < 320 || width > 4096 || height < 200 || height > 4096 || width % 2 || height % 2
        || (parser->format >= 0 && parser->format != AV_PIX_FMT_YUV420P && parser->format != AV_PIX_FMT_YUVJ420P
            && parser->format != AV_PIX_FMT_NV12)) return {};
    const QSize dimensions(width, height);
    EncodedKeyframe result{dimensions, dimensions, true};
    if (codec == VideoCodec::Hevc) {
        // FFmpeg's HEVC parser reports the conformance window; the coded picture is the SPS's.
        const int codedWidth = parser->coded_width > 0 ? parser->coded_width : context->coded_width;
        const int codedHeight = parser->coded_height > 0 ? parser->coded_height : context->coded_height;
        if (codedWidth >= width && codedHeight >= height) result.coded = QSize(codedWidth, codedHeight);
    } else {
        // FFmpeg's AV1 parser and decoders report the frame size; the render size is only in the
        // frame header. Both headers must be readable and agree with FFmpeg.
        const auto av1 = av1TemporalUnitSize(packet);
        if (!av1 || av1->frame != dimensions || av1->render.width() % 2 || av1->render.height() % 2
            || av1->render.width() < 320 || av1->render.height() < 200) return {};
        result.display = av1->render;
        result.displaySignalled = av1->renderSignalled;
    }
    if (!decoder || avcodec_open2(context, decoder, nullptr) < 0) {
        return result; // no software decoder here (an FFmpeg without dav1d): the parser's word
    }
    if (av_new_packet(input, int(packet.size())) < 0) return {};
    std::memcpy(input->data, packet.constData(), packet.size());
    if (avcodec_send_packet(context, input) < 0) return {};
    int received = avcodec_receive_frame(context, frame);
    if (received == AVERROR(EAGAIN)) {
        if (avcodec_send_packet(context, nullptr) < 0) return {};
        received = avcodec_receive_frame(context, frame);
    }
    if (received < 0 || frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)
        || QSize(frame->width, frame->height) != dimensions) return {};
    return result;
}

std::optional<QSize> encodedKeyframeSize(VideoCodec codec, const QByteArray &packet)
{
    const auto keyframe = encodedKeyframe(codec, packet);
    return keyframe ? std::optional(keyframe->display) : std::nullopt;
}

QSize encodedKeyframeAlignment(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::Av1:
        return {64, 16};
    case VideoCodec::Hevc:
        return {64, 64}; // the largest CTB (not used: the conformance window is always the display size)
    default:
        return {16, 16}; // macroblocks (not used: SPS cropping is always the display size)
    }
}

bool encodedKeyframeShows(VideoCodec codec, const QByteArray &packet, QSize pixels)
{
    if (pixels.isEmpty()) return false;
    const auto keyframe = encodedKeyframe(codec, packet);
    if (!keyframe) return false;
    if (keyframe->display == pixels) return true;
    if (keyframe->displaySignalled || keyframe->display != keyframe->coded) return false;
    // Residual risk, only for such streams: a keyframe of another size in the same alignment band
    // (a stale 1920x1080 encoder's 1082 rows for a 1920x1076 Fit) also passes.
    const QSize alignment = encodedKeyframeAlignment(codec);
    const auto within = [](int coded, int wanted, int alignment) {
        return coded >= wanted && coded <= (wanted + alignment - 1) / alignment * alignment;
    };
    return within(keyframe->coded.width(), pixels.width(), alignment.width())
        && within(keyframe->coded.height(), pixels.height(), alignment.height());
}
}
