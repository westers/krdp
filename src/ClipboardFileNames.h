// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>

#include "ClipboardFileLimits.h"

namespace KRdp::ClipboardFileNames
{
/**
 * OPT-078 (spec 4.1): the path and name sanitizer for clipboard file transfer, pure (QString only).
 *
 * Input is the relative path a peer sent in a FILEDESCRIPTORW (`cFileName`: UTF-16, `\` separators).
 * Hostile input is the normal case: a path that would leave the staging root, a name that
 * disguises its extension, a collision that would overwrite a file. The result is the only thing
 * ever used to create a file or to show a name; the original is never used as a path. Nothing here
 * puts a file name into an error or a log line (reasons are fixed enums).
 *
 * Whole-transfer rejection (a peer that sends such a name is hostile or broken): empty component, `.`
 * or `..`, an absolute path (leading `\` or `/`), a drive letter (`C:`), a UNC prefix, an alternate
 * data stream (`:`), NUL, any control character (U+0000-U+001F, U+007F, and the C1 controls
 * U+0080-U+009F), a `/` inside a component, a component over 255 UTF-8 bytes, a path over 4096 bytes,
 * more than 64 components, and format/bidi characters in the extension (the part after the last dot
 * of the last component). Format/bidi characters elsewhere become `_`.
 */
enum class Reject {
    EmptyComponent,
    DotComponent,
    AbsolutePath,
    UncPath,
    DriveLetter,
    StreamName,
    Nul,
    ControlCharacter,
    SlashInComponent,
    ComponentTooLong,
    PathTooLong,
    TooDeep,
    FormatInExtension,
    TooManyEntries,
    TotalPathBytes,
    TypeConflict, ///< a file entry is also used as a directory (or the reverse)
};

inline const char *rejectName(Reject reject)
{
    switch (reject) {
    case Reject::EmptyComponent: return "empty-component";
    case Reject::DotComponent: return "dot-component";
    case Reject::AbsolutePath: return "absolute-path";
    case Reject::UncPath: return "unc-path";
    case Reject::DriveLetter: return "drive-letter";
    case Reject::StreamName: return "stream-name";
    case Reject::Nul: return "nul";
    case Reject::ControlCharacter: return "control-character";
    case Reject::SlashInComponent: return "slash-in-component";
    case Reject::ComponentTooLong: return "component-too-long";
    case Reject::PathTooLong: return "path-too-long";
    case Reject::TooDeep: return "too-deep";
    case Reject::FormatInExtension: return "format-in-extension";
    case Reject::TooManyEntries: return "too-many-entries";
    case Reject::TotalPathBytes: return "total-path-bytes";
    case Reject::TypeConflict: return "type-conflict";
    }
    return "unknown";
}

struct Options {
    /// Also rename what Windows would refuse or alias (spec 4.1: client to host, so a Windows-origin set round-trips):
    /// reserved device names, trailing dots and spaces, `~1` aliases and `<>"|?*`.
    bool windowsSafe = false;
};

/// A code point that hides or reorders text, or has no visible form.
inline bool isFormatOrBidi(char32_t c)
{
    return c == 0x00AD || c == 0x061C || c == 0x180E || (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F)
        || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) || c == 0xFFFE || c == 0xFFFF || (c >= 0xE0000 && c <= 0xE007F);
}

inline bool isControl(char32_t c)
{
    return c <= 0x1F || c == 0x7F || (c >= 0x80 && c <= 0x9F);
}

