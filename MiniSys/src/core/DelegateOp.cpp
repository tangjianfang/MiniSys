#include "core/DelegateOp.h"

#include "core/OperationLog.h"
#include "util/Logger.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <sstream>

namespace minisys {

namespace {

std::wstring TrimSpace(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \r\n\t");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \r\n\t");
    return s.substr(b, e - b + 1);
}

} // namespace

DelegateOp::DelegateOp(std::wstring title, std::wstring commandLine)
    : title_(std::move(title)), command_(std::move(commandLine)) {
    rec_.id = OperationLog::NewId();
    rec_.type = OpType::Delegate;
    rec_.source = command_;
    rec_.isReversible = false;
    rec_.note = L"委派: " + title_;
}

bool DelegateOp::SplitCommandLine(const std::wstring& cmd,
                                  std::wstring& exeOut, std::wstring& argsOut) {
    if (cmd.empty()) return false;
    exeOut.clear();
    argsOut.clear();
    if (cmd[0] == L'"') {
        auto end = cmd.find(L'"', 1);
        if (end == std::wstring::npos) return false;
        exeOut = cmd.substr(1, end - 1);
        argsOut = TrimSpace(cmd.substr(end + 1));
    } else {
        auto sp = cmd.find(L' ');
        if (sp == std::wstring::npos) {
            exeOut = cmd;
        } else {
            exeOut = cmd.substr(0, sp);
            argsOut = TrimSpace(cmd.substr(sp + 1));
        }
    }
    return !exeOut.empty();
}

bool DelegateOp::Execute(std::wstring& errOut) {
    std::wstring exe, args;
    if (!SplitCommandLine(command_, exe, args)) {
        errOut = L"无效的命令行";
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        errOut = L"CreatePipe failed";
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + exe + L"\" " + args;   // CreateProcess wants a mutable buffer
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outWrite);   // child owns its end now
    if (!ok) {
        CloseHandle(outRead);
        errOut = FormatW(L"无法启动 %s (Win32 %lu)", exe.c_str(), GetLastError());
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    // Drain the pipe while the child runs so it never blocks on a full pipe.
    std::wstring output;
    char pipe[4096];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(outRead, pipe, sizeof(pipe), &n, nullptr) || n == 0) break;
        output += Utf8ToWide(std::string(pipe, n));
        if (output.size() > 64 * 1024) {
            output = output.substr(output.size() - 32 * 1024);   // keep the tail
        }
    }
    CloseHandle(outRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0) {
        // Keep the last few output lines as the failure summary.
        std::wstring tail = output.size() > 2000 ? output.substr(output.size() - 2000)
                                                 : output;
        errOut = FormatW(L"命令退出码 %lu: %s", exitCode, TrimSpace(tail).c_str());
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    rec_.status = OpStatus::Success;
    OperationLog::Instance().Append(rec_);
    return true;
}

bool DelegateOp::Undo(std::wstring& errOut) {
    errOut = L"委派操作不可自动撤销";
    return false;
}

} // namespace minisys
