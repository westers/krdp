// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerHostRuntime.h"
#include <QDBusConnection>
#include <functional>

namespace KRdp::BrokerHostRuntime {
struct ReaderDependencies {
    // C++-only closed fixtures. The production helper never accepts these as
    // input, argv, environment, QObject properties or alternate file targets.
    std::function<File(const QString &)> readFile;
    std::function<Process(const Unit &, const Contract &)> readProcess;
    bool requirePidOne = true;
};
// Read-only; callers must already have normal administrator authorization.
QJsonObject inspect(Scope scope, const QVariantMap &stored, const QString &revision,
    const QDBusConnection &bus, const ReaderDependencies &dependencies = {});
}
