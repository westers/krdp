// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualMaintenanceCommand.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {
bool output(int fd, const QByteArray &bytes) {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto n = ::write(fd, bytes.constData() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += n;
    }
    return true;
}
int refused(const QString &error, int code = 1) {
    output(STDERR_FILENO, QByteArray("Maintenance command refused: ") + error.toUtf8() + '\n');
    return code;
}
}
int main(int argc, char **argv) {
    // No arbitrary commands, paths, profiles, roles, bootstrap or clean setter.
    if (argc != 2 || (std::strcmp(argv[1], "status") && std::strcmp(argv[1], "invalidate"))) {
        output(STDERR_FILENO, QByteArray("Usage: krdp-virtual-maintenance status|invalidate\n"));
        return 64;
    }
    uid_t real, effective, saved;
    if (getresuid(&real, &effective, &saved) || real || effective || saved)
        return refused(QStringLiteral("explicit root caller required"));
    using Guard = KRdp::VirtualSessionMaintenanceGuard;
    QString error;
    if (!std::strcmp(argv[1], "status")) {
        const auto status = Guard::status(&error);
        if (!status) return refused(error);
        return output(STDOUT_FILENO, KRdp::maintenanceStatusJson(*status)) ? 0 : 1;
    }
    auto lease = Guard::maintenance(&error);
    if (!lease) return refused(error);
    const auto result = lease->invalidate(&error);
    if (result != Guard::Publication::Durable)
        return refused(error, result == Guard::Publication::UncertainAfterRename ? 2 : 1);
    // No success-output dependency: successful durable invalidation is the
    // entire operation. It does not authorize any writer outside its own policy.
    return 0;
}
