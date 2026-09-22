// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualHostTls.h"
#include <QFile>
#include <QFileInfo>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <memory>

namespace KRdp {
bool validVirtualHostTls(const QString &certificate, const QString &key)
{
    const auto readPem = [](const QString &path) {
        QFile file(path);
        if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) return QByteArray();
        auto bytes = file.read(1024 * 1024 + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > 1024 * 1024) return QByteArray();
        return bytes;
    };
    const auto certificateBytes = readPem(certificate), keyBytes = readPem(key);
    if (certificateBytes.isEmpty() || keyBytes.isEmpty()) return false;
    std::unique_ptr<BIO, decltype(&BIO_free)> certBio(BIO_new_mem_buf(certificateBytes.constData(), int(certificateBytes.size())), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(BIO_new_mem_buf(keyBytes.constData(), int(keyBytes.size())), BIO_free);
    if (!certBio || !keyBio) return false;
    std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free);
    // The service has no password-input UI; never let OpenSSL prompt on stdin.
    const auto noPassword = [](char *, int, int, void *) -> int { return 0; };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> privateKey(
        PEM_read_bio_PrivateKey(keyBio.get(), nullptr, noPassword, nullptr), EVP_PKEY_free);
    return cert && privateKey && X509_check_private_key(cert.get(), privateKey.get()) == 1;
}
}
