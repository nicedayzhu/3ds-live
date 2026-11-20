[CmdletBinding()]
param(
    [string]$QtSdk = $env:QTDIR,
    [string]$Models,
    [string]$DependencyCache,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$AccelerationDirectory,
    [switch]$TensorRT,
    [switch]$Zip
)
$ErrorActionPreference = 'Stop'
$liveRoot = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $liveRoot
$cache = if ($DependencyCache) { [IO.Path]::GetFullPath($DependencyCache) } else { Join-Path $liveRoot 'build/native-deps' }
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $liveRoot 'build/native-cmake' }
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo 'dist/Windows' }
$runtime = Join-Path $out 'runtime'
$flatBuild = (Test-Path -LiteralPath "$out/3DS-Live.exe") -and (Test-Path -LiteralPath "$out/Qt6Core.dll") -and (Test-Path -LiteralPath "$out/platforms/qwindows.dll")
# Retain the meaning of source paths used by incremental builds after moving a flat bundle.
if ($Models -and !(Test-Path -LiteralPath "$Models/fbcnn.onnx") -and
    [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($Models)) -eq [IO.Path]::GetFullPath("$out/models")) { $Models = "$runtime/models" }
if ($AccelerationDirectory -and !(Test-Path -LiteralPath "$AccelerationDirectory/nvinfer_10.dll") -and
    [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($AccelerationDirectory)) -eq [IO.Path]::GetFullPath("$out/acceleration")) { $AccelerationDirectory = "$runtime/acceleration" }
