// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QLocale>
#include <QMetaObject>
#include <QObject>
#include <QThreadPool>

#include "ClipboardFilePush.h"
#include "ConsoleWorkerWire.h"
#include "HostFileStaging.h"

namespace KRdp
{
/**
 * OPT-078 phase 3: the worker's file service for client-to-host copy. The broker (which holds the RDP connection and asks the client for
 * ranges) hands over the client's FileGroupDescriptorW (FilePushOffer) and then each range in order (FilePushData); this class plans it,
 * stages it into the private per-session tree (HostFileStaging) and puts the finished files on the host clipboard as text/uri-list.
 *
 *  - Policy (spec 4.5, amendment 12.1): a NEW transfer needs mayStageFiles() (active controller, files client-to-host flag, desktop worker).
 *    A transfer that started keeps going when the flag drops because the screen locked (the broker keeps the flag for it); the flag
 *    dropping for any other reason ends it. Control moving to another connection removes everything, offered files included.
 *  - All disk work (creating the tree, every write, fsync, rename, removal) runs on ONE background thread, so the worker's event loop
 *    (capture, input, the broker socket) never waits for a disk; results are delivered back to the thread that built this object, where
 *    the records are sent and the host clipboard is touched. Every range is acknowledged (the broker's window moves on the ack).
 *  - Offer rule (spec 3.2): a selection of at most offerWhenCompleteBytes (256 MiB) is offered when everything is there; a larger one after
 *    its first finished top-level unit (Offered), and the list is refreshed when it is complete (Done). A transfer ends in Done or Failed.
 *  - The user is told once when staging starts and once on a refusal or failure (org.freedesktop.Notifications, through Backend::notify),
 *    with counts, sizes and a reason; never a file name. Logs carry counts, bytes, ids and reasons only.
 *  - A newer offer replaces older transfers (their staging is removed); cancel removes one or all; on a clean exit everything including the
 *    instance directory goes; after a crash the next worker's sweep removes the leftovers (the instance's flock is gone with the process).
 */
class HostFileStager
{
public:
    using Sender = std::function<void(const QByteArray &record)>;

    struct Backend {
        /// Puts \a paths (absolute, the staged top-level units) on the host clipboard as text/uri-list; \a secret: written while locked (keep it out of Klipper's history). False = failed.
        std::function<bool(const QStringList &paths, bool secret)> writeUriList;
        /// Withdraws what writeUriList() offered, if it is still the host's selection.
        std::function<void()> clearUriList;
        /// A notification to the user in this session.
        std::function<void(const QString &summary, const QString &body)> notify;
    };
    struct Tuning {
        quint64 offerWhenCompleteBytes = ClipboardFileLimits::OfferWhenCompleteBytes;
        std::function<qint64()> nowSeconds = [] { return qint64(::time(nullptr)); };
    };

    HostFileStager(Sender send, Backend backend, bool desktop, HostFileStaging::Config config)
        : HostFileStager(std::move(send), std::move(backend), desktop, std::move(config), Tuning())
    {
    }

    HostFileStager(Sender send, Backend backend, bool desktop, HostFileStaging::Config config, Tuning tuning)
        : m_send(std::move(send))
        , m_backend(std::move(backend))
        , m_desktop(desktop)
        , m_config(std::move(config))
        , m_tuning(std::move(tuning))
        , m_state(std::make_shared<State>())
    {
        m_pool.setMaxThreadCount(1);
        m_pool.setExpiryTimeout(2000);
    }

    ~HostFileStager()
    {
        m_state->alive = false; // queued jobs are skipped; the cleanup below always runs
        m_pool.start([this] {
            if (m_area) {
                m_area->removeInstance();
                m_area.reset();
            }
        });
        m_pool.waitForDone();
    }

    void setControl(const ConsoleWorkerWire::ControlState &control)
    {
        m_control = control;
        std::vector<quint64> gone;
        for (const auto &[seq, entry] : m_entries) {
            if (!control.active || control.generation != entry.generation) gone.push_back(seq);
        }
        for (const quint64 seq : gone) drop(seq);
    }

