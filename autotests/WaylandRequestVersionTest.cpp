// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX F1: version gating of the Wayland requests KRdp sends.
//
// On Sol, ~FakeInput sent org_kde_kwin_fake_input.destroy on a v4 bind, but
// destroy is since="5": KWin raised a protocol error and krdpserver exited 255
// on every client disconnect. This checks the helper's decisions, that the
// generated *_SINCE_VERSION constants the code gates on agree with the
// protocol XML the build used, and that no request the code sends is newer
// than the version it asks Qt to bind.

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QTest>
#include <QXmlStreamReader>

#include "WaylandRequestVersion.h"
#include "wayland-fake-input-client-protocol.h"
#include "wayland-zkde-screencast-unstable-v1-client-protocol.h"

using namespace KRdp;
using WaylandRequestVersion::Teardown;

namespace
{
// request name -> since (1 when the attribute is absent), for one interface.
QHash<QString, int> requestSinceVersions(const QString &xmlPath, const QString &interfaceName, int *interfaceVersion = nullptr)
{
    QHash<QString, int> result;
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }
    QXmlStreamReader xml(&file);
    bool inInterface = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("interface")) {
            inInterface = xml.attributes().value(QLatin1String("name")) == interfaceName;
            if (inInterface && interfaceVersion) {
                *interfaceVersion = xml.attributes().value(QLatin1String("version")).toInt();
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("interface")) {
            inInterface = false;
        } else if (inInterface && xml.isStartElement() && xml.name() == QLatin1String("request")) {
            const auto attrs = xml.attributes();
            const auto since = attrs.value(QLatin1String("since"));
            result.insert(attrs.value(QLatin1String("name")).toString(), since.isEmpty() ? 1 : since.toInt());
        }
    }
    return result;
}

QString readSource(const QString &relative)
{
    QFile file(QStringLiteral(KRDP_SOURCE_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}
}

class WaylandRequestVersionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void supportsFollowsSince()
    {
        QVERIFY(WaylandRequestVersion::supports(5, 5));
        QVERIFY(WaylandRequestVersion::supports(6, 5));
        QVERIFY(!WaylandRequestVersion::supports(4, 5));
        QVERIFY(WaylandRequestVersion::supports(1, 1));
        // Nothing bound: nothing may be sent.
        QVERIFY(!WaylandRequestVersion::supports(0, 1));
    }

    // The Sol regression: a v4 bind must not send the v5 destructor.
    void fakeInputTeardown()
    {
        QCOMPARE(WaylandRequestVersion::teardownFor(4, ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION), Teardown::DropProxy);
        QCOMPARE(WaylandRequestVersion::teardownFor(1, ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION), Teardown::DropProxy);
        QCOMPARE(WaylandRequestVersion::teardownFor(5, ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION), Teardown::SendDestructor);
        QCOMPARE(WaylandRequestVersion::teardownFor(6, ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION), Teardown::SendDestructor);
        QCOMPARE(WaylandRequestVersion::teardownFor(0, ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION), Teardown::None);
    }

    // The generated constants the code gates on match the XML.
    void generatedConstantsMatchXml()
    {
        int fakeInputVersion = 0;
        const auto fakeInput = requestSinceVersions(QStringLiteral(PLASMA_PROTOCOLS_DIR "/fake-input.xml"), QStringLiteral("org_kde_kwin_fake_input"), &fakeInputVersion);
        QVERIFY2(!fakeInput.isEmpty(), "fake-input.xml not readable");
        QCOMPARE(fakeInput.value(QStringLiteral("destroy")), ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION);
        QCOMPARE(fakeInput.value(QStringLiteral("destroy")), 5);
        QCOMPARE(fakeInput.value(QStringLiteral("keyboard_key")), ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION);
        QCOMPARE(fakeInput.value(QStringLiteral("pointer_motion_absolute")), ORG_KDE_KWIN_FAKE_INPUT_POINTER_MOTION_ABSOLUTE_SINCE_VERSION);
        // FakeInput asks Qt for the destructor's version; the protocol must know it.
        QVERIFY(fakeInputVersion >= ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION);

        const auto screencast = requestSinceVersions(QStringLiteral(PLASMA_PROTOCOLS_DIR "/zkde-screencast-unstable-v1.xml"), QStringLiteral("zkde_screencast_unstable_v1"));
        QVERIFY2(!screencast.isEmpty(), "zkde-screencast-unstable-v1.xml not readable");
        QCOMPARE(screencast.value(QStringLiteral("stream_region")), ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_REGION_SINCE_VERSION);
        QCOMPARE(screencast.value(QStringLiteral("stream_virtual_output")), ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_VIRTUAL_OUTPUT_SINCE_VERSION);
    }

    // Every request the sources send is either within the requested bind
    // version or gated on its own *_SINCE_VERSION constant. Catches a new
    // request (or a hand-typed version check like the old `>= 4`) that skips
    // the gate.
    void sourcesGateRequestsNewerThanTheBind()
    {
        struct Case {
            const char *source;
            const char *xml;
            const char *iface;
            const char *macroPrefix;
            int requestedBind;
            QString receiver;
        };
        const Case cases[] = {
            {"src/PlasmaScreencastV1Session.cpp", "fake-input.xml", "org_kde_kwin_fake_input", "ORG_KDE_KWIN_FAKE_INPUT_", ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION,
             QStringLiteral("remoteInterface->")},
            {"src/screencasting.cpp", "zkde-screencast-unstable-v1.xml", "zkde_screencast_unstable_v1", "ZKDE_SCREENCAST_UNSTABLE_V1_",
             ZKDE_SCREENCAST_UNSTABLE_V1_STREAM_REGION_SINCE_VERSION, QStringLiteral("d->")},
        };
        for (const auto &c : cases) {
            const QString source = readSource(QString::fromLatin1(c.source));
            QVERIFY2(!source.isEmpty(), c.source);
            QVERIFY2(!source.contains(QRegularExpression(QStringLiteral(R"(wl_proxy_get_version\([^;]*\)\s*>=\s*\d)"))),
                     "hand-typed version check; gate on the generated *_SINCE_VERSION constant");
            const auto requests = requestSinceVersions(QStringLiteral(PLASMA_PROTOCOLS_DIR "/") + QString::fromLatin1(c.xml), QString::fromLatin1(c.iface));
            for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
                if (it.value() <= 1) {
                    continue;
                }
                const bool sent = source.contains(c.receiver + it.key() + QLatin1Char('('));
                if (!sent) {
                    continue;
                }
                const QString macro = QString::fromLatin1(c.macroPrefix) + it.key().toUpper() + QStringLiteral("_SINCE_VERSION");
                const bool gated = source.contains(macro);
                // Even within the requested bind this is only safe if the
                // compositor advertises that much, so anything newer than v1
                // must be gated.
                QVERIFY2(gated, qPrintable(QStringLiteral("%1 sends %2 (since %3, bind %4) without checking %5")
                                               .arg(QString::fromLatin1(c.source), it.key())
                                               .arg(it.value())
                                               .arg(c.requestedBind)
                                               .arg(macro)));
            }
        }
    }
};

QTEST_GUILESS_MAIN(WaylandRequestVersionTest)

#include "WaylandRequestVersionTest.moc"
