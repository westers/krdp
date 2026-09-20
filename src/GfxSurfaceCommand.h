// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <cmath>

#include <QByteArray>
#include <QSize>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/color.h>

#include "VideoCodecSupport.h"

namespace KRdp::GfxSurfaceCommand
{
// The FreeRDP structs a SurfaceCommand() call needs, owned for the lifetime of one build()/send.
// Non-copyable: they hold pointers into each other (command.extra, meta.regionRects/quantQualityVals).
struct Storage {
    RDPGFX_SURFACE_COMMAND command{};
    RDPGFX_AVC420_BITMAP_STREAM avc420{};
    RDPGFX_AVC444_BITMAP_STREAM avc444{};
    RECTANGLE_16 rect{};
    RDPGFX_H264_QUANT_QUALITY quant{};

    Storage() = default;
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
};

// Informational for the client (MS-RDPEGFX 2.2.4.4.1), but honest: the private KPipeWire's map
// (quality 100 -> QP 12, 0 -> QP 40).
inline quint8 qpForQuality(quint8 quality)
{
    return quint8(std::lround(40.0 - 0.28 * quality));
}

// numRegionRects (4) + RDP_RECT16 (8) and quantQualityVal (2) per rect.
inline uint32_t metablockBytes(uint32_t numRegionRects)
{
    return 4 + 10 * numRegionRects;
}

inline void fillMetablock(RDPGFX_H264_METABLOCK &meta, Storage &s)
{
    meta.numRegionRects = 1;
    meta.regionRects = &s.rect;
    meta.quantQualityVals = &s.quant;
}

// The encoder produces full-frame pictures (KPipeWire does not crop to damage), so one region
// rect covers the whole surface, as upstream and gnome-remote-desktop do for AVC420.
inline void build(Storage &s, VideoCodec codec, uint16_t surfaceId, const QSize &surfaceSize, const QByteArray &data, const QByteArray &aux, quint8 quality)
{
    s.rect = RECTANGLE_16{0, 0, static_cast<UINT16>(surfaceSize.width()), static_cast<UINT16>(surfaceSize.height())};
    s.quant = RDPGFX_H264_QUANT_QUALITY{};
    s.quant.qp = qpForQuality(quality);
    s.quant.qualityVal = quality;

    s.command = RDPGFX_SURFACE_COMMAND{};
    s.command.surfaceId = surfaceId;
    s.command.format = PIXEL_FORMAT_BGRX32;
    s.command.left = 0;
    s.command.top = 0;
    s.command.right = surfaceSize.width();
    s.command.bottom = surfaceSize.height();
    s.command.length = 0;
    s.command.data = nullptr;

    if (codec == VideoCodec::Avc420) {
        s.command.codecId = RDPGFX_CODECID_AVC420;
        s.avc420 = RDPGFX_AVC420_BITMAP_STREAM{};
        s.avc420.data = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(data.constData()));
        s.avc420.length = data.length();
        fillMetablock(s.avc420.meta, s);
        s.command.extra = &s.avc420;
        return;
    }

    s.command.codecId = VideoCodecSupport::rdpgfxCodecId(codec);
    s.avc444 = RDPGFX_AVC444_BITMAP_STREAM{};
    // LC 0 = luma + chroma, 1 = luma only, 2 = chroma only (the encoder's at-rest refresh: no main picture).
    // libfreerdp's server serialiser writes bitstream[0] always and bitstream[1] only for LC 0, so a
    // chroma-only frame carries its aux in bitstream[0].
    const bool chromaOnly = data.isEmpty() && !aux.isEmpty();
    const QByteArray &first = chromaOnly ? aux : data;
    auto &b0 = s.avc444.bitstream[0];
    b0.data = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(first.constData()));
    b0.length = first.length();
    fillMetablock(b0.meta, s);
    // MS-RDPEGFX 2.2.4.5: the byte count of the first RFX_AVC420_BITMAP_STREAM (metablock + bitstream).
    s.avc444.cbAvc420EncodedBitstream1 = metablockBytes(1) + uint32_t(first.length());
    s.avc444.LC = chromaOnly ? 2 : (aux.isEmpty() ? 1 : 0);
    if (s.avc444.LC == 0) {
        auto &chroma = s.avc444.bitstream[1];
        chroma.data = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(aux.constData()));
        chroma.length = aux.length();
        fillMetablock(chroma.meta, s);
    }
    s.command.extra = &s.avc444;
}
}
