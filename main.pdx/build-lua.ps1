$ErrorActionPreference = "Stop"

$sdk = if ($env:PLAYDATE_SDK_PATH) {
    $env:PLAYDATE_SDK_PATH
} else {
    "V:\PlaydateSDK"
}

$pdc = Join-Path $sdk "bin\pdc.exe"
if (-not (Test-Path $pdc)) {
    throw "Playdate compiler not found at $pdc"
}

$root = $PSScriptRoot
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "pdchess-lua-source"
$output = Join-Path $root "build\pdchess_lua.pdx"
$outputParent = Split-Path -Parent $output

if (Test-Path $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root "main.lua") -Destination $stage
Copy-Item -LiteralPath (Join-Path $root "mcumax.lua") -Destination $stage
Copy-Item -LiteralPath (Join-Path $root "pdxinfo") -Destination $stage

& $pdc $stage $output
if ($LASTEXITCODE -ne 0) {
    throw "pdc failed with exit code $LASTEXITCODE"
}

Write-Output "Built $output"
