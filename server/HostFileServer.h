// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QThreadPool>

#include "ConsoleWorkerWire.h"
#include "HostFiles.h"

namespace KRdp
{
/**
 * OPT-078 phase 2: the worker's file service for host-to-client copy. HostClipboard registers each file copy as an offer
 * (the paths the user copied); the broker asks for the descriptor (a Files ClipboardRequest), then for ranges (FileRead).
 *
 *  - A walk or a read can block on a slow disk, so both run on ONE background thread; the results are delivered to the thread
 *    that built this object (a queued call on an internal context), where the records are sent. The worker's event loop (capture,
 *    input, the broker socket) never waits for a disk.
 *  - An offer's snapshot is kept while it is the current offer or while the client holds a lock on it (at most MaxLocks); the
 *    descriptors it holds are closed when it is released. Everything is released when the policy stops allowing files, when
 *    control moves, and on FileCancel.
 *  - Reads are answered only for the current policy and control generation (mayServeFiles). A read for an offer that is gone is
 *    answered NoSuchOffer; a cancelled read is dropped silently (the broker already gave up on it).
 *  - Logs carry counts and reasons, never names or contents.
 */
class HostFileServer
{
public:
    using Sender = std::function<void(const QByteArray &record)>;

    HostFileServer(Sender send, bool desktop)
        : m_send(std::move(send))
        , m_desktop(desktop)
        , m_state(std::make_shared<State>())
    {
        m_pool.setMaxThreadCount(1);
        m_pool.setExpiryTimeout(2000);
    }

    ~HostFileServer()
    {
        m_state->alive = false;
        m_pool.waitForDone(); // a job in flight finishes (a read or a walk is bounded); its result is dropped
    }

    void setPolicy(const ConsoleWorkerWire::ClipboardPolicy &policy)
    {
        if (policy.revision < m_policy.revision) return;
        m_policy = policy;
        releaseIfNotAllowed();
    }
    void setControl(const ConsoleWorkerWire::ControlState &control)
    {
        m_control = control;
        releaseIfNotAllowed();
    }
    /// Test seam: the walk's clock and budget.
    void setWalkClock(std::function<qint64()> nowMs, qint64 budgetMs)
    {
        m_walkClock = std::move(nowMs);
        m_walkBudgetMs = budgetMs;
    }

    /// The host copied \a paths as offer \a seq (a file copy). An empty list makes \a seq the current offer without files (a text copy).
    void registerOffer(quint64 seq, const QList<QByteArray> &paths)
    {
        m_current = seq;
        Offer &offer = m_offers[seq];
        offer.paths = paths;
        trim();
    }

    /// The answer to a Files ClipboardRequest for the current offer.
    void list(const ConsoleWorkerWire::ClipboardRequest &request)
    {
        ConsoleWorkerWire::ClipboardData answer;
        answer.requestId = request.requestId;
        answer.kind = ConsoleWorkerWire::ClipboardKind::Files;
        const auto it = m_offers.find(m_current);
        if (it == m_offers.end() || it->second.paths.isEmpty()) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
            m_send(ConsoleWorkerWire::frame(answer));
            return;
        }
        Offer &offer = it->second;
        answer.seq = m_current;
        if (offer.snapshot) { // walked already (the client asked twice, or a lock came first)
            sendList(answer, offer);
            return;
        }
        const quint64 seq = m_current;
        const auto paths = offer.paths;
        HostFiles::WalkOptions options;
        options.hugeFiles = request.hugeFiles;
        options.budgetMs = m_walkBudgetMs;
        options.nowMs = m_walkClock;
        const quint64 epoch = offer.epoch;
        post([this, paths, options, seq, epoch, answer]() mutable {
            auto result = std::make_shared<HostFiles::WalkResult>(HostFiles::walk(paths, options));
            deliver([this, result, seq, epoch, answer]() mutable {
                const auto found = m_offers.find(seq);
                if (found == m_offers.end() || found->second.epoch != epoch) { // released while walking
                    answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
                    m_send(ConsoleWorkerWire::frame(answer));
                    return;
                }
                Offer &current = found->second;
                switch (result->status) {
                case HostFiles::WalkStatus::Ok:
                    current.snapshot = result->snapshot;
                    current.descriptor = result->descriptor;
                    current.skipped = result->skipped;
                    current.count = result->count;
                    ++m_counters.listsServed;
                    qInfo().nospace() << "Clipboard: files offered entries=" << result->count << " bytes=" << result->snapshot->totalBytes()
                                      << " skipped=" << result->skipped;
                    sendList(answer, current);
                    return;
                case HostFiles::WalkStatus::Empty:
                    answer.status = ConsoleWorkerWire::ClipboardStatus::Empty;
                    break;
                case HostFiles::WalkStatus::Failed:
                    answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
                    break;
                default: // too many, too deep, over the time budget
                    answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
                    break;
                }
                ++m_counters.listsRefused;
                // A reason, never a name (OPT-078).
                qInfo().nospace() << "Clipboard: refused (walk-" << int(result->status) << ") direction=host-to-client kind=files skipped=" << result->skipped;
                m_send(ConsoleWorkerWire::frame(answer));
            });
        });
    }

