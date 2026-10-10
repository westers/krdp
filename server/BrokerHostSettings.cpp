// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostSettings.h"
#include "CodecPolicy.h"
#include "VaapiDriverMode.h"
#include "VirtualSessionLaunchPlan.h"
#include <QHostAddress>
#include <QRegularExpression>
#include <QStringDecoder>

using namespace Qt::StringLiterals;
namespace KRdp::BrokerHostSettings {
namespace {
struct Field { QString key; const char *suffix; const char *value; };
QList<Field> fields(Scope scope)
{
    if (scope == Scope::VirtualSession)
        return {{u"RenderPci"_s, "RENDER_PCI", ""}, {u"VaapiDriver"_s, "VAAPI_DRIVER", "auto"}};
    if (scope != Scope::Console && scope != Scope::Virtual) return {};
    QList<Field> result{{u"Address"_s, "ADDRESS", "0.0.0.0"},
        {u"Port"_s, "PORT", scope == Scope::Console ? "3391" : "3395"},
        {u"Certificate"_s, "CERTIFICATE", scope == Scope::Console ? "/etc/farside/console.crt" : "/etc/farside/virtual-host.crt"},
        {u"CertificateKey"_s, "CERTIFICATE_KEY", scope == Scope::Console ? "/etc/farside/console.key" : "/etc/farside/virtual-host.key"},
        {u"Quality"_s, "QUALITY", "80"}, {u"AdaptiveQuality"_s, "ADAPTIVE_QUALITY", "false"},
        {u"PreferAudioQuality"_s, "PREFER_AUDIO_QUALITY", "false"},
        {u"StandardClientMedia"_s, "STANDARD_CLIENT_MEDIA", "true"},
        {u"CameraLoopbackDevice"_s, "CAMERA_LOOPBACK_DEVICE", "none"},
        {u"SoftwareEncoding"_s, "SOFTWARE_ENCODING", "auto"},
        {u"SoftwareAvc"_s, "SOFTWARE_AVC", "auto"}, {u"SoftwareHevc"_s, "SOFTWARE_HEVC", "auto"}, {u"SoftwareAv1"_s, "SOFTWARE_AV1", "auto"},
        {u"Av1Tiles"_s, "AV1_TILES", "auto"}};
    if (scope == Scope::Console) result.append({u"VaapiDriver"_s, "VAAPI_DRIVER", "auto"});
    return result;
}
bool horizontal(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool whitespace(char c) { return horizontal(c) || c == '\n'; }
bool printable(const QString &value)
{
    for (auto c : value) if (c.unicode() < 32 || c.unicode() == 127 || c == QChar(0xfeff)) return false;
    return true;
}
struct Assignment { qsizetype begin; qsizetype end; QString name; QString value; };
struct Document { QList<Assignment> assignments; QString error; };
Document scan(const QByteArray &contents)
{
    Document result;
    if (contents.size() > MaximumBytes) { result.error = u"host configuration is oversized"_s; return result; }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::ConvertInitialBom);
    const QString decoded = decoder(contents);
    if (decoder.hasError()) { result.error = u"host configuration is not UTF-8"_s; return result; }
    for (char32_t c : decoded.toUcs4()) {
        if (!c || c == 0xfeff || (c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffff) == 0xfffe || (c & 0xffff) == 0xffff) {
            result.error = u"host configuration contains invalid characters"_s; return result;
        }
    }
    qsizetype pos = 0;
    // EnvironmentFile semantics, not shell or KConfig: literal $, comments only
    // at the start of a statement, quoted multiline values, unquoted interior
    // whitespace, and backslash/newline continuation. Track complete byte spans
    // so unknown statements are never reconstructed or exposed to the UI.
    while (pos < contents.size()) {
        const auto begin = pos;
        while (pos < contents.size() && horizontal(contents[pos])) ++pos;
        if (pos == contents.size()) break;
        if (contents[pos] == '\n' || contents[pos] == '#' || contents[pos] == ';') {
            while (pos < contents.size() && contents[pos++] != '\n') {}
            continue;
        }
        const auto keyBegin = pos;
        while (pos < contents.size() && contents[pos] != '=' && contents[pos] != '\n') ++pos;
        if (pos == contents.size() || contents[pos] == '\n') { if (pos < contents.size()) ++pos; continue; }
        auto keyEnd = pos;
        while (keyEnd > keyBegin && horizontal(contents[keyEnd - 1])) --keyEnd;
        const auto name = QString::fromUtf8(contents.mid(keyBegin, keyEnd - keyBegin));
        ++pos;
        enum State { Before, Plain, Escape, Single, Double, DoubleEscape } state = Before;
        QByteArray value;
        qsizetype trailing = -1;
        bool complete = false;
        while (pos < contents.size()) {
            const char c = contents[pos++];
            if ((state == Before || state == Plain) && c == '\n') { complete = true; break; }
            switch (state) {
            case Before:
                if (c == '\'') state = Single;
                else if (c == '"') state = Double;
                else if (c == '\\') { state = Escape; trailing = -1; }
                else if (!whitespace(c)) { state = Plain; value += c; trailing = -1; }
                break;
            case Plain:
                if (c == '\\') { state = Escape; trailing = -1; }
                else {
                    if (!whitespace(c)) trailing = -1;
                    else if (trailing < 0) trailing = value.size();
                    value += c;
                }
                break;
            case Escape:
                state = Plain;
                if (c != '\n') value += c;
                break;
            case Single:
                if (c == '\'') state = Before;
                else value += c;
                break;
            case Double:
                if (c == '"') state = Before;
                else if (c == '\\') state = DoubleEscape;
                else value += c;
                break;
            case DoubleEscape:
                state = Double;
                if (c == '"' || c == '\\' || c == '$' || c == '`') value += c;
                else if (c != '\n') { value += '\\'; value += c; }
                break;
            }
        }
        // systemd accepts some malformed EOF forms; an editor must refuse to
        // append to an unterminated value and accidentally change its meaning.
        if (!complete && (state == Single || state == Double || state == DoubleEscape || state == Escape)) {
            result.error = u"host configuration has an unfinished quote or continuation"_s; return result;
        }
        if (state == Plain && trailing >= 0) value.truncate(trailing);
        result.assignments.append({begin, pos, name, QString::fromUtf8(value)});
    }
    return result;
}
QString fieldError(const QString &key) { return u"invalid host setting: "_s + key; }
QByteArray quoted(const QString &value)
{
    QByteArray result("\"");
    for (char c : value.toUtf8()) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') result += '\\';
        result += c;
    }
    return result + '"';
}
}

