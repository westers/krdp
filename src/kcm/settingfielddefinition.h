// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// What a settings page needs to draw one field, kept next to the field's key so QML holds no per-key tables.
// The models (BrokerHostSettings, BrokerPreferences) build one QVariantMap per key with makeFieldDefinition().
namespace KRdp::SettingFields
{
// The "Custom address" choice of an address control: a placeholder value, never stored.
inline const QString AddressCustom = QStringLiteral("@custom");

struct Spec {
    // choice | spin | slider | text | path | address (modes: inherit, wildcards, custom) | size. Empty derives "choice" from the options, else "text".
    QString control;
    // Page section the field belongs to (a stable id, not a title); advanced fields sit in the collapsed group.
    QString section;
    bool advanced = false;
    // The short label next to the control. Empty falls back to the long label used in summaries and help.
    QString formLabel;
    // Numeric bounds (spin, slider) or the width bounds and fixed height minimum (size).
    int min = 0;
    int max = 0;
    int heightMin = 0;
    QString unit;
    // An empty text value is a real value ("no GPU", "all interfaces") and must not mean "inherit".
    bool keepEmpty = false;
    // Show the field only while another key of the same model has this value.
    QString showWhenKey;
    QString showWhenValue;
    // A non-empty reason hides the control; the page may show it instead.
    QString unavailable;
    // Address controls only: the list entries (inherit, wildcards, custom) as {value, text}; the first inherits, the last is custom.
    QVariantList modes;
    // The empty choice / "Custom" selector wording for the scope this model belongs to ("Use host setting", "Use unit default").
    QString inheritText;
    // Wording of individual options on the form, by value, where it differs from the summary wording.
    QVariantMap optionText;
};

inline QString controlFor(const Spec &spec, const QVariantList &choices)
{
    return !spec.control.isEmpty() ? spec.control : choices.isEmpty() ? QStringLiteral("text") : QStringLiteral("choice");
}

inline QVariantMap makeFieldDefinition(const QString &key, const QString &group, const QString &label, const QString &help, const QVariantList &choices, const Spec &spec)
{
    QVariantList options;
    for (const auto &option : choices) {
        auto row = option.toMap();
        row.insert(QStringLiteral("formText"), spec.optionText.value(row.value(QStringLiteral("value")).toString(), row.value(QStringLiteral("text"))));
        options.append(row);
    }
    return QVariantMap{{QStringLiteral("key"), key},
                       {QStringLiteral("group"), group},
                       {QStringLiteral("label"), label},
                       {QStringLiteral("help"), help},
                       {QStringLiteral("choices"), options},
                       {QStringLiteral("control"), controlFor(spec, choices)},
                       {QStringLiteral("section"), spec.section},
                       {QStringLiteral("advanced"), spec.advanced},
                       {QStringLiteral("formLabel"), spec.formLabel.isEmpty() ? label : spec.formLabel},
                       {QStringLiteral("min"), spec.min},
                       {QStringLiteral("max"), spec.max},
                       {QStringLiteral("heightMin"), spec.heightMin},
                       {QStringLiteral("unit"), spec.unit},
                       {QStringLiteral("keepEmpty"), spec.keepEmpty},
                       {QStringLiteral("showWhenKey"), spec.showWhenKey},
                       {QStringLiteral("showWhenValue"), spec.showWhenValue},
                       {QStringLiteral("unavailable"), spec.unavailable},
                       {QStringLiteral("inheritText"), spec.inheritText},
                       {QStringLiteral("modes"), spec.modes}};
}

// Empty when the definition is complete and consistent, otherwise the first problem found.
inline QString definitionProblem(const QVariantMap &definition)
{
    static const QStringList controls{QStringLiteral("choice"), QStringLiteral("spin"), QStringLiteral("slider"), QStringLiteral("text"), QStringLiteral("path"),
                                      QStringLiteral("address"), QStringLiteral("size")};
    const auto control = definition.value(QStringLiteral("control")).toString();
    if (!controls.contains(control)) return QStringLiteral("unknown control '%1'").arg(control);
    if (definition.value(QStringLiteral("key")).toString().isEmpty() || definition.value(QStringLiteral("formLabel")).toString().isEmpty())
        return QStringLiteral("missing key or label");
    if (definition.value(QStringLiteral("section")).toString().isEmpty()) return QStringLiteral("missing section");
    const bool hasChoices = !definition.value(QStringLiteral("choices")).toList().isEmpty();
    if ((control == QStringLiteral("choice")) != hasChoices) return QStringLiteral("choice control and options disagree");
    if ((control == QStringLiteral("address")) == definition.value(QStringLiteral("modes")).toList().isEmpty()) return QStringLiteral("address control and modes disagree");
    if (definition.value(QStringLiteral("inheritText")).toString().isEmpty()) return QStringLiteral("missing inherit wording");
    if (control == QStringLiteral("spin") || control == QStringLiteral("slider") || control == QStringLiteral("size")) {
        const int min = definition.value(QStringLiteral("min")).toInt();
        const int max = definition.value(QStringLiteral("max")).toInt();
        if (min >= max) return QStringLiteral("min must be below max");
        if (control == QStringLiteral("size") && definition.value(QStringLiteral("heightMin")).toInt() >= max) return QStringLiteral("height minimum must be below max");
    }
    if (definition.value(QStringLiteral("showWhenKey")).toString().isEmpty() != definition.value(QStringLiteral("showWhenValue")).toString().isEmpty())
        return QStringLiteral("showWhen needs a key and a value");
    return {};
}
} // namespace KRdp::SettingFields
