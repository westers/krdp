#pragma once
#include <QDebug>
#include <QProcessEnvironment>
#include <QStringList>

namespace Farside
{
inline void warnLegacyEnvironment()
{
    QStringList old;
    for (const QString &name : QProcessEnvironment::systemEnvironment().keys()) {
        if (name.startsWith(QStringLiteral("KRDP_"))) old.append(name);
    }
    if (!old.isEmpty()) qWarning().noquote() << "Old KRDP environment names are ignored; use FARSIDE_ instead:" << old.join(QStringLiteral(", "));
}
}