    void setPolicy(const ConsoleWorkerWire::ClipboardPolicy &policy)
    {
        if (policy.revision < m_policy.revision) return;
        m_policy = policy;
        if (allowed()) return;
        // The flag is gone. What is still being received stops; what was received stays (a lock is not a reason to take it away).
        std::vector<quint64> running;
        for (const auto &[seq, entry] : m_entries) {
            if (entry.state != EntryState::Done) running.push_back(seq);
        }
        for (const quint64 seq : running) fail(seq, ConsoleWorkerWire::FilePushReason::Policy, {}); // (a no-op for one that went meanwhile)
    }

    /// A worker that starts cleans up after crashed ones (stale instances of this user), creating nothing.
    void sweepAtStart()
    {
        if (!m_desktop) return;
        post([this] {
            const auto report = HostFileStaging::Area::sweepStale(m_config, m_tuning.nowSeconds());
            if (report.removedTrees || report.unlinked || report.failed) {
                qInfo().nospace() << "Clipboard: swept stale file staging removed=" << report.removedTrees << " unlinked=" << report.unlinked << " failed=" << report.failed;
            }
        });
    }

    void offer(const ConsoleWorkerWire::FilePushOffer &offer)
    {
        using Reason = ConsoleWorkerWire::FilePushReason;
        if (!allowed()) {
            refuse(offer.seq, Reason::Policy, {});
            return;
        }
        auto plan = std::make_shared<const ClipboardFilePush::Plan>(ClipboardFilePush::makePlan(offer.descriptor, offer.hugeFiles));
        if (!plan->ok()) {
            Reason reason = Reason::Descriptor;
            if (plan->refusal == ClipboardFilePush::Refusal::Names) reason = Reason::Names;
            else if (plan->refusal == ClipboardFilePush::Refusal::NothingToCopy) reason = Reason::NothingToCopy;
            ConsoleWorkerWire::FilePushReply reply;
            reply.skipped = quint32(plan->skippedTotal());
            refuse(offer.seq, reason, reply);
            return;
        }
        // A newer copy replaces every older transfer, offered or not.
        std::vector<quint64> older;
        for (const auto &[seq, entry] : m_entries) older.push_back(seq);
        for (const quint64 seq : older) drop(seq);

        Entry entry;
        entry.plan = plan;
        entry.generation = m_control.generation;
        entry.timing = totalOfferTiming(*plan);
        const quint64 seq = offer.seq;
        m_entries.emplace(seq, std::move(entry));
        const std::optional<quint64> reserve = offer.reserveBytes ? std::optional<quint64>(offer.reserveBytes) : std::nullopt;
        post([this, seq, plan, reserve]() {
            CreateResult result;
            if (!ensureArea()) {
                result.status = HostFileStaging::Status::RootUnsafe;
                result.rootFailed = true;
            } else {
                result.fs = std::shared_ptr<HostFileStaging::Transfer>(m_area->begin(seq, *plan, reserve));
                result.status = result.fs->create(&result.failure);
                result.complete = result.status == HostFileStaging::Status::Ok && result.fs->complete();
                if (result.complete) result.finished = result.fs->finishedTopLevelPaths();
            }
            deliver([this, seq, result = std::move(result)]() mutable { created(seq, std::move(result)); });
        });
    }

    void data(const ConsoleWorkerWire::FilePushData &chunk)
    {
        using HostFileStaging::Status;
        if (!allowed()) {
            ack(chunk, ClipboardFileFetch::AckStatus::Denied);
            // (the ack may have ended the transfer: look it up after it)
            if (const Entry *entry = entryFor(chunk.seq); entry && entry->state != EntryState::Done) fail(chunk.seq, ConsoleWorkerWire::FilePushReason::Policy, {});
            return;
        }
        const auto it = m_entries.find(chunk.seq);
        if (it == m_entries.end() || it->second.state != EntryState::Staging || !it->second.fs) {
            ack(chunk, ClipboardFileFetch::AckStatus::Gone);
            return;
        }
        const auto fs = it->second.fs;
        const bool wantPaths = it->second.timing == ClipboardFileStaging::OfferTiming::AfterFirstFile;
        const bool alreadyOffered = it->second.offered;
        post([this, fs, chunk, wantPaths, alreadyOffered]() {
            WriteResult result;
            const qsizetype before = fs->completeCount();
            result.status = fs->write(chunk.index, chunk.offset, chunk.data, &result.failure);
            if (result.status == Status::Ok) {
                result.complete = fs->complete();
                result.fileFinished = fs->completeCount() > before;
                if (result.complete || (wantPaths && !alreadyOffered && result.fileFinished)) result.finished = fs->finishedTopLevelPaths();
            }
            deliver([this, chunk, result = std::move(result)]() mutable { written(chunk, std::move(result)); });
        });
    }