    void lock(const ConsoleWorkerWire::FileLock &lock)
    {
        if (!allowed()) return;
        const auto it = m_offers.find(lock.seq);
        if (it == m_offers.end()) return;
        if (m_locks.contains(lock.clipDataId)) unlock({lock.clipDataId});
        if (m_locks.size() >= ClipboardFileLimits::MaxLocks) return; // the broker keeps at most MaxLocks too
        m_locks.insert(lock.clipDataId, lock.seq);
        ++it->second.locks;
    }

    void unlock(const ConsoleWorkerWire::FileUnlock &unlock)
    {
        const auto seq = m_locks.take(unlock.clipDataId);
        if (!seq) return;
        if (const auto it = m_offers.find(seq); it != m_offers.end() && it->second.locks > 0) --it->second.locks;
        trim();
    }

    void read(const ConsoleWorkerWire::FileRead &request)
    {
        ConsoleWorkerWire::FileReadResult result;
        result.readId = request.readId;
        if (!allowed()) {
            result.status = ConsoleWorkerWire::FileReadStatus::Denied;
            m_send(ConsoleWorkerWire::frame(result));
            return;
        }
        const auto it = m_offers.find(request.seq);
        if (it == m_offers.end() || !it->second.snapshot) {
            result.status = ConsoleWorkerWire::FileReadStatus::NoSuchOffer;
            m_send(ConsoleWorkerWire::frame(result));
            return;
        }
        const auto snapshot = it->second.snapshot;
        const quint64 seq = request.seq;
        const quint64 epoch = it->second.epoch;
        post([this, snapshot, request, seq, epoch]() {
            const auto read = snapshot->read(request.index, request.offset, request.length);
            deliver([this, read, request, seq, epoch]() {
                const auto found = m_offers.find(seq);
                if (found == m_offers.end() || found->second.epoch != epoch) return; // cancelled or released: nobody is waiting
                Offer &offer = found->second;
                ConsoleWorkerWire::FileReadResult result;
                result.readId = request.readId;
                switch (read.status) {
                case HostFiles::ReadStatus::Ok:
                    result.status = ConsoleWorkerWire::FileReadStatus::Ok;
                    result.data = read.data;
                    offer.done += quint64(read.data.size());
                    ++m_counters.readsOk;
                    break;
                case HostFiles::ReadStatus::Changed:
                    result.status = ConsoleWorkerWire::FileReadStatus::Changed;
                    break;
                case HostFiles::ReadStatus::Gone:
                    result.status = ConsoleWorkerWire::FileReadStatus::Gone;
                    break;
                case HostFiles::ReadStatus::Denied:
                    result.status = ConsoleWorkerWire::FileReadStatus::Denied;
                    break;
                case HostFiles::ReadStatus::IoError:
                    result.status = ConsoleWorkerWire::FileReadStatus::IoError;
                    break;
                }
                if (result.status != ConsoleWorkerWire::FileReadStatus::Ok) {
                    ++m_counters.readsFailed;
                    qInfo().nospace() << "Clipboard: file read failed (status=" << int(result.status) << ") direction=host-to-client";
                }
                m_send(ConsoleWorkerWire::frame(result));
                progress(seq, offer, result.status != ConsoleWorkerWire::FileReadStatus::Ok);
            });
        });
    }

