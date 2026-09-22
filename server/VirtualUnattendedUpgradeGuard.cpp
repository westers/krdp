// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualUnattendedUpgradeGuard.h"
#include <QList>
#include <unistd.h>
#include <utility>

namespace KRdp {
namespace {
int fail(QString *error, const char *message) {
    if (error) *error = QString::fromLatin1(message);
    return 1;
}
}
int VirtualUnattendedUpgradeGuard::run(int argc, char *const argv[], QString *error) {
    if (error) error->clear();
    uid_t real{}, effective{}, saved{};
    if (getresuid(&real, &effective, &saved) || real || effective || saved)
        return fail(error, "Unattended upgrade guard requires real, effective and saved root");
    auto lease = VirtualSessionMaintenanceGuard::maintenance(error);
    return runWithLease(argc, argv, std::move(lease), QByteArray("/usr/libexec/krdp/unattended-upgrade.real"), error);
}
int VirtualUnattendedUpgradeGuard::runWithLease(int argc, char *const argv[],
    std::optional<VirtualSessionMaintenanceGuard::Lease> lease, const QByteArray &backend, QString *error) {
    if (!lease) return fail(error, "Cannot acquire unattended upgrade maintenance gate");
    if (argc < 1 || !argv || !argv[0] || backend.isEmpty() || backend.contains('\0'))
        return fail(error, "Invalid unattended upgrade arguments");
    QList<char *> arguments;
    arguments.reserve(qsizetype(argc) + 1);
    arguments.append(const_cast<char *>(backend.constData()));
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) return fail(error, "Invalid unattended upgrade argument boundary");
        arguments.append(argv[i]);
    }
    arguments.append(nullptr);
    if (lease->invalidate(error) != VirtualSessionMaintenanceGuard::Publication::Durable)
        return fail(error, "Unattended upgrade maintenance invalidation was not durable; backend not executed");
    // Release BEFORE exec, not merely via CLOEXEC: the backend/package hooks
    // may acquire the gate again. Never retain package locks across this call.
    lease.reset();
    ::execv(backend.constData(), arguments.data());
    return fail(error, "Cannot execute fixed unattended upgrade backend; maintenance remains blocked");
}
}