/// The UTF-8 length of \a text without allocating (an unpaired surrogate counts as its 3-byte replacement).
inline qsizetype utf8Length(QStringView text)
{
    qsizetype bytes = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t u = text[i].unicode();
        if (u < 0x80) {
            bytes += 1;
        } else if (u < 0x800) {
            bytes += 2;
        } else if (QChar::isHighSurrogate(u) && i + 1 < text.size() && QChar::isLowSurrogate(text[i + 1].unicode())) {
            bytes += 4;
            ++i;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

namespace Detail
{
inline bool isWindowsReserved(const QString &component)
{
    // The stem (up to the first dot, trailing spaces ignored) names a device: CON, PRN, AUX, NUL, COM1-9, LPT1-9 (also with an extension).
    QString stem = component.left(component.indexOf(QLatin1Char('.')) < 0 ? component.size() : component.indexOf(QLatin1Char('.')));
    while (stem.endsWith(QLatin1Char(' '))) stem.chop(1);
    const QString upper = stem.toUpper();
    if (upper == QLatin1String("CON") || upper == QLatin1String("PRN") || upper == QLatin1String("AUX") || upper == QLatin1String("NUL")) return true;
    if (upper.size() == 4 && (upper.startsWith(QLatin1String("COM")) || upper.startsWith(QLatin1String("LPT")))) {
        const QChar d = upper.at(3);
        return d >= QLatin1Char('1') && d <= QLatin1Char('9');
    }
    return false;
}

/// "abcdef~1" or "abc~12.txt": an 8.3 alias (at most six name characters, a tilde, digits, an extension of at most three).
inline bool isShortAlias(const QString &component)
{
    const int dot = component.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot < 0 ? component : component.left(dot);
    const QString ext = dot < 0 ? QString() : component.mid(dot + 1);
    if (ext.size() > 3) return false;
    const int tilde = stem.lastIndexOf(QLatin1Char('~'));
    if (tilde < 1 || tilde > 6 || tilde == stem.size() - 1) return false;
    for (int i = tilde + 1; i < stem.size(); ++i) {
        if (stem.at(i) < QLatin1Char('0') || stem.at(i) > QLatin1Char('9')) return false;
    }
    return true;
}

inline QString foldKey(const QString &component)
{
    return component.normalized(QString::NormalizationForm_C).toCaseFolded().normalized(QString::NormalizationForm_C);
}
}

/// One sanitized component, or the reason the whole transfer is refused. \a isLast: the last component of an entry (its extension is checked).
struct ComponentResult {
    QString name;
    std::optional<Reject> reject;
    bool changed = false; ///< differs from the input (replacement, NFC, Windows rename)
};

inline ComponentResult sanitizeComponent(const QString &raw, bool isLast, const Options &options = {})
{
    ComponentResult result;
    if (raw.isEmpty()) {
        result.reject = Reject::EmptyComponent;
        return result;
    }
    if (raw == QLatin1String(".") || raw == QLatin1String("..")) {
        result.reject = Reject::DotComponent;
        return result;
    }
    // A component of unreasonable length is refused before any allocation is made from it.
    if (raw.size() > ClipboardFileLimits::MaxComponentBytes || utf8Length(raw) > ClipboardFileLimits::MaxComponentBytes) {
        // (a UTF-16 string longer than 255 units is always over 255 UTF-8 bytes)
        result.reject = Reject::ComponentTooLong;
        return result;
    }
    // Hard refusals first (control characters decide before anything is rewritten).
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const char16_t u = raw[i].unicode();
        if (u == 0) {
            result.reject = Reject::Nul;
            return result;
        }
        if (isControl(u)) {
            result.reject = Reject::ControlCharacter;
            return result;
        }
        if (u == u'/') {
            result.reject = Reject::SlashInComponent;
            return result;
        }
        if (u == u':') {
            result.reject = Reject::StreamName;
            return result;
        }
    }
    // The extension decides what a file manager does with it; format characters there are refused.
    const qsizetype lastDot = raw.lastIndexOf(QLatin1Char('.'));
    QString out;
    out.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size();) {
        char32_t cp = raw[i].unicode();
        qsizetype units = 1;
        bool unpaired = false;
        if (QChar::isHighSurrogate(raw[i].unicode()) && i + 1 < raw.size() && QChar::isLowSurrogate(raw[i + 1].unicode())) {
            cp = QChar::surrogateToUcs4(raw[i], raw[i + 1]);
            units = 2;
        } else if (QChar::isSurrogate(raw[i].unicode())) {
            unpaired = true;
        }
        const bool bad = unpaired || isFormatOrBidi(cp);
        if (bad) {
            if (isLast && lastDot >= 0 && i > lastDot) {
                result.reject = Reject::FormatInExtension;
                return result;
            }
            out.append(QLatin1Char('_'));
            result.changed = true;
        } else if (options.windowsSafe && (cp == '<' || cp == '>' || cp == '"' || cp == '|' || cp == '?' || cp == '*')) {
            out.append(QLatin1Char('_'));
            result.changed = true;
        } else {
            out.append(raw.mid(i, units));
        }
        i += units;
    }
    const QString normalized = out.normalized(QString::NormalizationForm_C);
    if (normalized != out) {
        out = normalized;
        result.changed = true;
    }
    if (options.windowsSafe) {
        const int before = out.size();
        while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
        if (out.isEmpty()) out = QStringLiteral("_");
        if (out.size() != before) result.changed = true;
        if (Detail::isWindowsReserved(out)) {
            out.prepend(QLatin1Char('_'));
            result.changed = true;
        }
        if (Detail::isShortAlias(out)) {
            out.replace(QLatin1Char('~'), QLatin1Char('_'));
            result.changed = true;
        }
    }
    if (utf8Length(out) > ClipboardFileLimits::MaxComponentBytes) {
        result.reject = Reject::ComponentTooLong; // NFC can grow a name
        return result;
    }
    result.name = out;
    return result;
}