if (!$QtSdk) {
    $qmake = Get-Command qmake6.exe -ErrorAction SilentlyContinue
    if ($qmake) { $QtSdk = (& $qmake.Source -query QT_INSTALL_PREFIX) }
}
if (!(Test-Path "$QtSdk/bin/Qt6Widgets.dll")) { throw 'Supply the Qt 6 MSVC x64 kit using -QtSdk' }
if (!$Models) {
    $Models = if (Test-Path "$PSScriptRoot/models/fbcnn.onnx") { "$PSScriptRoot/models" } else { "$liveRoot/build/native-models" }
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = "$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
if (!(Test-Path $cmake)) { $cmake = (Get-Command cmake.exe).Source }
$seven = @((Get-Command 7z.exe -ErrorAction SilentlyContinue).Source,"$env:ProgramFiles/7-Zip/7z.exe") |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
function Invoke-Checked([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program returned $LASTEXITCODE" }
}
function Install-Dependency($id,$target,$marker,$url,$hash,$filter) {
    if (Test-Path "$cache/$target/$marker") { return }
    if (!$seven) { throw '7-Zip is required to extract the native SDKs' }
    $archive = "$cache/$id.download"
    if (!(Test-Path $archive) -or (Get-FileHash $archive).Hash -ne $hash) {
        Invoke-Checked curl.exe @('-fL','--retry','3','-o',"$archive.part",$url)
        if ((Get-FileHash "$archive.part").Hash -ne $hash) { throw "$id checksum mismatch" }
        Move-Item -LiteralPath "$archive.part" -Destination $archive -Force
    }
    Invoke-Checked $seven (@('x','-y',"-o$cache/$target",$archive) + $filter)
}
$projectOutput = [IO.Path]::GetFullPath((Join-Path $repo 'dist/Windows'))
$directoryTemplate = Join-Path $repo '.git/info/3ds-live-desktop-output'
if ([IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($out)) -eq $projectOutput -and
    ![IO.Directory]::Exists($out) -and [IO.Directory]::Exists($directoryTemplate)) {
    # This checkout can retain its local NTFS desktop-release label across clean builds.
    # Move only a new, empty directory. The executable and DLLs are built into their final location.
    $parent = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($projectOutput))
    foreach ($dir in @($parent.FullName, $directoryTemplate, "$repo/.git/info", "$repo/.git")) {
        if ([IO.File]::GetAttributes($dir).HasFlag([IO.FileAttributes]::ReparsePoint)) {
            throw "Expected a local release directory: $dir"
        }
    }
    $name = '.new-windows-' + [guid]::NewGuid().ToString('N')
    $empty = [IO.Directory]::CreateDirectory((Join-Path $directoryTemplate $name))
    $candidate = Join-Path $parent.FullName $name
    try {
        [IO.Directory]::Move($empty.FullName, $candidate)
        # Inherit today's destination access rules, retaining only the local integrity label.
        & icacls.exe $candidate /reset /Q | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not inherit release directory access rules.' }
        try { [IO.Directory]::Move($candidate, $projectOutput) }
        catch [IO.IOException] { if (![IO.Directory]::Exists($projectOutput)) { throw } }
    } finally {
        foreach ($path in @($empty.FullName, $candidate)) {
            if ([IO.Directory]::Exists($path)) { [IO.Directory]::Delete($path, $false) }
        }
    }
}
New-Item -ItemType Directory -Force $cache,$runtime | Out-Null
$payload = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Install-RuntimeFile([string]$src, [string]$relative) {
    $dest = Join-Path $runtime $relative
    if ([IO.Path]::GetFullPath($src) -ne [IO.Path]::GetFullPath($dest)) {
        $existing = Get-Item -LiteralPath $dest -ErrorAction SilentlyContinue
        if (!$existing -or $existing.Length -ne (Get-Item -LiteralPath $src).Length -or
            (Get-FileHash -LiteralPath $src).Hash -ne (Get-FileHash -LiteralPath $dest).Hash) {
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
            Copy-Item -LiteralPath $src -Destination $dest -Force
        }
    } elseif (!(Test-Path -LiteralPath $src -PathType Leaf)) {
        throw "Missing runtime file: $src"
    }
    [void]$payload.Add($relative.Replace('\','/'))
}
function Install-RuntimeFiles([string[]]$paths, [string]$folder = '') {
    foreach ($file in Get-Item -Path $paths) {
        Install-RuntimeFile $file.FullName ($folder + $file.Name)
    }
}
Install-Dependency ort ort 'runtimes/win-x64/native/onnxruntime.dll' 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.22.1/microsoft.ml.onnxruntime.directml.1.22.1.nupkg' '210823F9932F81F222E2D05D04531D948A4D75DBF527DFB94195AD61371AB728' @()
Install-Dependency dml dml 'bin/x64-win/DirectML.dll' 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg' '4E7CB7DDCE8CF837A7A75DC029209B520CA0101470FCDF275C1F49736A3615B9' @()
Install-Dependency opencv cv 'opencv/build/x64/vc16/bin/opencv_world4120.dll' 'https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-windows.exe' 'B753B14D880B9BC8D89D6ACD3B665C040BAEC0211078435432FCAE117DB707AF' @('opencv/build/include/*','opencv/build/x64/vc16/lib/*','opencv/build/x64/vc16/bin/opencv_world4120.dll','opencv/LICENSE*')
Install-Dependency ffmpeg av 'ffmpeg-9.0.2-full_build-shared/lib/avcodec.lib' 'https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-full_build-shared.7z' '4D2060A8B34A940AA47D785142055BB92A63053781E55F2ACE4546EDD519A8F5' @()
$trt = if ($TensorRT -or $AccelerationDirectory) { 'ON' } else { 'OFF' }
Invoke-Checked $cmake @('-S',$PSScriptRoot,'-B',$build,'-A','x64',"-DCMAKE_PREFIX_PATH=$QtSdk","-DLIVE_DEPS=$cache","-DLIVE_TENSORRT=$trt",'-DLIVE_NATIVE_APP=ON','-DBUILD_TESTING=ON')
Invoke-Checked $cmake @('--build',$build,'--config','Release','--parallel','4')
Invoke-Checked $cmake @('--install',$build,'--config','Release','--prefix',$out)
[void]$payload.Add('3ds-live-client.dll')
$av = "$cache/av/ffmpeg-9.0.2-full_build-shared"
foreach ($name in 'Core','Gui','Widgets','Network') { Install-RuntimeFiles "$QtSdk/bin/Qt6$name.dll" }
Install-RuntimeFiles @("$QtSdk/plugins/platforms/qwindows.dll","$QtSdk/plugins/platforms/qoffscreen.dll") 'platforms/'
Install-RuntimeFiles "$QtSdk/plugins/styles/qmodernwindowsstyle.dll" 'styles/'
Install-RuntimeFiles "$QtSdk/plugins/imageformats/qjpeg.dll" 'imageformats/'
$crt = Get-ChildItem "$vs/VC/Redist/MSVC" -Directory | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem "$($_.FullName)/x64" -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } | Select-Object -First 1
if (!$crt) { throw 'MSVC x64 redistributable directory missing' }
Install-RuntimeFiles @("$($crt.FullName)/*.dll","$cache/ort/runtimes/win-x64/native/*.dll","$cache/dml/bin/x64-win/DirectML.dll","$cache/cv/opencv/build/x64/vc16/bin/opencv_world4120.dll")
foreach ($name in 'avcodec','avformat','avutil','swscale','swresample') { Install-RuntimeFiles "$av/bin/$name-*.dll" }
Install-RuntimeFiles @("$PSScriptRoot/licenses/*","$repo/LICENSE") 'licenses/'
$notices = Join-Path $repo 'third_party/PCEnhance'
foreach ($file in Get-ChildItem -LiteralPath $notices -File -Recurse) {
    Install-RuntimeFile $file.FullName ('licenses/PCEnhance/' + [IO.Path]::GetRelativePath($notices, $file.FullName))
}
$spanDirectory = Join-Path $repo 'third_party/SPAN'
$spanModels = 'span-fp16.onnx','span.onnx'
$spanNotices = 'README.md','CC-BY-4.0.txt','SPAN-Apache-2.0.txt'
foreach ($name in $spanNotices) {
    Install-RuntimeFile (Join-Path $spanDirectory $name) "licenses/SPAN/$name"
}
Install-RuntimeFile "$cache/ort/LICENSE" 'licenses/ONNX-Runtime.txt'
Install-RuntimeFile "$cache/ort/ThirdPartyNotices.txt" 'licenses/ONNX-Notices.txt'
Install-RuntimeFile "$cache/dml/LICENSE.txt" 'licenses/DirectML.txt'
Install-RuntimeFile "$cache/cv/opencv/LICENSE.txt" 'licenses/OpenCV.txt'
Install-RuntimeFile "$av/LICENSE" 'licenses/FFmpeg.txt'
Install-RuntimeFile "$av/README.txt" 'licenses/FFmpeg-build.txt'
foreach ($entry in Get-Content "$PSScriptRoot/models.sha256") {
    $hash,$file = $entry.Split(' ')
    $asset = "$Models/$file"
    if ($file -in $spanModels -and !(Test-Path -LiteralPath $asset)) {
        $asset = Join-Path $spanDirectory $file
    }
    if (!(Test-Path -LiteralPath $asset)) { throw "Supply the models folder from the native release with -Models. Missing $file" }
    if ((Get-FileHash -LiteralPath $asset).Hash -ne $hash) { throw "$file checksum mismatch" }
    Install-RuntimeFile $asset "models/$file"
}
Install-RuntimeFiles @("$liveRoot/assets/fbcnn-reference-*.png","$PSScriptRoot/models.sha256") 'models/'
Install-RuntimeFile "$PSScriptRoot/README.md" 'README.md'
$requiredAcceleration = 'nvinfer_10.dll','cudart64_12.dll','fbcnn-fp16-400-l3.plan','fbcnn-fp16-320-l3.plan','CUDA-License.txt','TensorRT-License.txt'
$rifePlans = foreach ($precision in 'fp32','fp16') { foreach ($width in 320,400) { "rife-$precision-$width.plan" } }
if ($AccelerationDirectory) {
    $accel = (Resolve-Path -LiteralPath $AccelerationDirectory).Path
    foreach ($file in $requiredAcceleration) {
        Install-RuntimeFile (Join-Path $accel $file) "acceleration/$file"
    }
    foreach ($plan in $rifePlans) {
        if (Test-Path -LiteralPath (Join-Path $accel $plan)) {
            Install-RuntimeFile (Join-Path $accel $plan) "acceleration/$plan"
        } elseif (Test-Path -LiteralPath "$runtime/acceleration/$plan") {
            Remove-Item -LiteralPath "$runtime/acceleration/$plan"
        }
    }
}
# Windows ships a different ONNX Runtime in System32, which precedes PATH during DLL resolution.
Copy-Item -LiteralPath "$runtime/onnxruntime.dll","$runtime/DirectML.dll" -Destination "$build/Release" -Force
$previousPath, $previousPlugins = $env:PATH, $env:QT_PLUGIN_PATH
$previousPlatform, $previousFonts = $env:QT_QPA_PLATFORM, $env:QT_QPA_FONTDIR
$previousTemp, $previousTmp = $env:TEMP, $env:TMP
try {
    $env:PATH = "$runtime;$QtSdk/bin;$env:PATH"
    $env:QT_PLUGIN_PATH = "$QtSdk/plugins"
    $env:QT_QPA_PLATFORM = 'offscreen'
    if (!$env:QT_QPA_FONTDIR) { $env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot 'Fonts' }
    $env:TEMP = $env:TMP = Join-Path $build 'test-temp'
    [void][IO.Directory]::CreateDirectory($env:TEMP)
    Invoke-Checked (Join-Path (Split-Path $cmake) 'ctest.exe') @('--test-dir',$build,'-C','Release','--output-on-failure')
} finally {
    $env:PATH, $env:QT_PLUGIN_PATH = $previousPath, $previousPlugins
    $env:QT_QPA_PLATFORM, $env:QT_QPA_FONTDIR = $previousPlatform, $previousFonts
    $env:TEMP, $env:TMP = $previousTemp, $previousTmp
}
if ($flatBuild) {
    # Clear only managed entries from a previous flat deployment. Preserve local edits
    # and old acceleration files; an unrelated recording, DLL or subfolder stays in place.
    $legacyNames = @($payload) + 'liveview.dll' + @($requiredAcceleration + $rifePlans | ForEach-Object { "acceleration/$_" })
    $root = [IO.Path]::TrimEndingDirectorySeparator((Get-Item -LiteralPath $out).FullName)
    $backup = Join-Path $runtime ('previous-flat-' + [Guid]::NewGuid().ToString('N'))
    foreach ($relative in $legacyNames | Select-Object -Unique) {
        $old = Join-Path $root $relative
        if (!(Test-Path -LiteralPath $old -PathType Leaf)) { continue }
        $item = Get-Item -LiteralPath $old -Force
        $parent = $item.Directory
        $linked = $item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)
        while (!$linked -and $parent.FullName -ne $root) {
            $linked = $parent.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)
            $parent = $parent.Parent
        }
        if ($linked) { continue }
        $current = Join-Path $runtime $relative
        if ($payload.Contains($relative) -and $item.Length -eq (Get-Item -LiteralPath $current).Length -and
            (Get-FileHash -LiteralPath $old).Hash -eq (Get-FileHash -LiteralPath $current).Hash) {
            Remove-Item -LiteralPath $old
        } else {
            $saved = Join-Path $backup $relative
            if (!$AccelerationDirectory -and $relative.StartsWith('acceleration/') -and !(Test-Path -LiteralPath $current)) { $saved = $current }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($saved)) | Out-Null
            Move-Item -LiteralPath $old -Destination $saved
        }
        $parent = $item.Directory
        while ($parent.FullName -ne $root -and $parent.GetFileSystemInfos().Count -eq 0) {
            $parent.Delete()
            $parent = $parent.Parent
        }
    }
}
$deployedFiles = @('3DS-Live.exe') + @($payload | Sort-Object | ForEach-Object { "runtime/$_" })
$checksums = foreach ($entry in $deployedFiles) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $out $entry)).Hash.ToLowerInvariant()
    "$hash  $entry"
}
[IO.File]::WriteAllLines((Join-Path $out 'files.sha256'), [string[]]$checksums)
if ($Zip) {
    $archive = Join-Path $liveRoot 'dist/3DS-Live-Windows-x64.zip'
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($archive)) | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream=[IO.File]::Open("$archive.partial",[IO.FileMode]::Create)
    $playerZip = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
    try {
        $prefix = (Get-Item -LiteralPath $out).Name + '/'
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($playerZip,"$out/3DS-Live.exe",$prefix + '3DS-Live.exe',[IO.Compression.CompressionLevel]::Fastest) | Out-Null
        foreach ($entry in ($payload | Sort-Object)) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($playerZip,(Join-Path $runtime $entry),$prefix + 'runtime/' + $entry,[IO.Compression.CompressionLevel]::Fastest) | Out-Null
        }
    } finally { $playerZip.Dispose(); $stream.Dispose() }
    Move-Item -LiteralPath "$archive.partial" -Destination $archive -Force
    $source = Join-Path $liveRoot 'dist/3DS-Live-Qt-Source.zip'
    $stream = [IO.File]::Open("$source.partial",[IO.FileMode]::Create)
    $zipFile = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
    function Add-SourceFile($path,$entry) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zipFile,$path,$entry,[IO.Compression.CompressionLevel]::Fastest) | Out-Null
    }
    try {
        foreach ($item in Get-ChildItem $PSScriptRoot -File) { Add-SourceFile $item.FullName "live/qt/$($item.Name)" }
        foreach ($folder in 'live/qt/licenses','live/qt/assets','third_party/PCEnhance','live/assets','codec/include') {
            foreach ($item in Get-ChildItem "$repo/$folder" -File -Recurse) {
                Add-SourceFile $item.FullName ([IO.Path]::GetRelativePath($repo,$item.FullName).Replace('\','/'))
            }
        }
        foreach ($file in 'LICENSE','VERSION','codec/source/live_codec.c','codec/source/live_audio.c','codec/host/live_audio_output.c') {
            Add-SourceFile "$repo/$file" $file
        }
        foreach ($name in $spanNotices + $spanModels) {
            Add-SourceFile (Join-Path $spanDirectory $name) "third_party/SPAN/$name"
        }
        foreach ($entry in $payload) {
            if ($entry -like 'models/*.onnx' -and [IO.Path]::GetFileName($entry) -notin $spanModels) {
                Add-SourceFile (Join-Path $runtime $entry) "live/qt/$entry"
            }
        }
    } finally { $zipFile.Dispose(); $stream.Dispose() }
    Move-Item -LiteralPath "$source.partial" -Destination $source -Force
}
Write-Output "$out/3DS-Live.exe"
