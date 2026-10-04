// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Fixed-purpose privileged stdin/stdout protocol. Never accept caller paths.
#include "BrokerAuthenticationAdmin.h"
#include "PrivateExecutionContext.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <openssl/crypto.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace KRdp;
namespace {
int reply(const QJsonObject &object, int status)
{
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly)) return 1;
    return output.write(bytes) == bytes.size() && output.flush() ? status : 1;
}
int fail(const QString &error) { return reply({{u"error"_s, error}}, 1); }
}

int main(int argc, char **argv)
{
    if (!enterPrivateExecutionContext()) return fail(u"private execution context unavailable"_s);
    if (geteuid()) return fail(u"administrator authorization is required"_s);
    if (argc != 1) return fail(u"invalid helper invocation"_s);
    QCoreApplication app(argc, argv);
    // Refuse an unbounded/blocking stdin. The UI writes one bounded document
    // then closes its pipe; authorization cancellation never reaches this code.
    QByteArray input;
    QElapsedTimer deadline;
    deadline.start();
    const auto clear = qScopeGuard([&] { if (!input.isEmpty()) OPENSSL_cleanse(input.data(), size_t(input.size())); });
    while (true) {
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const auto remaining = 10000 - deadline.elapsed();
        if (remaining <= 0) return fail(u"authentication request timed out"_s);
        const auto polled = ::poll(&descriptor, 1, int(remaining));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) return fail(u"authentication request timed out"_s);
        char chunk[4096];
        const auto count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return fail(u"authentication request cannot be read"_s);
        if (!count) break;
        input.append(chunk, qsizetype(count));
        OPENSSL_cleanse(chunk, sizeof(chunk));
        if (input.size() > BrokerAuthentication::MaximumBytes) return fail(u"authentication request is oversized"_s);
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(input, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(u"invalid authentication request"_s);
    const auto request = document.object();
    const auto operation = request.value(u"operation"_s).toString();
    if (!request.value(u"version"_s).isDouble() || request.value(u"version"_s).toDouble() != 1
        || (operation != u"read"_s && operation != u"save"_s)
        || request.size() != (operation == u"read"_s ? 2 : 3)
        || (operation == u"save"_s && !request.value(u"update"_s).isObject())) return fail(u"invalid authentication request"_s);

    const int directory = ::open("/etc/farside", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return fail(u"Farside policy directory is unavailable"_s);
    const auto closeDirectory = qScopeGuard([directory] { ::close(directory); });
    struct stat parent{};
    if (::fstat(directory, &parent) || parent.st_uid || (parent.st_mode & 0022)) return fail(u"Farside policy directory is unsafe"_s);
    const int lock = ::openat(directory, ".authentication.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (lock < 0) return fail(u"authentication policy lock is unavailable"_s);
    const auto closeLock = qScopeGuard([lock] { ::close(lock); });
    struct stat lockState{};
    if (::fstat(lock, &lockState) || !S_ISREG(lockState.st_mode) || lockState.st_uid || lockState.st_nlink != 1
        || (lockState.st_mode & 0077) || ::flock(lock, LOCK_EX | LOCK_NB)) return fail(u"authentication policy is busy or its lock is unsafe"_s);
    const auto current = BrokerAuthentication::readFile(u"/etc/farside/authentication.json"_s, false);
    if (!current.policy) return fail(current.error);
    if (operation == u"read"_s) {
        const auto result = BrokerAuthenticationAdmin::view(current, BrokerAuthenticationAdmin::accountName);
        return result.error.isEmpty() ? reply({{u"snapshot"_s, result.value}}, 0) : fail(result.error);
    }
    const auto update = BrokerAuthenticationAdmin::prepare(current, request.value(u"update"_s).toObject(), BrokerAuthentication::resolveAccount);
    if (!update.error.isEmpty()) return fail(update.error);
    // Hashing passwords may take time. Refuse a concurrent administrator edit
    // even when its writer did not take our cooperating-writer lock.
    const auto fresh = BrokerAuthentication::readFile(u"/etc/farside/authentication.json"_s, false);
    if (!fresh.policy || fresh.document != current.document) return fail(u"authentication policy changed; reload before saving"_s);
    QSaveFile file(u"/etc/farside/authentication.json"_s);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(update.document) != update.document.size() || !file.flush() || ::fsync(file.handle()) || !file.commit())
        return fail(u"authentication policy could not be saved atomically"_s);
    const bool durable = ::fsync(directory) == 0;
    const auto saved = BrokerAuthentication::readFile(u"/etc/farside/authentication.json"_s, true);
    const auto result = BrokerAuthenticationAdmin::view(saved, BrokerAuthenticationAdmin::accountName);
    if (!result.error.isEmpty()) return reply({{u"saved"_s, true}, {u"error"_s, u"policy saved but readback failed; reload before restarting"_s}}, 1);
    QJsonObject response{{u"saved"_s, true}, {u"restartRequired"_s, true}, {u"snapshot"_s, result.value}};
    if (!durable) response.insert(u"warning"_s, u"policy saved but directory synchronization failed"_s);
    return reply(response, 0);
}
