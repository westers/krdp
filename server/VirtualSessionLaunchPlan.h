// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QDir>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSize>
#include <QStringList>
#include <QUuid>
#include <optional>

namespace KRdp
{
/** Pure construction of a production namespace launch, never from RDP JSON.
 * Account values must come from the OS account database for the PAM UID.
 * Installation paths and rendering policy come from administrator configuration.
 * The executor must validate directory ownership/no symlinks, resolve permitted
 * PCI identities to device nodes, drop identity and create runtime/token files.
 * This value alone is NOT authorization, filesystem validation or recovery.
 */
struct VirtualSessionLaunchPlan {
    struct Account {
        quint32 uid;
        QString login;
        QString home;
    };
    struct Configuration {
        QString launcher;
        QString worker;
        QString supportDirectory;
        QStringList allowedRenderPci; // stable identities, never renderD indexes
        QSize initialSize{1280, 720};
    };
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
    QString runtimeDirectory;
    QString profileDirectory;
    QString socketPath;
    QString tokenPath;

    static bool absoluteCleanPath(const QString &path)
    {
        return path != QStringLiteral("/") && QDir::isAbsolutePath(path)
            && QDir::cleanPath(path) == path && !path.contains(QChar::Null)
            && !path.contains(QLatin1Char('\n'));
    }
    static std::optional<VirtualSessionLaunchPlan> build(quint32 authenticatedUid,
            const Account &account, const QString &session, const Configuration &config, QString *error = nullptr,
            const QString &recordedLaunch = {})
    {
        const auto refuse = [error](const QString &why) -> std::optional<VirtualSessionLaunchPlan> {
            if (error) *error = why;
            return {};
        };
        if (!authenticatedUid || authenticatedUid != account.uid || account.login.isEmpty()
            || account.login.contains(QChar::Null) || !absoluteCleanPath(account.home)) {
            return refuse(QStringLiteral("A resolved nonroot account matching the PAM UID is required"));
        }
        const QUuid id(session);
        if (id.isNull() || id.toString(QUuid::WithoutBraces) != session) {
            return refuse(QStringLiteral("A canonical server-generated session UUID is required"));
        }
        if (!recordedLaunch.isEmpty() && (QUuid(recordedLaunch).isNull()
            || QUuid(recordedLaunch).toString(QUuid::WithoutBraces) != recordedLaunch)) {
            return refuse(QStringLiteral("Recorded launch identity must be a canonical non-null UUID"));
        }
        if (!absoluteCleanPath(config.launcher) || !absoluteCleanPath(config.worker)
            || !absoluteCleanPath(config.supportDirectory)) {
            return refuse(QStringLiteral("Installed launcher, worker and support paths must be absolute and clean"));
        }
        if (config.initialSize.width() < 320 || config.initialSize.height() < 200
            || config.initialSize.width() > 4096 || config.initialSize.height() > 4096
            || config.initialSize.width() % 2 || config.initialSize.height() % 2) {
            return refuse(QStringLiteral("Initial capture size must be even and within 320x200 through 4096x4096"));
        }
        // Empty means no GPU grant, not implicit access to every GPU. Until a
        // CPU compositor has capture acceptance, fail before creating a desktop.
        if (config.allowedRenderPci.isEmpty()) {
            return refuse(QStringLiteral("An explicit render-device allow-list is required; CPU-only capture is not yet accepted"));
        }
        static const QRegularExpression pci(QStringLiteral("^[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\\.[0-7]$"));
        QStringList unique;
        for (const auto &device : config.allowedRenderPci) {
            if (!pci.match(device).hasMatch() || unique.contains(device)) {
                return refuse(QStringLiteral("Render devices must be distinct canonical PCI identities"));
            }
            unique.append(device);
        }
        VirtualSessionLaunchPlan plan;
        // Production receives the fresh identity already persisted by the broker;
        // diagnostics allocate it here. This does not authorize runtime reuse:
        // the executor still refuses any preexisting generation's socket/token.
        plan.runtimeDirectory = QStringLiteral("/run/user/%1/krdp-virtual/%2")
            .arg(account.uid).arg(recordedLaunch.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : recordedLaunch);
        plan.profileDirectory = account.home + QStringLiteral("/.krdp-virtual/sessions/") + session;
        plan.socketPath = plan.runtimeDirectory + QStringLiteral("/worker.sock");
        plan.tokenPath = plan.runtimeDirectory + QStringLiteral("/worker-token");
        plan.program = config.launcher;
        plan.arguments = {QStringLiteral("--runtime"), plan.runtimeDirectory,
            QStringLiteral("--profile"), plan.profileDirectory, QStringLiteral("--session"), session,
            QStringLiteral("--uid"), QString::number(account.uid), QStringLiteral("--worker"), config.worker,
            QStringLiteral("--support"), config.supportDirectory, QStringLiteral("--width"), QString::number(config.initialSize.width()),
            QStringLiteral("--height"), QString::number(config.initialSize.height())};
        for (const auto &device : unique) plan.arguments.append({QStringLiteral("--allow-render-pci"), device});
        // No inherited display, bus, systemd-manager, Qt plugin, loader, or
        // physical-session audio environment. The namespace supplies its own.
        plan.environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
        plan.environment.insert(QStringLiteral("HOME"), account.home);
        plan.environment.insert(QStringLiteral("USER"), account.login);
        plan.environment.insert(QStringLiteral("LOGNAME"), account.login);
        plan.environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        if (error) error->clear();
        return plan;
    }
};
}
