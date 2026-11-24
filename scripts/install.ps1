param(
    [switch]$SkipBuild,
    [switch]$RestartExplorer
)

$ErrorActionPreference = "Stop"

$modId = "sysbar-taskbar-monitor"
$modVersion = "1.1.1"
$root = Split-Path -Parent $PSScriptRoot
$windhawkData = Join-Path $env:ProgramData "Windhawk"
$modsDir = Join-Path $windhawkData "Engine\Mods\64"
$configKey = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Installation requires administrator rights. Run this script as administrator."
}

$logPath = Join-Path (Split-Path -Parent $PSScriptRoot) "build\install.log"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $logPath) | Out-Null
Start-Transcript -Path $logPath -Force | Out-Null

if (-not (Test-Path (Join-Path $windhawkData "Engine"))) {
    throw "Windhawk is not installed (missing '$windhawkData\Engine')."
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Target Mod
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
}

$builtDll = Join-Path $root "build\SysBar.dll"
if (-not (Test-Path $builtDll)) {
    throw "Built mod not found at '$builtDll'. Run build.ps1 first."
}

New-Item -ItemType Directory -Force -Path $modsDir | Out-Null

# Windhawk reads the mod page and settings schema from the ID-named source.
$sourceDir = Join-Path $windhawkData "ModsSource"
New-Item -ItemType Directory -Force -Path $sourceDir | Out-Null
Copy-Item -LiteralPath (Join-Path $root "mods\SysBar.wh.cpp") -Destination (Join-Path $sourceDir "$modId.wh.cpp") -Force

# Remove previous copies of this mod. A copy loaded into explorer.exe is locked
# until explorer restarts, so locked files are cleaned up afterwards.
Get-ChildItem -Path $modsDir -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object {
        try {
            Remove-Item -Path $_.FullName -Force -ErrorAction Stop
        } catch {
            Write-Host "Keeping locked old copy '$($_.Name)' (will be removed after explorer restart)."
        }
    }

$suffix = Get-Random -Minimum 100000 -Maximum 999999
$dllName = "{0}_{1}_{2}.dll" -f $modId, $modVersion, $suffix
$targetDll = Join-Path $modsDir $dllName
Copy-Item -Path $builtDll -Destination $targetDll -Force

New-Item -Path $configKey -Force | Out-Null
New-ItemProperty -Path $configKey -Name "LibraryFileName" -Value $dllName -PropertyType String -Force | Out-Null
New-ItemProperty -Path $configKey -Name "Architecture" -Value "x86-64" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $configKey -Name "Include" -Value "explorer.exe" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $configKey -Name "Disabled" -Value 0 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $configKey -Name "Version" -Value $modVersion -PropertyType String -Force | Out-Null

Write-Host "Installed '$modId' as '$dllName'."

Write-Host "Restarting Windhawk service..."
Restart-Service -Name "Windhawk" -Force
for ($i = 0; $i -lt 30; $i++) {
    if ((Get-Service -Name "Windhawk").Status -eq "Running") {
        break
    }
    Start-Sleep -Milliseconds 500
}

if ($RestartExplorer) {
    Write-Host "Restarting explorer.exe..."
    Get-Process -Name "explorer" -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
    if (-not (Get-Process -Name "explorer" -ErrorAction SilentlyContinue)) {
        Start-Process "explorer.exe"
    }
    Start-Sleep -Seconds 4

    Get-ChildItem -Path $modsDir -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne $dllName } |
        ForEach-Object {
            try {
                Remove-Item -Path $_.FullName -Force -ErrorAction Stop
            } catch {
                Write-Host "Could not remove stale copy '$($_.Name)'."
            }
        }
}

Write-Host ""
Write-Host "Done. The widget is injected into explorer.exe on its next start."
Write-Host "If you installed it for the first time, restart explorer.exe or sign out and back in."

Stop-Transcript | Out-Null
