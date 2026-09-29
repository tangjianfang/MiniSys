#pragma once
#include <filesystem>
#include <string>

namespace minisys {

// Compute SHA-256 of file content. Returns hex string (lowercase).
// On failure returns empty string.
std::wstring Sha256OfFile(const std::filesystem::path& p);

// Compute SHA-256 of the first 'maxBytes' bytes of the file (for fast dedup pre-screening).
// Returns empty string on failure.
std::wstring Sha256OfFileHead(const std::filesystem::path& p, unsigned long long maxBytes);

// Compute SHA-256 of an in-memory buffer. Returns hex string (lowercase).
std::wstring Sha256OfBuffer(const void* data, size_t bytes);

// 64-bit digest (first 8 bytes of SHA-256, big-endian) — used for planHash.
uint64_t Hash64OfBuffer(const void* data, size_t bytes);

} // namespace minisys
