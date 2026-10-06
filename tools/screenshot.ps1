# MiniSys 自动视觉评审工具 (v2.13b)
# 被动截取正在运行的 MiniSys 窗口:PrintWindow 后台渲染,不抢焦点、不发送任何输入,
# 对提权实例安全。评审循环用:开着应用 → 跑本脚本(可轮询)→ 把 PNG 交给视觉评审。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\screenshot.ps1            # 单张 → minisys-shot.png
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\screenshot.ps1 -Loop 10   # 每 10 秒一张,时间戳命名
#   参数 -OutDir shots 控制输出目录
param(
    [int]$Loop = 0,                # 0 = 单张;>0 = 轮询间隔秒数(Ctrl+C 停止)
    [string]$OutDir = "."
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class WinCap {
    [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr hdc);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

function Capture([string]$path) {
    $h = [WinCap]::FindWindowW("MiniSysMainWnd", $null)
    if ($h -eq [IntPtr]::Zero) { Write-Host "MiniSys 未运行(找不到 MiniSysMainWnd)"; return $false }
    $r = New-Object WinCap+RECT
    [WinCap]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { Write-Host "窗口尺寸无效(最小化?)"; return $false }
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    # PW_RENDERFULLCONTENT (2) 捕获 DWM 合成内容,后台窗口也完整
    [WinCap]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "已保存 $path ($w x $ht)"
    return $true
}

if ($Loop -gt 0) {
    Write-Host "轮询模式:每 $Loop 秒截一张,Ctrl+C 停止…"
    while ($true) {
        $ts = Get-Date -Format "yyyyMMdd-HHmmss"
        $null = $(Capture (Join-Path $OutDir "minisys-$ts.png"))
        Start-Sleep -Seconds $Loop
    }
} else {
    $null = $(Capture (Join-Path $OutDir "minisys-shot.png"))
}
