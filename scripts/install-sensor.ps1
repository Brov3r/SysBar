param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$targetDir = Join-Path $env:ProgramFiles "SysBar"
$targetExe = Join-Path $targetDir "SysBarSensor.exe"
$taskName = "SysBar Sensor"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run as administrator."
}

$logPath = Join-Path $root "build\sensor-setup.log"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $logPath) | Out-Null
Start-Transcript -Path $logPath -Force | Out-Null

try {
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build.ps1") -Target Sensor
    }

    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Get-Process -Name "SysBarSensor" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Copy-Item (Join-Path $root "build\SysBarSensor.exe") $targetExe -Force
    Write-Host "Installed sensor at '$targetExe'."

    # LHM is no longer used.
    Unregister-ScheduledTask -TaskName "SysBar LibreHardwareMonitor" -Confirm:$false -ErrorAction SilentlyContinue

    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    $action = New-ScheduledTaskAction -Execute $targetExe -WorkingDirectory $targetDir
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -StartWhenAvailable `
        -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
        -Principal $taskPrincipal -Settings $settings -Force | Out-Null
    Write-Host "Registered scheduled task '$taskName' (SYSTEM, at startup)."

    Start-ScheduledTask -TaskName $taskName

    # The old helper wrote text files; clean them up.
    Remove-Item (Join-Path $env:ProgramData "SysBar") -Recurse -Force -ErrorAction SilentlyContinue

    $value = $null
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 1
        try {
            $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
                "Global\SysBarSensors",
                [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
            try {
                $accessor = $mapping.CreateViewAccessor(
                    0, 16,
                    [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
                try {
                    $sequence = $accessor.ReadInt32(0)
                    [Threading.Thread]::MemoryBarrier()
                    $tick = $accessor.ReadInt32(4)
                    $temperature = $accessor.ReadDouble(8)
                    [Threading.Thread]::MemoryBarrier()
                    $stable = $sequence -eq $accessor.ReadInt32(0) -and ($sequence -band 1) -eq 0
                    $age = ([long][Environment]::TickCount - $tick) -band 0xffffffffL
                    if ($stable -and $age -le 15000 -and $temperature -ge 1 -and $temperature -le 150) {
                        $value = $temperature
                        break
                    }
                } finally {
                    $accessor.Dispose()
                }
            } finally {
                $mapping.Dispose()
            }
        } catch {
            # Not created yet.
        }
    }

    if ($null -ne $value) {
        Write-Host ("Sensor value: {0:N1} C" -f $value)
    } else {
        Write-Host "Shared memory value not available yet."
    }
    Get-ScheduledTask -TaskName $taskName | Select-Object TaskName, State
} finally {
    Stop-Transcript | Out-Null
}
