#include "platform/Hash.h"
#include "util/PathUtils.h"

#include <windows.h>
#include <bcrypt.h>
#include <vector>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((s) >= 0)
#endif

namespace minisys {

namespace {
struct AlgGuard {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct HashGuard {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~HashGuard() { if (h) BCryptDestroyHash(h); }
};

std::wstring HashStreamHandle(HANDLE file, unsigned long long maxBytes /*0=all*/) {
    AlgGuard alg;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};
    DWORD hashObjLen = 0, cb = 0;
    if (!NT_SUCCESS(BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
            (PUCHAR)&hashObjLen, sizeof(hashObjLen), &cb, 0))) return {};
    DWORD hashLen = 0;
    if (!NT_SUCCESS(BCryptGetProperty(alg.h, BCRYPT_HASH_LENGTH,
            (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0))) return {};
    std::vector<UCHAR> hashObj(hashObjLen);
    HashGuard hg;
    if (!NT_SUCCESS(BCryptCreateHash(alg.h, &hg.h, hashObj.data(), hashObjLen,
            nullptr, 0, 0))) return {};
    constexpr DWORD kBuf = 1 << 20; // 1 MiB
    std::vector<UCHAR> buf(kBuf);
    unsigned long long remaining = (maxBytes == 0) ? ~0ULL : maxBytes;
    DWORD n = 0;
    while (remaining > 0) {
        DWORD want = (remaining > kBuf) ? kBuf : (DWORD)remaining;
        if (!ReadFile(file, buf.data(), want, &n, nullptr) || n == 0) break;
        if (!NT_SUCCESS(BCryptHashData(hg.h, buf.data(), n, 0))) return {};
        if (maxBytes != 0) remaining -= n;
    }
    std::vector<UCHAR> out(hashLen);
    if (!NT_SUCCESS(BCryptFinishHash(hg.h, out.data(), hashLen, 0))) return {};
    static const wchar_t* hex = L"0123456789abcdef";
    std::wstring r;
    r.reserve(static_cast<size_t>(hashLen) * 2);
    for (DWORD i = 0; i < hashLen; ++i) {
        r += hex[out[i] >> 4];
        r += hex[out[i] & 0xF];
    }
    return r;
}
} // namespace

std::wstring Sha256OfFile(const std::filesystem::path& p) {
    HANDLE file = CreateFileW(LongPath(p).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    auto r = HashStreamHandle(file, 0);
    CloseHandle(file);
    return r;
}

std::wstring Sha256OfFileHead(const std::filesystem::path& p, unsigned long long maxBytes) {
    if (maxBytes == 0) return Sha256OfFile(p);
    HANDLE file = CreateFileW(LongPath(p).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    auto r = HashStreamHandle(file, maxBytes);
    CloseHandle(file);
    return r;
}

} // namespace minisys
