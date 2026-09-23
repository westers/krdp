// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QPoint>
#include <QSize>
#include <QStringList>
#include <QList>
#include <optional>

namespace KRdp::VirtualResize
{
// Only the explicitly virtual-launched, unprivileged worker may use this
// planner. Output names alone never establish compositor isolation.
struct Mode {
    QString id;
    QSize pixels;
    int refresh = 0; // mHz
};
struct Snapshot {
    QString name;
    int id = 0;
    QPoint position;
    double scale = 1;
    Mode current;
    QList<Mode> modes;
};
struct Plan {
    Snapshot before;
    QSize pixels;
    double scale = 1;
    bool valid() const { return !before.name.isEmpty(); }
};
bool validRequest(QSize pixels, double scale);
double normalizedScale(double scale);
bool sameScale(double a, double b);
std::optional<Snapshot> snapshot(const QByteArray &json, QString *error);
bool sameOutput(const Snapshot &a, const Snapshot &b);
bool matches(const Snapshot &state, const Plan &plan);
std::optional<Mode> matchingMode(const Snapshot &state, QSize pixels, int refresh);
QStringList select(const Snapshot &state, const Mode &mode, double scale);
QStringList add(const Plan &plan);
}
