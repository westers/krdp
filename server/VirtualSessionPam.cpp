// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionPam.h"
#include <QRegularExpression>
#include <cstdlib>

namespace KRdp {
int VirtualSessionPam::conversation(int count, const pam_message **messages, pam_response **responses, void *)
{
    if (!responses) return PAM_CONV_ERR;
    *responses = nullptr;
    if (count <= 0 || count > PAM_MAX_NUM_MSG || !messages) return PAM_CONV_ERR;
    // Session/account modules may emit information, but this service never
    // collects a password or accepts a prompt from a background process.
    for (int i = 0; i < count; ++i) {
        if (!messages[i] || (messages[i]->msg_style != PAM_TEXT_INFO && messages[i]->msg_style != PAM_ERROR_MSG))
            return PAM_CONV_ERR;
    }
    *responses = static_cast<pam_response *>(calloc(size_t(count), sizeof(pam_response)));
    return *responses ? PAM_SUCCESS : PAM_BUF_ERR;
}

std::unique_ptr<VirtualSessionPam> VirtualSessionPam::open(uid_t uid, const QByteArray &account, std::function<void()> beforeClose)
{
    if (!uid || uid == uid_t(-1) || account.isEmpty() || account.contains('\0') || account.size() > 256) return {};
    auto session = std::unique_ptr<VirtualSessionPam>(new VirtualSessionPam);
    auto &s = *session;
    s.m_beforeClose = std::move(beforeClose);
    pam_handle_t *handle = nullptr;
    s.m_status = pam_start("krdp-virtual-session", account.constData(), &s.m_conversation, &handle);
    // The output is undefined on failure; only a successful start transfers
    // ownership to us. Never pass a failed start's output back to PAM.
    if (s.m_status != PAM_SUCCESS || !handle) return {};
    s.m_handle = handle;
    s.m_status = pam_acct_mgmt(s.m_handle, PAM_SILENT);
    if (s.m_status != PAM_SUCCESS) return {};
    const auto sameAccount = [&] {
        const void *value = nullptr;
        s.m_status = pam_get_item(s.m_handle, PAM_USER, &value);
        if (s.m_status != PAM_SUCCESS) return false;
        if (!value || QByteArray(static_cast<const char *>(value)) != account) {
            s.m_status = PAM_USER_UNKNOWN;
            return false;
        }
        return true;
    };
    if (!sameAccount()) return {};
    // Deliberately no seat/VT/display or inherited user-manager environment.
    for (const char *value : {"XDG_SESSION_CLASS=background", "XDG_SESSION_TYPE=wayland", "XDG_SESSION_DESKTOP=KDE"}) {
        s.m_status = pam_putenv(s.m_handle, value);
        if (s.m_status != PAM_SUCCESS) return {};
    }
    // A later session module can fail after an earlier one already registered
    // resources. Always run the close stack after an attempted open.
    s.m_openAttempted = true;
    s.m_status = pam_open_session(s.m_handle, PAM_SILENT);
    if (s.m_status != PAM_SUCCESS) return {};
    if (!sameAccount()) return {};
    const char *id = pam_getenv(s.m_handle, "XDG_SESSION_ID");
    const char *runtime = pam_getenv(s.m_handle, "XDG_RUNTIME_DIR");
    s.m_sessionId = QString::fromUtf8(id ? id : "");
    s.m_runtime = QString::fromUtf8(runtime ? runtime : "");
    // pam_systemd may return success without registering a new session. Missing
    // metadata must not silently degrade to borrowing a physical login.
    if (!QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z")).match(s.m_sessionId).hasMatch()
        || s.m_runtime != QStringLiteral("/run/user/%1").arg(uid)) {
        s.m_status = PAM_SESSION_ERR;
        return {};
    }
    return session;
}

bool VirtualSessionPam::close()
{
    if (!m_handle) return true;
    if (m_beforeClose) m_beforeClose();
    int closeStatus = PAM_SUCCESS;
    if (m_openAttempted) closeStatus = pam_close_session(m_handle, PAM_SILENT);
    const int endStatus = pam_end(m_handle, closeStatus == PAM_SUCCESS ? m_status : closeStatus);
    m_handle = nullptr;
    m_openAttempted = false;
    m_sessionId.clear(); m_runtime.clear();
    return closeStatus == PAM_SUCCESS && endStatus == PAM_SUCCESS;
}
VirtualSessionPam::~VirtualSessionPam() { close(); }
}
