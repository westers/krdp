// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QImage>
#include <QMimeData>
#include <QStringList>

namespace KRdp
{
/**
 * OPT-072: a clipboard offer that is one PNG.
 *
 * Measured on Plasma 6.6.6 / KF6 6.24 (evidence 2026-10-15-image-wayland-verify in ~/dev/rdp): KSystemClipboard's data-control
 * source, asked for image/png, writes the pixels of imageData() when hasImage() is true. A plain QMimeData that only did
 * setData("image/png", bytes) has no imageData, so every other process read an empty offer (the writer itself, reading its own
 * selection locally, saw the bytes, which hid the bug). This class answers both: image/png is the bytes as they came, and
 * application/x-qt-image decodes them lazily, on the paste that asks for it. Nothing is decoded while the offer only sits
 * on the clipboard. The caller has already validated the PNG (ClipboardImage::validatePng) before it gets here.
 */
class PngMimeData : public QMimeData
{
public:
    static constexpr const char *PngFormat = "image/png";
    static constexpr const char *QtImageFormat = "application/x-qt-image";

    explicit PngMimeData(QByteArray png, QByteArray passwordManagerHint = {})
        : m_png(std::move(png))
        , m_hint(std::move(passwordManagerHint))
    {
    }

    QStringList formats() const override
    {
        QStringList list{QString::fromLatin1(PngFormat), QString::fromLatin1(QtImageFormat)};
        if (!m_hint.isEmpty()) list.append(hintFormat());
        return list;
    }

    bool hasFormat(const QString &mimeType) const override { return formats().contains(mimeType); }

    static QString hintFormat() { return QStringLiteral("x-kde-passwordManagerHint"); }

protected:
    QVariant retrieveData(const QString &mimeType, QMetaType type) const override
    {
        if (mimeType == QLatin1String(PngFormat)) return m_png;
        if (mimeType == QLatin1String(QtImageFormat)) return QImage::fromData(m_png, "PNG");
        if (!m_hint.isEmpty() && mimeType == hintFormat()) return m_hint;
        return QMimeData::retrieveData(mimeType, type);
    }

private:
    QByteArray m_png;
    QByteArray m_hint;
};
}
