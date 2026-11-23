using System;
using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Management;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;
using System.Reflection;

[assembly: AssemblyTitle("SysBar Sensor")]
[assembly: AssemblyDescription("Publishes ACPI thermal zone readings for the SysBar taskbar monitor.")]
[assembly: AssemblyProduct("SysBar")]
[assembly: AssemblyCompany("Brov3r")]
[assembly: AssemblyVersion("1.1.0.0")]
[assembly: AssemblyFileVersion("1.1.0.0")]

// Reads the ACPI thermal zone temperature and publishes it to a global
// shared-memory section. Runs as SYSTEM (scheduled task), has no window, no
// tray icon and writes no files. ACPI temperature is only readable with
// administrative rights, which is the reason this tiny helper exists.
internal static class SysBarSensor
{
    private const int IntervalMilliseconds = 3000;
    private const string ManagementScope = @"root\WMI";
    private const string Query = "SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature";
    private const string MappingName = @"Global\SysBarSensors";
    private const int MappingSize = 16;

    private static void Main()
    {
        var security = new MemoryMappedFileSecurity();
        security.AddAccessRule(
            new AccessRule<MemoryMappedFileRights>(
                new SecurityIdentifier(WellKnownSidType.WorldSid, null),
                MemoryMappedFileRights.Read,
                AccessControlType.Allow));

        security.AddAccessRule(
            new AccessRule<MemoryMappedFileRights>(
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                MemoryMappedFileRights.ReadWrite,
                AccessControlType.Allow));

        while (true)
        {
            try
            {
                using (MemoryMappedFile mapping = OpenMapping(security))
                {
                    if (mapping == null) continue;
                    using (var view = mapping.CreateViewAccessor(0, MappingSize))
                    {
                        int sequence = view.ReadInt32(0) & ~1;
                        while (true)
                        {
                            double? temperature = ReadTemperature();
                            if (temperature.HasValue)
                            {
                                view.Write(0, ++sequence);
                                Thread.MemoryBarrier();
                                view.Write(4, Environment.TickCount);
                                view.Write(8, temperature.Value);
                                Thread.MemoryBarrier();
                                view.Write(0, ++sequence);
                            }
                            Thread.Sleep(IntervalMilliseconds);
                        }
                    }
                }

            }
            catch (IOException)
            {
                Thread.Sleep(IntervalMilliseconds);
            }
        }
    }

    // The SysBar mod (inside explorer) keeps the section open. When this
    // helper is updated or restarted, CreateNew fails with "already exists",
    // so fall back to the existing section instead of exiting.
    private static MemoryMappedFile OpenMapping(MemoryMappedFileSecurity security)
    {
        for (int attempt = 0; attempt < 30; attempt++)
        {
            try
            {
                return MemoryMappedFile.CreateNew(
                    MappingName,
                    MappingSize,
                    MemoryMappedFileAccess.ReadWrite,
                    MemoryMappedFileOptions.None,
                    security,
                    HandleInheritability.None);
            }
            catch (IOException)
            {
                try
                {
                    return MemoryMappedFile.OpenExisting(
                        MappingName, MemoryMappedFileRights.ReadWrite);
                }
                catch
                {
                    // Section is being torn down; retry.
                }
            }
            catch
            {
                // Access or privilege issue; retry briefly.
            }

            Thread.Sleep(1000);
        }

        return null;
    }

    private static double? ReadTemperature()
    {
        try
        {
            double best = double.MinValue;
            using (var searcher = new ManagementObjectSearcher(ManagementScope, Query))
            using (ManagementObjectCollection results = searcher.Get())
            {
                foreach (ManagementObject item in results)
                {
                    using (item)
                    {
                        object raw = item["CurrentTemperature"];
                        if (raw == null)
                        {
                            continue;
                        }

                        double celsius =
                            Convert.ToDouble(raw, CultureInfo.InvariantCulture) / 10.0 - 273.15;
                        if (celsius >= 1 && celsius <= 150 && celsius > best)
                        {
                            best = celsius;
                        }
                    }
                }
            }

            if (best != double.MinValue)
            {
                return best;
            }
        }
        catch
        {
            // Keep running; a later sample may succeed.
        }

        return null;
    }
}


