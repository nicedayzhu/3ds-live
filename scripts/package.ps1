[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo 'dist'
$windows = Join-Path $dist 'Windows'
# Handheld releases can ship with the existing Windows player.
$version = (Get-Content -LiteralPath "$repo/VERSION" -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid 3DS Live bundle VERSION.' }
$archive = Join-Path $dist "3DS-Live-$version-Windows-x64-New3DS.zip"
$files = [ordered]@{
    '使用指南.md' = "$repo/INSTALL.md"
    'LICENSE' = "$repo/LICENSE"
    'licenses/JPEGENC-LICENSE' = "$repo/third_party/JPEGENC/LICENSE"
    'licenses/NotoSansSC-OFL.txt' = "$repo/app/assets/NotoSansSC-OFL.txt"
}
foreach ($entry in $files.GetEnumerator()) {
    $target = Join-Path $dist $entry.Key
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    Copy-Item -LiteralPath $entry.Value -Destination $target -Force
}
$files['Windows/files.sha256'] = "$windows/files.sha256"
$files['SD/3ds/3ds-live/3ds-live.3dsx'] = "$dist/3ds-live.3dsx"
$files['SD/3ds/3ds-live/live.ini.example'] = "$dist/SD/3ds/3ds-live/live.ini.example"
$files['3ds-live.cia'] = "$dist/3ds-live.cia"
$module = [IO.File]::ReadAllBytes("$dist/SD/luma/sysmodules/0004013000F1D402.cxi")
if ($module.Length -lt 0x400 -or [Text.Encoding]::ASCII.GetString($module,0x100,4) -ne 'NCCH' -or
    [BitConverter]::ToUInt64($module,0x118) -ne 0x0004013000F1D402L) { throw 'Unexpected handheld module.' }
$launcher = [IO.File]::ReadAllBytes("$dist/3ds-live.3dsx")
if ($launcher.Length -lt 0x2c -or [Text.Encoding]::ASCII.GetString($launcher,0,4) -ne '3DSX' -or
    [BitConverter]::ToUInt16($launcher,4) -lt 0x2c) { throw 'Unexpected 3DSX header.' }
$romfsOffset = [BitConverter]::ToUInt32($launcher,0x28)
if ($romfsOffset -lt 0x2c -or $romfsOffset -ge $launcher.Length) { throw 'Missing 3DSX RomFS.' }
$cia = [IO.File]::ReadAllBytes("$dist/3ds-live.cia")
if ($cia.Length -lt 0x2020 -or [BitConverter]::ToUInt32($cia,0) -ne 0x2020) { throw 'Unexpected CIA header.' }
# Latin-1 maps each byte to one character, so ordinal search checks the exact module contents.
$moduleBytes = [Text.Encoding]::Latin1.GetString($module)
if (![Text.Encoding]::Latin1.GetString($launcher,$romfsOffset,$launcher.Length - $romfsOffset).Contains($moduleBytes,[StringComparison]::Ordinal)) {
    throw 'The standalone 3DSX does not embed the current module in its RomFS.'
}
if (![Text.Encoding]::Latin1.GetString($cia).Contains($moduleBytes,[StringComparison]::Ordinal)) {
    throw 'The standalone CIA does not embed the current module.'
}
$root = [IO.Path]::GetFullPath($windows) + [IO.Path]::DirectorySeparatorChar
foreach ($line in Get-Content -LiteralPath "$windows/files.sha256") {
    if ($line -notmatch '^([a-f0-9]{64})  (.+)$') { throw 'Invalid Windows files.sha256.' }
    $expected, $name = $Matches[1], $Matches[2]
    $path = [IO.Path]::GetFullPath((Join-Path $windows $name))
    if (!$path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or
        $files.Contains("Windows/$name") -or
        (Get-FileHash -LiteralPath $path).Hash -ne $expected) { throw "Unexpected Windows file $name." }
    $files["Windows/$name"] = $path
}
foreach ($name in '3DS-Live.exe','runtime/3ds-live-client.dll','runtime/platforms/qwindows.dll','runtime/models/models.sha256') {
    if (!$files.Contains("Windows/$name")) { throw "Missing Windows file $name." }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temp = "$archive.$([Guid]::NewGuid().ToString('N')).tmp"
try {
    $zip = [IO.Compression.ZipFile]::Open($temp,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $files.GetEnumerator()) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$entry.Value,$entry.Key,[IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $zip.Dispose() }
    $zip = [IO.Compression.ZipFile]::OpenRead($temp)
    try {
        foreach ($entry in $zip.Entries) {
            $stream = $entry.Open()
            try { $digest = (Get-FileHash -InputStream $stream).Hash } finally { $stream.Dispose() }
            if ($digest -ne (Get-FileHash -LiteralPath $files[$entry.FullName]).Hash) {
                throw "Archive readback failed for $($entry.FullName)."
            }
        }
    } finally { $zip.Dispose() }
    [IO.File]::Move($temp,$archive,$true)
    $sha = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$archive.sha256","$sha  $([IO.Path]::GetFileName($archive))`n",[Text.Encoding]::ASCII)
    Write-Output $archive
} finally {
    if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
}
