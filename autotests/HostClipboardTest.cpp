// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-067: the worker's side of the clipboard bridge against a fake host clipboard (no compositor, no daemon).

#include <QLoggingCategory>
#include <QTest>

#include "HostClipboard.h"

using namespace KRdp;
using namespace KRdp::ConsoleWorkerWire;

namespace
{
const QString Canary = QStringLiteral("CANARY-7f3a91-do-not-log");

struct FakeHost {
    QString text;
    bool hasText = true;
    bool writeOk = true;
    int reads = 0;
    QList<QPair<QString, bool>> writes; // (text, secret)
    HostClipboard::Backend backend()
    {
        return {[this] { return hasText; },
                [this] { ++reads; return text; },
                [this](const QString &t, bool secret) {
                    writes.append({t, secret});
                    if (writeOk) text = t;
                    return writeOk;
                }};
    }
};

struct Harness {
    FakeHost host;
    QList<Record> sent;
    Deframer deframer;
    std::unique_ptr<HostClipboard> board;
    explicit Harness(bool desktop = true)
        : board(new HostClipboard(host.backend(), [this](const QByteArray &record) {
              deframer.feed(record);
              while (const auto next = deframer.next()) sent.append(*next);
          }, desktop))
    {
    }
    void allow(bool hostToClient, bool clientToHost, bool greeter = false, bool locked = false, quint64 revision = 1, quint64 generation = 5)
    {
        board->setControl({generation, true});
        board->setPolicy({revision, generation, hostToClient, clientToHost, locked, greeter});
    }
    template<typename T, typename F>
    QList<T> take(F decode)
    {
        QList<T> out;
        for (const auto &record : std::as_const(sent)) {
            if (const auto value = decode(record)) out.append(*value);
        }
        return out;
    }
    QList<ClipboardOffer> offers() { return take<ClipboardOffer>([](const Record &r) { return clipboardOffer(r); }); }
    QList<ClipboardData> data() { return take<ClipboardData>([](const Record &r) { return clipboardData(r); }); }
    QList<ClipboardSetResult> results() { return take<ClipboardSetResult>([](const Record &r) { return clipboardSetResult(r); }); }
};

QStringList g_messages;
void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_messages.append(message);
}
}

class HostClipboardTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hostCopyIsOfferedOnceWithoutContents()
    {
        Harness h;
        h.allow(true, true);
        h.host.text = Canary;
        for (int i = 0; i < 5; ++i) h.board->hostChanged(); // a burst
        QTRY_COMPARE(h.offers().size(), 1);
        QCOMPARE(h.offers().first().seq, quint64(1));
        QCOMPARE(h.host.reads, 0); // announcing a copy reads nothing
        for (const auto &record : std::as_const(h.sent)) QVERIFY(!record.payload.contains(Canary.toUtf8()));
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 2);
        QCOMPARE(h.offers().last().seq, quint64(2));
    }

    void noOfferWhenPolicyDeniesOrNoText()
    {
        Harness h;
        h.host.text = QStringLiteral("x");
        h.board->hostChanged(); // no policy yet
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        h.allow(false, true);
        h.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty()); // client to host only
        h.allow(true, true, false, false, 2);
        h.host.hasText = false; // an image, say
        h.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        // A policy that turns host to client off cancels an offer that was about to go out.
        h.host.hasText = true;
        h.board->hostChanged();
        h.allow(false, true, false, false, 3);
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
    }

    void greeterNeverOffers()
    {
        Harness greeter(false);
        greeter.allow(true, true, true); // even a policy that says so
        greeter.host.text = QStringLiteral("a previous visitor's paste");
        greeter.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(greeter.offers().isEmpty());
        greeter.board->request({1, 1000});
        QCOMPARE(greeter.data().size(), 1);
        QCOMPARE(greeter.data().first().status, ClipboardStatus::Denied);
        QVERIFY(greeter.data().first().text.isEmpty());
        QCOMPARE(greeter.host.reads, 0);
    }

    void requestReadsTheTextNowAndNormalisesIt()
    {
        Harness h;
        h.allow(true, true);
        h.host.text = QStringLiteral("one\r\ntwo") + QChar(0) + QStringLiteral("three");
        h.board->request({1, 1000});
        QCOMPARE(h.data().size(), 1);
        QCOMPARE(h.data().first().status, ClipboardStatus::Ok);
        QCOMPARE(h.data().first().text, QStringLiteral("one\ntwothree"));
        h.host.text = QStringLiteral("changed since");
        h.board->request({2, 1000});
        QCOMPARE(h.data().last().text, QStringLiteral("changed since")); // the clipboard as it is at paste time
    }

    void requestStatuses()
    {
        Harness h;
        h.allow(true, true);
        h.host.hasText = false;
        h.board->request({1, 1000});
        QCOMPARE(h.data().last().status, ClipboardStatus::Gone);
        h.host.hasText = true;
        h.host.text = QString();
        h.board->request({2, 1000});
        QCOMPARE(h.data().last().status, ClipboardStatus::Empty);
        h.host.text = QString(11, QLatin1Char('a'));
        h.board->request({3, 10});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        QVERIFY(h.data().last().text.isEmpty());
        h.host.text = QString(MaxClipboardBytes + 1, QLatin1Char('a'));
        h.board->request({4, MaxClipboardBytes});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        h.host.text = QStringLiteral("é"); // two bytes in UTF-8
        h.board->request({5, 1});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        h.board->request({6, 2});
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        // Not newer than the last answered id.
        h.board->request({6, 2});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.board->request({3, 2});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        QCOMPARE(h.data().size(), 8);
    }

    void requestDeniedWithoutPolicyControlOrDirection()
    {
        Harness h;
        h.host.text = QStringLiteral("secret");
        h.board->request({1, 100}); // nothing granted yet
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.allow(false, true);
        h.board->request({2, 100});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.allow(true, true, false, false, 2);
        h.board->setControl({5, false}); // control withdrawn
        h.board->request({3, 100});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.board->setControl({6, true}); // a new generation the policy was not made for
        h.board->request({4, 100});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        QCOMPARE(h.host.reads, 0);
        for (const auto &answer : h.data()) QVERIFY(answer.text.isEmpty());
    }

    void staleAndRepeatedPoliciesAreIgnored()
    {
        Harness h;
        h.allow(true, true, false, false, 5);
        h.board->setPolicy({4, 5, false, false, false, false}); // older revision
        h.board->setPolicy({5, 5, false, false, false, false}); // same revision
        h.host.text = QStringLiteral("x");
        h.board->request({1, 100});
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        h.board->setPolicy({6, 5, false, false, false, false});
        h.board->request({2, 100});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
    }

    void setWritesWhenAllowedAndNeverEchoes()
    {
        Harness h;
        h.allow(true, true);
        h.board->set({1, 5, QStringLiteral("from client")});
        QCOMPARE(h.host.writes.size(), 1);
        QCOMPARE(h.host.writes.first().first, QStringLiteral("from client"));
        QVERIFY(!h.host.writes.first().second); // an unlocked write is ordinary
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
        // The change KSystemClipboard reports for that very write (asynchronous, later) is not offered back.
        h.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        // A real copy that follows is.
        h.host.text = QStringLiteral("typed on the host");
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
    }

    void setGates()
    {
        Harness h;
        h.host.text = QStringLiteral("original");
        h.board->set({1, 5, QStringLiteral("x")}); // no policy, no control
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.allow(true, false);
        h.board->set({2, 5, QStringLiteral("x")}); // client to host off
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.allow(true, true, false, false, 2);
        h.board->set({3, 4, QStringLiteral("x")}); // another control generation
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.board->set({4, 5, QString(MaxClipboardBytes + 1, QLatin1Char('a'))});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.board->set({5, 5, QStringLiteral("ok")});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
        h.board->set({5, 5, QStringLiteral("replayed")}); // the same id again
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        QCOMPARE(h.host.writes.size(), 1);
        QCOMPARE(h.host.text, QStringLiteral("ok"));
        h.host.writeOk = false;
        h.board->set({6, 5, QStringLiteral("fails")});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Failed);
        // An empty set clears.
        h.host.writeOk = true;
        h.board->set({7, 5, QString()});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
        QVERIFY(h.host.text.isEmpty());
    }

    void greeterWritesOnlyWithItsOwnPolicyAndAreSecret()
    {
        Harness greeter(false);
        greeter.allow(false, true, true, false);
        greeter.board->set({1, 5, QStringLiteral("a username")});
        QCOMPARE(greeter.results().last().status, ClipboardSetStatus::Written);
        QVERIFY(greeter.host.writes.last().second); // password-manager hint on the login screen
        // A desktop policy (greeter=false) is not enough for a greeter worker.
        Harness mismatch(false);
        mismatch.allow(true, true, false);
        mismatch.board->set({1, 5, QStringLiteral("x")});
        QCOMPARE(mismatch.results().last().status, ClipboardSetStatus::Denied);
        // And a greeter policy does not let a desktop worker write.
        Harness desktop(true);
        desktop.allow(false, true, true);
        desktop.board->set({1, 5, QStringLiteral("x")});
        QCOMPARE(desktop.results().last().status, ClipboardSetStatus::Denied);
        // A locked session's write is secret too.
        Harness locked(true);
        locked.allow(true, true, false, true);
        locked.board->set({1, 5, QStringLiteral("a password")});
        QVERIFY(locked.host.writes.last().second);
    }

    void contentsNeverReachALog()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Harness h;
            h.allow(true, true);
            h.host.text = Canary;
            h.board->hostChanged();
            QTRY_COMPARE(h.offers().size(), 1);
            h.board->request({1, 1000});
            QCOMPARE(h.data().last().text, Canary);
            h.board->set({1, 5, Canary + QStringLiteral(" from the client")});
            h.board->request({2, 5});
            h.board->set({2, 4, Canary});
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        for (const auto &message : std::as_const(g_messages)) {
            QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
        }
    }
};

QTEST_GUILESS_MAIN(HostClipboardTest)

#include "HostClipboardTest.moc"
