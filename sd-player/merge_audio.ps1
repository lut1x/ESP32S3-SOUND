param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Left,
    [Parameter(Mandatory = $true, Position = 1)][string]$Right,
    [Parameter(Position = 2)][string]$Output = "combined.wav",
    [switch]$LoopShorter,
    [string]$FFmpeg
)

$ErrorActionPreference = "Stop"
$mergePython = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $mergePython)) {
    $mergeCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $mergeCommand -or $mergeCommand.Source -like "*\WindowsApps\*") {
        throw "Python not found. Install Python 3.10+ or PlatformIO."
    }
    $mergePython = $mergeCommand.Source
}

$mergeArgs = @((Join-Path $PSScriptRoot "merge_audio.py"), $Left, $Right, $Output)
if ($LoopShorter) { $mergeArgs += "--loop-shorter" }
if ($FFmpeg) { $mergeArgs += @("--ffmpeg", $FFmpeg) }
& $mergePython @mergeArgs
if ($LASTEXITCODE -ne 0) { throw "Audio merge failed ($LASTEXITCODE)." }
