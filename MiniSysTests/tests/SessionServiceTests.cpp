#include <gtest/gtest.h>

#include "core/SessionService.h"
#include "core/TabId.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace minisys {
namespace {

// Waits until IsScanning() turns false (with timeout). Returns false on timeout.
bool WaitForIdle(SessionService& svc, int timeoutMs = 10000) {
    int waited = 0;
    while (svc.IsScanning() && waited < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
    return !svc.IsScanning();
}

class FakeScanner : public Scanner {
public:
    explicit FakeScanner(std::vector<ScanItem> items, unsigned progressCalls = 3)
        : items_(std::move(items)), progressCalls_(progressCalls) {}

    void Scan(std::vector<ScanItem>& out, ProgressFn progress,
              const std::atomic<bool>& cancel) override {
        for (unsigned i = 0; i < progressCalls_; ++i) {
            if (cancel.load()) return;
            if (progress) progress(i, progressCalls_, L"step");
        }
        if (!cancel.load()) out = items_;
    }

private:
    std::vector<ScanItem> items_;
    unsigned progressCalls_;
};

// Stays in Scan until cancelled — for busy-rejection tests.
class BlockingScanner : public Scanner {
public:
    void Scan(std::vector<ScanItem>&, ProgressFn,
              const std::atomic<bool>& cancel) override {
        while (!cancel.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

class ThrowingScanner : public Scanner {
public:
    void Scan(std::vector<ScanItem>&, ProgressFn, const std::atomic<bool>&) override {
        throw std::runtime_error("boom");
    }
};

ScanItem MakeItem(const wchar_t* title) {
    ScanItem it;
    it.title = title;
    it.path = std::wstring(L"C:\\nonexistent\\") + title;
    it.sizeBytes = 123;
    return it;
}

TEST(SessionServiceTests, StartScanStoresResultsAndCompletes) {
    auto& svc = SessionService::Instance();
    svc.SetWindow(nullptr);
    std::vector<ScanItem> items{MakeItem(L"a"), MakeItem(L"b"), MakeItem(L"c")};
    ASSERT_TRUE(svc.StartScan(TabId::Junk,
                              std::make_unique<FakeScanner>(std::move(items))));
    ASSERT_TRUE(WaitForIdle(svc));
    EXPECT_EQ(svc.Results(TabId::Junk).size(), 3u);
    EXPECT_EQ(svc.Results(TabId::Junk)[0].title, L"a");
    // Completion progress text mentions the item count.
    EXPECT_NE(svc.ProgressText().find(L"3"), std::wstring::npos);
}

TEST(SessionServiceTests, StartScanRejectsWhenBusy) {
    auto& svc = SessionService::Instance();
    svc.SetWindow(nullptr);
    ASSERT_TRUE(svc.StartScan(TabId::LargeFiles, std::make_unique<BlockingScanner>()));
    EXPECT_FALSE(svc.StartScan(TabId::Junk,
                               std::make_unique<FakeScanner>(std::vector<ScanItem>{})));
    svc.CancelScan();
    ASSERT_TRUE(WaitForIdle(svc));
    // Busy scan was cancelled before producing items.
    EXPECT_TRUE(svc.Results(TabId::LargeFiles).empty());
}

TEST(SessionServiceTests, CancelProducesEmptyResults) {
    auto& svc = SessionService::Instance();
    svc.SetWindow(nullptr);
    ASSERT_TRUE(svc.StartScan(TabId::Junk,
                              std::make_unique<FakeScanner>(std::vector<ScanItem>{MakeItem(L"x")})));
    svc.CancelScan();
    ASSERT_TRUE(WaitForIdle(svc));
    EXPECT_TRUE(svc.Results(TabId::Junk).empty());
}

TEST(SessionServiceTests, ScannerExceptionIsContained) {
    auto& svc = SessionService::Instance();
    svc.SetWindow(nullptr);
    ASSERT_TRUE(svc.StartScan(TabId::Apps, std::make_unique<ThrowingScanner>()));
    ASSERT_TRUE(WaitForIdle(svc));
    EXPECT_TRUE(svc.Results(TabId::Apps).empty());
}

TEST(SessionServiceTests, ResultsArePerTab) {
    auto& svc = SessionService::Instance();
    svc.SetWindow(nullptr);
    ASSERT_TRUE(svc.StartScan(TabId::Junk,
                              std::make_unique<FakeScanner>(std::vector<ScanItem>{MakeItem(L"j")})));
    ASSERT_TRUE(WaitForIdle(svc));
    EXPECT_EQ(svc.Results(TabId::Junk).size(), 1u);
    EXPECT_TRUE(svc.Results(TabId::Apps).empty());
    EXPECT_TRUE(svc.Results(TabId::FolderTree).empty());
}

TEST(SessionServiceTests, ShutdownJoinsThread) {
    auto& svc = SessionService::Instance();
    svc.SetWindow(nullptr);
    ASSERT_TRUE(svc.StartScan(TabId::FolderTree, std::make_unique<BlockingScanner>()));
    svc.Shutdown();
    EXPECT_FALSE(svc.IsScanning());
    // Service must remain usable after shutdown.
    ASSERT_TRUE(svc.StartScan(TabId::FolderTree,
                              std::make_unique<FakeScanner>(std::vector<ScanItem>{MakeItem(L"f")})));
    ASSERT_TRUE(WaitForIdle(svc));
    EXPECT_EQ(svc.Results(TabId::FolderTree).size(), 1u);
}

} // namespace
} // namespace minisys
