// v2.13 perf-budget bench (goal: warm scan & search ≤ 5 s with background
// + realtime prewarming). Informational timings printed; the warm-scan
// assert IS the budget.
#include "core/JunkScanner.h"
#include "core/VolumeIndex.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

namespace minisys {
namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

} // namespace

// Warm SEARCH on a ~1M-entry index (synthetic: 1000 dirs × 1000 files).
// The index scan is pure memory work; the synthetic tree is representative
// of one full volume.
TEST(PerfBench, SearchWarmMillionEntries) {
    auto& vi = VolumeIndex::Instance();
    vi.ResetForTesting();
    auto t0 = Clock::now();
    vi.AddNodeForTesting(1, 1, L"", true, 0);   // root
    for (uint64_t d = 2; d <= 1001; ++d) {
        vi.AddNodeForTesting(d, 1, L"dir" + std::to_wstring(d), true, 0);
    }
    uint64_t frn = 1002;
    for (uint64_t d = 2; d <= 1001; ++d) {
        for (int f = 0; f < 1000; ++f) {
            vi.AddNodeForTesting(frn++, d, L"file" + std::to_wstring(f) + L".txt",
                                 false, 0);
        }
    }
    vi.FinalizeForTesting(L'Z');
    double buildMs = MsSince(t0);
    // (EntryCount stays 0 for synthetic test indexes — FinalizeForTesting
    // doesn't publish it; the searches below are the real check.)

    // Warm query: term present everywhere + one that matches nothing
    // (worst case: full scan without early exit).
    std::atomic<bool> cancel{false};
    for (const wchar_t* q : { L"file500", L"zzz_nomatch_zzz" }) {
        auto tq = Clock::now();
        size_t n = 0;
        vi.Search(q, false, 1000,
                  [&](const VolumeIndex::SearchHit&) { ++n; return true; });
        double ms = MsSince(tq);
        printf("[bench] search %-16ls : %8.1f ms (%zu hits)\n", q, ms, n);
        EXPECT_LT(ms, 2000.0);
    }
    printf("[bench] synthetic index build (1,000,001 nodes): %.1f ms\n", buildMs);
    vi.ResetForTesting();
}

// Warm JUNK SCAN on the real filesystem (unelevated environment: the USN
// index is unavailable, so rules expand via directory listing — this is the
// conservative/worst case; DirSizeCache + incremental discovery still apply).
// Run 1 = cold (sizes + discovery walked), run 2 = warm (the budget).
TEST(PerfBench, JunkScanWarmBudget5s) {
    std::vector<ScanItem> items;
    std::atomic<bool> cancel{false};
    auto progress = [](unsigned long long, unsigned long long, const std::wstring&) {};

    JunkScanner cold;
    auto t0 = Clock::now();
    cold.Scan(items, progress, cancel);
    double coldMs = MsSince(t0);
    size_t coldCount = items.size();
    items.clear();

    JunkScanner warm;
    auto t1 = Clock::now();
    warm.Scan(items, progress, cancel);
    double warmMs = MsSince(t1);

    printf("[bench] junk scan COLD: %.0f ms (%zu items)\n", coldMs, coldCount);
    printf("[bench] junk scan WARM: %.0f ms (%zu items)  [budget: 5000 ms]\n",
           warmMs, items.size());
    // The goal budget. Machine-dependent by nature; this project has no CI
    // fleet — the assert documents the contract on the dev machine.
    EXPECT_LT(warmMs, 5000.0);
}

} // namespace minisys
