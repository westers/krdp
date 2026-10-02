// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QObject>
#include <memory>
#include <linux/input.h>

namespace KRdp {
/** Read-only seat0 physical activity. No grabs, key recording, or input injection. */
class PhysicalInputWatcher final : public QObject {
    Q_OBJECT
public:
    explicit PhysicalInputWatcher(QObject *parent = nullptr);
    ~PhysicalInputWatcher() override;
    void start();
    static bool isActivity(const input_event &event) {
        return (event.type == EV_KEY && event.value > 0)
            || (event.type == EV_REL && event.value != 0)
            || event.type == EV_ABS;
    }
Q_SIGNALS:
    void activity();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
