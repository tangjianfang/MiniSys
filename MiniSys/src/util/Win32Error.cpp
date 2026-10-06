#include "util/Win32Error.h"

#include "util/StringUtils.h"

namespace minisys {

std::wstring Win32ErrorText(unsigned long e) {
    switch (e) {
        case 5:   // ERROR_ACCESS_DENIED
            return L"文件或其子项正被其他程序占用（常见：node/npm、杀毒软件、"
                   L"搜索索引），也可能是权限不足。关闭相关程序后重试；"
                   L"或重启电脑后立即执行。";
        case 32:  // ERROR_SHARING_VIOLATION
            return L"文件正被其他程序使用。关闭正在使用它的程序后重试。";
        case 2:   // ERROR_FILE_NOT_FOUND
        case 3:   // ERROR_PATH_NOT_FOUND
            return L"路径已不存在（可能刚被其他工具处理）。重新扫描即可刷新列表。";
        case 17:  // ERROR_NOT_SAME_DEVICE
            return L"跨磁盘移动不受支持（隔离区必须与源文件在同一磁盘）。";
        case 87:  // ERROR_INVALID_PARAMETER
            return L"系统拒绝了该操作参数。请重试；若持续出现请重启后重试。";
        case 112: // ERROR_DISK_FULL
            return L"磁盘空间不足。请先释放目标磁盘空间。";
        case 183: // ERROR_ALREADY_EXISTS
            return L"目标位置已存在同名项目。";
        case 1314: // ERROR_PRIVILEGE_NOT_HELD
            return L"权限不足（需要管理员权限的操作系统）。";
        default:
            return FormatW(L"系统错误 %lu。可重试一次；若持续出现请重启后重试。", e);
    }
}

std::wstring FriendlyWin32Error(const std::wstring& what,
                                unsigned long e) {
    return FormatW(L"%s：%s（Win32 %lu）", what.c_str(),
                   Win32ErrorText(e).c_str(), e);
}

} // namespace minisys
