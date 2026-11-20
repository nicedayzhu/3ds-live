[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo 'dist'
$windows = Join-Path $dist 'Windows'
$version = (Get-Content -LiteralPath "$repo/VERSION" -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid 3DS Live bundle VERSION.' }
$archive = Join-Path $dist "3DS-Live-$version-Windows-x64-New3DS.zip"
$files = [ordered]@{
    '使用指南.md' = "$repo/INSTALL.md"
    'licenses/JPEGENC-LICENSE' = "$repo/third_party/JPEGENC/LICENSE"
    'licenses/NotoSansSC-OFL.txt' = "$repo/app/assets/NotoSansSC-OFL.txt"
}
# Keep relative links usable in the extracted README, guides and measurement reports.
foreach ($relative in @(
    'README.md', 'INSTALL.md', 'LICENSE'
    'docs/DEVELOPMENT.md', 'docs/CREDITS.md'
    'docs/images/launcher-states.png', 'docs/images/windows-player.png', 'docs/images/mk7-race.png'
    'docs/images/home-homebrew.png', 'docs/images/home-mario-maker.png', 'docs/images/home-mario-kart.png'
    'docs/images/platforms.svg', 'live/qt/assets/3ds-live.png'
    'live/qt/README.md', 'live/docs/PROTOCOL.md'
    'live/docs/HARDWARE-1.2.8.md', 'live/docs/hardware-1.2.8-mk7.png'
    'live/docs/SOURCE-QUALITY.md', 'live/docs/native-handheld-080.png', 'live/docs/native-jpeg-budget.csv'
    'live/docs/SUPER-RESOLUTION.md', 'live/docs/sr-handheld-crops.png', 'live/docs/sr-panel-scores.csv'
    'live/docs/sr-quality-sweep.csv'
    'codec/include/live_pair.h', 'codec/include/live_audio_transport.h', 'sysmod/source/live_stream.c', 'live/qt/pipeline.cpp'
    'app/assets/NotoSansSC-OFL.txt', 'third_party/JPEGENC/LICENSE'
    'live/qt/licenses/NOTICE.txt', 'live/qt/licenses/LGPL-3.0-only.txt', 'live/qt/licenses/GPL-3.0-only.txt'
    'third_party/PCEnhance/README.md', 'third_party/PCEnhance/FBCNN-LICENSE', 'third_party/PCEnhance/KAIR-LICENSE'
    'third_party/PCEnhance/RealESRGAN-LICENSE', 'third_party/PCEnhance/RIFE-LICENSE'
    'third_party/SPAN/README.md', 'third_party/SPAN/CC-BY-4.0.txt', 'third_party/SPAN/SPAN-Apache-2.0.txt'
)) {
    $files[$relative] = Join-Path $repo $relative
}
$documentation = @($files.Keys)
foreach ($name in '3DS-Live.exe','runtime/3ds-live-client.dll') {
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $windows $name))
    if ($info.ProductVersion -ne $version) { throw "Rebuild $name for release $version." }
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
$smdhOffset = [BitConverter]::ToUInt32($launcher,0x20)
$smdhSize = [BitConverter]::ToUInt32($launcher,0x24)
if ($smdhSize -lt 0x188 -or [long]$smdhOffset + $smdhSize -gt $launcher.Length -or
    [Text.Encoding]::ASCII.GetString($launcher,$smdhOffset,4) -ne 'SMDH') { throw 'Missing 3DSX icon metadata.' }
$description = [Text.Encoding]::Unicode.GetString($launcher,$smdhOffset + 0x88,0x100).TrimEnd([char]0)
if (!$description.EndsWith("(v$version)",[StringComparison]::Ordinal)) { throw "Rebuild the 3DSX for release $version." }
$cia = [IO.File]::ReadAllBytes("$dist/3ds-live.cia")
if ($cia.Length -lt 0x2020 -or [BitConverter]::ToUInt32($cia,0) -ne 0x2020) { throw 'Unexpected CIA header.' }
# Latin-1 maps each byte to one character, so ordinal search checks the exact module contents.
$moduleBytes = [Text.Encoding]::Latin1.GetString($module)
if (![Text.Encoding]::Latin1.GetString($launcher,$romfsOffset,$launcher.Length - $romfsOffset).Contains($moduleBytes,[StringComparison]::Ordinal)) {
    throw 'The standalone 3DSX does not embed the current module in its RomFS.'
}
$ciaBytes = [Text.Encoding]::Latin1.GetString($cia)
if (!$ciaBytes.Contains($moduleBytes,[StringComparison]::Ordinal)) {
    throw 'The standalone CIA does not embed the current module.'
}
$smdhBytes = [Text.Encoding]::Latin1.GetString($launcher,$smdhOffset,$smdhSize)
if (!$ciaBytes.Contains($smdhBytes,[StringComparison]::Ordinal)) { throw 'The CIA and 3DSX icon metadata differ.' }
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
    foreach ($relative in $documentation) {
        $target = Join-Path $dist $relative
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        Copy-Item -LiteralPath $files[$relative] -Destination $target -Force
    }
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
