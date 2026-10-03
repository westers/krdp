// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// Test-only LD_PRELOAD shim (OPT-055 T-K4b): makes libavfilter's av_buffersink_get_frame fail with
// ENOMEM, the way a dying encoder does, so the worker's encoder-failure ladder can be driven from
// outside. Never installed.
//
//   FARSIDE_TEST_FAIL_FILTER_AFTER=N   fail from the (N+1)th call on (process-wide); unset = never fail
//   FARSIDE_TEST_FAIL_FILTER_COUNT=M   fail M calls, then work again (default: forever)
//   FARSIDE_TEST_FAIL_FILTER_FILE=path fail calls while that file exists (at most COUNT of them)
//   FARSIDE_TEST_BLOCK_FILTER_FILE=path block the calling thread forever once that file exists (a wedged driver call)

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

namespace
{
long envLong(const char *name, long fallback)
{
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return std::strtol(value, nullptr, 10);
}
std::atomic<long> calls{0};
std::atomic<long> failures{0};
}

extern "C" __attribute__((visibility("default"))) int av_buffersink_get_frame(void *context, void *frame)
{
    using Fn = int (*)(void *, void *);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "av_buffersink_get_frame"));
    static const long after = envLong("FARSIDE_TEST_FAIL_FILTER_AFTER", -1);
    static const long count = envLong("FARSIDE_TEST_FAIL_FILTER_COUNT", 1L << 40);
    static const char *const block = std::getenv("FARSIDE_TEST_BLOCK_FILTER_FILE");
    if (block && *block && access(block, F_OK) == 0) {
        std::fputs("failfilter: av_buffersink_get_frame blocks forever\n", stderr);
        for (;;) {
            pause();
        }
    }
    static const char *const file = std::getenv("FARSIDE_TEST_FAIL_FILTER_FILE");
    if (file && *file && access(file, F_OK) == 0 && failures.fetch_add(1) < count) {
        std::fputs("failfilter: av_buffersink_get_frame -> ENOMEM\n", stderr);
        return -ENOMEM;
    }
    if (after >= 0) {
        const long n = calls.fetch_add(1);
        if (n >= after && n < after + count) {
            std::fputs("failfilter: av_buffersink_get_frame -> ENOMEM\n", stderr);
            return -ENOMEM; // AVERROR(ENOMEM)
        }
    }
    return real ? real(context, frame) : -ENOSYS;
}
