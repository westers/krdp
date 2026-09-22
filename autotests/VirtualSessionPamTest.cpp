// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionPam.h"
#include <QTest>
#include <cstdlib>
using namespace Qt::StringLiterals;

// This target compiles the implementation against these PAM doubles, NOT
// libpam: tests cannot open/close a real login or touch installed PAM policy.
namespace {
QStringList calls;
QString failAt;
QByteArray pamUser, sessionId, runtime;
pam_conv conversation;
int lastEndStatus;
bool remapAtOpen;
bool remapAtAccount, nullMetadata;
int userCalls;
int invoke(const char *name) {
    calls.append(QString::fromLatin1(name));
    return failAt == QLatin1String(name) ? PAM_SYSTEM_ERR : PAM_SUCCESS;
}
}
extern "C" {
int pam_start(const char *service, const char *user, const pam_conv *conv, pam_handle_t **handle) {
    Q_ASSERT(QByteArray(service) == "krdp-virtual-session");
    pamUser = user; conversation = *conv;
    *handle = reinterpret_cast<pam_handle_t *>(quintptr(1));
    return invoke("start");
}
int pam_acct_mgmt(pam_handle_t *, int) {
    if (remapAtAccount) pamUser = "other";
    return invoke("account");
}
int pam_get_item(const pam_handle_t *, int item, const void **value) {
    Q_ASSERT(item == PAM_USER); *value = pamUser.constData();
    ++userCalls;
    if (userCalls == 2 && failAt == u"second user"_s) return invoke("second user");
    return invoke("user");
}
int pam_putenv(pam_handle_t *, const char *value) { return invoke(value); }
int pam_open_session(pam_handle_t *, int) {
    if (remapAtOpen) pamUser = "other";
    return invoke("open");
}
const char *pam_getenv(pam_handle_t *, const char *name) {
    if (nullMetadata) return nullptr;
    return QByteArray(name) == "XDG_SESSION_ID" ? sessionId.constData() : runtime.constData();
}
int pam_close_session(pam_handle_t *, int) { return invoke("close"); }
int pam_end(pam_handle_t *, int status) { lastEndStatus = status; return invoke("end"); }
}

class VirtualSessionPamTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init() {
        calls.clear(); failAt.clear(); pamUser.clear();
        sessionId = "c42"; runtime = "/run/user/1000";
        lastEndStatus = -1; remapAtOpen = false;
        remapAtAccount = nullMetadata = false; userCalls = 0;
    }
    void closesOnceAndInOrder() {
        auto session = KRdp::VirtualSessionPam::open(1000, "fixture");
        QVERIFY(session);
        QCOMPARE(session->sessionId(), QStringLiteral("c42"));
        QCOMPARE(session->runtimeDirectory(), QStringLiteral("/run/user/1000"));
        QCOMPARE(calls, QStringList({u"start"_s, u"account"_s, u"user"_s, u"XDG_SESSION_CLASS=background"_s,
            u"XDG_SESSION_TYPE=wayland"_s, u"XDG_SESSION_DESKTOP=KDE"_s, u"open"_s, u"user"_s}));
        QVERIFY(session->close()); QVERIFY(session->close()); session.reset();
        QCOMPARE(calls.count(u"close"_s), 1); QCOMPARE(calls.count(u"end"_s), 1);
        QCOMPARE(calls.last(), QStringLiteral("end")); QCOMPARE(lastEndStatus, PAM_SUCCESS);
    }
    void failureCleanup_data() {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"start", "account", "user", "XDG_SESSION_CLASS=background",
                "XDG_SESSION_TYPE=wayland", "XDG_SESSION_DESKTOP=KDE", "open"})
            QTest::newRow(stage) << QString::fromLatin1(stage);
    }
    void failureCleanup() {
        QFETCH(QString, stage); failAt = stage;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture"));
        QCOMPARE(calls.count(u"end"_s), stage == u"start"_s ? 0 : 1);
        QCOMPARE(calls.count(u"close"_s), stage == u"open"_s ? 1 : 0);
        if (stage == u"start"_s) {
            QCOMPARE(calls, QStringList({u"start"_s})); return;
        }
        QCOMPARE(lastEndStatus, PAM_SYSTEM_ERR);
    }
    void invalidMetadataClosesOpenedSession_data() {
        QTest::addColumn<QByteArray>("id"); QTest::addColumn<QByteArray>("path");
        QTest::newRow("missing session") << QByteArray() << QByteArray("/run/user/1000");
        QTest::newRow("invalid session") << QByteArray("../c42") << QByteArray("/run/user/1000");
        QTest::newRow("trailing newline") << QByteArray("c42\n") << QByteArray("/run/user/1000");
        QTest::newRow("wrong uid") << QByteArray("c42") << QByteArray("/run/user/1001");
        QTest::newRow("missing runtime") << QByteArray("c42") << QByteArray();
        QTest::newRow("private runtime borrowed") << QByteArray("c42") << QByteArray("/run/user/1000/krdp-virtual/x");
    }
    void invalidMetadataClosesOpenedSession() {
        QFETCH(QByteArray, id); QFETCH(QByteArray, path); sessionId = id; runtime = path;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture"));
        QCOMPARE(calls.sliced(calls.size() - 2), QStringList({u"close"_s, u"end"_s}));
        QCOMPARE(lastEndStatus, PAM_SESSION_ERR);
    }
    void changedAccountClosesOpenedSession() {
        remapAtOpen = true;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture"));
        QCOMPARE(calls.count(u"close"_s), 1); QCOMPARE(lastEndStatus, PAM_USER_UNKNOWN);
    }
    void changedAccountBeforeOpen() {
        remapAtAccount = true;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture"));
        QCOMPARE(calls.count(u"open"_s), 0); QCOMPARE(calls.count(u"end"_s), 1);
        QCOMPARE(lastEndStatus, PAM_USER_UNKNOWN);
    }
    void secondUserReadFailureAndNullMetadata() {
        failAt = u"second user"_s;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture"));
        QCOMPARE(calls.count(u"close"_s), 1); QCOMPARE(lastEndStatus, PAM_SYSTEM_ERR);
        init(); nullMetadata = true;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture"));
        QCOMPARE(calls.count(u"close"_s), 1); QCOMPARE(lastEndStatus, PAM_SESSION_ERR);
    }
    void endFailureIsReportedWithoutRetry() {
        auto session = KRdp::VirtualSessionPam::open(1000, "fixture"); QVERIFY(session);
        failAt = u"end"_s; QVERIFY(!session->close()); session.reset();
        QCOMPARE(calls.count(u"end"_s), 1);
    }
    void destructorClosesAndEndRunsAfterCloseFailure() {
        auto session = KRdp::VirtualSessionPam::open(1000, "fixture"); QVERIFY(session);
        failAt = u"close"_s; QVERIFY(!session->close()); session.reset();
        QCOMPARE(calls.count(u"close"_s), 1); QCOMPARE(calls.count(u"end"_s), 1);
        QCOMPARE(lastEndStatus, PAM_SYSTEM_ERR);
    }
    void destructorCloses() {
        { auto session = KRdp::VirtualSessionPam::open(1000, "fixture"); QVERIFY(session); }
        QCOMPARE(calls.count(u"close"_s), 1); QCOMPARE(calls.count(u"end"_s), 1);
    }
    void cleanupDeadlineArmedBeforePartialFailureCleanup() {
        failAt = u"open"_s;
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, "fixture", [] { calls.append(u"deadline"_s); }));
        QCOMPARE(calls.sliced(calls.size() - 3), QStringList({u"deadline"_s, u"close"_s, u"end"_s}));
        init();
        auto session = KRdp::VirtualSessionPam::open(1000, "fixture", [] { calls.append(u"deadline"_s); });
        QVERIFY(session); QVERIFY(session->close()); session.reset();
        QCOMPARE(calls.count(u"deadline"_s), 1);
        QCOMPARE(calls.sliced(calls.size() - 3), QStringList({u"deadline"_s, u"close"_s, u"end"_s}));
    }
    void neverPromptsOrLogsModuleText() {
        auto session = KRdp::VirtualSessionPam::open(1000, "fixture"); QVERIFY(session);
        pam_message info{PAM_TEXT_INFO, "information"}, prompt{PAM_PROMPT_ECHO_OFF, "password"};
        const pam_message *messages[] = {&info, &prompt};
        pam_response *responses = nullptr;
        QCOMPARE(conversation.conv(2, messages, &responses, nullptr), PAM_CONV_ERR); QVERIFY(!responses);
        QCOMPARE(conversation.conv(1, messages, &responses, nullptr), PAM_SUCCESS);
        QVERIFY(responses); QVERIFY(!responses[0].resp); free(responses);
        responses = nullptr;
        info.msg_style = PAM_ERROR_MSG;
        QCOMPARE(conversation.conv(1, messages, &responses, nullptr), PAM_SUCCESS); free(responses);
        responses = nullptr;
        QCOMPARE(conversation.conv(0, messages, &responses, nullptr), PAM_CONV_ERR);
        QCOMPARE(conversation.conv(1, nullptr, &responses, nullptr), PAM_CONV_ERR);
        QCOMPARE(conversation.conv(1, messages, nullptr, nullptr), PAM_CONV_ERR);
        prompt.msg_style = PAM_PROMPT_ECHO_ON;
        QCOMPARE(conversation.conv(2, messages, &responses, nullptr), PAM_CONV_ERR);
        QVERIFY(!responses);
    }
    void refusesInvalidIdentityBeforePam() {
        QVERIFY(!KRdp::VirtualSessionPam::open(0, "root"));
        QVERIFY(!KRdp::VirtualSessionPam::open(uid_t(-1), "fixture"));
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, QByteArray("user\0suffix", 11)));
        QVERIFY(!KRdp::VirtualSessionPam::open(1000, {})); QVERIFY(calls.isEmpty());
    }
};
QTEST_GUILESS_MAIN(VirtualSessionPamTest)
#include "VirtualSessionPamTest.moc"
