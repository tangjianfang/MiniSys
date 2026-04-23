#pragma once
#include <filesystem>
#include <string>

namespace minisys {

// Create a directory junction at `linkPath` pointing to `targetDir`.
// `linkPath` must not exist; `targetDir` must exist and be on a fixed volume.
// Returns true on success; on failure errOut is populated.
bool CreateDirectoryJunction(const std::filesystem::path& linkPath,
                             const std::filesystem::path& targetDir,
                             std::wstring& errOut);

// Create a directory symbolic link at `linkPath` pointing to `targetDir`.
// Requires SeCreateSymbolicLinkPrivilege (admin OK) or Developer Mode.
bool CreateDirectorySymlink(const std::filesystem::path& linkPath,
                            const std::filesystem::path& targetDir,
                            std::wstring& errOut);

// Remove a directory junction or symlink (does NOT delete the target).
bool RemoveDirectoryReparsePoint(const std::filesystem::path& linkPath,
                                 std::wstring& errOut);

// If `p` is a junction or symlink, write its target into `targetOut` and return true.
bool ReadReparseTarget(const std::filesystem::path& p, std::wstring& targetOut);

} // namespace minisys
