@echo off
setlocal
set CL_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe
set MSVC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231
set SRC=C:\tjf\github\MiniSys\MiniSys\src
set KITS=C:\Program Files (x86)\Windows Kits\10
set KITINC=%KITS%\Include\10.0.26100.0
set KITLIB=%KITS%\Lib\10.0.26100.0\um\x64
cd /d C:\tjf\github\MiniSys\.tmp-review2\bench
"%CL_EXE%" /nologo /std:c++17 /EHsc /O2 /DNDEBUG /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 /I"%MSVC%\include" /I"%KITINC%\um" /I"%KITINC%\shared" /I"%KITINC%\ucrt" /I%SRC% bench_link_only.cpp ^
 %SRC%\ui\Presenters.cpp %SRC%\ui\Controls.cpp %SRC%\ui\Layout.cpp %SRC%\ui\Icons.cpp ^
 %SRC%\core\SessionService.cpp %SRC%\core\VolumeIndex.cpp %SRC%\core\OperationLog.cpp ^
 %SRC%\core\PlanBuilder.cpp %SRC%\core\GuardRails.cpp %SRC%\core\JunkRules.cpp ^
 %SRC%\core\JunkScanner.cpp %SRC%\core\LargeFileScanner.cpp %SRC%\core\AppScanner.cpp ^
 %SRC%\core\FolderTreeScanner.cpp %SRC%\core\QuarantineOp.cpp %SRC%\core\DeleteOp.cpp ^
 %SRC%\core\DelegateOp.cpp %SRC%\core\MoveJunctionOp.cpp %SRC%\core\Settings.cpp ^
 %SRC%\util\Json.cpp %SRC%\util\StringUtils.cpp %SRC%\util\Logger.cpp ^
 %SRC%\util\PathUtils.cpp %SRC%\util\FastWalk.cpp %SRC%\util\Win32Error.cpp ^
 %SRC%\platform\CrashDump.cpp %SRC%\platform\Hash.cpp %SRC%\platform\Junction.cpp ^
 %SRC%\platform\Privilege.cpp %SRC%\platform\RestartManager.cpp %SRC%\platform\SystemRestore.cpp ^
 /link /LIBPATH:"%MSVC%\lib\x64" /LIBPATH:"%KITLIB%" /LIBPATH:"%KITS%\Lib\10.0.26100.0\ucrt\x64" ^
 comctl32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib uuid.lib bcrypt.lib rstrtmgr.lib srclient.lib user32.lib gdi32.lib dbghelp.lib /OUT:linkonly.exe
endlocal
