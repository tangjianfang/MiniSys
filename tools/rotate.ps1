# MiniSys 版本轮换(用户指令:每次修改完成后自动关闭进程,启动新构建)
# 用法: powershell -File tools\rotate.ps1 [-Probe pdb] [-Elevated]
# 行为: 启动 build\Latest\MiniSys.exe -takeover;新实例优雅关闭旧实例
#       (保存设置/等待 worker)后接管。v2.17 起应用默认 asInvoker——
#       不再弹 UAC,只有需要管理员的功能才在应用内自行请求提权;
#       -Elevated 可选恢复旧的提权启动(如需测试管理员路径)。
param([string]$Probe = "", [switch]$Elevated)

$exe = Join-Path $PSScriptRoot "..\build\Latest\MiniSys.exe"
if (-not (Test-Path $exe)) { Write-Host "build\Latest\MiniSys.exe 不存在——先构建"; exit 1 }

$argv = "-takeover"
if ($Probe -ne "") { $argv = "-takeover -probe $Probe" }

Write-Host "轮换到 $((Get-Item $exe).VersionInfo.FileVersion) ..."
if ($Elevated) {
    Start-Process -FilePath $exe -ArgumentList $argv -Verb RunAs
} else {
    Start-Process -FilePath $exe -ArgumentList $argv
}
Write-Host "已启动(新实例会自动关闭旧实例)。"
