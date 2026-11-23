param(
    [switch]$RestartExplorer
)

$ErrorActionPreference = "Stop"

$modId = "sysbar-taskbar-monitor"
$windhawkData = Join-Path $env:ProgramData "Windhawk"
$modsDir = Join-Path $windhawkData "Engine\Mods\64"
$configKey = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Uninstallation requires administrator rights. Run this script as administrator."
}

if (Test-Path $configKey) {
    Remove-Item -Path $configKey -Recurse -Force
    Write-Host "Removed registry configuration."
}

Get-ChildItem -Path $modsDir -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object {
        try {
            Remove-Item -Path $_.FullName -Force -ErrorAction Stop
        } catch {
            Write-Host "Keeping locked mod copy '$($_.Name)' (will be removed after explorer restart)."
        }
    }
Write-Host "Removed compiled mod files."

# Remove the sensor helper.
Unregister-ScheduledTask -TaskName "SysBar Sensor" -Confirm:$false -ErrorAction SilentlyContinue
Get-Process -Name "SysBarSensor" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramFiles "SysBar") -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData "SysBar") -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Removed the sensor helper."

Restart-Service -Name "Windhawk" -Force

if ($RestartExplorer) {
    Get-Process -Name "explorer" -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
    if (-not (Get-Process -Name "explorer" -ErrorAction SilentlyContinue)) {
        Start-Process "explorer.exe"
    }
    Start-Sleep -Seconds 4

    Get-ChildItem -Path $modsDir -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
        ForEach-Object {
            try {
                Remove-Item -Path $_.FullName -Force -ErrorAction Stop
            } catch {
                Write-Host "Could not remove '$($_.Name)'."
            }
        }
}

Write-Host "SysBar mod and sensor uninstalled."
