[CmdletBinding()]
param(
    [string]$QtSdk = (Join-Path (Split-Path -Parent $PSScriptRoot) '.toolchains\qt\6.10.3\msvc2022_64'),
    [string]$Models = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Windows\runtime\models'),
    [string]$AccelerationDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Windows\runtime\acceleration'),
    [switch]$Zip,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$builder = Join-Path $repo 'live/qt/build.ps1'

foreach ($required in @('nvinfer_10.dll', 'cudart64_12.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $AccelerationDirectory $required))) {
        throw "TensorRT acceleration directory is missing ${required}: $AccelerationDirectory"
    }
}
if (!(Test-Path -LiteralPath (Join-Path $Models 'fbcnn.onnx'))) {
    throw "Models directory is missing fbcnn.onnx: $Models"
}
if (!(Test-Path -LiteralPath (Join-Path $QtSdk 'bin\Qt6Widgets.dll'))) {
    throw "Qt SDK is missing Qt6Widgets.dll: $QtSdk"
}

$arguments = @{
    QtSdk = $QtSdk
    Models = $Models
    TensorRT = $true
    AccelerationDirectory = $AccelerationDirectory
}
if ($Zip) { $arguments.Zip = $true }
if ($SkipTests) { $arguments.SkipTests = $true }

& $builder @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host 'Built the local TensorRT Windows package in dist\Windows.'
