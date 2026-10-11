@echo off
setlocal

set SLN=%~dp0MiniSys.sln
set CONFIG=%1
set PLATFORM=%2

if "%CONFIG%"==""  set CONFIG=Release
if "%PLATFORM%"=="" set PLATFORM=x64

:: Locate MSBuild via vswhere
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% set VSWHERE="%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist %VSWHERE% (
    echo [ERROR] vswhere.exe not found. Please install Visual Studio 2019 or later.
    exit /b 1
)

for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i

if not defined MSBUILD (
    echo [ERROR] MSBuild.exe not found via vswhere.
    exit /b 1
)

echo.
echo ================================================================
echo  Building MiniSys  [Config: %CONFIG%  Platform: %PLATFORM%]
echo  MSBuild: %MSBUILD%
echo ================================================================
echo.

"%MSBUILD%" "%SLN%" ^
    /p:Configuration=%CONFIG% ^
    /p:Platform=%PLATFORM% ^
    /property:GenerateFullPaths=true ^
    /consoleloggerparameters:NoSummary ^
    /m

if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED] Build failed with error code %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)

echo.
echo [OK] Build succeeded.
echo Output: %~dp0build\%CONFIG%\MiniSys.exe
echo.
endlocal

REM v2.16: canonical deployment — build\Latest is THE exe users run.
REM Rebuilds land there automatically so "which exe is current" has one answer.
if exist "%~dp0build\%CONFIG%\MiniSys.exe" (
    if not exist "%~dp0build\Latest" mkdir "%~dp0build\Latest"
    copy /Y "%~dp0build\%CONFIG%\MiniSys.exe" "%~dp0build\Latest\MiniSys.exe" >nul
    if exist "%~dp0build\%CONFIG%\rules.json" copy /Y "%~dp0build\%CONFIG%\rules.json" "%~dp0build\Latest\rules.json" >nul
    echo [Latest] deployed to build\Latest\MiniSys.exe
)
