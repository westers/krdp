// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-060 S3 test helper (never installed): a stand-in for org.freedesktop.ScreenSaver, run INSIDE the private
// fixture session (the bus daemon refuses clients from outside the sandbox). It is NOT a real kscreenlocker:
// "locked" is a flag and the greeter is a marker file. Commands arrive as lines in <dir>/cmd:
//   enable | lock | kill | reset        (kill = the OPT-049 failure: the greeter dies and the lock is gone)
// State is written to <dir>/state as: active=<0|1> greeter=<0|1> relocks=<n> kills=<n>
// The service is only taken on "enable", so other fixture tests see an unchanged bus.

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QFile>
#include <QTimer>

class FakeScreenSaver : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    FakeScreenSaver(const QString &dir, const QString &greeter)
        : m_dir(dir), m_greeter(greeter)
    {
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &FakeScreenSaver::poll);
        timer->start(15);
        writeState();
    }
public Q_SLOTS:
    bool GetActive() { return m_active; }
    void Lock() { ++m_relocks; setLocked(true); }
    void SetActive(bool active) { if (active) setLocked(true); }
Q_SIGNALS:
    void ActiveChanged(bool active);
private:
    void poll()
    {
        QFile file(m_dir + QStringLiteral("/cmd"));
        if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;
        const auto lines = file.readAll().split('\n');
        file.close(); QFile::remove(m_dir + QStringLiteral("/cmd"));
        for (const auto &line : lines) {
            if (line == "enable" && !m_enabled) {
                auto bus = QDBusConnection::sessionBus();
                m_enabled = bus.registerObject(QStringLiteral("/ScreenSaver"), this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)
                    && bus.registerService(QStringLiteral("org.freedesktop.ScreenSaver"));
                if (!m_enabled) qWarning() << "fake-locker: cannot own org.freedesktop.ScreenSaver:" << bus.lastError().message();
            } else if (line == "lock") {
                setLocked(true);
            } else if (line == "reset") { // unlocked, no greeter, counters zero (between fixture rows)
                m_relocks = 0; m_kills = 0; QFile::remove(m_greeter); setLocked(false);
            } else if (line == "kill") {
                ++m_kills; QFile::remove(m_greeter); setLocked(false);
            }
        }
        writeState();
    }
    void setLocked(bool locked)
    {
        if (locked) { QFile marker(m_greeter); if (marker.open(QIODevice::WriteOnly)) marker.write("1"); }
        if (m_active != locked) { m_active = locked; Q_EMIT ActiveChanged(locked); }
        writeState();
    }
    void writeState()
    {
        QFile out(m_dir + QStringLiteral("/state.tmp"));
        if (!out.open(QIODevice::WriteOnly)) return;
        out.write(QStringLiteral("active=%1 greeter=%2 relocks=%3 kills=%4 enabled=%5\n").arg(m_active ? 1 : 0).arg(QFile::exists(m_greeter) ? 1 : 0)
                      .arg(m_relocks).arg(m_kills).arg(m_enabled ? 1 : 0).toUtf8());
        out.close();
        QFile::remove(m_dir + QStringLiteral("/state"));
        QFile::rename(m_dir + QStringLiteral("/state.tmp"), m_dir + QStringLiteral("/state"));
    }
    QString m_dir, m_greeter;
    bool m_active = false, m_enabled = false;
    int m_relocks = 0, m_kills = 0;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) return 2;
    FakeScreenSaver fake(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    return app.exec();
}

#include "FakeScreenSaverHelper.moc"