    /// The offer \a cancel.seq is over for the client (0: every offer): queued reads are dropped, descriptors closed.
    void cancel(const ConsoleWorkerWire::FileCancel &cancel)
    {
        if (cancel.seq == 0) {
            for (auto &[seq, offer] : m_offers) ++offer.epoch;
            return;
        }
        if (const auto it = m_offers.find(cancel.seq); it != m_offers.end()) ++it->second.epoch;
    }

    struct Counters {
        quint64 listsServed = 0;
        quint64 listsRefused = 0;
        quint64 readsOk = 0;
        quint64 readsFailed = 0;
    };
    Counters counters() const { return m_counters; }
    int offerCount() const { return int(m_offers.size()); }
    bool hasSnapshot(quint64 seq) const
    {
        const auto it = m_offers.find(seq);
        return it != m_offers.end() && it->second.snapshot;
    }
    quint64 currentSeq() const { return m_current; }
    /// Test seam: waits until the background thread is idle (results still need the event loop to be delivered).
    bool waitForIdle(int ms) { return m_pool.waitForDone(ms); }

private:
    struct State {
        std::atomic<bool> alive = true;
    };
    struct Offer {
        QList<QByteArray> paths;
        std::shared_ptr<HostFiles::Snapshot> snapshot;
        QByteArray descriptor;
        quint32 skipped = 0;
        int count = 0;
        int locks = 0;
        quint64 epoch = 1; ///< bumped by a cancel: results of reads queued before it are dropped
        quint64 done = 0;
        QElapsedTimer sinceProgress;
        bool finished = false;
    };

    bool allowed() const { return ConsoleWorkerWire::mayServeFiles(m_control, m_policy, m_desktop); }

    void releaseIfNotAllowed()
    {
        if (allowed()) return;
        for (auto &[seq, offer] : m_offers) ++offer.epoch;
        m_offers.clear();
        m_locks.clear();
        m_current = 0;
    }

    void trim()
    {
        for (auto it = m_offers.begin(); it != m_offers.end();) {
            if (it->first != m_current && it->second.locks == 0) it = m_offers.erase(it);
            else ++it;
        }
    }

    void sendList(ConsoleWorkerWire::ClipboardData answer, const Offer &offer)
    {
        answer.status = ConsoleWorkerWire::ClipboardStatus::Ok;
        answer.files = offer.descriptor;
        answer.skipped = offer.skipped;
        m_send(ConsoleWorkerWire::frame(answer));
    }

    void progress(quint64 seq, Offer &offer, bool failed)
    {
        if (!offer.snapshot || offer.finished) return;
        const quint64 total = offer.snapshot->totalBytes();
        const bool done = !failed && offer.done >= total;
        if (!failed && !done && offer.sinceProgress.isValid() && offer.sinceProgress.elapsed() < 1000 / ConsoleWorkerWire::FileProgressMaxPerSecond) return;
        offer.sinceProgress.start();
        offer.finished = failed || done;
        m_send(ConsoleWorkerWire::frame(ConsoleWorkerWire::FileProgress{seq, std::min(offer.done, total), total,
                                                                         failed ? ConsoleWorkerWire::FileProgressState::Failed
                                                                                : done ? ConsoleWorkerWire::FileProgressState::Done : ConsoleWorkerWire::FileProgressState::Reading}));
    }

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
    bool m_desktop;
    std::shared_ptr<State> m_state;
    QObject m_context; // lives on the thread that built the server: results are delivered there
    QThreadPool m_pool;
    ConsoleWorkerWire::ClipboardPolicy m_policy;
    ConsoleWorkerWire::ControlState m_control;
    std::map<quint64, Offer> m_offers;
    QHash<quint32, quint64> m_locks; ///< clipDataId -> offer seq
    quint64 m_current = 0;
    std::function<qint64()> m_walkClock;
    qint64 m_walkBudgetMs = ClipboardFileLimits::WalkBudgetMs;
    Counters m_counters;
};
}
