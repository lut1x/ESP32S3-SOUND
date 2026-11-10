$ErrorActionPreference = "Stop"
$projectDir = Join-Path $PSScriptRoot "firmware"
$pioPython = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $pioPython)) {
    throw "PlatformIO Python not found: $pioPython. Install PlatformIO first."
}
Push-Location $projectDir
try {
    & $pioPython -m platformio run -e soundboard-n16r8
    if ($LASTEXITCODE -ne 0) { throw "Firmware build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