    void cancel(const ConsoleWorkerWire::FilePushCancel &cancel)
    {
        if (cancel.seq == 0) {
            std::vector<quint64> all;
            for (const auto &[seq, entry] : m_entries) all.push_back(seq);
            for (const quint64 seq : all) drop(seq);
            post([this] {
                if (m_area) m_area->removeAll();
            });
        } else {
            drop(cancel.seq);
        }
    }

    int transferCount() const { return int(m_entries.size()); }
    /// Test seam: waits until the background thread is idle (results still need the event loop to be delivered).
    bool waitForIdle(int ms) { return m_pool.waitForDone(ms); }

private:
    enum class EntryState { Creating, Staging, Done };
    struct Entry {
        std::shared_ptr<HostFileStaging::Transfer> fs; ///< only ever used on the background thread
        std::shared_ptr<const ClipboardFilePush::Plan> plan;
        quint64 generation = 0;
        EntryState state = EntryState::Creating;
        ClipboardFileStaging::OfferTiming timing = ClipboardFileStaging::OfferTiming::Now;
        quint64 done = 0;
        bool offered = false;
        QElapsedTimer sinceProgress;
    };
    struct CreateResult {
        HostFileStaging::Status status = HostFileStaging::Status::IoError;
        HostFileStaging::SpaceFailure failure;
        std::shared_ptr<HostFileStaging::Transfer> fs;
        bool complete = false;
        bool rootFailed = false; ///< the staging folder itself could not be opened or made (told apart in the notification)
        QStringList finished;
    };
    struct WriteResult {
        HostFileStaging::Status status = HostFileStaging::Status::IoError;
        HostFileStaging::SpaceFailure failure;
        bool complete = false;
        bool fileFinished = false;
        QStringList finished;
    };
    struct State {
        std::atomic<bool> alive = true;
    };
    using Reason = ConsoleWorkerWire::FilePushReason;

    bool allowed() const { return ConsoleWorkerWire::mayStageFiles(m_control, m_policy, m_desktop); }

    ClipboardFileStaging::OfferTiming totalOfferTiming(const ClipboardFilePush::Plan &plan) const
    {
        quint64 total = plan.totalBytes;
        if (total == 0) return ClipboardFileStaging::OfferTiming::Now;
        return total <= m_tuning.offerWhenCompleteBytes ? ClipboardFileStaging::OfferTiming::Now : ClipboardFileStaging::OfferTiming::AfterFirstFile;
    }

    bool ensureArea() // background thread
    {
        if (m_area) return true;
        HostFileStaging::Status status = HostFileStaging::Status::Ok;
        m_area = HostFileStaging::Area::open(m_config, &status);
        if (!m_area) {
            // (the status names the cause: io-error = the folder could not be created or entered, root-unsafe = a link, someone else's, or no cache directory)
            qWarning().nospace() << "Clipboard: the file staging folder is not usable (" << HostFileStaging::statusName(status) << ") cacheDir=" << m_config.cacheDir;
            return false;
        }
        const qint64 now = m_tuning.nowSeconds();
        const auto report = m_area->sweep(now);
        const int old = m_area->removeOlderThan(now);
        if (report.removedTrees || report.unlinked || report.failed || old) {
            qInfo().nospace() << "Clipboard: swept stale file staging removed=" << report.removedTrees << " unlinked=" << report.unlinked << " failed=" << report.failed << " expired=" << old;
        }
        return true;
    }

    // ---- results, on the main thread ----

