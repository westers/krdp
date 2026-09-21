// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ConsoleResize.h"
#include <QObject>
#include <functional>

namespace KRdp
{
// Session-worker helper. Never instantiate in the root console host.
class ConsoleResizeExecutor : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(bool, QByteArray)>;
    using Runner = std::function<void(const QStringList &, Reply)>;
    explicit ConsoleResizeExecutor(QObject *parent = nullptr, Runner runner = {});
    bool resize(const QString &output, QSize pixels, double scale);
    bool restore(const ConsoleResize::Plan &plan);
    // Cancel discovery before a mode command is launched. An already launched
    // command must settle and be read back so the lifecycle can restore safely.
    void cancelBeforeApply() { m_cancelBeforeApply = true; }
    bool busy() const { return m_busy; }

Q_SIGNALS:
    void changing();
    // Success verifies mode/scale only; caller still waits for capture geometry.
    void finished(const KRdp::ConsoleResize::Plan &plan, const QString &error);

private:
    void run(const QStringList &arguments, Reply reply);
    void apply(ConsoleResize::Plan plan, bool restoring);
    void finish(const ConsoleResize::Plan &plan, const QString &error);
    Runner m_runner;
    bool m_busy = false;
    bool m_cancelBeforeApply = false;
};
}
