// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-067: the worker's side of the clipboard bridge against a fake host clipboard (no compositor, no daemon).

#include <QBuffer>
#include <QImage>
#include <QLoggingCategory>
#include <QTest>

#include <optional>

#include "ClipboardLimits.h"
#include "HostClipboard.h"

using namespace KRdp;
using namespace KRdp::ConsoleWorkerWire;
using namespace Qt::StringLiterals;

namespace
{
const QString Canary = QStringLiteral("CANARY-7f3a91-do-not-log");

struct FakeHost {
    QString text;
    bool hasText = true;
    bool writeOk = true;
    int reads = 0;
    QList<QPair<QString, bool>> writes; // (text, secret)
    QByteArray image; // OPT-072: what the host holds as an image, in whatever format
    int imageReads = 0;
    std::optional<HostClipboard::ImageRead> bounded; // OPT-072: what the capped read returns; unset = the image as it is
    int boundedReads = 0;
    QList<QPair<QByteArray, bool>> imageWrites; // (png, secret)
    // OPT-077
    QByteArray html; // what the host holds as text/html, in whatever encoding
    std::optional<HostClipboard::ImageRead> htmlRead; // what the capped read returns; unset = the html as it is
    int htmlReads = 0;
    struct RichWrite {
        QString text;
        QByteArray html;
        bool secret;
    };
    QList<RichWrite> richWrites;
    HostClipboard::Backend backend()
    {
        return {[this] { return hasText; },
                [this] { ++reads; return text; },
                [this](const QString &t, bool secret) {
                    writes.append({t, secret});
                    if (writeOk) text = t;
                    return writeOk;
                },
                [this] { return !image.isEmpty(); },
                [this] { ++imageReads; return image; },
                [this](const QByteArray &png, bool secret) {
                    imageWrites.append({png, secret});
                    if (writeOk) image = png;
                    return writeOk;
                },
                [this] {
                    ++boundedReads;
                    return bounded ? *bounded : HostClipboard::ImageRead{image, false, qint64(image.size())};
                },
                [this] { return !html.isEmpty(); },
                [this] {
                    ++htmlReads;
                    return htmlRead ? *htmlRead : HostClipboard::ImageRead{html, false, qint64(html.size())};
                },
                [this](const QString &t, const QByteArray &h, bool secret) {
                    richWrites.append({t, h, secret});
                    if (writeOk) {
                        text = t;
                        html = h;
                    }
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

QByteArray encode(const QImage &image, const char *format)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return out;
}
QImage picture()
{
    QImage image(5, 3, QImage::Format_RGB32);
    image.fill(qRgb(10, 120, 200));
    return image;
}
QByteArray bombPng(quint32 w, quint32 h)
{
    QByteArray data = encode(picture(), "PNG");
    for (int i = 0; i < 4; ++i) {
        data[16 + i] = char((w >> (8 * (3 - i))) & 0xff);
        data[20 + i] = char((h >> (8 * (3 - i))) & 0xff);
    }
    return data;
}

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

    // ---- OPT-072: images ----
    void imageCopyIsOfferedWithItsFlagAndNoPixels()
    {
        Harness h;
        h.allow(true, true);
        h.host.hasText = false;
        h.host.image = encode(picture(), "PNG");
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
        QCOMPARE(h.offers().last().flags, ClipboardOfferImage);
        QCOMPARE(h.host.imageReads, 0); // announcing reads nothing
        h.host.hasText = true; // a copy with both
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 2);
        QCOMPARE(h.offers().last().flags, quint8(ClipboardOfferText | ClipboardOfferImage));
        for (const auto &record : std::as_const(h.sent)) QVERIFY(!record.payload.contains(h.host.image));
    }

    void noImageOfferWhenDenied()
    {
        Harness h;
        h.host.hasText = false;
        h.host.image = encode(picture(), "PNG");
        h.allow(false, true);
        h.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        Harness greeter(false);
        greeter.host.hasText = false;
        greeter.host.image = h.host.image;
        greeter.allow(false, true, true);
        greeter.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(greeter.offers().isEmpty());
    }

    void imageRequestPassesAPngThroughAndConvertsOtherFormats()
    {
        Harness h;
        h.allow(true, true);
        h.host.image = encode(picture(), "PNG");
        h.board->request({1, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        QCOMPARE(h.data().last().kind, ClipboardKind::Image);
        QCOMPARE(h.data().last().image, h.host.image);
        h.host.image = encode(picture(), "BMP"); // a host application that only offers BMP
        h.board->request({2, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        QVERIFY(ClipboardImage::validatePng(h.data().last().image).ok());
        QImage back;
        QVERIFY(back.loadFromData(h.data().last().image, "PNG"));
        QCOMPARE(back.size(), QSize(5, 3));
    }

    void imageRequestStatuses()
    {
        Harness h;
        h.allow(true, true);
        h.board->request({1, 1000, ClipboardKind::Image}); // nothing there
        QCOMPARE(h.data().last().status, ClipboardStatus::Gone);
        QCOMPARE(h.data().last().kind, ClipboardKind::Image);
        h.host.image = encode(picture(), "PNG");
        h.board->request({2, 20, ClipboardKind::Image}); // the requester's own limit
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        QVERIFY(h.data().last().image.isEmpty());
        h.host.image = QByteArray("not a picture at all");
        h.board->request({3, 1000, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::Gone);
        // A decompression bomb: 70 bytes declaring 60000 x 60000.
        h.host.image = bombPng(60000, 60000);
        h.board->request({4, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        // Over the encoded cap.
        h.host.image = encode(picture(), "PNG") + QByteArray(MaxClipboardImageBytes, 'x');
        h.board->request({5, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
    }

    void imageRequestsAreDeniedLikeText()
    {
        Harness h;
        h.host.image = encode(picture(), "PNG");
        h.board->request({1, 1000, ClipboardKind::Image}); // no policy, no control
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.allow(false, true);
        h.board->request({2, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        Harness greeter(false);
        greeter.host.image = h.host.image;
        greeter.allow(false, true, true);
        greeter.board->request({1, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(greeter.data().last().status, ClipboardStatus::Denied);
        QCOMPARE(greeter.host.imageReads, 0);
    }

    void imageSetWritesAValidatedPngAndNeverEchoes()
    {
        Harness h;
        h.allow(true, true);
        h.host.hasText = false; // the write replaced whatever text there was
        const auto png = encode(picture(), "PNG");
        h.board->set({1, 5, {}, ClipboardKind::Image, png});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
        QCOMPARE(h.host.imageWrites.size(), 1);
        QCOMPARE(h.host.imageWrites.first().first, png);
        QVERIFY(!h.host.imageWrites.first().second);
        h.board->hostChanged(); // the compositor reporting our own write
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        h.host.image = encode(QImage(2, 2, QImage::Format_RGB32), "PNG"); // a real copy follows
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
        QCOMPARE(h.offers().last().flags, ClipboardOfferImage);
    }

    void imageSetRefusesWhatIsNotAnAcceptablePng()
    {
        Harness h;
        h.allow(true, true);
        h.board->set({1, 5, {}, ClipboardKind::Image, QByteArray("GIF89a not a png")});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Failed);
        h.board->set({2, 5, {}, ClipboardKind::Image, bombPng(60000, 60000)});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Failed);
        h.board->set({3, 5, {}, ClipboardKind::Image, {}}); // empty: not a legal image set
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.board->set({4, 5, {}, ClipboardKind::Image, QByteArray(MaxClipboardImageBytes + 1, 'p')});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        QVERIFY(h.host.imageWrites.isEmpty());
    }

    void imageSetGatesAndSecretHint()
    {
        const auto png = encode(picture(), "PNG");
        Harness h;
        h.board->set({1, 5, {}, ClipboardKind::Image, png}); // no policy, no control
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.allow(true, false); // client to host off
        h.board->set({2, 5, {}, ClipboardKind::Image, png});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.allow(true, true, false, false, 2);
        h.board->set({3, 4, {}, ClipboardKind::Image, png}); // another generation
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.board->set({4, 5, {}, ClipboardKind::Image, png});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
        h.board->set({4, 5, {}, ClipboardKind::Image, png}); // replayed id
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.host.writeOk = false;
        h.board->set({5, 5, {}, ClipboardKind::Image, png});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Failed);
        // The login screen and a locked session write images as secrets, like text.
        Harness greeter(false);
        greeter.allow(false, true, true);
        greeter.board->set({1, 5, {}, ClipboardKind::Image, png});
        QCOMPARE(greeter.results().last().status, ClipboardSetStatus::Written);
        QVERIFY(greeter.host.imageWrites.last().second);
        Harness locked(true);
        locked.allow(true, true, false, true);
        locked.board->set({1, 5, {}, ClipboardKind::Image, png});
        QVERIFY(locked.host.imageWrites.last().second);
        // A desktop worker with a greeter policy (and the reverse) refuses.
        Harness mismatch(true);
        mismatch.allow(false, true, true);
        mismatch.board->set({1, 5, {}, ClipboardKind::Image, png});
        QCOMPARE(mismatch.results().last().status, ClipboardSetStatus::Denied);
    }

    void aBackendWithoutImagesOffersNoneAndWritesNone()
    {
        Harness h;
        auto backend = h.host.backend();
        backend.hasImage = nullptr;
        backend.readImage = nullptr;
        backend.writeImage = nullptr;
        QList<Record> sent;
        Deframer deframer;
        HostClipboard board(backend, [&](const QByteArray &record) {
            deframer.feed(record);
            while (const auto next = deframer.next()) sent.append(*next);
        }, true);
        board.setControl({5, true});
        board.setPolicy({1, 5, true, true, false, false});
        board.request({1, 1000, ClipboardKind::Image});
        QCOMPARE(clipboardData(sent.last())->status, ClipboardStatus::Gone);
        board.set({1, 5, {}, ClipboardKind::Image, encode(picture(), "PNG")});
        QCOMPARE(clipboardSetResult(sent.last())->status, ClipboardSetStatus::Failed);
    }

    void aCappedReadThatWasAbandonedAnswersTooLargeAndLogsOversize()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        {
            Harness h;
            h.allow(true, true);
            h.host.image = encode(picture(), "PNG"); // offered as an image; the capped read says it was over the cap
            h.host.bounded = HostClipboard::ImageRead{{}, true, ClipboardLimits::MaxImageBytes + 4096};
            h.board->request({1, MaxClipboardImageBytes, ClipboardKind::Image});
            QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
            QVERIFY(h.data().last().image.isEmpty());
            QCOMPARE(h.host.imageReads, 0); // the uncapped read is not used when the backend has a capped one
            QCOMPARE(h.host.boundedReads, 1);
        }
        qInstallMessageHandler(previous);
        bool logged = false;
        for (const auto &message : std::as_const(g_messages)) {
            if (!message.contains(QStringLiteral("refused (oversize)"))) continue;
            logged = true;
            QVERIFY2(message.contains(QStringLiteral("direction=host-to-client kind=image bytes=%1").arg(ClipboardLimits::MaxImageBytes + 4096)), qPrintable(message));
            QVERIFY2(!message.contains(QStringLiteral("size=")), "no size is known for an abandoned read");
        }
        QVERIFY(logged);
    }

    void aCappedReadReturningAPictureAnswersLikeTheUncappedOne()
    {
        Harness h;
        h.allow(true, true);
        h.host.image = encode(picture(), "PNG");
        h.board->request({1, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        QCOMPARE(h.data().last().image, h.host.image);
        QCOMPARE(h.host.boundedReads, 1);
    }

    void aBackendWithOnlyTheUncappedReadStillWorks()
    {
        QList<Record> sent;
        Deframer deframer;
        QByteArray image = encode(picture(), "PNG");
        HostClipboard::Backend backend;
        backend.hasText = [] { return false; };
        backend.readText = [] { return QString(); };
        backend.writeText = [](const QString &, bool) { return true; };
        backend.hasImage = [] { return true; };
        backend.readImage = [&] { return image; };
        HostClipboard board(backend, [&](const QByteArray &record) {
            deframer.feed(record);
            while (const auto next = deframer.next()) sent.append(*next);
        }, true);
        board.setControl({5, true});
        board.setPolicy({1, 5, true, true, false, false});
        board.request({1, MaxClipboardImageBytes, ClipboardKind::Image});
        QCOMPARE(clipboardData(sent.last())->status, ClipboardStatus::Ok);
        QCOMPARE(clipboardData(sent.last())->image, image);
    }

    void ourOwnPictureComingBackReEncodedIsStillAnEcho()
    {
        // KSystemClipboard answers other processes with its own PNG encoding of a picture, so the host copy that follows a
        // write of ours can be the same pixels in different bytes. That must not be offered to the client as a new copy.
        Harness h;
        h.allow(true, true);
        h.host.hasText = false;
        QImage image(16, 16, QImage::Format_ARGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) image.setPixelColor(x, y, QColor(x * 16, y * 16, 80, 255));
        const QByteArray written = encode(image, "PNG");
        h.board->set({1, 5, {}, ClipboardKind::Image, written});
        QByteArray reEncoded;
        {
            QBuffer buffer(&reEncoded);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG", 0); // a different compression level: other bytes, same pixels
        }
        QVERIFY(reEncoded != written);
        h.host.image = reEncoded;
        h.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        image.setPixelColor(0, 0, Qt::white); // one pixel different: a real copy
        h.host.image = encode(image, "PNG");
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
    }

    void imageRefusalsLogASizeNeverContents()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Harness h;
            h.allow(true, true);
            h.host.image = bombPng(60000, 60000) + Canary.toUtf8();
            h.board->request({1, MaxClipboardImageBytes, ClipboardKind::Image});
            h.board->set({1, 5, {}, ClipboardKind::Image, bombPng(70000, 70000) + Canary.toUtf8()});
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        bool logged = false;
        for (const auto &message : std::as_const(g_messages)) {
            QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
            logged = logged || message.contains(QStringLiteral("refused (dimensions)"));
        }
        QVERIFY(logged);
    }

    // ---- OPT-077: formatted text ----
    void htmlCopyIsOfferedWithItsFlagAndNoContents()
    {
        Harness h;
        h.allow(true, true);
        h.host.text = QStringLiteral("plain");
        h.host.html = Canary.toUtf8();
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
        QCOMPARE(h.offers().first().flags, quint8(ClipboardOfferText | ClipboardOfferHtml));
        QCOMPARE(h.host.htmlReads, 0); // announcing reads nothing
        for (const auto &record : std::as_const(h.sent)) QVERIFY(!record.payload.contains(Canary.toUtf8()));
        // HTML with no text is offered alone.
        h.host.hasText = false;
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 2);
        QCOMPARE(h.offers().last().flags, ClipboardOfferHtml);
    }

    void theOfferFollowsTheCombinationRule()
    {
        // Chrome "Copy image": html (an <img>) and a picture, no text: only the picture is offered.
        Harness h;
        h.allow(true, true);
        h.host.hasText = false;
        h.host.html = "<img src=x>";
        h.host.image = encode(picture(), "PNG");
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
        QCOMPARE(h.offers().first().flags, ClipboardOfferImage);
        // A spreadsheet range: text, html and a picture: text and html, no picture.
        h.host.hasText = true;
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 2);
        QCOMPARE(h.offers().last().flags, quint8(ClipboardOfferText | ClipboardOfferHtml));
        // Text and a picture, no html: both, as before.
        h.host.html.clear();
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 3);
        QCOMPARE(h.offers().last().flags, quint8(ClipboardOfferText | ClipboardOfferImage));
    }

    void htmlRequestReadsAndNormalisesToUtf8()
    {
        Harness h;
        h.allow(true, true);
        h.host.html = "<p>caf\xC3\xA9</p>";
        h.board->request({1, 1000000, ClipboardKind::Html});
        QCOMPARE(h.data().last().kind, ClipboardKind::Html);
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        QCOMPARE(h.data().last().html, QByteArray("<p>caf\xC3\xA9</p>"));
        // UTF-16 with a byte order mark (some sources) becomes UTF-8.
        const QString text = QStringLiteral("<p>café €</p>");
        QByteArray utf16("\xFF\xFE", 2);
        utf16.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
        h.host.html = utf16;
        h.board->request({2, 1000000, ClipboardKind::Html});
        QCOMPARE(h.data().last().html, text.toUtf8());
        // Invalid bytes never leave as invalid UTF-8.
        h.host.html = "<p>\xFF</p>";
        h.board->request({3, 1000000, ClipboardKind::Html});
        QVERIFY(QByteArrayView(h.data().last().html).isValidUtf8());
    }

    void htmlRequestStatuses()
    {
        Harness h;
        h.allow(true, true);
        h.board->request({1, 1000000, ClipboardKind::Html}); // nothing there
        QCOMPARE(h.data().last().status, ClipboardStatus::Gone);
        h.host.html = "<p>x</p>";
        h.board->request({2, 3, ClipboardKind::Html}); // the client's limit is smaller than the document
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        QVERIFY(h.data().last().html.isEmpty());
        h.host.htmlRead = HostClipboard::ImageRead{{}, true, ClipboardLimits::MaxHtmlBytes * 2 + 1}; // the capped read was abandoned
        h.board->request({3, 1000000, ClipboardKind::Html});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        h.host.htmlRead = HostClipboard::ImageRead{QByteArray(ClipboardLimits::MaxHtmlBytes + 1, 'a'), false, ClipboardLimits::MaxHtmlBytes + 1};
        h.board->request({4, ClipboardLimits::MaxHtmlBytes, ClipboardKind::Html});
        QCOMPARE(h.data().last().status, ClipboardStatus::TooLarge);
        h.host.htmlRead = HostClipboard::ImageRead{QByteArray("\0\0 ", 3), false, 3}; // nothing but NULs: gone
        h.board->request({5, 1000000, ClipboardKind::Html});
        QCOMPARE(h.data().last().status, ClipboardStatus::Gone);
        for (const auto &answer : h.data()) QCOMPARE(answer.kind, ClipboardKind::Html);
    }

    void htmlRequestsAreDeniedLikeText()
    {
        Harness h;
        h.host.html = "<p>x</p>";
        h.board->request({1, 1000000, ClipboardKind::Html}); // no policy, no control
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.allow(false, true);
        h.board->request({2, 1000000, ClipboardKind::Html}); // host to client off
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        h.allow(true, true, false, false, 2);
        h.board->request({3, 1000000, ClipboardKind::Html});
        QCOMPARE(h.data().last().status, ClipboardStatus::Ok);
        h.board->request({3, 1000000, ClipboardKind::Html}); // the id must increase
        QCOMPARE(h.data().last().status, ClipboardStatus::Denied);
        Harness greeter(false);
        greeter.host.html = "<p>x</p>";
        greeter.allow(false, true, true);
        greeter.board->request({1, 1000000, ClipboardKind::Html});
        QCOMPARE(greeter.data().last().status, ClipboardStatus::Denied);
        QCOMPARE(greeter.host.htmlReads, 0);
    }

    void htmlSetWritesTextAndHtmlTogetherAndNeverEchoes()
    {
        Harness h;
        h.allow(true, true);
        h.board->set({1, 5, QStringLiteral("rich text"), ClipboardKind::Html, {}, "<b>rich</b> text"});
        QCOMPARE(h.host.richWrites.size(), 1);
        QCOMPARE(h.host.richWrites.first().text, QStringLiteral("rich text"));
        QCOMPARE(h.host.richWrites.first().html, QByteArray("<b>rich</b> text"));
        QVERIFY(!h.host.richWrites.first().secret);
        QVERIFY(h.host.writes.isEmpty()); // one offer, not a text write and a second write
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
        // The change that write causes (asynchronous) is not offered back, text or html.
        h.board->hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        QVERIFY(h.offers().isEmpty());
        // A new HTML copy on the host is.
        h.host.html = "<i>other</i>";
        h.board->hostChanged();
        QTRY_COMPARE(h.offers().size(), 1);
        QCOMPARE(h.offers().first().flags, quint8(ClipboardOfferText | ClipboardOfferHtml));
    }

    void htmlSetWithoutTextWritesHtmlAlone()
    {
        Harness h;
        h.allow(true, true);
        h.board->set({1, 5, {}, ClipboardKind::Html, {}, "<b>x</b>"});
        QCOMPARE(h.host.richWrites.size(), 1);
        QVERIFY(h.host.richWrites.first().text.isEmpty());
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Written);
    }

    void htmlSetGatesSecretHintAndFailures()
    {
        Harness h;
        h.allow(true, false); // client to host off
        h.board->set({1, 5, {}, ClipboardKind::Html, {}, "<b>x</b>"});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        QVERIFY(h.host.richWrites.isEmpty());
        h.allow(true, true, false, true, 2); // locked session: written with the password manager hint
        h.board->set({2, 5, {}, ClipboardKind::Html, {}, "<b>x</b>"});
        QCOMPARE(h.host.richWrites.size(), 1);
        QVERIFY(h.host.richWrites.first().secret);
        h.board->set({2, 5, {}, ClipboardKind::Html, {}, "<b>y</b>"}); // id must increase
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied);
        h.host.writeOk = false;
        h.board->set({3, 5, {}, ClipboardKind::Html, {}, "<b>z</b>"});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Failed);
        h.host.writeOk = true;
        h.board->set({4, 5, {}, ClipboardKind::Html, {}, QByteArray(ClipboardLimits::MaxHtmlBytes + 1, 'a')});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Denied); // over the cap: the gate refuses it
        h.board->set({5, 5, {}, ClipboardKind::Html, {}, "\0\0"_ba});
        QCOMPARE(h.results().last().status, ClipboardSetStatus::Failed); // nothing left after NULs
        Harness greeter(false);
        greeter.allow(false, true, true);
        greeter.board->set({1, 5, {}, ClipboardKind::Html, {}, "<b>x</b>"});
        QCOMPARE(greeter.host.richWrites.size(), 1);
        QVERIFY(greeter.host.richWrites.first().secret);
    }

    void aBackendWithoutHtmlOffersNoneAndWritesNone()
    {
        HostClipboard::Backend backend;
        backend.hasText = [] { return true; };
        backend.readText = [] { return QStringLiteral("t"); };
        backend.writeText = [](const QString &, bool) { return true; };
        QList<Record> sent;
        Deframer deframer;
        HostClipboard board(backend, [&](const QByteArray &record) {
            deframer.feed(record);
            while (const auto next = deframer.next()) sent.append(*next);
        }, true);
        board.setControl({5, true});
        board.setPolicy({1, 5, true, true, false, false});
        board.request({1, 1000, ClipboardKind::Html});
        QCOMPARE(clipboardData(sent.last())->status, ClipboardStatus::Gone);
        board.set({1, 5, {}, ClipboardKind::Html, {}, "<b>x</b>"});
        QCOMPARE(clipboardSetResult(sent.last())->status, ClipboardSetStatus::Failed);
        board.hostChanged();
        QTest::qWait(HostClipboard::SettleMs * 2);
        for (const auto &record : std::as_const(sent)) {
            if (const auto offer = clipboardOffer(record)) QCOMPARE(offer->flags, ClipboardOfferText);
        }
    }

    void htmlRefusalsLogASizeNeverContents()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Harness h;
            h.allow(true, true);
            h.host.html = (u"<p>"_s + Canary + u"</p>"_s).toUtf8();
            h.board->request({1, 5, ClipboardKind::Html}); // over the client's limit
            h.host.htmlRead = HostClipboard::ImageRead{{}, true, ClipboardLimits::MaxHtmlBytes * 3};
            h.board->request({2, 1000000, ClipboardKind::Html});
            h.host.htmlRead.reset();
            h.board->set({1, 5, Canary, ClipboardKind::Html, {}, (u"<p>"_s + Canary + u"</p>"_s).toUtf8()});
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        bool sawRefusal = false;
        for (const auto &message : std::as_const(g_messages)) {
            QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
            sawRefusal |= message.contains(QStringLiteral("kind=html"));
        }
        QVERIFY(sawRefusal);
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
