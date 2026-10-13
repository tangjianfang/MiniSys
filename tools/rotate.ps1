# MiniSys 版本轮换(用户指令:每次修改完成后自动关闭进程,启动新构建)
# 用法: powershell -File tools\rotate.ps1 [-Probe pdb]
# 行为: 以提权方式启动 build\Latest\MiniSys.exe -takeover(会弹一次 UAC);
#       新实例优雅关闭旧实例(保存设置/等待 worker)后接管。
param([string]$Probe = "")

$exe = Join-Path $PSScriptRoot "..\build\Latest\MiniSys.exe"
if (-not (Test-Path $exe)) { Write-Host "build\Latest\MiniSys.exe 不存在——先构建"; exit 1 }

$args = "-takeover"
if ($Probe -ne "") { $args = "-takeover -probe $Probe" }

Write-Host "轮换到 $((Get-Item $exe).VersionInfo.FileVersion) ..."
Start-Process -FilePath $exe -ArgumentList $args -Verb RunAs
Write-Host "已启动(新实例会自动关闭旧实例)。"
