// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QString>
namespace KRdp {
// Bounded PEM parsing; no interactive passphrase prompt and no secret diagnostics.
bool validVirtualHostTls(const QString &certificate, const QString &key);
}
