// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtEndian>

#include "ClipboardFileLimits.h"

namespace KRdp::ClipboardFileDescriptor
{
/**
 * OPT-078 (spec 4.3): the codec for FileGroupDescriptorW (MS-RDPECLIP 2.2.5.2.3), pure.
 *
 *   u32 cItems, then cItems * FILEDESCRIPTORW of 592 bytes:
 *     u32 flags; u8 clsid[16]; u32 sizel.cx, cy; u32 pointl.x, y; u32 fileAttributes;
 *     u64 ftCreationTime, ftLastAccessTime, ftLastWriteTime (FILETIME);
 *     u32 nFileSizeHigh, nFileSizeLow; WCHAR cFileName[260]
 *
 * A peer's descriptor is hostile input: counts, lengths and strings are all checked before
 * anything is allocated from them. cFileName is a FIXED 260 UTF-16 units including the NUL, so a
 * relative path on the wire is at most 259 units (a finding against spec 4.1's 4096 bytes: that is
 * only the ceiling of the internal worker wire). The decoder returns the name raw (unsanitized,
 * `\` separators, unpaired surrogates kept) for ClipboardFileNames::sanitize().
 */
/// The registered clipboard format the list travels as (MS-RDPECLIP 1.3.1.1.3), and the id this side gives it in its own lists.
inline const char *formatName()
{
    return "FileGroupDescriptorW";
}
constexpr quint32 FormatId = 0xC003;
inline bool isFormatName(const QString &name)
{
    return name.compare(QLatin1String("FileGroupDescriptorW"), Qt::CaseInsensitive) == 0;
}

constexpr int DescriptorBytes = 592;
constexpr int NameUnits = 260;
constexpr int MaxNameUnits = NameUnits - 1;
constexpr int NameOffset = DescriptorBytes - NameUnits * 2;

// FD_* flags
constexpr quint32 FdAttributes = 0x00000004;
constexpr quint32 FdWritesTime = 0x00000020;
constexpr quint32 FdFileSize = 0x00000040;
constexpr quint32 FdProgressUi = 0x00004000;
// FILE_ATTRIBUTE_*
constexpr quint32 AttrReadOnly = 0x01;
constexpr quint32 AttrDirectory = 0x10;
constexpr quint32 AttrNormal = 0x80;
constexpr quint32 AttrReparsePoint = 0x400;

enum class Error {
    TooShort, ///< fewer than 4 bytes
    TooLarge, ///< over MaxDescriptorWireBytes
    TooManyItems, ///< cItems over MaxEntries
    LengthMismatch, ///< the payload is not exactly 4 + cItems * 592 bytes
    UnterminatedName, ///< no NUL in cFileName
    EmptyName,
    NameTooLong, ///< (encoder) more than 259 UTF-16 units
    NameHasNul, ///< (encoder) a NUL inside a name
    BadSize, ///< (encoder) a negative or unrepresentable value
};

inline const char *errorName(Error error)
{
    switch (error) {
    case Error::TooShort: return "too-short";
    case Error::TooLarge: return "too-large";
    case Error::TooManyItems: return "too-many-items";
    case Error::LengthMismatch: return "length-mismatch";
    case Error::UnterminatedName: return "unterminated-name";
    case Error::EmptyName: return "empty-name";
    case Error::NameTooLong: return "name-too-long";
    case Error::NameHasNul: return "name-has-nul";
    case Error::BadSize: return "bad-size";
    }
    return "unknown";
}

struct Descriptor {
    QString name; ///< raw, `\` separators
    quint32 flags = 0;
    quint32 attributes = 0;
    std::optional<quint64> size; ///< FD_FILESIZE set; else unknown (ask with FILECONTENTS_SIZE)
    std::optional<quint64> writeTime; ///< FILETIME (100 ns since 1601-01-01), FD_WRITESTIME set
    bool isDirectory() const
    {
        return attributes & AttrDirectory;
    }
    bool isReparsePoint() const
    {
        return attributes & AttrReparsePoint;
    }
    bool operator==(const Descriptor &) const = default;
};

struct DecodeResult {
    std::optional<Error> error;
    QList<Descriptor> items;
    bool ok() const
    {
        return !error;
    }
};

inline DecodeResult decode(QByteArrayView data)
{
    DecodeResult result;
    const auto fail = [&result](Error error) {
        result.error = error;
        result.items.clear();
        return result;
    };
    if (data.size() < 4) return fail(Error::TooShort);
    if (data.size() > ClipboardFileLimits::MaxDescriptorWireBytes) return fail(Error::TooLarge);
    const quint32 count = qFromLittleEndian<quint32>(data.constData());
    if (count > ClipboardFileLimits::MaxEntries) return fail(Error::TooManyItems);
    // quint64 arithmetic: count is already <= 100000 but the check does not depend on it
    if (quint64(data.size()) != 4 + quint64(count) * DescriptorBytes) return fail(Error::LengthMismatch);
    result.items.reserve(qsizetype(count));
    for (quint32 i = 0; i < count; ++i) {
        const char *base = data.constData() + 4 + qsizetype(i) * DescriptorBytes;
        Descriptor item;
        item.flags = qFromLittleEndian<quint32>(base);
        // clsid 4..20, sizel 20..28, pointl 28..36
        item.attributes = qFromLittleEndian<quint32>(base + 36);
        const quint64 writeTime = qFromLittleEndian<quint64>(base + 56);
        const quint32 high = qFromLittleEndian<quint32>(base + 64);
        const quint32 low = qFromLittleEndian<quint32>(base + 68);
        if (!(item.flags & FdAttributes)) {
            item.attributes = 0; // the field is only meaningful with the flag
        }
        if (item.flags & FdFileSize) {
            item.size = (quint64(high) << 32) | low;
        }
        if (item.flags & FdWritesTime) {
            item.writeTime = writeTime;
        }
        const char *name = base + NameOffset;
        int length = -1;
        for (int u = 0; u < NameUnits; ++u) {
            if (qFromLittleEndian<quint16>(name + u * 2) == 0) {
                length = u;
                break;
            }
        }
        if (length < 0) return fail(Error::UnterminatedName);
        if (length == 0) return fail(Error::EmptyName);
        QString text(length, QChar());
        for (int u = 0; u < length; ++u) {
            text[u] = QChar(qFromLittleEndian<quint16>(name + u * 2));
        }
        item.name = text;
        result.items.append(item);
    }
    return result;
}

/// FILETIME of a Unix time (seconds), or nullopt before 1601 / after the 64-bit range.
inline std::optional<quint64> fileTimeFromUnixSeconds(qint64 seconds, quint32 nanoseconds = 0)
{
    constexpr qint64 EpochDelta = 11'644'473'600LL;
    if (seconds < -EpochDelta || seconds > (qint64(0x7fffffffffffffffLL) / 10'000'000) - EpochDelta - 1) {
        return std::nullopt;
    }
    return quint64(seconds + EpochDelta) * 10'000'000ULL + quint64(nanoseconds / 100);
}

/// Unix seconds of a FILETIME (floor), for display and `futimens`.
inline qint64 unixSecondsFromFileTime(quint64 fileTime)
{
    constexpr qint64 EpochDelta = 11'644'473'600LL;
    return qint64(fileTime / 10'000'000ULL) - EpochDelta;
}

/// What the encoder needs per entry. A directory carries size 0 and no content.
struct EncodeItem {
    QString name; ///< `\` separators, at most 259 UTF-16 units
    bool isDirectory = false;
    quint64 size = 0;
    std::optional<quint64> writeTime;
};

struct EncodeResult {
    std::optional<Error> error;
    QByteArray data;
    bool ok() const
    {
        return !error;
    }
};

inline EncodeResult encode(const QList<EncodeItem> &items)
{
    EncodeResult result;
    if (items.size() > qsizetype(ClipboardFileLimits::MaxEntries)) {
        result.error = Error::TooManyItems;
        return result;
    }
    QByteArray out(4 + items.size() * DescriptorBytes, '\0');
    qToLittleEndian<quint32>(quint32(items.size()), out.data());
    for (qsizetype i = 0; i < items.size(); ++i) {
        const EncodeItem &item = items.at(i);
        if (item.name.isEmpty()) {
            result.error = Error::EmptyName;
            return result;
        }
        if (item.name.size() > MaxNameUnits) {
            result.error = Error::NameTooLong;
            return result;
        }
        if (item.name.contains(QChar(0))) {
            result.error = Error::NameHasNul;
            return result;
        }
        char *base = out.data() + 4 + i * DescriptorBytes;
        quint32 flags = FdAttributes | FdFileSize | FdProgressUi;
        if (item.writeTime) flags |= FdWritesTime;
        qToLittleEndian<quint32>(flags, base);
        qToLittleEndian<quint32>(item.isDirectory ? AttrDirectory : AttrNormal, base + 36);
        if (item.writeTime) qToLittleEndian<quint64>(*item.writeTime, base + 56);
        const quint64 size = item.isDirectory ? 0 : item.size;
        qToLittleEndian<quint32>(quint32(size >> 32), base + 64);
        qToLittleEndian<quint32>(quint32(size & 0xffffffffu), base + 68);
        for (qsizetype u = 0; u < item.name.size(); ++u) {
            qToLittleEndian<quint16>(item.name.at(u).unicode(), base + NameOffset + u * 2);
        }
    }
    result.data = out;
    return result;
}
}
