[CmdletBinding()]
param(
    [string]$Bash = 'F:\Tools\msys64\usr\bin\bash.exe',
    [string]$QtSdk = (Join-Path $PSScriptRoot '..\.toolchains\qt\6.10.3\msvc2022_64'),
    [string]$Flutter = 'D:\Games\Old_Times\3ds\cia-bridge\.toolchains\flutter-sdk\flutter\bin\flutter.bat',
    [string]$JavaHome = 'F:\work\music_online_apk\.toolchains\jdk17',
    [string]$AndroidSdk = 'F:\work\music_online_apk\.toolchains\android-sdk',
    [string]$GradleCache = 'D:\Games\Old_Times\3ds\cia-bridge\.toolchains\gradle-cache'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$failed = @()

function Invoke-Step([string]$name, [scriptblock]$body) {
    Write-Host "=== $name ==="
    try {
        & $body
        if ($LASTEXITCODE -ne 0) { throw "$name exited $LASTEXITCODE" }
    } catch {
        $failed += "${name}: $_"
        Write-Warning "${name} failed: $_"
    }
}

Push-Location $repo
try {
    Invoke-Step '3DS cross build' {
        pwsh -NoProfile -File scripts\build-handheld.ps1 -Bash $Bash
    }
    Invoke-Step 'kernel style' {
        pwsh -NoProfile -File scripts\check-3ds-style.ps1
    }
    Invoke-Step 'native tests build' {
        & $Bash -lc "cd '$(($repo -replace '\\','/'))' && make -C live/tests/native all"
    }
    Invoke-Step 'native tests run' {
        $env:PATH = "F:\Tools\msys64\usr\bin;$env:PATH"
        foreach ($test in Get-ChildItem live\build\native-tests\test_*.exe) {
            & $test.FullName *> $null
            if ($LASTEXITCODE -ne 0) { throw "$($test.Name): $LASTEXITCODE" }
        }
    }
    Invoke-Step 'PC client build' {
        foreach ($name in @('NO_PROXY','no_proxy','HTTP_PROXY','http_proxy',
                            'HTTPS_PROXY','https_proxy','ALL_PROXY','all_proxy')) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
        $cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        & $cmake --build 'live\build\native-cmake' --config Release --target ThreeDSLiveClient
    }
    Invoke-Step 'Android analyze' {
        $env:JAVA_HOME = $JavaHome
        $env:ANDROID_HOME = $AndroidSdk
        $env:ANDROID_SDK_ROOT = $AndroidSdk
        $env:GRADLE_USER_HOME = $GradleCache
        $env:PUB_CACHE = 'D:\Games\Old_Times\3ds\cia-bridge\.toolchains\pub-cache'
        & $Flutter analyze --suppress-analytics
    }
    Invoke-Step 'Android unit tests' {
        Push-Location android\android
        try { & .\gradlew.bat -q testDebugUnitTest } finally { Pop-Location }
    }
} finally {
    Pop-Location
}

if ($failed) {
    $failed | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Host 'All 3DS Live verification steps passed.'
