#include "core/DelegateOp.h"

#include "core/OperationLog.h"
#include "util/Logger.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <mutex>
#include <sstream>
#include <thread>

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

    // REVIEW P1-4 (03-B9): a job object with KILL_ON_JOB_CLOSE guarantees
    // the child (and its own children, e.g. DismHost) die with us instead
    // of outliving the app as orphans.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool jobOk = job != nullptr &&
        SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));

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
        if (jobOk) CloseHandle(job);
        errOut = FormatW(L"无法启动 %s (Win32 %lu)", exe.c_str(), GetLastError());
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    if (jobOk) {
        AssignProcessToJobObject(job, pi.hProcess);
    }

    // REVIEW P1-4 (03-B9): the child is drained on a reader THREAD and the
    // process waited with a bounded poll — an interactive child (cleanmgr)
    // or a grandchild holding the pipe write end can no longer wedge the
    // worker forever; the job object tears everything down at the timeout.
    std::wstring output;
    std::mutex outMu;
    HANDLE readDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread reader([outRead, &output, &outMu, readDone]() {
        std::wstring local;
        char pipeBuf[4096];
        DWORD n = 0;
        for (;;) {
            if (!ReadFile(outRead, pipeBuf, sizeof(pipeBuf), &n, nullptr) || n == 0) break;
            local += Utf8ToWide(std::string(pipeBuf, n));
            if (local.size() > 64 * 1024) {
                local = local.substr(local.size() - 32 * 1024);   // keep the tail
            }
        }
        {
            std::lock_guard<std::mutex> g(outMu);
            output = std::move(local);
        }
        SetEvent(readDone);
    });

    // DISM component cleanup can legitimately take tens of minutes — the
    // bound only catches genuinely hung commands.
    constexpr DWORD kTimeoutMs = 30ull * 60ull * 1000ull;
    DWORD waited = 0;
    bool exited = false;
    while (waited < kTimeoutMs) {
        if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) { exited = true; break; }
        waited += 500;
    }
    if (!exited) {
        if (jobOk) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
    }
    // Give the reader a moment to see EOF after (forced) exit, then detach
    // if a grandchild still holds the write end — the job kill closes it.
    if (WaitForSingleObject(readDone, 3000) == WAIT_OBJECT_0) {
        reader.join();
    } else {
        CancelIoEx(outRead, nullptr);
        if (WaitForSingleObject(readDone, 2000) == WAIT_OBJECT_0) {
            reader.join();
        } else {
            reader.detach();   // thread exits soon after the handle closes
        }
    }
    CloseHandle(outRead);
    CloseHandle(readDone);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (jobOk) CloseHandle(job);

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
