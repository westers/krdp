// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Manual Sol-only systemd EnvironmentFile agreement. Does not touch installed
// broker files/services or launch a desktop, PipeWire or an RDP listener.
#include "BrokerHostSettings.h"
#include <QCoreApplication>
#include <QFile>
#include <QProcessEnvironment>
#include <QSysInfo>
using namespace Qt::StringLiterals;
using namespace KRdp::BrokerHostSettings;
namespace {
QByteArray fixture(Scope scope, bool edited)
{
    QByteArray document("# retained comment\\\n"
        "FARSIDE_FORMAT_OTHER='multi\nline $HOME \\ literal'\n"
        "CUSTOM_EXECUTION=\"fixture-only-secret\"\n");
    const auto values = defaults(scope);
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        document += environmentName(scope, it.key()).toUtf8() + "='" + it.value().toString().toUtf8() + "'\n";
    const auto prefix = scope == Scope::Console ? QByteArray("FARSIDE_CONSOLE_") : QByteArray("FARSIDE_VIRTUAL_");
    if (scope == Scope::VirtualSession) {
        document += "FARSIDE_VIRTUAL_RENDER_PCI=0000:01:00.0,\\\n0000:c5:00.0\n"
            "FARSIDE_VIRTUAL_VAAPI_DRIVER='i'\"HD\"\n";
    } else {
        document += prefix + "PORT=invalid-earlier-duplicate\n";
        document += prefix + "PORT=43\\\n21\n";
        document += prefix + "CERTIFICATE=\"/root/TLS space \\$HOME \\`tick \\\"quote \\\\slash \\q.pem\"\n";
        document += prefix + "CERTIFICATE_KEY='/root/TLS key \\ literal.pem'\n";
        document += prefix + "ADAPTIVE_QUALITY='tr'\"ue\"\n";
    }
    if (!edited) return document;
    auto desired = parse(scope, document).overrides;
    if (scope == Scope::VirtualSession) desired[u"VaapiDriver"_s] = u"off"_s;
    else {
        desired[u"Port"_s] = u"5432"_s;
        desired[u"Certificate"_s] = u"/root/TLS apostrophe' dollar$HOME slash\\ tick` double\".pem"_s;
    }
    const auto result = edit(scope, document, desired);
    return result.error.isEmpty() ? result.contents : QByteArray();
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4 || (args[1] != u"--emit" && args[1] != u"--check")
        || (args[3] != u"raw" && args[3] != u"edited")) return 2;
    Scope scope;
    if (args[2] == u"console") scope = Scope::Console;
    else if (args[2] == u"virtual") scope = Scope::Virtual;
    else if (args[2] == u"session") scope = Scope::VirtualSession;
    else return 2;
    const auto document = fixture(scope, args[3] == u"edited");
    const auto snapshot = parse(scope, document);
    if (document.isEmpty() || !snapshot.error.isEmpty()) return 3;
    if (args[1] == u"--emit") {
        QFile output;
        return output.open(stdout, QIODevice::WriteOnly) && output.write(document) == document.size() && output.flush() ? 0 : 4;
    }
    if (QSysInfo::machineHostName().section(u'.', 0, 0) != u"sol" || qgetenv("FARSIDE_FORMAT_NATIVE") != "1") return 5;
    const auto environment = QProcessEnvironment::systemEnvironment();
    for (auto it = snapshot.overrides.cbegin(); it != snapshot.overrides.cend(); ++it) {
        const auto name = environmentName(scope, it.key());
        if (!environment.contains(name) || environment.value(name) != it.value().toString()) {
            qCritical().noquote() << "EnvironmentFile mismatch for" << it.key();
            return 6;
        }
    }
    if (environment.value(u"FARSIDE_FORMAT_OTHER"_s) != u"multi\nline $HOME \\ literal"_s
        || environment.value(u"CUSTOM_EXECUTION"_s) != u"fixture-only-secret"_s) return 7;
    qInfo().noquote() << "PASS actual systemd EnvironmentFile agrees with typed" << args[2] << args[3]
        << "and preserves unrelated quoted multiline values";
    return 0;
}
