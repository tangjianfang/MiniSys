#include "util/FastWalk.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace minisys {

namespace {

inline unsigned long long PackFiletime(const FILETIME& ft) {
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32)
         | static_cast<unsigned long long>(ft.dwLowDateTime);
}

inline bool IsDotDir(const wchar_t* n) {
    return n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0));
}

// Returns true if 'lowerPath' starts with any prefix in 'excludes'.
// Both inputs must already be lowercase. We treat trailing backslash as
// boundary so "C:\\windows" excludes "C:\\windows\\foo" but not "C:\\windowsapps".
bool IsExcluded(const std::wstring& lowerPath,
                const std::vector<std::wstring>& excludes) {
    for (const auto& ex : excludes) {
        if (ex.empty()) continue;
        if (lowerPath.size() < ex.size()) continue;
        if (lowerPath.compare(0, ex.size(), ex) != 0) continue;
        // Boundary check: either exact match or next char is separator.
        if (lowerPath.size() == ex.size()) return true;
        wchar_t next = lowerPath[ex.size()];
        if (next == L'\\' || next == L'/') return true;
        // Also treat excludes ending in \ as already boundary-terminated.
        if (!ex.empty() && (ex.back() == L'\\' || ex.back() == L'/')) return true;
    }
    return false;
}

class Pool {
public:
    Pool(const std::vector<std::wstring>& excludes,
         const std::function<void(const FastFileInfo&)>& onFile,
         const std::atomic<bool>& cancel,
         const std::function<void(unsigned long long, const std::wstring&)>& onProgress,
         int numThreads)
        : excludes_(excludes), onFile_(onFile), cancel_(cancel), onProgress_(onProgress) {
        int n = numThreads;
        if (n <= 0) {
            unsigned hw = std::thread::hardware_concurrency();
            if (hw == 0) hw = 4;
            n = static_cast<int>(hw);
            if (n > 8) n = 8;
            if (n < 2) n = 2;
        }
        threads_.reserve(n);
    }

    void Push(const fs::path& dir) {
        {
            std::lock_guard<std::mutex> g(mu_);
            queue_.push_back(dir);
        }
        cv_.notify_one();
    }

    void Run() {
        for (size_t i = 0; i < threads_.capacity(); ++i) {
            threads_.emplace_back([this] { Worker(); });
        }
        for (auto& t : threads_) t.join();
    }

private:
    void Worker() {
        for (;;) {
            fs::path dir;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] {
                    return !queue_.empty() || (inflight_.load() == 0) || cancel_.load();
                });
                if (cancel_.load()) {
                    cv_.notify_all();
                    return;
                }
                if (queue_.empty()) {
                    if (inflight_.load() == 0) {
                        cv_.notify_all();
                        return;
                    }
                    continue;
                }
                dir = std::move(queue_.front());
                queue_.pop_front();
                inflight_.fetch_add(1);
            }

            EnumerateOne(dir);

            inflight_.fetch_sub(1);
            cv_.notify_all();
        }
    }

    void EnumerateOne(const fs::path& dir) {
        if (cancel_.load()) return;

        std::wstring search = LongPath(dir);
        if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
            search.push_back(L'\\');
        search.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(
            search.c_str(),
            FindExInfoBasic,                 // skip 8.3 short name lookup
            &fd,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);      // bigger buffer per syscall

        if (h == INVALID_HANDLE_VALUE) return;

        if (onProgress_) {
            unsigned long long v = visited_.load();
            onProgress_(v, dir.wstring());
        }

        do {
            if (cancel_.load()) { FindClose(h); return; }
            if (IsDotDir(fd.cFileName)) continue;

            fs::path full = dir / fd.cFileName;
            DWORD attr = fd.dwFileAttributes;

            // Reparse points are NEVER followed (safety: avoids loops + keeps us on volume).
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring lower = ToLower(full.wstring());
                if (IsExcluded(lower, excludes_)) continue;
                Push(full);
            } else {
                FastFileInfo fi;
                fi.path = std::move(full);
                fi.sizeBytes = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32)
                             | static_cast<unsigned long long>(fd.nFileSizeLow);
                fi.lastWriteFiletime = PackFiletime(fd.ftLastWriteTime);
                fi.createFiletime    = PackFiletime(fd.ftCreationTime);
                visited_.fetch_add(1);
                onFile_(fi);
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }

    const std::vector<std::wstring>& excludes_;
    const std::function<void(const FastFileInfo&)>& onFile_;
    const std::atomic<bool>& cancel_;
    const std::function<void(unsigned long long, const std::wstring&)>& onProgress_;

    std::vector<std::thread> threads_;
    std::deque<fs::path>     queue_;
    std::mutex               mu_;
    std::condition_variable  cv_;
    std::atomic<int>         inflight_{0};
    std::atomic<unsigned long long> visited_{0};
};

} // namespace

void FastWalk(const std::vector<fs::path>& roots,
              const std::vector<std::wstring>& excludesLower,
              const std::function<void(const FastFileInfo&)>& onFile,
              const std::atomic<bool>& cancel,
              int numThreads,
              const std::function<void(unsigned long long, const std::wstring&)>& onProgress) {
    Pool pool(excludesLower, onFile, cancel, onProgress, numThreads);
    bool any = false;
    for (const auto& r : roots) {
        if (!DirExists(r)) continue;
        std::wstring lower = ToLower(r.wstring());
        if (IsExcluded(lower, excludesLower)) continue;
        pool.Push(r);
        any = true;
    }
    if (!any) return;
    pool.Run();
}

} // namespace minisys
