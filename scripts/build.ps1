param(
    [ValidateSet("All", "Mod", "Sensor")][string]$Target = "All",
    [string]$WindhawkRoot = (Join-Path $env:ProgramFiles "Windhawk")
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$compilerRoot = Join-Path $windhawkRoot "Compiler"
$clang = Join-Path $compilerRoot "bin\clang++.exe"
$source = Join-Path $root "mods\SysBar.wh.cpp"
$outputDir = Join-Path $root "build"
$output = Join-Path $outputDir "SysBar.dll"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
if ($Target -ne "Mod") {
    $csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
    & $csc /nologo /target:winexe /optimize+ "/out:$outputDir\SysBarSensor.exe" `
        /reference:System.Management.dll (Join-Path $root "tools\SysBarSensor.cs")
    if ($LASTEXITCODE -ne 0) { throw "Sensor build failed." }
    if ($Target -eq "Sensor") { return }
}
$engineDir = Get-ChildItem (Join-Path $windhawkRoot "Engine") -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$windhawkLib = if ($engineDir) { Join-Path $engineDir.FullName "64\windhawk.lib" } else { "" }

if (-not (Test-Path $clang)) {
    throw "Windhawk compiler not found at '$clang'. Install Windhawk with development tools."
}
if (-not $windhawkLib -or -not (Test-Path $windhawkLib)) {
    throw "windhawk.lib not found. Install Windhawk with development tools."
}
if (-not (Test-Path $source)) {
    throw "Mod source not found at '$source'."
}

$arguments = @(
    $windhawkLib,
    "-x", "c++",
    "-std=c++23",
    "-target", "x86_64-w64-mingw32",
    "-DUNICODE", "-D_UNICODE",
    "-DWINVER=0x0A00", "-D_WIN32_WINNT=0x0A00", "-D_WIN32_IE=0x0A00",
    "-DNTDDI_VERSION=0x0A000008", "-D__USE_MINGW_ANSI_STDIO=0",
    "-DWH_MOD", '-DWH_MOD_ID=L\"sysbar-taskbar-monitor\"', '-DWH_MOD_VERSION=L\"1.1.1\"',
    "-include", "winsock2.h", "-include", "windhawk_api.h",
    "-Wno-pragma-pack",
    "-Wno-pragma-system-header-outside-header",
    "-fms-extensions",
    "-Wall", "-Wextra",
    "-I", (Join-Path $compilerRoot "include"),
    "-shared", "-fuse-ld=lld",
    "-Wl,--export-all-symbols",
    "-O2",
    "-o", $output,
    $source,
    "-L", (Join-Path $compilerRoot "x86_64-w64-mingw32\lib"),
    "-lole32", "-loleaut32", "-lruntimeobject", "-liphlpapi", "-lpdh", "-lwbemuuid", "-lws2_32"
)

Write-Host "Compiling SysBar mod..."
& $clang @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Compilation failed with exit code $LASTEXITCODE."
}

$item = Get-Item $output
Write-Host ("Built {0} ({1:N0} bytes)" -f $item.FullName, $item.Length)




