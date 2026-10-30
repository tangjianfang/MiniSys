<#
.SYNOPSIS
    Layout smoke test of MiniSys: starts it with -selftest, which walks every tab at the default and the
    minimum window size, audits the visible controls (bounds / overlap / covered / caption fit / content),
    writes %TEMP%\minisys-selftest.txt and exits. Passes when the program ends by itself with exit code 0.

.NOTES
    Exit codes of MiniSys: 0 clean, 5 layout problem(s); anything else is a crash code.
    It does NOT judge how the window looks - spacing, colour and alignment still need a human.
    Runs next to a live MiniSys (own mutex), never writes settings.json, never builds the search index.
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\MiniSys.exe'),
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8     # the report holds Chinese tab names
if (-not (Test-Path $Exe)) { throw "not found: $Exe" }

$report = Join-Path $env:TEMP 'minisys-selftest.txt'
Remove-Item $report -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -ArgumentList '-selftest' -PassThru
$null = $p.Handle                                   # keeps the exit code readable after exit

if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    Write-Host "FAILED: still running after $TimeoutSeconds s (a dialog may be blocking)" -ForegroundColor Red
    $p.Kill()
    exit 1
}
$code = $p.ExitCode
Get-Content $report -Encoding UTF8 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
if ($code -ne 0) {
    $meaning = switch ($code) { 5 { 'layout problem(s), see PROBLEM lines above' } default { 'crash or abnormal exit' } }
    Write-Host ("FAILED: exit code {0} (0x{1:X8}): {2}" -f $code, $code, $meaning) -ForegroundColor Red
    exit 1
}
Write-Host 'ok: every tab was audited at default and minimum size; the program closed itself with exit code 0.'
Write-Host '    This does NOT judge how the window looks.'
exit 0
