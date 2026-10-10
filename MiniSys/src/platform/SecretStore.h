#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace minisys {

// review-05 T-A1/T-A2 fix (v2.14): the trust root for history-record
// signatures and index-cache MACs. A 32-byte random secret stored under
// HKLM\SOFTWARE\MiniSys with a security descriptor granting read access to
// ADMINISTRATORS and SYSTEM only — a standard user can neither read nor
// replace it, so records/caches they forge cannot carry a valid signature.
//
// Returns an empty vector when the secret is unavailable (e.g. unelevated
// test processes) — callers then treat data as unsigned/legacy.
std::vector<uint8_t> GetOrCreateMachineSecret();

// Streaming HMAC-SHA256 (BCrypt-native, secret as the key). Not copyable.
class HmacSha256 {
public:
    explicit HmacSha256(const std::vector<uint8_t>& secret);
    ~HmacSha256();
    HmacSha256(const HmacSha256&) = delete;
    HmacSha256& operator=(const HmacSha256&) = delete;

    bool Ok() const { return hash_ != nullptr; }
    void Update(const void* data, size_t bytes);
    std::vector<uint8_t> Finish();   // 32 bytes (handle becomes invalid)

private:
    void* hash_ = nullptr;              // BCRYPT_HASH_HANDLE
    unsigned char* objBuf_ = nullptr;   // hash object storage (BCrypt contract)
    bool finished_ = false;
};

// One-shot convenience — hex-encoded lowercase; empty when secret is empty.
std::wstring HmacSha256Hex(const std::vector<uint8_t>& secret,
                           const void* data, size_t bytes);
inline std::wstring HmacSha256Hex(const std::vector<uint8_t>& secret,
                                  const std::wstring& text) {
    return HmacSha256Hex(secret, text.c_str(), text.size() * sizeof(wchar_t));
}

} // namespace minisys
