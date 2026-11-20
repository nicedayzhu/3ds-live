[CmdletBinding()]
param(
    [string]$Sdk = $env:DEVKITPRO,
    [string]$Bash
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$Sdk) { $Sdk = Join-Path $repo '../../toolchain/sdk/opt/devkitpro' }
if (!(Test-Path "$Sdk/devkitARM/bin/arm-none-eabi-gcc.exe")) { throw 'Specify the devkitPro SDK directory with -Sdk.' }
if (!$Bash) {
    $Bash = @("$Sdk/msys2/usr/bin/bash.exe", (Get-Command bash.exe -ErrorAction SilentlyContinue).Source) |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (!$Bash -or !(Test-Path $Bash)) { throw 'Specify the MSYS2 bash executable with -Bash.' }
$env:THREEDS_LIVE_SDK = (Resolve-Path -LiteralPath $Sdk).Path
$env:THREEDS_LIVE_SOURCE = $repo
& $Bash -lc 'set -eu
export DEVKITPRO="$(cygpath -u "$THREEDS_LIVE_SDK")"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:/usr/bin:$PATH"
cd "$(cygpath -u "$THREEDS_LIVE_SOURCE")"
make -j2 handheld'
if ($LASTEXITCODE) { throw "Handheld build failed ($LASTEXITCODE)." }
