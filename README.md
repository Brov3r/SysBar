<h1 align="center">SysBar</h1>

<p align="center">
    <a href="https://github.com/Brov3r/SysBar/actions/workflows/build.yml"><img alt="Build" src="https://github.com/Brov3r/SysBar/actions/workflows/build.yml/badge.svg"></a>
    <img alt="Windows 11 x64" src="https://img.shields.io/badge/Windows_11-x64-0078D4">
    <a href="./LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-blue"></a>
</p>

**SysBar** is a [Windhawk](https://windhawk.net/) mod that displays system stats next to the notification area on the Windows 11 taskbar.

<p align="center">
    <img src="./docs/sysbar-demo.png" alt="SysBar showing CPU and GPU load and temperature, RAM usage, and download and upload speeds">
</p>

## ✨ Features

- CPU and GPU load and temperature (`25% · 69°C`), RAM usage, and network speeds.
- Configurable font size, icons, refresh interval, and temperature alerts.
- Automatically hides less important readings when space is tight.

## 🚀 Quick Start

Install Windhawk with its development tools. From the project folder, run in **PowerShell as administrator**:

```powershell
.\scripts\install.ps1 -RestartExplorer
.\scripts\install-sensor.ps1  # Optional: read the ACPI thermal zone
```

To remove both the mod and the sensor, run `.\scripts\uninstall.ps1 -RestartExplorer`.

## ⚖️ License

This project is licensed under the [MIT License](./LICENSE).
