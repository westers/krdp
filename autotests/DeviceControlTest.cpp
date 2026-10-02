// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-D2: the KRDPCTL `device` record (KRDPCTL-V2-CONTRACT.md §d) as krdpserver
// handles it (PhysicalDeviceControl, SessionController's glue), the `devices`
// capabilities group, DeviceConsent generations and StandardClientMedia.
// Socket-free: a detached RdpConnection never runs a session loop.

#include <QSignalSpy>
#include <QTest>

#include "DeviceConsent.h"
#include "CameraAvailability.h"
#include "DeviceControl.h"
#include "LayoutControl.h"
#include "PhysicalDeviceControl.h"
#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;

namespace
{
QJsonObject device(const QString &name, const QString &action)
{
    return {{QStringLiteral("type"), QStringLiteral("device")}, {QStringLiteral("v"), 1}, {QStringLiteral("device"), name}, {QStringLiteral("action"), action}};
}

QString errorCode(const std::variant<DeviceControl::Request, LayoutControl::Error> &parsed)
{
    const auto *error = std::get_if<LayoutControl::Error>(&parsed);
    return error ? error->code : QString();
}
}

class DeviceControlTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesEveryDeviceAndAction()
    {
        using Action = DeviceControl::Action;
        const QList<std::pair<QString, MediaDevice>> devices{{QStringLiteral("playback"), MediaDevice::Playback},
                                                               {QStringLiteral("microphone"), MediaDevice::Microphone},
                                                               {QStringLiteral("camera"), MediaDevice::Camera}};
        const QList<std::pair<QString, Action>> actions{{QStringLiteral("on"), Action::On},
                                                          {QStringLiteral("off"), Action::Off},
                                                          {QStringLiteral("reselect"), Action::Reselect},
                                                          {QStringLiteral("query"), Action::Query}};
        for (const auto &[name, expectedDevice] : devices) {
            for (const auto &[action, expectedAction] : actions) {
                const auto parsed = DeviceControl::parseRequest(device(name, action));
                QVERIFY(std::holds_alternative<DeviceControl::Request>(parsed));
                const auto request = std::get<DeviceControl::Request>(parsed);
                QCOMPARE(request.device, expectedDevice);
                QCOMPARE(request.action, expectedAction);
                QVERIFY(!request.silenceHost);
            }
        }
        auto silent = device(QStringLiteral("playback"), QStringLiteral("on"));
        silent.insert(QStringLiteral("silenceHost"), true);
        const auto parsed = DeviceControl::parseRequest(silent);
        QVERIFY(std::get<DeviceControl::Request>(parsed).silenceHost);
    }

    void refusesMalformedRecords_data()
    {
        QTest::addColumn<QJsonObject>("record");
        QTest::addColumn<QString>("code");
        auto record = device(QStringLiteral("microphone"), QStringLiteral("on"));
        record.remove(QStringLiteral("device"));
        QTest::newRow("no device") << record << QStringLiteral("invalid");
        QTest::newRow("unknown device") << device(QStringLiteral("printer"), QStringLiteral("on")) << QStringLiteral("invalid");
        QTest::newRow("unknown action") << device(QStringLiteral("camera"), QStringLiteral("toggle")) << QStringLiteral("invalid");
        record = device(QStringLiteral("camera"), QStringLiteral("on"));
        record.insert(QStringLiteral("action"), true);
        QTest::newRow("action not a string") << record << QStringLiteral("invalid");
        record = device(QStringLiteral("camera"), QStringLiteral("on"));
        record.insert(QStringLiteral("enabled"), true);
        QTest::newRow("unknown key") << record << QStringLiteral("invalid");
        record = device(QStringLiteral("microphone"), QStringLiteral("on"));
        record.insert(QStringLiteral("silenceHost"), true);
        QTest::newRow("silenceHost on the microphone") << record << QStringLiteral("invalid");
        record = device(QStringLiteral("playback"), QStringLiteral("off"));
        record.insert(QStringLiteral("silenceHost"), false);
        QTest::newRow("silenceHost with off") << record << QStringLiteral("invalid");
        record = device(QStringLiteral("playback"), QStringLiteral("on"));
        record.insert(QStringLiteral("silenceHost"), 1);
        QTest::newRow("silenceHost not a boolean") << record << QStringLiteral("invalid");
        record = device(QStringLiteral("playback"), QStringLiteral("on"));
        record.insert(QStringLiteral("v"), 2);
        QTest::newRow("version 2") << record << QStringLiteral("unsupported");
        record.remove(QStringLiteral("v"));
        QTest::newRow("no version") << record << QStringLiteral("unsupported");
    }

    void refusesMalformedRecords()
    {
        QFETCH(QJsonObject, record);
        QFETCH(QString, code);
        QCOMPARE(errorCode(DeviceControl::parseRequest(record)), code);
    }

    void checksWhatEachHostSupports()
    {
        using Action = DeviceControl::Action;
        const auto physical = std::optional(PhysicalDeviceControl::Capabilities);
        const LayoutControl::DeviceCapabilities broker{true, true, true, false, false}; // console and virtual
        const auto refused = [](DeviceControl::Request request, const std::optional<LayoutControl::DeviceCapabilities> &capabilities) {
            const auto error = DeviceControl::checkSupported(request, capabilities);
            return error ? error->code : QString();
        };
        for (const auto device : {MediaDevice::Playback, MediaDevice::Microphone, MediaDevice::Camera}) {
            QVERIFY(refused({device, Action::On, false}, physical).isEmpty());
            QVERIFY(refused({device, Action::Off, false}, physical).isEmpty());
            QVERIFY(refused({device, Action::Query, false}, std::nullopt).isEmpty()); // always answerable
            QCOMPARE(refused({device, Action::On, false}, std::nullopt), QStringLiteral("unsupported"));
        }
        QVERIFY(refused({MediaDevice::Camera, Action::Reselect, false}, physical).isEmpty());
        QCOMPARE(refused({MediaDevice::Microphone, Action::Reselect, false}, physical), QStringLiteral("unsupported"));
        QCOMPARE(refused({MediaDevice::Playback, Action::Reselect, false}, physical), QStringLiteral("unsupported"));
        QVERIFY(refused({MediaDevice::Playback, Action::On, true}, physical).isEmpty());
        // The brokers: no camera yet (DEVICES-DESIGN.md §7 risk 5).
        QCOMPARE(refused({MediaDevice::Camera, Action::On, false}, broker), QStringLiteral("unsupported"));
        QCOMPARE(refused({MediaDevice::Camera, Action::Reselect, false}, broker), QStringLiteral("unsupported"));
        QVERIFY(refused({MediaDevice::Camera, Action::Query, false}, broker).isEmpty());
        QVERIFY(refused({MediaDevice::Microphone, Action::On, false}, broker).isEmpty());
        QVERIFY(refused({MediaDevice::Playback, Action::On, true}, broker).isEmpty());
        auto noSilence = broker;
        noSilence.playbackSilenceHost = false;
        QCOMPARE(refused({MediaDevice::Playback, Action::On, true}, noSilence), QStringLiteral("unsupported"));
    }

    void missingCameraBridgeDisablesOnlyCameraAndExplainsRepair()
    {
        const auto reason = CameraAvailability::reason(QStringLiteral("none"));
        QVERIFY(reason.contains(QStringLiteral("v4l2loopback-dkms")));
        QVERIFY(reason.contains(QStringLiteral("restart")));
        const auto caps = CameraAvailability::capabilities(PhysicalDeviceControl::Capabilities, reason);
        QVERIFY(!caps.cameraToggle && !caps.cameraReselect);
        QVERIFY(caps.playbackToggle && caps.microphoneToggle);
        const auto error = DeviceControl::checkSupported({MediaDevice::Camera, DeviceControl::Action::On}, caps);
        QVERIFY(error);
        QCOMPARE(error->message, reason);
        QVERIFY(!DeviceControl::checkSupported({MediaDevice::Camera, DeviceControl::Action::Off}, caps));
        QVERIFY(!DeviceControl::checkSupported({MediaDevice::Camera, DeviceControl::Action::Query}, caps));
        LayoutControl::ChannelCapabilities channel; channel.devices = caps;
        QCOMPARE(LayoutControl::capabilitiesRecord(channel).value(QStringLiteral("devices")).toObject()
            .value(QStringLiteral("camera")).toObject().value(QStringLiteral("unavailableReason")).toString(), reason);
        QVERIFY(!CameraAvailability::reason(QStringLiteral("/dev/video999999")).isEmpty());
        QVERIFY(!CameraAvailability::reason(QStringLiteral("/dev/null")).isEmpty());
        QVERIFY(!CameraAvailability::virtualReason().isEmpty());
    }

    void stateRecordShape()
    {
        const auto on = DeviceControl::stateRecord(MediaDevice::Microphone, {DeviceStatus::State::On, true, {}, {}});
        QCOMPARE(on,
                 (QJsonObject{{QStringLiteral("type"), QStringLiteral("device")},
                              {QStringLiteral("v"), 1},
                              {QStringLiteral("device"), QStringLiteral("microphone")},
                              {QStringLiteral("state"), QStringLiteral("on")},
                              {QStringLiteral("inUse"), true}}));
        const auto revoked = DeviceControl::stateRecord(MediaDevice::Playback, {DeviceStatus::State::Off, false, DeviceControl::Revoked, QStringLiteral("gone")});
        QCOMPARE(revoked.value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(revoked.value(QStringLiteral("code")).toString(), QStringLiteral("revoked"));
        QCOMPARE(revoked.value(QStringLiteral("message")).toString(), QStringLiteral("gone"));
        QVERIFY(!revoked.contains(QStringLiteral("requestId")));
        const auto starting = DeviceControl::stateRecord(MediaDevice::Camera, {DeviceStatus::State::Starting, false, {}, {}});
        QCOMPARE(starting.value(QStringLiteral("state")).toString(), QStringLiteral("starting"));
        QCOMPARE(DeviceControl::stateRecord(MediaDevice::Camera, {DeviceStatus::State::Error, false, DeviceControl::Timeout, {}}).value(QStringLiteral("state")).toString(),
                 QStringLiteral("error"));
    }

    void capabilitiesCarryTheDevicesGroup()
    {
        LayoutControl::ChannelCapabilities capabilities;
        capabilities.host = QStringLiteral("physical");
        const auto without = LayoutControl::capabilitiesRecord(capabilities);
        QVERIFY(!without.contains(QStringLiteral("devices"))); // optional: no runtime control
        capabilities.devices = PhysicalDeviceControl::Capabilities;
        const auto with = LayoutControl::capabilitiesRecord(capabilities);
        QCOMPARE(with.value(QStringLiteral("devices")).toObject(),
                 (QJsonObject{{QStringLiteral("playback"), QJsonObject{{QStringLiteral("toggle"), true}, {QStringLiteral("silenceHost"), true}}},
                              {QStringLiteral("microphone"), QJsonObject{{QStringLiteral("toggle"), true}}},
                              {QStringLiteral("camera"), QJsonObject{{QStringLiteral("toggle"), true}, {QStringLiteral("reselect"), true}}}}));
        // Everything else is as before.
        auto rest = with;
        rest.remove(QStringLiteral("devices"));
        QCOMPARE(rest, without);
    }

    void physicalHostRefusesWithTheRequestId()
    {
        Server server;
        RdpConnection connection(&server, -1);
        QSignalSpy states(&connection, &RdpConnection::deviceState);
        auto record = device(QStringLiteral("microphone"), QStringLiteral("on"));
        record.insert(QStringLiteral("extra"), 1);
        auto refused = PhysicalDeviceControl::request(connection, record, QStringLiteral("r1"));
        QVERIFY(refused);
        QCOMPARE(refused->value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        QCOMPARE(refused->value(QStringLiteral("code")).toString(), QStringLiteral("invalid"));
        QCOMPARE(refused->value(QStringLiteral("requestId")).toString(), QStringLiteral("r1"));

        refused = PhysicalDeviceControl::request(connection, device(QStringLiteral("playback"), QStringLiteral("reselect")), QStringLiteral("r2"));
        QVERIFY(refused);
        QCOMPARE(refused->value(QStringLiteral("code")).toString(), QStringLiteral("unsupported"));
        QCOMPARE(refused->value(QStringLiteral("requestId")).toString(), QStringLiteral("r2"));

        auto versioned = device(QStringLiteral("camera"), QStringLiteral("on"));
        versioned.insert(QStringLiteral("v"), 7);
        refused = PhysicalDeviceControl::request(connection, versioned, QStringLiteral("r3"));
        QVERIFY(refused);
        QCOMPARE(refused->value(QStringLiteral("code")).toString(), QStringLiteral("unsupported"));
        QCOMPARE(refused->value(QStringLiteral("requestId")).toString(), QStringLiteral("r3"));
        // Nothing reached the connection.
        QVERIFY(states.isEmpty());
        for (const auto device : {MediaDevice::Playback, MediaDevice::Microphone, MediaDevice::Camera}) {
            QCOMPARE(connection.deviceStatus(device), DeviceStatus{});
        }
    }

    void physicalHostAnswersAQueryAtOnceAndARequestLater()
    {
        Server server;
        RdpConnection connection(&server, -1);
        QSignalSpy states(&connection, &RdpConnection::deviceState);
        QVERIFY(!PhysicalDeviceControl::request(connection, device(QStringLiteral("camera"), QStringLiteral("query")), QStringLiteral("q1")));
        QCOMPARE(states.size(), 1);
        QCOMPARE(states.first().at(0).value<MediaDevice>(), MediaDevice::Camera);
        QCOMPARE(states.first().at(1).value<DeviceStatus>().state, DeviceStatus::State::Off);
        QCOMPARE(states.first().at(2).toString(), QStringLiteral("q1"));
        const auto record = PhysicalDeviceControl::stateRecord(MediaDevice::Camera, states.first().at(1).value<DeviceStatus>(), QStringLiteral("q1"));
        QCOMPARE(record.value(QStringLiteral("requestId")).toString(), QStringLiteral("q1"));
        QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        // An `on` is only stored: the session loop answers it (none runs here).
        QVERIFY(!PhysicalDeviceControl::request(connection, device(QStringLiteral("microphone"), QStringLiteral("on")), QStringLiteral("m1")));
        QCOMPARE(states.size(), 1);
        QVERIFY(connection.audioPriorityActive() == false); // no default/override set
        connection.setAudioPriority(true);
        QVERIFY(connection.audioPriorityActive()); // the microphone consent is on
        QVERIFY(!PhysicalDeviceControl::request(connection, device(QStringLiteral("microphone"), QStringLiteral("off")), QStringLiteral("m2")));
        QVERIFY(!connection.audioPriorityActive());
    }

    void standardClientMediaRule()
    {
        QVERIFY(PhysicalDeviceControl::wantsStandardConsent(false, false));
        QVERIFY(!PhysicalDeviceControl::wantsStandardConsent(true, false));
        QVERIFY(!PhysicalDeviceControl::wantsStandardConsent(false, true));
        QVERIFY(!PhysicalDeviceControl::wantsStandardConsent(true, true));
        Server server;
        QVERIFY(server.standardClientMedia()); // the default
        RdpConnection connection(&server, -1);
        QVERIFY(connection.applyStandardConsent());
        // Brokers: the setting, and the channels the client joined (none before it authenticated).
        const auto channels = connection.standardMediaChannels();
        QVERIFY(channels);
        QVERIFY(!channels->playback);
        QVERIFY(!channels->dynamic);
        server.setStandardClientMedia(false);
        QVERIFY(!connection.applyStandardConsent());
        QVERIFY(!connection.standardMediaChannels());
    }

    void consentGenerations()
    {
        DeviceConsent consent;
        int delivered = 0;
        const auto sink = [&] { ++delivered; };
        QCOMPARE(consent.snapshot().generation, uint64_t(0));
        QVERIFY(!consent.snapshot().enabled);
        consent.setEnabled(false); // no change, no generation
        QCOMPARE(consent.snapshot().generation, uint64_t(0));
        const auto first = consent.renew(); // every `on` is a fresh period
        QVERIFY(consent.snapshot().enabled);
        QVERIFY(consent.deliver(first, sink));
        const auto second = consent.renew(); // `on` again, or `reselect`
        QVERIFY(second > first);
        QVERIFY(consent.snapshot().enabled);
        QVERIFY(!consent.deliver(first, sink)); // the previous context's data is stale
        QVERIFY(consent.deliver(second, sink));
        consent.setEnabled(true); // brokers: idempotent
        QCOMPARE(consent.snapshot().generation, second);
        consent.setEnabled(false);
        QVERIFY(!consent.deliver(second, sink));
        QVERIFY(consent.snapshot().generation > second);
        QCOMPARE(delivered, 2);
    }
};

QTEST_GUILESS_MAIN(DeviceControlTest)
#include "DeviceControlTest.moc"
