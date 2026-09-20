// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QImage>
#include <QTextStream>

#include <cmath>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <freerdp/codec/color.h>
#include <freerdp/primitives.h>
}

using namespace Qt::StringLiterals;

namespace
{
struct Decoder {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool open() { ctx = avcodec_alloc_context3(codec); ctx->flags |= AV_CODEC_FLAG_LOW_DELAY; return avcodec_open2(ctx, codec, nullptr) >= 0; }
    // Feeds one picture (an Annex-B access unit); returns the decoded pictures it produced (0, 1 or, after a flush, more).
    int feed(const QByteArray &data, std::vector<AVFrame *> &out)
    {
        if (!data.isEmpty()) { pkt->data = reinterpret_cast<uint8_t *>(const_cast<char *>(data.constData())); pkt->size = data.size(); if (avcodec_send_packet(ctx, pkt) < 0) return -1; }
        else if (avcodec_send_packet(ctx, nullptr) < 0) return -1;
        int n = 0;
        while (avcodec_receive_frame(ctx, frame) >= 0) { out.push_back(av_frame_clone(frame)); av_frame_unref(frame); ++n; }
        return n;
    }
};

struct Yuv444 { int w, h; std::vector<uint8_t> y, u, v; explicit Yuv444(int width, int height) : w(width), h(height), y(size_t(width) * height), u(y.size()), v(y.size()) {} };

// The decoder's own reconstruction for one frame: LUMA combine from the main picture, CHROMA combine from the aux.
void combine(primitives_t *prims, const AVFrame *mainPic, const AVFrame *auxPic, avc444_frame_type chromaType, Yuv444 &out)
{
    BYTE *dst[3] = {out.y.data(), out.u.data(), out.v.data()};
    const UINT32 dstStep[3] = {UINT32(out.w), UINT32(out.w), UINT32(out.w)};
    RECTANGLE_16 roi{0, 0, UINT16(out.w), UINT16(out.h)};
    const BYTE *m[3] = {mainPic->data[0], mainPic->data[1], mainPic->data[2]};
    const UINT32 ms[3] = {UINT32(mainPic->linesize[0]), UINT32(mainPic->linesize[1]), UINT32(mainPic->linesize[2])};
    prims->YUV420CombineToYUV444(AVC444_LUMA, m, ms, out.w, out.h, dst, dstStep, &roi);
    if (auxPic) {
        const BYTE *a[3] = {auxPic->data[0], auxPic->data[1], auxPic->data[2]};
        const UINT32 as[3] = {UINT32(auxPic->linesize[0]), UINT32(auxPic->linesize[1]), UINT32(auxPic->linesize[2])};
        prims->YUV420CombineToYUV444(chromaType, a, as, out.w, out.h, dst, dstStep, &roi);
    }
}

QImage toRgb(primitives_t *prims, const Yuv444 &yuv)
{
    QImage img(yuv.w, yuv.h, QImage::Format_RGBX8888);
    const BYTE *src[3] = {yuv.y.data(), yuv.u.data(), yuv.v.data()};
    const UINT32 step[3] = {UINT32(yuv.w), UINT32(yuv.w), UINT32(yuv.w)};
    const prim_size_t roi{UINT32(yuv.w), UINT32(yuv.h)};
    prims->YUV444ToRGB_8u_P3AC4R(src, step, img.bits(), UINT32(img.bytesPerLine()), PIXEL_FORMAT_RGBX32, &roi);
    return img;
}

QImage toRgb420(primitives_t *prims, const AVFrame *pic, int w, int h)
{
    QImage img(w, h, QImage::Format_RGBX8888);
    const BYTE *src[3] = {pic->data[0], pic->data[1], pic->data[2]};
    const UINT32 step[3] = {UINT32(pic->linesize[0]), UINT32(pic->linesize[1]), UINT32(pic->linesize[2])};
    const prim_size_t roi{UINT32(w), UINT32(h)};
    prims->YUV420ToRGB_8u_P3AC4R(src, step, img.bits(), UINT32(img.bytesPerLine()), PIXEL_FORMAT_RGBX32, &roi);
    return img;
}

// Both images through the same RGB->YUV444 (libfreerdp's), PSNR per plane over the interior (8 px border dropped).
void psnr(primitives_t *prims, const QImage &a, const QImage &b, double out[3])
{
    const int w = a.width(), h = a.height();
    Yuv444 ya(w, h), yb(w, h);
    for (auto [img, yuv] : {std::pair{&a, &ya}, std::pair{&b, &yb}}) {
        BYTE *dst[3] = {yuv->y.data(), yuv->u.data(), yuv->v.data()};
        const UINT32 step[3] = {UINT32(w), UINT32(w), UINT32(w)};
        const prim_size_t roi{UINT32(w), UINT32(h)};
        prims->RGBToYUV444_8u_P3AC4R(img->constBits(), PIXEL_FORMAT_RGBX32, UINT32(img->bytesPerLine()), dst, step, &roi);
    }
    const std::vector<uint8_t> *pa[3] = {&ya.y, &ya.u, &ya.v}, *pb[3] = {&yb.y, &yb.u, &yb.v};
    for (int p = 0; p < 3; ++p) {
        double se = 0; qint64 n = 0;
        for (int y = 8; y < h - 8; ++y) for (int x = 8; x < w - 8; ++x) { const double d = double((*pa[p])[y * w + x]) - double((*pb[p])[y * w + x]); se += d * d; ++n; }
        const double mse = n ? se / n : 0.0;
        out[p] = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{u"frames"_s, u"<out>.frames index written by krdpplasmastreamer"_s, u"file"_s},
                       {u"main"_s, u"main H.264 stream (<out>.main.raw, or <out> for avc420)"_s, u"file"_s},
                       {u"aux"_s, u"aux H.264 stream (<out>.aux.raw)"_s, u"file"_s},
                       {u"codec"_s, u"avc420|avc444|avc444v2"_s, u"codec"_s, u"avc444v2"_s},
                       {u"size"_s, u"WIDTHxHEIGHT of the captured frames"_s, u"wxh"_s},
                       {u"reference"_s, u"raw RGBA reference (KPIPEWIRE_DUMP_RGBA) of the last frame"_s, u"file"_s},
                       {u"out"_s, u"PNG of the last decoded frame"_s, u"file"_s}});
    parser.process(app);
    const QStringList wh = parser.value(u"size"_s).split(u'x');
    if (wh.size() != 2) { qWarning() << "--size WxH required"; return 2; }
    const int w = wh[0].toInt(), h = wh[1].toInt();
    const QString codec = parser.value(u"codec"_s);
    const bool avc444 = codec != u"avc420"_s;
    QFile index(parser.value(u"frames"_s)), mainFile(parser.value(u"main"_s)), auxFile(parser.value(u"aux"_s));
    if (!index.open(QIODevice::ReadOnly) || !mainFile.open(QIODevice::ReadOnly) || (avc444 && !auxFile.open(QIODevice::ReadOnly))) { qWarning() << "cannot open inputs"; return 2; }

