#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
"""AUD-FIX10 fixture tool. Usage: craft-av1-render-size.py IN OUT WIDTH HEIGHT BIT (BIT = 14 for
the VA-API fixtures: the payload bit of render_and_frame_size_different in their OBU_FRAME_HEADER).

Insert an explicit AV1 render_size() into a hardware keyframe whose frame header (OBU_FRAME_HEADER)
has render_and_frame_size_different = 0 at payload bit 14 (as FFmpeg av1_vaapi on radeonsi writes it).
Adding exactly 33 bits (flag 1 bit set + 2x16) shifts the header by 32 bits, so trailing bits realign
and the header grows by exactly 4 bytes; the tile group OBU is untouched."""
import sys

def leb128(data, i):
    v = 0
    for n in range(8):
        b = data[i + n]
        v |= (b & 0x7f) << (7 * n)
        if not b & 0x80:
            return v, n + 1
    raise ValueError

def enc_leb128(v):
    out = bytearray()
    while True:
        b = v & 0x7f; v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)

src, dst, w, h, bit = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
data = open(src, 'rb').read()
out = bytearray(); i = 0; done = False
while i < len(data):
    hdr = data[i]; typ = (hdr >> 3) & 15; ext = (hdr >> 2) & 1
    assert hdr & 2, 'needs obu_has_size_field'
    hl = 1 + ext
    size, n = leb128(data, i + hl)
    payload = data[i + hl + n:i + hl + n + size]
    if typ == 3 and not done:
        bits = ''.join(f'{b:08b}' for b in payload)
        assert bits[bit] == '0', 'render_and_frame_size_different is not 0 at that bit'
        bits = bits[:bit] + '1' + f'{w - 1:016b}' + f'{h - 1:016b}' + bits[bit + 1:]
        assert len(bits) % 8 == 0  # 32 more bits: trailing_bits() keep their alignment
        payload = int(bits, 2).to_bytes(len(bits) // 8, 'big')
        done = True
    out += data[i:i + hl] + enc_leb128(len(payload)) + payload
    i += hl + n + size
assert done
open(dst, 'wb').write(out)
