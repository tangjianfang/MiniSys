#include "platform/SecretStore.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace minisys {

namespace {

// "Administrators + SYSTEM, full; everyone else: nothing." The elevated app
// can read/write; a standard user cannot even read the value.
constexpr const wchar_t* kSddl = L"D:P(A;;GA;;;BA)(A;;GA;;;SY)";
constexpr const wchar_t* kKeyPath = L"SOFTWARE\\MiniSys";
constexpr const wchar_t* kValueName = L"RecordKey";

bool ReadSecret(std::vector<uint8_t>& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKeyPath, 0, KEY_READ, &k) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, size = 0;
    bool ok = false;
    if (RegQueryValueExW(k, kValueName, nullptr, &type, nullptr, &size) ==
            ERROR_SUCCESS &&
        type == REG_BINARY && size == 32) {
        out.resize(size);
        ok = RegQueryValueExW(k, kValueName, nullptr, nullptr, out.data(),
                              &size) == ERROR_SUCCESS;
        if (!ok) out.clear();
    }
    RegCloseKey(k);
    return ok;
}

bool WriteSecret(const std::vector<uint8_t>& secret) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSddl, SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    sa.lpSecurityDescriptor = sd;
    HKEY k = nullptr;
    DWORD disp = 0;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kKeyPath, 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, &sa, &k,
                             &disp);
    LocalFree(sd);
    if (r != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(k, kValueName, 0, REG_BINARY, secret.data(),
                             static_cast<DWORD>(secret.size())) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

} // namespace

std::vector<uint8_t> GetOrCreateMachineSecret() {
    std::vector<uint8_t> secret;
    if (ReadSecret(secret)) return secret;
    secret.resize(32);
    if (BCryptGenRandom(nullptr, secret.data(), 32,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    if (!WriteSecret(secret)) return {};
    return secret;
}

HmacSha256::HmacSha256(const std::vector<uint8_t>& secret) {
    if (secret.empty()) return;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    0) != 0) {
        return;
    }
    ULONG objLen = 0, got = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen),
                          &got, 0) != 0 ||
        objLen == 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return;
    }
    objBuf_ = new (std::nothrow) unsigned char[objLen];
    if (!objBuf_) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return;
    }
    BCRYPT_HASH_HANDLE h = nullptr;
    NTSTATUS st = BCryptCreateHash(
        alg, &h, objBuf_, objLen,
        const_cast<PUCHAR>(secret.data()),
        static_cast<ULONG>(secret.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);   // the hash handle holds its own ref
    if (st != 0) {
        delete[] objBuf_;
        objBuf_ = nullptr;
        return;
    }
    hash_ = h;
}

HmacSha256::~HmacSha256() {
    if (hash_) BCryptDestroyHash(reinterpret_cast<BCRYPT_HASH_HANDLE>(hash_));
    delete[] objBuf_;
}

void HmacSha256::Update(const void* data, size_t bytes) {
    if (!hash_ || finished_) return;
    BCryptHashData(reinterpret_cast<BCRYPT_HASH_HANDLE>(hash_),
                   static_cast<PUCHAR>(const_cast<void*>(data)),
                   static_cast<ULONG>(bytes), 0);
}

std::vector<uint8_t> HmacSha256::Finish() {
    std::vector<uint8_t> out(32, 0);
    if (!hash_ || finished_) {
        out.clear();
        return out;
    }
    finished_ = true;
    if (BCryptFinishHash(reinterpret_cast<BCRYPT_HASH_HANDLE>(hash_),
                         out.data(), 32, 0) != 0) {
        out.clear();
    }
    return out;
}

std::wstring HmacSha256Hex(const std::vector<uint8_t>& secret,
                           const void* data, size_t bytes) {
    if (secret.empty()) return {};
    HmacSha256 h(secret);
    if (!h.Ok()) return {};
    h.Update(data, bytes);
    auto mac = h.Finish();
    if (mac.empty()) return {};
    static const wchar_t* kHex = L"0123456789abcdef";
    std::wstring out;
    out.reserve(mac.size() * 2);
    for (uint8_t b : mac) {
        out += kHex[b >> 4];
        out += kHex[b & 0xf];
    }
    return out;
}

} // namespace minisys