    void created(quint64 seq, CreateResult result)
    {
        using HostFileStaging::Status;
        const auto it = m_entries.find(seq);
        if (it == m_entries.end()) { // replaced or cancelled while it was being created: undo it
            if (result.fs) {
                auto fs = result.fs;
                post([fs] { fs->abort(); });
            }
            return;
        }
        Entry &entry = it->second;
        if (result.status != Status::Ok) {
            ConsoleWorkerWire::FilePushReply reply;
            Reason reason = Reason::StagingFailed;
            if (result.status == Status::NoSpace) {
                reason = Reason::NoSpace;
                reply.freeBytes = result.failure.freeBytes;
                reply.neededBytes = result.failure.neededBytes;
            } else if (result.status == Status::SpaceUnknown) {
                reason = Reason::SpaceUnknown;
            }
            reply.totalBytes = entry.plan->totalBytes;
            m_entries.erase(it);
            refuse(seq, reason, reply, result.rootFailed ? QStringLiteral("The folder for received files could not be prepared on the host.") : QString());
            return;
        }
        entry.fs = result.fs;
        entry.state = EntryState::Staging;
        // From the send on, the broker may end this transfer (cancel() erases the entry) before we return: keep what is needed in values
        // and look the entry up again after every call that can re-enter (send, notify, the host clipboard).
        const auto planPtr = entry.plan;
        const auto &plan = *planPtr;
        ConsoleWorkerWire::FilePushReply reply;
        reply.seq = seq;
        reply.accepted = true;
        reply.reason = Reason::None;
        reply.files = quint32(plan.files);
        reply.directories = quint32(plan.directories);
        reply.skipped = quint32(plan.skippedTotal());
        reply.totalBytes = plan.totalBytes;
        m_send(ConsoleWorkerWire::frame(reply));
        qInfo().nospace() << "Clipboard: files from the client accepted entries=" << (plan.files + plan.directories) << " bytes=" << plan.totalBytes << " skipped=" << plan.skippedTotal()
                          << " renamed=" << plan.renamed;
        if (!entryFor(seq)) return; // the broker ended it on the reply: nothing to announce or finish
        notify(QStringLiteral("Receiving files from the remote client"),
               QStringLiteral("%1 (%2)%3").arg(plural(plan.files, QStringLiteral("file")), QLocale().formattedDataSize(qint64(std::min<quint64>(plan.totalBytes, quint64(INT64_MAX)))),
                                               plan.skippedTotal() ? QStringLiteral("; %1 left out (links or files that cannot be copied)").arg(plan.skippedTotal()) : QString()));
        if (result.complete) { // nothing but folders and empty files: there is nothing to wait for
            finish(seq, result.finished); // (looks the entry up again: the notification may have ended it)
        }
    }

    void written(const ConsoleWorkerWire::FilePushData &chunk, WriteResult result)
    {
        using HostFileStaging::Status;
        if (!entryFor(chunk.seq)) return; // cancelled while the write was queued: nobody is waiting
        const quint64 seq = chunk.seq;
        if (result.status != Status::Ok) {
            ack(chunk, HostFileStaging::ackFor(result.status));
            Reason reason = Reason::WriteFailed;
            switch (result.status) {
            case Status::NoSpace: reason = Reason::NoSpace; break;
            case Status::SpaceUnknown: reason = Reason::SpaceUnknown; break;
            case Status::OutOfOrder:
            case Status::Hostile: reason = Reason::Hostile; break;
            case Status::RootUnsafe: reason = Reason::StagingFailed; break;
            case Status::Gone: reason = Reason::Cancelled; break;
            default: break;
            }
            ConsoleWorkerWire::FilePushReply numbers;
            numbers.freeBytes = result.failure.freeBytes;
            numbers.neededBytes = result.failure.neededBytes;
            fail(seq, reason, numbers); // by number: the ack went to the broker, which may have ended (erased) the transfer already
            return;
        }
        ack(chunk, ClipboardFileFetch::AckStatus::Ok);
        Entry *entry = entryFor(seq); // the ack can re-enter: nothing taken before it is valid any more
        if (!entry) return;
        entry->done += quint64(chunk.data.size());
        if (result.complete) {
            finish(seq, result.finished);
            return;
        }
        if (entry->timing == ClipboardFileStaging::OfferTiming::AfterFirstFile && !entry->offered && !result.finished.isEmpty()) {
            if (!offerList(seq, result.finished)) return;
            if (!(entry = entryFor(seq))) return;
            sendState(*entry, seq, ConsoleWorkerWire::FilePushPhase::Offered, Reason::None, true);
            return;
        }
        sendState(*entry, seq, ConsoleWorkerWire::FilePushPhase::Staging, Reason::None, false);
    }

