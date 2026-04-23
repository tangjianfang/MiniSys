#include "platform/Junction.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <winioctl.h>
#include <vector>

namespace minisys {

namespace {

// REPARSE_DATA_BUFFER for mount-point (junction) reparse tags. The Windows SDK
// defines this structure in ntifs.h, which is part of the WDK. Re-declare here.
typedef struct {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG  Flags;
            WCHAR  PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR  PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR  DataBuffer[1];
        } GenericReparseBuffer;
    };
} MS_REPARSE_DATA_BUFFER;

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif
#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 42, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_DELETE_REPARSE_POINT
#define FSCTL_DELETE_REPARSE_POINT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 43, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif

std::wstring LastErr(const wchar_t* prefix) {
    DWORD e = GetLastError();
    return std::wstring(prefix) + L" (Win32 " + std::to_wstring(e) + L")";
}

} // namespace

bool CreateDirectoryJunction(const std::filesystem::path& linkPath,
                             const std::filesystem::path& targetDir,
                             std::wstring& errOut) {
    auto link = LongPath(linkPath);
    auto tgt  = std::filesystem::absolute(targetDir).wstring();
    // Substitute name needs the NT-style "\??\C:\..." prefix; print name is human-readable.
    std::wstring sub = L"\\??\\" + tgt;
    std::wstring print = tgt;

    if (!CreateDirectoryW(link.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        errOut = LastErr(L"CreateDirectory failed for junction link");
        return false;
    }

    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errOut = LastErr(L"Open junction handle failed");
        return false;
    }

    USHORT subBytes = static_cast<USHORT>(sub.size() * sizeof(wchar_t));
    USHORT printBytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    USHORT pathBytes  = subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    USHORT headerSize = static_cast<USHORT>(
        offsetof(MS_REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer));
    USHORT total = headerSize + pathBytes;

    std::vector<UCHAR> buf(total, 0);
    auto* rdb = reinterpret_cast<MS_REPARSE_DATA_BUFFER*>(buf.data());
    rdb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rdb->ReparseDataLength = static_cast<USHORT>(pathBytes + 8); // path bytes + four USHORTs
    rdb->MountPointReparseBuffer.SubstituteNameOffset = 0;
    rdb->MountPointReparseBuffer.SubstituteNameLength = subBytes;
    rdb->MountPointReparseBuffer.PrintNameOffset = subBytes + sizeof(wchar_t);
    rdb->MountPointReparseBuffer.PrintNameLength = printBytes;
    memcpy(rdb->MountPointReparseBuffer.PathBuffer, sub.c_str(), subBytes);
    memcpy(reinterpret_cast<UCHAR*>(rdb->MountPointReparseBuffer.PathBuffer) +
        subBytes + sizeof(wchar_t), print.c_str(), printBytes);

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), total,
        nullptr, 0, &ret, nullptr);
    if (!ok) {
        errOut = LastErr(L"DeviceIoControl FSCTL_SET_REPARSE_POINT failed");
        CloseHandle(h);
        RemoveDirectoryW(link.c_str());
        return false;
    }
    CloseHandle(h);
    return true;
}

bool CreateDirectorySymlink(const std::filesystem::path& linkPath,
                            const std::filesystem::path& targetDir,
                            std::wstring& errOut) {
    DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY |
                  SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    auto tgt = std::filesystem::absolute(targetDir).wstring();
    if (!CreateSymbolicLinkW(LongPath(linkPath).c_str(), tgt.c_str(), flags)) {
        errOut = LastErr(L"CreateSymbolicLink failed");
        return false;
    }
    return true;
}

bool RemoveDirectoryReparsePoint(const std::filesystem::path& linkPath,
                                 std::wstring& errOut) {
    auto link = LongPath(linkPath);
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        // Build minimal mount-point reparse buffer to delete.
        MS_REPARSE_DATA_BUFFER rdb{};
        rdb.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
        rdb.ReparseDataLength = 0;
        DWORD ret = 0;
        DeviceIoControl(h, FSCTL_DELETE_REPARSE_POINT, &rdb,
            REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, nullptr, 0, &ret, nullptr);
        CloseHandle(h);
    }
    if (!RemoveDirectoryW(link.c_str())) {
        errOut = LastErr(L"RemoveDirectory failed for reparse link");
        return false;
    }
    return true;
}

bool ReadReparseTarget(const std::filesystem::path& p, std::wstring& targetOut) {
    auto path = LongPath(p);
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::vector<UCHAR> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    auto* rdb = reinterpret_cast<MS_REPARSE_DATA_BUFFER*>(buf.data());
    if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        const wchar_t* base = rdb->MountPointReparseBuffer.PathBuffer +
            rdb->MountPointReparseBuffer.SubstituteNameOffset / sizeof(wchar_t);
        size_t len = rdb->MountPointReparseBuffer.SubstituteNameLength / sizeof(wchar_t);
        std::wstring s(base, len);
        if (s.rfind(L"\\??\\", 0) == 0) s.erase(0, 4);
        targetOut = s;
        return true;
    }
    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        const wchar_t* base = rdb->SymbolicLinkReparseBuffer.PathBuffer +
            rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(wchar_t);
        size_t len = rdb->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(wchar_t);
        std::wstring s(base, len);
        if (s.rfind(L"\\??\\", 0) == 0) s.erase(0, 4);
        targetOut = s;
        return true;
    }
    return false;
}

} // namespace minisys