/// One entry of a selection: the peer's path (`\` separators) and whether it names a directory.
struct RawEntry {
    QString path;
    bool isDirectory = false;
};

/// The sanitized form: relative components (never empty, never `.`/`..`), joined with `/` for the host.
struct Entry {
    QStringList components;
    QString relativePath; ///< components joined with '/'
    bool isDirectory = false;
    bool renamed = false; ///< sanitizing or collision handling changed any component
};

struct Result {
    std::optional<Reject> reject; ///< set: refuse the whole transfer, `entries` is empty
    QList<Entry> entries;
    bool ok() const
    {
        return !reject;
    }
};

namespace Detail
{
inline std::optional<Reject> splitPath(const QString &raw, QStringList &components)
{
    if (raw.isEmpty()) return Reject::EmptyComponent;
    if (raw.startsWith(QLatin1String("\\\\")) || raw.startsWith(QLatin1String("//")) || raw.startsWith(QLatin1String("\\/")) || raw.startsWith(QLatin1String("/\\"))) {
        return Reject::UncPath;
    }
    if (raw.startsWith(QLatin1Char('\\')) || raw.startsWith(QLatin1Char('/'))) return Reject::AbsolutePath;
    if (raw.size() >= 2 && raw.at(1) == QLatin1Char(':') && raw.at(0).isLetter() && raw.at(0).unicode() < 128) return Reject::DriveLetter;
    if (raw.size() > ClipboardFileLimits::MaxPathBytes || utf8Length(raw) > ClipboardFileLimits::MaxPathBytes) return Reject::PathTooLong;
    components = raw.split(QLatin1Char('\\'));
    if (components.size() > ClipboardFileLimits::MaxDepth) return Reject::TooDeep;
    return std::nullopt;
}

inline QString joinKey(const QStringList &parts, int count)
{
    return parts.mid(0, count).join(QLatin1Char('\\'));
}

/// "name (2).ext" with the stem shortened so the whole stays within the component limit.
inline QString numbered(const QString &name, int n)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    QString stem = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    const QString suffix = QStringLiteral(" (%1)").arg(n);
    while (!stem.isEmpty() && utf8Length(stem) + utf8Length(ext) + utf8Length(suffix) > ClipboardFileLimits::MaxComponentBytes) {
        stem.chop(1);
        if (!stem.isEmpty() && QChar::isHighSurrogate(stem.back().unicode())) stem.chop(1);
    }
    return stem + suffix + ext;
}
}

