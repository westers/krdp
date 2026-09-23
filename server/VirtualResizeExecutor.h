// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualResize.h"
#include <QObject>
#include <functional>

namespace KRdp
{
// Worker-only asynchronous command execution. No shell, environment supplied by
// the authenticated launch, no client executable/path/output selector accepted.
class VirtualResizeExecutor : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(bool, QByteArray)>;
    using Runner = std::function<void(const QStringList &, Reply)>;
    struct Result {
        VirtualResize::Plan plan;
        std::optional<VirtualResize::Snapshot> observed;
        QString error;
        bool mutated = false; // Mode/scale command launched, not just a mode added.
    };
    explicit VirtualResizeExecutor(QObject *parent = nullptr, Runner runner = {});
    bool resize(QSize pixels, double scale);
    bool rollback(const VirtualResize::Plan &plan);
    void cancelBeforeApply() { m_cancelled = true; }
    bool busy() const { return m_busy; }
Q_SIGNALS:
    void mutationStarting();
    void finished(const KRdp::VirtualResizeExecutor::Result &result);
private:
    void run(const QStringList &arguments, Reply reply);
    void apply(const VirtualResize::Plan &plan, const VirtualResize::Snapshot &state);
    void finish(Result result);
    Runner m_runner;
    bool m_busy = false;
    bool m_cancelled = false;
};
}