    primitives_t *prims = primitives_get();
    Decoder dec;
    if (!prims || !dec.open()) { qWarning() << "no primitives / no h264 decoder"; return 2; }

    // One decoder, pictures in wire order: main, then aux when the frame has one.
    std::vector<AVFrame *> pictures;
    std::vector<bool> hasAux, hasMain;
    QTextStream in(&index);
    while (!in.atEnd()) {
        const QStringList f = in.readLine().split(u' ', Qt::SkipEmptyParts);
        if (f.size() < 4) continue;
        const qint64 mainBytes = f[1].toLongLong(), auxBytes = f[2].toLongLong();
        if (mainBytes > 0 && dec.feed(mainFile.read(mainBytes), pictures) < 0) { qWarning() << "main picture" << f[0] << "failed to decode"; return 1; }
        if (avc444 && auxBytes > 0 && dec.feed(auxFile.read(auxBytes), pictures) < 0) { qWarning() << "aux picture" << f[0] << "failed to decode"; return 1; }
        hasAux.push_back(avc444 && auxBytes > 0);
        hasMain.push_back(mainBytes > 0);       // 0 for an aux-only refresh
    }
    dec.feed(QByteArray(), pictures); // flush
    size_t expected = 0; for (size_t i = 0; i < hasAux.size(); ++i) expected += (hasMain[i] ? 1 : 0) + (hasAux[i] ? 1 : 0);
    qInfo() << "frames" << hasAux.size() << "pictures decoded" << pictures.size() << "expected" << expected;
    if (pictures.size() != expected || pictures.empty()) return 1;

    // Walk to the last frame's pictures.
    size_t pos = 0; const AVFrame *lastMain = nullptr, *lastAux = nullptr;
    // The client keeps the last luma and applies the newest chroma, which is what an aux-only refresh relies on.
    for (size_t i = 0; i < hasAux.size(); ++i) { if (hasMain[i]) lastMain = pictures[pos++]; if (hasAux[i]) lastAux = pictures[pos++]; }
    if (!lastMain) { qWarning() << "no main picture in the run"; return 1; }
    if (lastMain->width < w || lastMain->height < h) { qWarning() << "decoded" << lastMain->width << "x" << lastMain->height << "smaller than --size"; return 1; }

    QImage decoded;
    if (avc444) {
        Yuv444 yuv(w, h);
        combine(prims, lastMain, lastAux, codec == u"avc444"_s ? AVC444_CHROMAv1 : AVC444_CHROMAv2, yuv);
        decoded = toRgb(prims, yuv);
    } else {
        decoded = toRgb420(prims, lastMain, w, h);
    }
    if (parser.isSet(u"out"_s)) decoded.save(parser.value(u"out"_s));
    if (parser.isSet(u"reference"_s)) {
        QFile ref(parser.value(u"reference"_s));
        if (!ref.open(QIODevice::ReadOnly) || ref.size() != qint64(w) * h * 4) { qWarning() << "reference size mismatch"; return 1; }
        const QByteArray bytes = ref.readAll();
        const QImage reference(reinterpret_cast<const uchar *>(bytes.constData()), w, h, w * 4, QImage::Format_RGBX8888);
        double p[3]; psnr(prims, decoded, reference, p);
        qInfo().noquote() << QStringLiteral("psnr Y=%1 U=%2 V=%3 lastFrameHasAux=%4").arg(p[0], 0, 'f', 2).arg(p[1], 0, 'f', 2).arg(p[2], 0, 'f', 2).arg(lastAux ? 1 : 0);
    }
    for (AVFrame *f : pictures) av_frame_free(&f);
    return 0;
}
