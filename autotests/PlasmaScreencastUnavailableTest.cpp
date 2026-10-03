// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 (wave 1): a compositor that withholds zkde_screencast_unstable_v1 / org_kde_kwin_fake_input
// (no X-KDE-Wayland-Interfaces grant, a login greeter) must end the SESSION with a reason, never the
// process. The offscreen platform has neither global, which is exactly that state.

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <dirent.h>
#include <memory>

#include "PlasmaScreencastV1Session.h"
#include "RelativePointerEvent.h"

using namespace KRdp;

namespace
{
int openFdCount()
{
    int n = 0;
    if (DIR *dir = opendir("/proc/self/fd")) {
        while (readdir(dir)) {
            ++n;
        }
        closedir(dir);
    }
    return n;
}
}

class PlasmaScreencastUnavailableTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void unavailableInterfacesEndTheSessionNotTheProcess()
    {
        // Process-wide lazy singletons (clipboard, XKB) open their fds on first use; measure the second round.
        { PlasmaScreencastV1Session warmup; }
        const int fdsBefore = openFdCount();
        {
            auto session = std::make_unique<PlasmaScreencastV1Session>(); // used to abort() here (Q_ASSERT)
            QSignalSpy unavailable(session.get(), &AbstractSession::captureUnavailable);
            QSignalSpy errors(session.get(), &AbstractSession::error);

            session->start();
            session->start(); // idempotent: still one reason, no crash

            QCOMPARE(unavailable.count(), 1);
            QVERIFY(unavailable.first().first().toString().contains(QLatin1String("X-KDE-Wayland-Interfaces")));
            QVERIFY(errors.count() >= 1);

            // Every input path dereferenced the missing fake-input object.
            auto move = std::make_shared<QMouseEvent>(QEvent::MouseMove, QPointF(5, 5), QPointF(5, 5), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            auto press = std::make_shared<QMouseEvent>(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            auto wheel = std::make_shared<QWheelEvent>(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            auto key = std::make_shared<QKeyEvent>(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 30, 0, 0, QStringLiteral("a"));
            for (const std::shared_ptr<QEvent> &event : {std::shared_ptr<QEvent>(move), std::shared_ptr<QEvent>(press), std::shared_ptr<QEvent>(wheel), std::shared_ptr<QEvent>(key)}) {
                session->sendEvent(event);
                session->sendGlobalEvent(event);
            }
            QTest::qWait(400); // cursor settle / recovery timers fire against the missing object
        }
        QCOMPARE(openFdCount(), fdsBefore);
    }
};

QTEST_MAIN(PlasmaScreencastUnavailableTest)
#include "PlasmaScreencastUnavailableTest.moc"
