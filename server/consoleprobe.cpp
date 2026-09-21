// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include "ConsoleSeat.h"

using namespace KRdp::ConsoleSeat;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("seat"), QStringLiteral("logind seat to inspect (default: seat0)"), QStringLiteral("seat"), QStringLiteral("seat0")});
    parser.process(app);

    const QString seat = parser.value(QStringLiteral("seat"));
    QString error;
    const QList<Session> sessions = readLogindSessions(&error);
    if (!error.isEmpty()) {
        QTextStream(stderr) << "logind: " << error << Qt::endl;
        return 1;
    }

    QTextStream out(stdout);
    for (const auto &session : sessions) {
        if (session.seat == seat) {
            out << "session=" << session.id << " user=" << session.user << " class=" << session.sessionClass << " type=" << session.type
                << " state=" << session.state << " active=" << session.active << Qt::endl;
        }
    }
    const Adapter adapter = adapterFor(sessions, seat);
    const QString kind = adapter == Adapter::PhysicalUser ? QStringLiteral("physical-user")
        : adapter == Adapter::Greeter ? QStringLiteral("greeter")
        : QStringLiteral("none");
    out << "adapter=" << kind << " session=" << activeSessionId(sessions, adapter, seat) << Qt::endl;
    return adapter == Adapter::None ? 2 : 0;
}