    /// Every byte is in: offer (or refresh) the list and end the transfer in Done.
    void finish(quint64 seq, const QStringList &finished)
    {
        if (!entryFor(seq)) return; // ended meanwhile: nothing to offer
        if (!offerList(seq, finished)) return;
        Entry *entry = entryFor(seq); // the host clipboard write can re-enter
        if (!entry) return;
        entry->state = EntryState::Done;
        entry->done = entry->plan->totalBytes;
        qInfo().nospace() << "Clipboard: files from the client received bytes=" << entry->plan->totalBytes << " entries=" << (entry->plan->files + entry->plan->directories);
        sendState(*entry, seq, ConsoleWorkerWire::FilePushPhase::Done, Reason::None, true);
    }

    /// Hands the finished top-level units to the host clipboard. False: it refused, the transfer was failed and removed, or the transfer ended meanwhile.
    bool offerList(quint64 seq, const QStringList &paths)
    {
        if (paths.isEmpty()) return true;
        if (!m_backend.writeUriList || !m_backend.writeUriList(paths, m_policy.locked || m_policy.greeter)) {
            fail(seq, Reason::WriteFailed, {});
            return false;
        }
        Entry *entry = entryFor(seq); // writing the host clipboard can re-enter (a selection change reaches the broker)
        if (!entry) {
            // The transfer was ended and its staging removed while the list was being written: the host would paste files that are gone.
            if (m_backend.clearUriList) m_backend.clearUriList();
            return false;
        }
        entry->offered = true;
        m_offeredSeq = seq;
        return true;
    }

    // ---- ends ----

    /// The entry of \a seq, or null. Never keep the result across a call that can re-enter (send, notify, the host clipboard): ask again.
    Entry *entryFor(quint64 seq)
    {
        const auto it = m_entries.find(seq);
        return it == m_entries.end() ? nullptr : &it->second;
    }

    /// The transfer failed (or was stopped): tell the broker, the user, withdraw what was offered, remove the staging. Does nothing for a
    /// transfer that is not (or no longer) there: the broker ended it, and it already has the answer.
    void fail(quint64 seq, Reason reason, const ConsoleWorkerWire::FilePushReply &numbers)
    {
        const auto it = m_entries.find(seq);
        if (it == m_entries.end()) return;
        Entry entry = std::move(it->second);
        m_entries.erase(it);
        if (entry.offered && m_offeredSeq == seq && m_backend.clearUriList) {
            m_backend.clearUriList();
            m_offeredSeq = 0;
        }
        if (entry.fs) {
            auto fs = entry.fs;
            post([fs] { fs->abort(); });
        }
        m_send(ConsoleWorkerWire::frame(ConsoleWorkerWire::FilePushState{seq, ConsoleWorkerWire::FilePushPhase::Failed, reason, std::min(entry.done, entry.plan->totalBytes), entry.plan->totalBytes}));
        qInfo().nospace() << "Clipboard: files from the client failed reason=" << ConsoleWorkerWire::filePushReasonName(reason) << " bytes=" << entry.done;
        notifyFailure(reason, numbers.freeBytes, numbers.neededBytes);
    }

    /// Removes a transfer without telling anyone (the broker ended it, or a newer copy replaced it).
    void drop(quint64 seq)
    {
        const auto it = m_entries.find(seq);
        if (it == m_entries.end()) return;
        Entry entry = std::move(it->second);
        m_entries.erase(it);
        if (entry.offered && m_offeredSeq == seq && m_backend.clearUriList) {
            m_backend.clearUriList();
            m_offeredSeq = 0;
        }
        if (entry.fs) {
            auto fs = entry.fs;
            post([fs] { fs->abort(); });
        }
    }

    void refuse(quint64 seq, Reason reason, ConsoleWorkerWire::FilePushReply reply, const QString &notice = {})
    {
        reply.seq = seq;
        reply.accepted = false;
        reply.reason = reason;
        reply.files = reply.directories = 0;
        m_send(ConsoleWorkerWire::frame(reply));
        qInfo().nospace() << "Clipboard: refused (" << ConsoleWorkerWire::filePushReasonName(reason) << ") direction=client-to-host kind=files skipped=" << reply.skipped;
        if (!notice.isEmpty()) {
            notify(QStringLiteral("Could not receive files from the remote client"), notice);
        } else if (reason != Reason::Policy) {
            notifyFailure(reason, reply.freeBytes, reply.neededBytes);
        }
    }

