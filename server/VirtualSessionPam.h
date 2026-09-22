// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QString>
#include <memory>
#include <security/pam_appl.h>
#include <sys/types.h>

namespace KRdp {
/** Session management for an already authenticated, journal-bound account.
 * Not an authentication API. Only the independent privileged desktop owner
 * may use this, after validating its installed PAM policy and clean context.
 * Keep this object in the privileged parent until its desktop has exited;
 * never exec/drop privilege in the owner or copy its PAM handle into a child.
 * The caller must additionally verify the newly created logind session's
 * UID/leader/seat against logind, not infer ownership from PAM environment.
 */
class VirtualSessionPam {
public:
    static std::unique_ptr<VirtualSessionPam> open(uid_t uid, const QByteArray &account);
    ~VirtualSessionPam();
    VirtualSessionPam(const VirtualSessionPam &) = delete;
    VirtualSessionPam &operator=(const VirtualSessionPam &) = delete;
    QString sessionId() const { return m_sessionId; }
    QString runtimeDirectory() const { return m_runtime; }
    // Idempotent. Attempts pam_end even when close_session fails.
    bool close();
private:
    VirtualSessionPam() = default;
    static int conversation(int, const pam_message **, pam_response **, void *);
    pam_conv m_conversation{conversation, nullptr};
    pam_handle_t *m_handle = nullptr;
    bool m_openAttempted = false;
    int m_status = PAM_SUCCESS;
    QString m_sessionId;
    QString m_runtime;
};
}
