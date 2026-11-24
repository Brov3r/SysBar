<h1 align="center">SysBar</h1>

<p align="center">
    <a href="https://github.com/Brov3r/SysBar/actions/workflows/build.yml"><img alt="Build" src="https://github.com/Brov3r/SysBar/actions/workflows/build.yml/badge.svg"></a>
    <img alt="Windows 11 x64" src="https://img.shields.io/badge/Windows_11-x64-0078D4">
    <a href="./LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-blue"></a>
</p>

**SysBar** is a [Windhawk](https://windhawk.net/) mod that displays system stats next to the notification area on the Windows 11 taskbar.

[Taskbar System Info](https://windhawk.net/mods/taskbar-system-info) covers many of the same metrics. SysBar is an alternative if you want an uptime reading beside them, scalable drawn icons, or a read-only connection to the optional SysBar Sensor helper. It overlays the primary taskbar rather than reserving space for buttons.

<p align="center">
    <img src="./docs/sysbar-demo.png" alt="SysBar showing CPU and GPU load and temperature, RAM usage, and download and upload speeds">
</p>

## ✨ Features

- CPU and GPU load and temperature (`25% · 69°C`), RAM usage, and network speeds.
- Choose memory usage in percent, used GiB or available GiB; optionally show system uptime.
- Configure visible metrics, font size, icons, spacing, refresh interval, and temperature alerts in Windhawk's visual settings editor.
- Automatically hides less important readings when space is tight.

## 🚀 Quick Start

Install Windhawk and extract the [latest release](https://github.com/Brov3r/SysBar/releases/latest) ZIP. From the extracted folder, run in **PowerShell as administrator**:

```powershell
.\scripts\install.ps1 -SkipBuild -RestartExplorer
.\scripts\install-sensor.ps1 -SkipBuild  # Optional: read the ACPI thermal zone
```

Building from source? Install Windhawk's development tools and omit `-SkipBuild`. To uninstall, run `.\scripts\uninstall.ps1 -RestartExplorer`.

The [standalone Windhawk mod source](./mods/sysbar-taskbar-monitor.wh.cpp) also works without the helper. When no helper reading is available, SysBar tries Open Hardware Monitor and Windows thermal-zone providers. A thermal-zone reading isn't necessarily the CPU package temperature.

## ⚖️ License

This project is licensed under the [MIT License](./LICENSE).