    void ack(const ConsoleWorkerWire::FilePushData &chunk, ClipboardFileFetch::AckStatus status)
    {
        m_send(ConsoleWorkerWire::frame(ConsoleWorkerWire::FilePushAck{chunk.seq, chunk.index, chunk.offset, quint32(chunk.data.size()), status}));
    }

    void sendState(Entry &entry, quint64 seq, ConsoleWorkerWire::FilePushPhase phase, Reason reason, bool force)
    {
        if (!force && entry.sinceProgress.isValid() && entry.sinceProgress.elapsed() < 1000 / ConsoleWorkerWire::FileProgressMaxPerSecond) return;
        entry.sinceProgress.start();
        m_send(ConsoleWorkerWire::frame(ConsoleWorkerWire::FilePushState{seq, phase, reason, std::min(entry.done, entry.plan->totalBytes), entry.plan->totalBytes}));
    }

    // ---- notifications (counts, sizes, reasons; never a name) ----

    static QString plural(int count, const QString &noun)
    {
        return QStringLiteral("%1 %2%3").arg(count).arg(noun, count == 1 ? QString() : QStringLiteral("s"));
    }

    void notify(const QString &summary, const QString &body)
    {
        if (m_backend.notify) m_backend.notify(summary, body);
    }

    void notifyFailure(Reason reason, quint64 freeBytes, quint64 neededBytes)
    {
        const QString summary = QStringLiteral("Could not receive files from the remote client");
        const auto size = [](quint64 bytes) { return QLocale().formattedDataSize(qint64(std::min<quint64>(bytes, quint64(INT64_MAX)))); };
        switch (reason) {
        case Reason::NoSpace:
            notify(summary,
                   neededBytes ? QStringLiteral("Not enough space on the host: %1 needed, %2 free.").arg(size(neededBytes), size(freeBytes))
                               : QStringLiteral("Not enough space on the host."));
            break;
        case Reason::SpaceUnknown:
            notify(summary, QStringLiteral("The free space on the host could not be determined."));
            break;
        case Reason::Names:
            notify(summary, QStringLiteral("The list contains an unsafe file name; nothing was copied."));
            break;
        case Reason::Descriptor:
        case Reason::Hostile:
            notify(summary, QStringLiteral("The remote client sent data that was not valid; nothing was copied."));
            break;
        case Reason::NothingToCopy:
            notify(summary, QStringLiteral("There was nothing that could be copied (links and special files are skipped)."));
            break;
        case Reason::Policy:
            notify(summary, QStringLiteral("File copy to this computer is not allowed now."));
            break;
        case Reason::StagingFailed:
        case Reason::WriteFailed:
            notify(summary, QStringLiteral("The files could not be written on the host."));
            break;
        case Reason::None:
        case Reason::Superseded:
        case Reason::Cancelled:
            break;
        }
    }

    // ---- the background thread ----

    template<typename F>
    void post(F &&job)
    {
        const auto state = m_state;
        m_pool.start([state, job = std::forward<F>(job)]() mutable {
            if (state->alive) job();
        });
    }

    template<typename F>
    void deliver(F &&result)
    {
        const auto state = m_state;
        QMetaObject::invokeMethod(&m_context, [state, result = std::forward<F>(result)]() mutable {
            if (state->alive) result();
        }, Qt::QueuedConnection);
    }

    Sender m_send;
    Backend m_backend;
    bool m_desktop;
    HostFileStaging::Config m_config;
    Tuning m_tuning;
    std::shared_ptr<State> m_state;
    QObject m_context; // lives on the thread that built the stager: results are delivered there
    QThreadPool m_pool;
    std::unique_ptr<HostFileStaging::Area> m_area; // background thread only
    ConsoleWorkerWire::ClipboardPolicy m_policy;
    ConsoleWorkerWire::ControlState m_control;
    std::map<quint64, Entry> m_entries;
    quint64 m_offeredSeq = 0;
};
}
