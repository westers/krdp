// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "farsideidentity.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

// Farside and KDE's own remote desktop can both be installed; whichever starts
// first gets the port (REBRAND-PLAN.md §3, option b). This is what the page
// needs to explain a busy port and offer a way out.
namespace Coexistence
{
Q_NAMESPACE

// Who listens on a TCP port, read from /proc (like `ss -ltnp`).
struct PortHolder {
    bool listening = false;
    qint64 pid = 0; // 0: unknown (another user's process)
    QString executable; // absolute path, empty when unknown
    QString name; // the process name, empty when unknown
};

class ProcScanner
{
public:
    explicit ProcScanner(const QString &procRoot = QStringLiteral("/proc"));
    bool isListening(quint16 port) const;
    PortHolder holder(quint16 port) const;
    // The first port from `from` up that nothing listens on and that is not
    // in `skip`; 0 when there is none below 65536.
    quint16 firstFreePort(quint16 from, const QList<quint16> &skip) const;

private:
    QList<quint64> listeningInodes(quint16 port) const;
    QString m_root;
};

enum class Conflict {
    None,
    // KDE's remote desktop listens on Farside's port.
    StockHoldsPort,
    // KDE's remote desktop is not running but starts at login, on Farside's port.
    StockAutostart,
    // Some other program listens on Farside's port.
    OtherHoldsPort,
};
Q_ENUM_NS(Conflict)

struct UnitState {
    bool known = false; // the unit file exists
    QString activeState; // systemd ActiveState
    QString unitFileState; // systemd UnitFileState
    qint64 mainPid = 0;

    bool active() const
    {
        return activeState == QLatin1String("active") || activeState == QLatin1String("activating") || activeState == QLatin1String("reloading");
    }
    bool autostart() const
    {
        return unitFileState == QLatin1String("enabled") || unitFileState == QLatin1String("enabled-runtime");
    }
};

struct Inputs {
    quint16 port = 0; // Farside's saved port
    UnitState server;
    UnitState stock; // ignored unless the identity tells the two apart
    PortHolder holder;
};

struct Result {
    Conflict conflict = Conflict::None;
    QString holderName;
    qint64 holderPid = 0;
};

Result evaluate(const Farside::Identity &identity, const Inputs &inputs);

// The user systemd manager, reduced to what the page does to other units.
class ServiceManager
{
public:
    using StateDone = std::function<void(const UnitState &)>;
    using Done = std::function<void(const QString &error)>;

    virtual ~ServiceManager() = default;
    virtual void queryUnit(const QString &unit, StateDone done) = 0;
    virtual void stopUnit(const QString &unit, Done done) = 0;
    virtual void disableUnitFile(const QString &unit, Done done) = 0;
    // Whether a system unit file is installed.
    virtual bool systemUnitInstalled(const QString &unit) const = 0;
};

class Controller : public QObject
{
    Q_OBJECT
    // "none", "stock", "stock-autostart" or "other", for QML.
    Q_PROPERTY(QString state READ stateName NOTIFY changed)
    Q_PROPERTY(QString holderName READ holderName NOTIFY changed)
    Q_PROPERTY(int port READ port NOTIFY changed)
    Q_PROPERTY(int stockPort READ stockPort CONSTANT)
    Q_PROPERTY(int alternativePort READ alternativePort NOTIFY changed)
    Q_PROPERTY(bool stockInstalled READ stockInstalled NOTIFY changed)
    Q_PROPERTY(bool virtualHostInstalled READ virtualHostInstalled CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    using PortProvider = std::function<quint16()>;

    Controller(std::unique_ptr<ServiceManager> manager,
               const Farside::Identity &identity,
               const QString &procRoot,
               PortProvider savedPort,
               QObject *parent = nullptr);
    ~Controller() override;

    Conflict conflict() const;
    QString stateName() const;
    QString holderName() const;
    int port() const;
    int stockPort() const;
    int alternativePort() const;
    bool stockInstalled() const;
    bool virtualHostInstalled() const;
    bool busy() const;
    QString lastError() const;

    // Reads both units and the port again; `changed` follows.
    Q_INVOKABLE void refresh();
    // "Stop It and Start Farside": stops KDE's remote desktop, then asks the
    // page to start Farside.
    Q_INVOKABLE void stopStockAndStartServer();
    // "Use Port N": asks the page to move Farside to alternativePort().
    Q_INVOKABLE void useAlternativePort();
    // "Turn Its Autostart Off": disables KDE's remote desktop at login.
    Q_INVOKABLE void disableStockAutostart();

Q_SIGNALS:
    void changed();
    void busyChanged();
    void lastErrorChanged();
    void refreshed();
    void startServerRequested();
    void portChangeRequested(int port);

private:
    void setBusy(bool busy);
    void setLastError(const QString &error);
    void finishRefresh(quint64 generation, const Inputs &inputs);

    std::unique_ptr<ServiceManager> m_manager;
    Farside::Identity m_identity;
    ProcScanner m_scanner;
    PortProvider m_savedPort;
    quint64 m_generation = 0;
    Result m_result;
    quint16 m_port = 0;
    quint16 m_alternativePort = 0;
    bool m_stockInstalled = false;
    bool m_virtualHostInstalled = false;
    bool m_busy = false;
    QString m_lastError;
};
}
