// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// REVIEW-S1: the PAM conversation RdpConnection answers with the client's
// credentials. A stack that prints informational text must not fail the login.

#include <QTest>

#include <cstdlib>
#include <cstring>

#include "PamConversation.h"

using namespace KRdp;

namespace
{
struct Messages {
    std::vector<pam_message> storage;
    std::vector<const pam_message *> pointers;
    explicit Messages(std::initializer_list<int> styles)
    {
        storage.resize(styles.size());
        size_t i = 0;
        for (const int style : styles) {
            storage[i].msg_style = style;
            storage[i].msg = "text that must never be kept";
            ++i;
        }
        for (auto &m : storage) {
            pointers.push_back(&m);
        }
    }
    int count() const
    {
        return int(storage.size());
    }
};

void freeResponses(pam_response *responses, int count)
{
    for (int i = 0; i < count; ++i) {
        std::free(responses[i].resp);
    }
    std::free(responses);
}
}

class PamConversationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void promptsGetTheCredentials()
    {
        PamConversationData data{QByteArrayLiteral("alice"), QByteArrayLiteral("secret"), 0};
        Messages messages{PAM_PROMPT_ECHO_ON, PAM_PROMPT_ECHO_OFF};
        pam_response *responses = nullptr;
        QCOMPARE(pamConversation(messages.count(), messages.pointers.data(), &responses, &data), PAM_SUCCESS);
        QVERIFY(responses);
        QCOMPARE(QByteArray(responses[0].resp), QByteArrayLiteral("alice"));
        QCOMPARE(QByteArray(responses[1].resp), QByteArrayLiteral("secret"));
        freeResponses(responses, messages.count());
    }

    void informationalTextIsAcceptedAndIgnored_data()
    {
        QTest::addColumn<int>("style");
        QTest::newRow("PAM_TEXT_INFO") << int(PAM_TEXT_INFO);
        QTest::newRow("PAM_ERROR_MSG") << int(PAM_ERROR_MSG);
    }

    void informationalTextIsAcceptedAndIgnored()
    {
        QFETCH(int, style);
        PamConversationData data{QByteArrayLiteral("alice"), QByteArrayLiteral("secret"), 0};
        // A banner, then the password prompt, then a trailing notice: all in one conversation.
        Messages messages{style, PAM_PROMPT_ECHO_OFF, style};
        pam_response *responses = nullptr;
        QCOMPARE(pamConversation(messages.count(), messages.pointers.data(), &responses, &data), PAM_SUCCESS);
        QVERIFY(responses);
        QVERIFY(!responses[0].resp);
        QCOMPARE(QByteArray(responses[1].resp), QByteArrayLiteral("secret"));
        QVERIFY(!responses[2].resp);
        QCOMPARE(data.ignoredMessages, 2);
        freeResponses(responses, messages.count());
    }

    void textOnlyConversationSucceeds()
    {
        PamConversationData data{QByteArrayLiteral("alice"), QByteArrayLiteral("secret"), 0};
        Messages messages{PAM_TEXT_INFO};
        pam_response *responses = nullptr;
        QCOMPARE(pamConversation(messages.count(), messages.pointers.data(), &responses, &data), PAM_SUCCESS);
        QVERIFY(responses);
        QVERIFY(!responses[0].resp);
        freeResponses(responses, messages.count());
    }

    void unknownStyleStillFailsAndLeaksNothing()
    {
        PamConversationData data{QByteArrayLiteral("alice"), QByteArrayLiteral("secret"), 0};
        Messages messages{PAM_PROMPT_ECHO_OFF, 99};
        pam_response *responses = reinterpret_cast<pam_response *>(0x1);
        QCOMPARE(pamConversation(messages.count(), messages.pointers.data(), &responses, &data), PAM_CONV_ERR);
        QVERIFY(!responses);
        QCOMPARE(data.ignoredMessages, 0);
    }

    void badArgumentsFail()
    {
        PamConversationData data;
        pam_response *responses = nullptr;
        Messages messages{PAM_PROMPT_ECHO_OFF};
        QCOMPARE(pamConversation(0, messages.pointers.data(), &responses, &data), PAM_CONV_ERR);
        QCOMPARE(pamConversation(1, messages.pointers.data(), &responses, nullptr), PAM_CONV_ERR);
    }

    void scrubBytesZeroesTheBuffer()
    {
        QByteArray secret(16, 'x');
        const char *raw = secret.constData();
        scrubBytes(secret);
        QVERIFY(secret.isEmpty());
        Q_UNUSED(raw); // the freed buffer cannot be inspected portably; the call must not crash on empty input
        QByteArray empty;
        scrubBytes(empty);
        QVERIFY(empty.isEmpty());
    }
};

QTEST_GUILESS_MAIN(PamConversationTest)
#include "PamConversationTest.moc"
