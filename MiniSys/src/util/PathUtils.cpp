#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace minisys {

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        r = path;
    }
    if (path) CoTaskMemFree(path);
    return r;
}

fs::path AppDataDir() {
    auto p = KnownFolder(FOLDERID_LocalAppData) / L"MiniSys";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path LogsDir() {
    auto p = AppDataDir() / L"logs";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path HistoryDir() {
    auto p = AppDataDir() / L"history";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::wstring LongPath(const fs::path& p) {
    auto s = p.wstring();
    if (s.size() < 2) return s;
    if (s.rfind(LR"(\\?\)", 0) == 0) return s;
    if (s.rfind(LR"(\\)", 0) == 0)  return LR"(\\?\UNC\)" + s.substr(2);
    if (s.size() >= 2 && s[1] == L':') return LR"(\\?\)" + s;
    return s;
}

bool QueryDiskSpace(const std::wstring& driveRoot, DiskSpace& out) {
    ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
    if (!GetDiskFreeSpaceExW(driveRoot.c_str(), &freeAvail, &total, &freeTotal)) return false;
    out.totalBytes = total.QuadPart;
    out.freeBytes  = freeAvail.QuadPart;
    return true;
}

std::wstring DriveRootOf(const fs::path& p) {
    auto s = p.wstring();
    if (s.size() >= 2 && s[1] == L':') return std::wstring{s[0], L':', L'\\'};
    return {};
}

std::wstring SystemDriveRoot() {
    wchar_t buf[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    if (n >= 3 && buf[1] == L':') return std::wstring{buf[0], L':', L'\\'};
    return L"C:\\";
}

std::vector<std::wstring> EnumerateDrives() {
    std::vector<std::wstring> result;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = L'A' + static_cast<wchar_t>(i);
        std::wstring root = {letter, L':', L'\\'};
        UINT type = GetDriveTypeW(root.c_str());
        // Only include fixed drives (local disks)
        if (type == DRIVE_FIXED) {
            result.push_back(root);
        }
    }
    return result;
}

bool IsOnSystemDrive(const fs::path& p) {
    auto root = DriveRootOf(p);
    return !root.empty() && IEquals(root, SystemDriveRoot());
}

fs::path UserProfileDir() {
    return KnownFolder(FOLDERID_Profile);
}

bool QueryRecycleBin(RecycleBinInfo& out) {
    SHQUERYRBINFO info{ sizeof(info), 0, 0 };
    HRESULT hr = SHQueryRecycleBinW(nullptr, &info);
    if (FAILED(hr)) return false;
    out.sizeBytes = static_cast<unsigned long long>(info.i64Size);
    out.itemCount = static_cast<unsigned long long>(info.i64NumItems);
    return true;
}

bool EmptyRecycleBinAll() {
    HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr,
        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    return SUCCEEDED(hr);
}

bool FileExists(const fs::path& p) {
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const fs::path& p) {
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsReparsePoint(const fs::path& p) {
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

unsigned long long DirectorySize(const fs::path& p) {
    // Iterative DFS using FindFirstFileExW (basic info + large fetch).
    // Skips reparse points (junctions/symlinks) so we never double-count or escape volume.
    unsigned long long total = 0;
    std::vector<fs::path> stack;
    stack.reserve(64);
    stack.push_back(p);
    WIN32_FIND_DATAW fd{};
    while (!stack.empty()) {
        fs::path dir = std::move(stack.back());
        stack.pop_back();
        std::wstring search = LongPath(dir);
        if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
            search.push_back(L'\\');
        search.push_back(L'*');
        HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            DWORD attr = fd.dwFileAttributes;
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(dir / n);
            } else {
                total += (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32)
                       | static_cast<unsigned long long>(fd.nFileSizeLow);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return total;
}

namespace {

// Shared worker pool over a directory queue (REVIEW P1-3).
class SizePool {
public:
    SizePool(int numThreads, std::atomic<unsigned long long>& total)
        : total_(total) {
        if (numThreads <= 0) {
            unsigned hw = std::thread::hardware_concurrency();
            if (hw == 0) hw = 4;
            numThreads = static_cast<int>(hw);
            if (numThreads > 8) numThreads = 8;
            if (numThreads < 2) numThreads = 2;
        }
        for (int i = 0; i < numThreads; ++i) {
            threads_.emplace_back([this] { Worker(); });
        }
    }
    ~SizePool() { Join(); }

    void Push(const fs::path& dir) {
        {
            std::lock_guard<std::mutex> g(mu_);
            queue_.push_back(dir);
        }
        cv_.notify_one();
    }

    void Join() {
        {
            std::lock_guard<std::mutex> g(mu_);
            done_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

private:
    void Worker() {
        std::vector<fs::path> localStack;
        WIN32_FIND_DATAW fd{};
        for (;;) {
            fs::path dir;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !queue_.empty() || done_; });
                if (queue_.empty()) return;   // done_ and nothing left
                dir = std::move(queue_.front());
                queue_.pop_front();
                ++inflight_;
            }
            localStack.clear();
            localStack.push_back(dir);
            while (!localStack.empty()) {
                fs::path d = std::move(localStack.back());
                localStack.pop_back();
                std::wstring search = LongPath(d);
                if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
                    search.push_back(L'\\');
                search.push_back(L'*');
                HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                            FindExSearchNameMatch, nullptr,
                                            FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    const wchar_t* n = fd.cFileName;
                    if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
                    DWORD attr = fd.dwFileAttributes;
                    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                        localStack.push_back(d / n);
                    } else {
                        total_ += (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32)
                                | static_cast<unsigned long long>(fd.nFileSizeLow);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            {
                std::lock_guard<std::mutex> g(mu_);
                --inflight_;
                if (queue_.empty() && inflight_ == 0) {
                    done_ = true;
                    cv_.notify_all();
                }
            }
        }
    }

    std::atomic<unsigned long long>& total_;
    std::vector<std::thread> threads_;
    std::deque<fs::path> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    int inflight_ = 0;
    bool done_ = false;
};

} // namespace

unsigned long long DirectorySizeParallel(const fs::path& p, int numThreads) {
    if (!DirExists(p)) return 0;
    std::atomic<unsigned long long> total{0};
    {
        SizePool pool(numThreads, total);
        pool.Push(p);
        pool.Join();   // dtor would join too; explicit for clarity
    }
    return total.load();
}

} // namespace minisys
