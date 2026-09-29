// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "coexistence.h"

#include <QDir>
#include <QFile>
#include <QPointer>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace Coexistence
{
namespace
{
QString readLink(const QString &path)
{
    QByteArray buffer(4096, Qt::Uninitialized);
    const auto length = ::readlink(QFile::encodeName(path).constData(), buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    return QFile::decodeName(buffer.left(length));
}

bool isPid(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (const QChar c : name) {
        if (!c.isDigit()) {
            return false;
        }
    }
    return true;
}
}

ProcScanner::ProcScanner(const QString &procRoot)
    : m_root(procRoot)
{
}

QList<quint64> ProcScanner::listeningInodes(quint16 port) const
{
    // /proc/net/tcp{,6}: "sl local_address rem_address st ... uid timeout inode".
    // local_address is ADDR:PORT in hex, st 0A is LISTEN.
    QList<quint64> inodes;
    for (const auto &table : {u"/net/tcp"_s, u"/net/tcp6"_s}) {
        QFile file(m_root + table);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        file.readLine(); // header
        while (!file.atEnd()) {
            const auto fields = file.readLine().simplified().split(' ');
            if (fields.size() < 10 || fields.at(3) != "0A") {
                continue;
            }
            const auto local = fields.at(1);
            const auto colon = local.lastIndexOf(':');
            bool ok = false;
            const auto localPort = local.mid(colon + 1).toUInt(&ok, 16);
            if (!ok || localPort != port) {
                continue;
            }
            inodes << fields.at(9).toULongLong();
        }
    }
    return inodes;
}

bool ProcScanner::isListening(quint16 port) const
{
    return !listeningInodes(port).isEmpty();
}

PortHolder ProcScanner::holder(quint16 port) const
{
    PortHolder result;
    const auto inodes = listeningInodes(port);
    if (inodes.isEmpty()) {
        return result;
    }
    result.listening = true;

    QStringList targets;
    for (const auto inode : inodes) {
        if (inode != 0) {
            targets << u"socket:[%1]"_s.arg(inode);
        }
    }
    // Only processes this user may inspect are found; anything else stays
    // "unknown", which is enough to say the port is taken.
    const auto processes = QDir(m_root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : processes) {
        if (!isPid(entry)) {
            continue;
        }
        const QString fdDir = m_root + u'/' + entry + u"/fd"_s;
        const auto fds = QDir(fdDir).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        for (const auto &fd : fds) {
            if (!targets.contains(readLink(fdDir + u'/' + fd))) {
                continue;
            }
            result.pid = entry.toLongLong();
            QString exe = readLink(m_root + u'/' + entry + u"/exe"_s);
            if (exe.endsWith(u" (deleted)"_s)) {
                exe.chop(10);
            }
            result.executable = exe;
            QFile comm(m_root + u'/' + entry + u"/comm"_s);
            if (comm.open(QIODevice::ReadOnly)) {
                result.name = QString::fromUtf8(comm.readAll()).trimmed();
            }
            if (result.name.isEmpty() && !exe.isEmpty()) {
                result.name = exe.section(u'/', -1);
            }
            return result;
        }
    }
    return result;
}

quint16 ProcScanner::firstFreePort(quint16 from, const QList<quint16> &skip) const
{
    for (quint32 port = from; port < 65536; ++port) {
        if (!skip.contains(quint16(port)) && !isListening(quint16(port))) {
            return quint16(port);
        }
    }
    return 0;
}

Result evaluate(const Farside::Identity &identity, const Inputs &inputs)
{
    Result result;
    const bool distinct = identity.stockIsDistinct();
    const auto &holder = inputs.holder;
    // Stock is not running but starts at login, on the port Farside uses.
    const bool stockTakesPortAtLogin =
        distinct && inputs.stock.known && inputs.stock.autostart() && !inputs.stock.active() && inputs.port == identity.stockPort;

    if (!holder.listening) {
        result.conflict = stockTakesPortAtLogin ? Conflict::StockAutostart : Conflict::None;
        return result;
    }

    // Farside itself.
    const bool serverPid = holder.pid > 0 && holder.pid == inputs.server.mainPid;
    const bool serverBinary = holder.pid > 0 && inputs.server.mainPid <= 0 && inputs.server.active() && identity.serverExecutables.contains(holder.executable)
        && !(distinct && identity.stockExecutables.contains(holder.executable));
    if (serverPid || serverBinary) {
        result.conflict = stockTakesPortAtLogin ? Conflict::StockAutostart : Conflict::None;
        return result;
    }

    result.holderPid = holder.pid;
    result.holderName = holder.name;
    if (distinct) {
        const bool stockProcess = (holder.pid > 0 && holder.pid == inputs.stock.mainPid) || identity.stockExecutables.contains(holder.executable);
        // Stock runs as this user, so its process is readable. An unreadable
        // one counts as stock only while stock's unit is running.
        const bool likelyStock = holder.pid <= 0 && inputs.stock.active();
        if (stockProcess || likelyStock) {
            result.conflict = Conflict::StockHoldsPort;
            return result;
        }
    }
    result.conflict = Conflict::OtherHoldsPort;
    return result;
}

Controller::Controller(std::unique_ptr<ServiceManager> manager,
                       const Farside::Identity &identity,
                       const QString &procRoot,
                       PortProvider savedPort,
                       QObject *parent)
    : QObject(parent)
    , m_manager(std::move(manager))
    , m_identity(identity)
    , m_scanner(procRoot)
    , m_savedPort(std::move(savedPort))
{
    m_virtualHostInstalled = !m_identity.virtualHostUnit.isEmpty() && m_manager->systemUnitInstalled(m_identity.virtualHostUnit);
}

Controller::~Controller() = default;

Conflict Controller::conflict() const
{
    return m_result.conflict;
}

QString Controller::stateName() const
{
    switch (m_result.conflict) {
    case Conflict::StockHoldsPort:
        return u"stock"_s;
    case Conflict::StockAutostart:
        return u"stock-autostart"_s;
    case Conflict::OtherHoldsPort:
        return u"other"_s;
    case Conflict::None:
        break;
    }
    return u"none"_s;
}

QString Controller::holderName() const
{
    return m_result.holderName;
}

int Controller::port() const
{
    return m_port;
}

int Controller::stockPort() const
{
    return m_identity.stockPort;
}

int Controller::alternativePort() const
{
    return m_alternativePort;
}

bool Controller::stockInstalled() const
{
    return m_stockInstalled;
}

bool Controller::virtualHostInstalled() const
{
    return m_virtualHostInstalled;
}

bool Controller::busy() const
{
    return m_busy;
}

QString Controller::lastError() const
{
    return m_lastError;
}

void Controller::refresh()
{
    const quint64 generation = ++m_generation;
    QPointer self(this);
    m_manager->queryUnit(m_identity.serverUnit, [self, generation](const UnitState &server) {
        if (!self || generation != self->m_generation) {
            return;
        }
        Inputs inputs;
        inputs.server = server;
        if (!self->m_identity.stockIsDistinct()) {
            self->finishRefresh(generation, inputs);
            return;
        }
        self->m_manager->queryUnit(self->m_identity.stockUnit, [self, generation, inputs](const UnitState &stock) mutable {
            if (!self || generation != self->m_generation) {
                return;
            }
            inputs.stock = stock;
            self->finishRefresh(generation, inputs);
        });
    });
}

void Controller::finishRefresh(quint64 generation, const Inputs &unitInputs)
{
    Q_UNUSED(generation)
    Inputs inputs = unitInputs;
    inputs.port = m_savedPort ? m_savedPort() : 0;
    inputs.holder = inputs.port > 0 ? m_scanner.holder(inputs.port) : PortHolder{};

    const auto result = evaluate(m_identity, inputs);
    // A suggestion that is neither Farside's port nor stock's.
    QList<quint16> skip = m_identity.reservedPorts;
    skip << m_identity.stockPort << inputs.port;
    const quint16 alternative = m_scanner.firstFreePort(m_identity.stockPort + 1, skip);
    const bool stockInstalled = m_identity.stockIsDistinct() && inputs.stock.known;

    const bool different = result.conflict != m_result.conflict || result.holderName != m_result.holderName || result.holderPid != m_result.holderPid
        || inputs.port != m_port || alternative != m_alternativePort || stockInstalled != m_stockInstalled;
    m_result = result;
    m_port = inputs.port;
    m_alternativePort = alternative;
    m_stockInstalled = stockInstalled;
    if (different) {
        Q_EMIT changed();
    }
    Q_EMIT refreshed();
}

void Controller::stopStockAndStartServer()
{
    if (m_busy || !m_identity.stockIsDistinct()) {
        return;
    }
    setBusy(true);
    setLastError({});
    QPointer self(this);
    m_manager->stopUnit(m_identity.stockUnit, [self](const QString &error) {
        if (!self) {
            return;
        }
        self->setBusy(false);
        if (!error.isEmpty()) {
            self->setLastError(error);
            self->refresh();
            return;
        }
        Q_EMIT self->startServerRequested();
        self->refresh();
    });
}

void Controller::useAlternativePort()
{
    if (m_alternativePort == 0) {
        return;
    }
    setLastError({});
    Q_EMIT portChangeRequested(m_alternativePort);
    refresh();
}

void Controller::disableStockAutostart()
{
    if (m_busy || !m_identity.stockIsDistinct()) {
        return;
    }
    setBusy(true);
    setLastError({});
    QPointer self(this);
    m_manager->disableUnitFile(m_identity.stockUnit, [self](const QString &error) {
        if (!self) {
            return;
        }
        self->setBusy(false);
        if (!error.isEmpty()) {
            self->setLastError(error);
        }
        self->refresh();
    });
}

void Controller::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

void Controller::setLastError(const QString &error)
{
    if (m_lastError != error) {
        m_lastError = error;
        Q_EMIT lastErrorChanged();
    }
}
}
