// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// REVIEW-S1: the PAM conversation callback RdpConnection answers with the
// client's user name and password. Header-only so a test can drive it with
// fake messages without a PAM stack.

#pragma once

#include <QByteArray>

#include <cstdlib>
#include <cstring>

#include <security/pam_appl.h>

namespace KRdp
{

struct PamConversationData {
    QByteArray user;
    QByteArray password;
    /** PAM_TEXT_INFO / PAM_ERROR_MSG messages that were accepted and dropped (their text is never kept or logged). */
    int ignoredMessages = 0;
};

/** Overwrites @p bytes in place (not optimised away) and empties it. */
inline void scrubBytes(QByteArray &bytes)
{
    if (!bytes.isEmpty()) {
        bytes.detach();
        ::explicit_bzero(bytes.data(), size_t(bytes.size()));
    }
    bytes.clear();
}

/**
 * Prompts get the user name (echo on) or the password (echo off). A stack that
 * only prints informational text (PAM_TEXT_INFO) or an error text
 * (PAM_ERROR_MSG) expects an acknowledgement and no answer, so those are
 * accepted and ignored; any other message style fails the conversation.
 */
inline int pamConversation(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr)
{
    if (num_msg <= 0 || !msg || !resp || !appdata_ptr) {
        return PAM_CONV_ERR;
    }
    auto *appdata = static_cast<PamConversationData *>(appdata_ptr);
    auto *response = static_cast<struct pam_response *>(std::calloc(size_t(num_msg), sizeof(struct pam_response)));
    if (!response) {
        return PAM_BUF_ERR;
    }

    int status = PAM_SUCCESS;
    int ignored = 0;
    for (int index = 0; index < num_msg && status == PAM_SUCCESS; index++) {
        switch (msg[index]->msg_style) {
        case PAM_PROMPT_ECHO_ON:
            response[index].resp = ::strdup(appdata->user.constData());
            if (!response[index].resp) {
                status = PAM_BUF_ERR;
            }
            response[index].resp_retcode = PAM_SUCCESS;
            break;
        case PAM_PROMPT_ECHO_OFF:
            response[index].resp = ::strdup(appdata->password.constData());
            if (!response[index].resp) {
                status = PAM_BUF_ERR;
            }
            response[index].resp_retcode = PAM_SUCCESS;
            break;
        case PAM_TEXT_INFO:
        case PAM_ERROR_MSG:
            response[index].resp = nullptr;
            response[index].resp_retcode = PAM_SUCCESS;
            ++ignored;
            break;
        default:
            status = PAM_CONV_ERR;
            break;
        }
    }

    if (status != PAM_SUCCESS) {
        for (int index = 0; index < num_msg; ++index) {
            if (response[index].resp) {
                ::explicit_bzero(response[index].resp, ::strlen(response[index].resp));
                std::free(response[index].resp);
            }
        }
        ::explicit_bzero(response, sizeof(struct pam_response) * size_t(num_msg));
        std::free(response);
        *resp = nullptr;
        return status;
    }

    appdata->ignoredMessages += ignored;
    *resp = response;
    return PAM_SUCCESS;
}

}
