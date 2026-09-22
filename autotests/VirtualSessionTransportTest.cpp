// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <Server.h>
#include "VirtualSessionTransport.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

class VirtualSessionTransportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void unauthenticatedConnectionCannotCreateOrEnableMedia()
    {
        int launched = 0;
        VirtualSessionSupervisor supervisor([&](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            ++launched;
            return {};
        });
        VirtualSessionControl control(supervisor, {});
        Server server;
        RdpConnection connection(&server, -1);
        quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        const auto response = transport.request({{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"1"_s}, {u"action"_s, u"create"_s}});
        QVERIFY(!response.value(u"ok"_s).toBool());
        QCOMPARE(launched, 0);
        const auto media = transport.request({{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"playback"_s, true}, {u"microphone"_s, false}, {u"camera"_s, false}});
        QVERIFY(!media.value(u"ok"_s).toBool());
        QVERIFY(!control.attachment(1));
        transport.revoke();
        transport.revoke();
        QCOMPARE(sequence, quint64(0));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionTransportTest)
#include "VirtualSessionTransportTest.moc"
