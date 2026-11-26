[CmdletBinding()]
param(
    [string]$ClangFormat = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-format.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$roots = @('sysmod/source', 'sysmod/include', 'app/source')
$files = foreach ($root in $roots) {
    $path = Join-Path $repo $root
    if (Test-Path $path) {
        Get-ChildItem $path -File -Include '*.c', '*.h' -Recurse
    }
}
if (!$files) { throw 'No 3DS C sources found.' }
$failed = @()
foreach ($file in $files) {
    & $ClangFormat --dry-run --Werror --style=file $file.FullName 2>&1 | ForEach-Object {
        $failed += "$($file.FullName): $_"
    }
    if ($LASTEXITCODE -ne 0 -and !$failed) {
        $failed += "$($file.FullName): clang-format reported a difference"
    }
}
if ($failed) {
    $failed | Select-Object -First 40
    throw "Kernel style check failed for $($failed.Count) file(s). Run clang-format --style=file -i on them."
}
Write-Host "Kernel style check passed for $($files.Count) file(s)."