QString fileName(Scope scope)
{
    switch (scope) {
    case Scope::Console: return u"console-host.conf"_s;
    case Scope::Virtual: return u"virtual-host.conf"_s;
    case Scope::VirtualSession: return u"virtual-session.conf"_s;
    }
    return {};
}
QStringList keys(Scope scope)
{
    QStringList result;
    for (const auto &field : fields(scope)) result.append(field.key);
    return result;
}
QVariantMap defaults(Scope scope)
{
    QVariantMap result;
    for (const auto &field : fields(scope)) result.insert(field.key, QString::fromLatin1(field.value));
    return result;
}
QString environmentName(Scope scope, const QString &key)
{
    for (const auto &field : fields(scope)) {
        if (field.key == key) return (scope == Scope::Console ? u"FARSIDE_CONSOLE_"_s : u"FARSIDE_VIRTUAL_"_s)
            + QString::fromLatin1(field.suffix);
    }
    return {};
}
std::optional<QString> normalize(Scope scope, const QString &key, const QString &value)
{
    if (environmentName(scope, key).isEmpty() || value.size() > MaximumValue || !printable(value)) return {};
    if (key == u"Address") {
        QHostAddress address;
        if (value.trimmed() == value && address.setAddress(value) && !address.isNull()) return address.toString();
    } else if (key == u"Port" || key == u"Quality") {
        static const QRegularExpression digits(u"^[0-9]{1,5}$"_s);
        if (!digits.match(value).hasMatch()) return {};
        const int number = value.toInt();
        if ((key == u"Port" && number >= 1 && number <= 65535) || (key == u"Quality" && number <= 100)) return QString::number(number);
    } else if (key == u"Certificate" || key == u"CertificateKey") {
        if (VirtualSessionLaunchPlan::absoluteCleanPath(value)) return value;
    } else if (key == u"CameraLoopbackDevice") {
        if (value == u"none" || (VirtualSessionLaunchPlan::absoluteCleanPath(value) && value.startsWith(u"/dev/"))) return value;
    } else if (key == u"AdaptiveQuality" || key == u"PreferAudioQuality" || key == u"StandardClientMedia") {
        const auto v = value.toLower();
        if (v == u"true" || v == u"false") return v;
    } else if (key == u"SoftwareEncoding") {
        if (value.trimmed().isEmpty()) return u"auto"_s;
        if (const auto mode = CodecPolicy::parseSoftwareEncoding(value)) return QString::fromLatin1(CodecPolicy::softwareEncodingName(*mode));
    } else if (key == u"SoftwareAvc" || key == u"SoftwareHevc" || key == u"SoftwareAv1") {
        // OPT-062 S3: the host's software ceiling per codec. auto follows SoftwareEncoding (never -> restricted).
        const auto family = key == u"SoftwareAvc" ? CodecPolicy::Family::Avc : key == u"SoftwareHevc" ? CodecPolicy::Family::Hevc : CodecPolicy::Family::Av1;
        if (const auto ceiling = CodecPolicy::parseCeilingSetting(value, family)) return QString::fromLatin1(CodecPolicy::ceilingSettingName(*ceiling, family));
    } else if (key == u"Av1Tiles") {
        if (const auto tiles = CodecPolicy::parseAv1Tiles(value)) return *tiles ? QString::number(*tiles) : u"auto"_s;
    } else if (key == u"VaapiDriver") {
        return VaapiDriverMode::normalize(value);
    } else if (key == u"RenderPci") {
        if (value.isEmpty()) return value; // no grant; not permission to use all GPUs
        const auto devices = value.split(u',');
        if (devices.size() > 16) return {};
        static const QRegularExpression pci(u"^[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\\.[0-7]$"_s);
        QStringList unique;
        for (const auto &device : devices) {
            if (!pci.match(device).hasMatch() || unique.contains(device)) return {};
            unique.append(device);
        }
        return unique.join(u',');
    }
    return {};
}
Snapshot parse(Scope scope, const QByteArray &contents)
{
    Snapshot result;
    if (fileName(scope).isEmpty()) { result.error = u"invalid host settings scope"_s; return result; }
    const auto document = scan(contents);
    if (!document.error.isEmpty()) { result.error = document.error; return result; }
    QMap<QString, QString> values;
    for (const auto &assignment : document.assignments) values.insert(assignment.name, assignment.value);
    result.effective = defaults(scope);
    for (const auto &key : keys(scope)) {
        const auto name = environmentName(scope, key);
        if (!values.contains(name)) continue;
        const auto value = normalize(scope, key, values[name]);
        // The brokers accept only canonical boolean text. Drafts may normalize
        // case before writing, but an existing invalid file cannot be described
        // as working when it would prevent the real host from starting.
        const bool boolean = key == u"AdaptiveQuality" || key == u"PreferAudioQuality" || key == u"StandardClientMedia";
        if (!value || (boolean && values[name] != *value)) {
            result.error = fieldError(key); result.overrides.clear(); result.effective.clear(); return result;
        }
        result.overrides.insert(key, *value);
        result.effective.insert(key, *value);
    }
    if (scope != Scope::VirtualSession && result.effective[u"Certificate"_s] == result.effective[u"CertificateKey"_s]) {
        result.error = u"certificate and private key must have distinct paths"_s;
        result.overrides.clear(); result.effective.clear();
    }
    return result;
}
EnvironmentRead privateEnvironment(const QByteArray &contents)
{
    const auto document = scan(contents);
    EnvironmentRead result;
    result.error = document.error;
    if (!result.error.isEmpty()) return result;
    static const QRegularExpression name(u"\\A[A-Za-z_][A-Za-z0-9_]*\\z"_s);
    for (const auto &item : document.assignments)
        if (name.match(item.name).hasMatch()) result.assignments[item.name] = item.value;
    return result;
}
EditResult edit(Scope scope, const QByteArray &original, const QVariantMap &desired)
{
    const auto current = parse(scope, original);
    if (!current.error.isEmpty()) return {{}, current.error};
    QVariantMap normalized;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        // No caller-provided unknown key or nonstring value enters diagnostics.
        if (environmentName(scope, it.key()).isEmpty() || it.value().metaType().id() != QMetaType::QString)
            return {{}, u"unknown or untyped host setting"_s};
        const auto value = normalize(scope, it.key(), it.value().toString());
        if (!value) return {{}, fieldError(it.key())};
        normalized.insert(it.key(), *value);
    }
    if (normalized == current.overrides) return {original, {}};
    QStringList names;
    for (const auto &key : keys(scope)) names.append(environmentName(scope, key));
    const auto document = scan(original);
    QByteArray candidate;
    qsizetype previous = 0;
    for (const auto &assignment : document.assignments) {
        if (!names.contains(assignment.name)) continue;
        candidate += original.mid(previous, assignment.begin - previous);
        previous = assignment.end;
    }
    candidate += original.mid(previous);
    const QByteArray newline = original.contains("\r\n") ? QByteArray("\r\n") : QByteArray("\n");
    if (!normalized.isEmpty() && !candidate.isEmpty() && !candidate.endsWith('\n')) candidate += newline;
    for (const auto &key : keys(scope)) {
        if (normalized.contains(key)) candidate += environmentName(scope, key).toUtf8() + '=' + quoted(normalized[key].toString()) + newline;
    }
    const auto verified = parse(scope, candidate);
    if (!verified.error.isEmpty()) return {{}, verified.error};
    if (verified.overrides != normalized) return {{}, u"host settings readback mismatch"_s};
    return {candidate, {}};
}
}
