[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw 'Version must be MAJOR.MINOR.PATCH.'
}

$repo = Split-Path -Parent $PSScriptRoot
$pubspecPath = Join-Path $repo 'android/pubspec.yaml'
$versionPath = Join-Path $repo 'android/lib/version.dart'
$pubspec = Get-Content -LiteralPath $pubspecPath -Raw
$current = [regex]::Match($pubspec, '(?m)^version:\s*\d+\.\d+\.\d+\+(\d+)\s*$')
if (!$current.Success) {
    throw 'Could not find the pubspec version line.'
}
$build = [int]$current.Groups[1].Value + 1

Set-Content -LiteralPath (Join-Path $repo 'VERSION') -Value $Version -NoNewline
$pubspec = $pubspec -replace '(?m)^version:\s*\d+\.\d+\.\d+\+\d+\s*$', "version: $Version+$build"
Set-Content -LiteralPath $pubspecPath -Value $pubspec -NoNewline

$dart = Get-Content -LiteralPath $versionPath -Raw
$dart = $dart -replace "defaultValue: '\d+\.\d+\.\d+'", "defaultValue: '$Version'"
Set-Content -LiteralPath $versionPath -Value $dart -NoNewline

Write-Host "Bumped to $Version (Android build $build)."
Write-Host 'Next: commit, tag v' + $Version + ', and push the tag.'