/**
 * Sanitizes a whole selection. Order and hierarchy are kept: a directory renamed because of a
 * collision carries its children with it. Case-insensitive and normalization-insensitive
 * collisions inside one transfer rename the later entry to `name (2).ext`, `(3)`, ... An entry
 * listed twice with the same directory path is merged; the same file path twice is a collision.
 */
inline Result sanitize(const QList<RawEntry> &entries, const Options &options = {})
{
    Result result;
    if (entries.size() > qsizetype(ClipboardFileLimits::MaxEntries)) {
        result.reject = Reject::TooManyEntries;
        return result;
    }
    // raw prefix (exact, original text) -> final components of that prefix, and whether it is a directory
    QHash<QString, QStringList> finalOfRaw;
    QHash<QString, bool> isDirOfRaw;
    // final parent key (folded) -> set of folded names already used in it
    QHash<QString, QSet<QString>> used;
    // (parent, folded wanted name) -> the last number tried, so N identical names cost O(N) and not O(N^2)
    QHash<QString, int> lastNumber;
    qint64 totalBytes = 0;
    const auto fail = [&result](Reject reject) {
        result.reject = reject;
        result.entries.clear();
        return result;
    };

    for (const RawEntry &entry : entries) {
        QStringList parts;
        if (const auto reject = Detail::splitPath(entry.path, parts)) {
            return fail(*reject);
        }
        QStringList finalParts;
        bool renamed = false;
        for (int i = 0; i < parts.size(); ++i) {
            const bool last = i == parts.size() - 1;
            const QString rawPrefix = Detail::joinKey(parts, i + 1);
            const bool wantDir = !last || entry.isDirectory;
            if (const auto known = finalOfRaw.constFind(rawPrefix); known != finalOfRaw.constEnd()) {
                // The same raw prefix again: directories merge; a file used as a directory or twice is a collision/conflict.
                if (!last) {
                    if (!isDirOfRaw.value(rawPrefix)) return fail(Reject::TypeConflict);
                    finalParts = *known;
                    continue;
                }
                if (isDirOfRaw.value(rawPrefix) && entry.isDirectory) {
                    finalParts = *known;
                    continue;
                }
                if (isDirOfRaw.value(rawPrefix) != entry.isDirectory) return fail(Reject::TypeConflict);
                // a second file with the same path: falls through and is renamed below (it gets a fresh identity)
            }
            const ComponentResult component = sanitizeComponent(parts.at(i), last, options);
            if (component.reject) {
                return fail(*component.reject);
            }
            const QString parentKey = Detail::joinKey(finalParts, finalParts.size());
            QSet<QString> &siblings = used[parentKey];
            QString name = component.name;
            if (siblings.contains(Detail::foldKey(name))) {
                int &counter = lastNumber[parentKey + QLatin1Char('\n') + Detail::foldKey(component.name)];
                counter = std::max(counter, 1);
                do {
                    name = Detail::numbered(component.name, ++counter);
                } while (siblings.contains(Detail::foldKey(name)));
                renamed = true;
            }
            siblings.insert(Detail::foldKey(name));
            renamed = renamed || component.changed;
            finalParts.append(name);
            if (!finalOfRaw.contains(rawPrefix)) {
                finalOfRaw.insert(rawPrefix, finalParts);
                isDirOfRaw.insert(rawPrefix, wantDir);
            }
        }
        Entry out;
        out.components = finalParts;
        out.relativePath = finalParts.join(QLatin1Char('/'));
        out.isDirectory = entry.isDirectory;
        out.renamed = renamed;
        const qint64 bytes = utf8Length(out.relativePath);
        if (bytes > ClipboardFileLimits::MaxPathBytes) return fail(Reject::PathTooLong);
        totalBytes += bytes;
        if (totalBytes > ClipboardFileLimits::MaxTotalPathBytes) return fail(Reject::TotalPathBytes);
        result.entries.append(out);
    }
    return result;
}
}
