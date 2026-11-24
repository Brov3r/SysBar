// ==WindhawkMod==
// @id              sysbar-taskbar-monitor
// @name            SysBar - Taskbar Monitor
// @description     CPU and GPU load and temperature, RAM usage and network speed with icons, embedded into the Windows 11 taskbar
// @name:ru-RU      SysBar — монитор панели задач
// @description:ru-RU Загрузка и температура CPU и GPU, использование памяти и скорость сети в панели задач Windows 11.
// @version         1.1.1
// @author          Brov3r
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -liphlpapi -lpdh -lwbemuuid -lwinhttp
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# SysBar
Displays CPU, GPU, memory and network readings on the primary Windows 11
taskbar. CPU/GPU groups show **load · temperature**; yellow and red mark the
configured temperature thresholds. Unavailable temperatures are hidden.

GPU load uses NVIDIA NVML or Windows performance counters. The optional
SysBar Sensor reads ACPI thermal zones: its value is a zone temperature,
not necessarily the CPU package temperature.

Restart Explorer after first installation. Configure text size, refresh
interval, visible metrics and temperature thresholds in the mod settings.

License: MIT. Icon reference: Nieobie's game-icon-pack (CC0-1.0).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- fontSize: 12
  $name: Font size
  $description: Text size in DIPs (12 matches the taskbar clock).
- showTemps: 1
  $name: Show temperatures
- showNet: 1
  $name: Show network speed
- intervalSeconds: 1
  $name: Refresh interval (seconds)
  $description: Sampling and display interval, from 1 to 10 seconds.
- showIcons: 1
  $name: Show group icons
- warnTemp: 75
  $name: Warning temperature (C)
- critTemp: 88
  $name: Critical temperature (C)
- debugLog: 0
  $name: Write debug log
  $description: Writes %LOCALAPPDATA%\SysBar\debug.log with sensor diagnostics.
*/
// ==/WindhawkModSettings==

#include <winsock2.h>
#include <windows.h>
#include <netioapi.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;

using winrt::Windows::UI::Color;

namespace {

constexpr wchar_t kHostName[] = L"SysBarMonitorWidget";
constexpr UINT kRunOnTaskbarThreadMessage = WM_APP + 0x5B1;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings {
    int fontSize = 12;
    bool showTemps = true;
    bool showNet = true;
    bool showIcons = true;
    int warnTemp = 75;
    int critTemp = 88;
    bool debugLog = false;
    int intervalSeconds = 1;
};

Settings g_settings;
std::mutex g_settingsMutex;
Settings CurrentSettings() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

void DebugLog(const wchar_t* format, ...);

void LoadSettings() {
    // Values are zero when the mod has never been configured (for example,
    // when installed manually). Fall back to sane defaults in that case.
    const int fontSize = Wh_GetIntSetting(L"fontSize");
    const int showTemps = Wh_GetIntSetting(L"showTemps");
    const int showNet = Wh_GetIntSetting(L"showNet");
    const int showIcons = Wh_GetIntSetting(L"showIcons");
    const int warnTemp = Wh_GetIntSetting(L"warnTemp");
    const int critTemp = Wh_GetIntSetting(L"critTemp");
    const int debugLog = Wh_GetIntSetting(L"debugLog");

    const bool unconfigured = fontSize == 0 && showTemps == 0 && showNet == 0 &&
                              showIcons == 0 && warnTemp == 0 && critTemp == 0;

    Settings s;
    s.fontSize = std::clamp(unconfigured ? 12 : fontSize, 8, 24);
    s.showTemps = unconfigured || showTemps != 0;
    s.showNet = unconfigured || showNet != 0;
    s.showIcons = unconfigured || showIcons != 0;
    s.warnTemp = std::clamp(unconfigured ? 75 : warnTemp, 30, 120);
    s.critTemp = std::clamp(unconfigured ? 88 : critTemp, s.warnTemp + 1, 130);
    s.debugLog = debugLog != 0;
    s.intervalSeconds = std::clamp(Wh_GetIntSetting(L"intervalSeconds"), 1, 10);
    std::lock_guard lock(g_settingsMutex);
    g_settings = s;
}

void DebugLog(const wchar_t* format, ...) {
    if (!CurrentSettings().debugLog) {
        return;
    }
    wchar_t directory[MAX_PATH];
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, MAX_PATH);
    if (!size || size >= MAX_PATH) {
        return;
    }
    std::wstring folder = std::wstring(directory) + L"\\SysBar";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring path = folder + L"\\debug.log";

    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf(message, 1024, format, args);
    message[1023] = L'\0';
    va_end(args);

    SYSTEMTIME time;
    GetLocalTime(&time);

    char line[2048];
    int offset = snprintf(line, sizeof(line), "[%02u:%02u:%02u] ", time.wHour,
                          time.wMinute, time.wSecond);
    int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, line + offset,
                                     static_cast<int>(sizeof(line)) - offset,
                                     nullptr, nullptr);
    if (length > 0) {
        line[offset + length - 1] = '\n';
    } else {
        line[offset] = '\n';
        length = 1;
    }

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(offset + length), &written,
                  nullptr);
        CloseHandle(file);
    }
}

// ---------------------------------------------------------------------------
// Metrics snapshot (produced by a worker thread, consumed by the taskbar UI)
// ---------------------------------------------------------------------------

struct Snapshot {
    double cpuPct = -1;
    double cpuTemp = -1;
    double gpuPct = -1;
    double gpuTemp = -1;
    double ramPct = -1;
    double netDown = -1;
    double netUp = -1;
    bool gpuAvailable = false;
    bool netAvailable = false;
};

std::mutex g_snapMutex;
Snapshot g_snap;

std::thread g_worker;
std::atomic<bool> g_workerStop{false};
std::mutex g_workerWakeMutex;
std::condition_variable g_workerWake;

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::optional<double> ReadCpuLoad() {
    static bool initialized = false;
    static uint64_t prevIdle = 0, prevKernel = 0, prevUser = 0;

    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return std::nullopt;
    }

    const uint64_t idleNow = FileTimeToU64(idle);
    const uint64_t kernelNow = FileTimeToU64(kernel);
    const uint64_t userNow = FileTimeToU64(user);

    if (!initialized) {
        initialized = true;
        prevIdle = idleNow;
        prevKernel = kernelNow;
        prevUser = userNow;
        return std::nullopt;
    }

    const uint64_t idleDelta = idleNow - prevIdle;
    const uint64_t kernelDelta = kernelNow - prevKernel;
    const uint64_t userDelta = userNow - prevUser;
    prevIdle = idleNow;
    prevKernel = kernelNow;
    prevUser = userNow;

    const uint64_t total = kernelDelta + userDelta;
    if (!total || idleDelta > total) {
        return std::nullopt;
    }
    return std::clamp(100.0 * static_cast<double>(total - idleDelta) /
                          static_cast<double>(total),
                      0.0, 100.0);
}

double ReadRamPct() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) {
        return -1;
    }
    return static_cast<double>(memory.dwMemoryLoad);
}

// Published by the tiny SYSTEM helper (tools/SysBarSensor.cs) through a
// global shared-memory section, because the ACPI thermal zone can only be
// queried with administrative rights.
struct SensorSharedMemory {
    volatile LONG sequence;
    volatile LONG tick;
    double temperature;
};

struct SensorReader {
    winrt::handle mapping;
    SensorSharedMemory* view = nullptr;
    ~SensorReader() { if (view) UnmapViewOfFile(view); }

    std::optional<double> Read() {
        if (!view) {
            mapping.attach(OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\SysBarSensors"));
            if (!mapping) return std::nullopt;
            view = static_cast<SensorSharedMemory*>(
                MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(SensorSharedMemory)));
            if (!view) {
                mapping.close();
                return std::nullopt;
            }
        }

        const LONG before = view->sequence;
        MemoryBarrier();
        const LONG tick = view->tick;
        const double value = view->temperature;
        MemoryBarrier();
        const LONG after = view->sequence;

        const DWORD age = GetTickCount() - static_cast<DWORD>(tick);
        if (age > 15000) {
            UnmapViewOfFile(view);
            mapping.close();
            view = nullptr;
            return std::nullopt;
        }
        if (before != after || (before & 1) || !(value >= 1 && value <= 150))
            return std::nullopt;
        return value;
    }
};

struct NetCounters {
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> previous;
    std::chrono::steady_clock::time_point time{};
};

bool IsRealNetworkAdapter(const MIB_IF_ROW2& row) {
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
        return false;
    }
    if (row.OperStatus != IfOperStatusUp) {
        return false;
    }
    return true;
}

std::pair<double, double> ReadNetwork(NetCounters& state, bool& available) {
    available = false;

    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) {
        state = {};
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> current;
    uint64_t received = 0, sent = 0;
    bool comparable = false;

    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (!IsRealNetworkAdapter(row)) {
            continue;
        }
        const uint64_t luid = row.InterfaceLuid.Value;
        current[luid] = {row.InOctets, row.OutOctets};

        auto it = state.previous.find(luid);
        if (it != state.previous.end() && row.InOctets >= it->second.first &&
            row.OutOctets >= it->second.second) {
            comparable = true;
            received += row.InOctets - it->second.first;
            sent += row.OutOctets - it->second.second;
        }
    }
    FreeMibTable(table);

    const double seconds =
        std::chrono::duration<double>(now - state.time).count();
    if (comparable && state.time != std::chrono::steady_clock::time_point{} &&
        seconds >= 0.2) {
        available = true;
        state.previous = std::move(current);
        state.time = now;
        return {static_cast<double>(received) / seconds,
                static_cast<double>(sent) / seconds};
    }

    state.previous = std::move(current);
    state.time = now;
    return {};
}

// ---------------------------------------------------------------------------
// GPU metrics: NVML first, PDH "\GPU Engine(*)\Utilization Percentage" fallback
// ---------------------------------------------------------------------------

struct NvmlGpu {
    HMODULE lib = nullptr;
    void* device = nullptr;
    using InitFn = int (*)();
    using CountFn = int (*)(unsigned*);
    using HandleFn = int (*)(unsigned, void**);
    using TempFn = int (*)(void*, int, int*);
    using UtilFn = int (*)(void*, void*);

    InitFn init = nullptr;
    InitFn shutdown = nullptr;
    bool initialized = false;
    ~NvmlGpu() {
        if (initialized) shutdown();
        if (lib) FreeLibrary(lib);
    }
    CountFn count = nullptr;
    HandleFn handle = nullptr;
    TempFn temperature = nullptr;
    UtilFn utilization = nullptr;
    bool failed = false;
    std::chrono::steady_clock::time_point nextRetry{};
    char name[96]{};

    bool Ensure() {
        if (device) {
            return true;
        }
        if (failed && std::chrono::steady_clock::now() < nextRetry) {
            return false;
        }
        if (!lib) {
            lib = LoadLibraryExW(L"nvml.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (!lib) {
            DebugLog(L"nvml: LoadLibrary failed, error=%lu", GetLastError());
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        init = reinterpret_cast<InitFn>(GetProcAddress(lib, "nvmlInit_v2"));
        shutdown = reinterpret_cast<InitFn>(GetProcAddress(lib, "nvmlShutdown"));
        count =
            reinterpret_cast<CountFn>(GetProcAddress(lib, "nvmlDeviceGetCount_v2"));
        handle = reinterpret_cast<HandleFn>(
            GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2"));
        temperature = reinterpret_cast<TempFn>(
            GetProcAddress(lib, "nvmlDeviceGetTemperature"));
        utilization = reinterpret_cast<UtilFn>(
            GetProcAddress(lib, "nvmlDeviceGetUtilizationRates"));
        if (!init || !shutdown || !count || !handle || !temperature || !utilization) {
            DebugLog(L"nvml: missing exports init=%d count=%d handle=%d temp=%d util=%d",
                     init != nullptr, count != nullptr, handle != nullptr,
                     temperature != nullptr, utilization != nullptr);
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        const int initResult = initialized ? 0 : init();
        if (initResult != 0) {
            DebugLog(L"nvml: init failed, code=%d", initResult);
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        unsigned deviceCount = 0;
        initialized = true;
        const int countResult = count(&deviceCount);
        if (countResult != 0 || deviceCount == 0) {
            DebugLog(L"nvml: count failed, code=%d count=%u", countResult,
                     deviceCount);
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        const int handleResult = handle(0, &device);
        if (handleResult != 0) {
            DebugLog(L"nvml: handle failed, code=%d", handleResult);
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        using NameFn = int (*)(void*, char*, unsigned);
        if (auto getName = reinterpret_cast<NameFn>(
                GetProcAddress(lib, "nvmlDeviceGetName"))) {
            getName(device, name, sizeof(name));
        }
        DebugLog(L"nvml: ready, gpu=%S", name);
        return true;
    }

    struct Util {
        unsigned gpu;
        unsigned memory;
    };

    bool Sample(double& util, double& temp, bool readTemp) {
        if (!device) {
            return false;
        }
        int t = -1;
        Util u{};
        const int tempResult = readTemp ? temperature(device, 0, &t) : -1;
        const int utilResult = utilization(device, &u);

        const bool tempValid = tempResult == 0 && t > 0 && t <= 150;
        const bool utilValid = utilResult == 0;

        if (!tempValid && !utilValid) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= lastLog) {
                lastLog = now + std::chrono::seconds(30);
                DebugLog(L"nvml: sample failed temp=%d/%d util=%d",
                         tempResult, t, utilResult);
            }
        }
        if (tempValid) {
            temp = static_cast<double>(t);
        }
        if (utilValid) {
            util = std::clamp(static_cast<double>(u.gpu), 0.0, 100.0);
        }
        return tempValid || utilValid;
    }

    std::chrono::steady_clock::time_point lastLog{};
};

struct PdhGpuLoad {
    PDH_HQUERY query = nullptr;
    ~PdhGpuLoad() { if (query) PdhCloseQuery(query); }
    std::vector<BYTE> buffer;
    PDH_HCOUNTER counter = nullptr;
    bool failed = false;
    std::chrono::steady_clock::time_point nextRetry{};

    bool Ensure() {
        if (counter) {
            return true;
        }
        if (failed && std::chrono::steady_clock::now() < nextRetry) {
            return false;
        }
        failed = false;
        if (query) {
            PdhCloseQuery(query);
            query = nullptr;
        }
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        if (PdhAddEnglishCounterW(query,
                                  L"\\GPU Engine(*)\\Utilization Percentage", 0,
                                  &counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            counter = nullptr;
            failed = true;
            nextRetry = std::chrono::steady_clock::now() +
                        std::chrono::seconds(60);
            return false;
        }
        PdhCollectQueryData(query);
        return true;
    }

    std::optional<double> Sample() {
        if (!Ensure() || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return std::nullopt;
        }

        DWORD bufferSize = 0, itemCount = 0;
        PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize,
                                     &itemCount, nullptr);
        if (!bufferSize || !itemCount) {
            return std::nullopt;
        }

        buffer.resize(bufferSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize,
                                         &itemCount, items) != ERROR_SUCCESS) {
            return std::nullopt;
        }

        std::unordered_map<std::wstring, double> engines;
        double busiest = -1;
        for (DWORD i = 0; i < itemCount; i++) {
            const wchar_t* instance = items[i].szName;
            const auto engine = instance ? wcsstr(instance, L"luid_") : nullptr;
            if (!engine) {
                continue;
            }
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            auto& load = engines[engine];  // Same adapter/engine across process IDs.
            load += items[i].FmtValue.doubleValue;
            busiest = std::max(busiest, load);
        }
        if (busiest < 0) return std::nullopt;
        return std::clamp(busiest, 0.0, 100.0);
    }
};

// ---------------------------------------------------------------------------
// Temperature sources: LibreHardwareMonitor / OpenHardwareMonitor WMI, ACPI
// ---------------------------------------------------------------------------

class WmiConnection {
   public:
    WmiConnection() = default;
    ~WmiConnection() { Close(); }

    WmiConnection(const WmiConnection&) = delete;
    WmiConnection& operator=(const WmiConnection&) = delete;

    bool Ensure(const wchar_t* ns) {
        if (services_) {
            return true;
        }
        if (failed_ && std::chrono::steady_clock::now() < nextRetry_) {
            return false;
        }

        IWbemLocator* locator = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                    reinterpret_cast<void**>(&locator)))) {
            MarkFailed();
            return false;
        }
        BSTR path = SysAllocString(ns);
        HRESULT hr = locator->ConnectServer(
            path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_);
        SysFreeString(path);
        locator->Release();
        if (FAILED(hr) || !services_) {
            Close();
            MarkFailed();
            return false;
        }
        CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                          nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        failed_ = false;
        return true;
    }

    // Returns hottest CPU sensor, if any sensor name looks like a CPU one.
    std::optional<double> QueryCpuTemperature() {
        if (!services_) {
            return std::nullopt;
        }
        IEnumWbemClassObject* enumerator = nullptr;
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT Name, SensorType, Value FROM Sensor WHERE SensorType='Temperature'");
        HRESULT hr = services_->ExecQuery(
            language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &enumerator);
        SysFreeString(language);
        SysFreeString(query);
        if (FAILED(hr) || !enumerator) {
            Close();
            MarkFailed();
            return std::nullopt;
        }

        std::optional<double> best;
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        while (!g_workerStop && enumerator->Next(500, 1, &object, &returned) == S_OK &&
               returned == 1) {
            VARIANT nameVar, typeVar, valueVar;
            VariantInit(&nameVar);
            VariantInit(&typeVar);
            VariantInit(&valueVar);
            object->Get(L"Name", 0, &nameVar, nullptr, nullptr);
            object->Get(L"SensorType", 0, &typeVar, nullptr, nullptr);
            object->Get(L"Value", 0, &valueVar, nullptr, nullptr);

            std::wstring name = nameVar.vt == VT_BSTR && nameVar.bstrVal
                                    ? nameVar.bstrVal
                                    : L"";
            std::wstring type = typeVar.vt == VT_BSTR && typeVar.bstrVal
                                    ? typeVar.bstrVal
                                    : L"";
            double value = -1;
            if (valueVar.vt == VT_R4) {
                value = valueVar.fltVal;
            } else if (valueVar.vt == VT_R8) {
                value = valueVar.dblVal;
            }

            VariantClear(&nameVar);
            VariantClear(&typeVar);
            VariantClear(&valueVar);
            object->Release();

            if (type != L"Temperature" || !(value > 0 && value <= 150)) {
                continue;
            }
            std::wstring lower = name;
            CharLowerW(lower.data());
            if (lower.find(L"gpu") != std::wstring::npos) {
                continue;
            }
            const bool cpuLike =
                lower.find(L"cpu") != std::wstring::npos ||
                lower.find(L"tctl") != std::wstring::npos ||
                lower.find(L"tdie") != std::wstring::npos ||
                lower.find(L"ccd") != std::wstring::npos ||
                lower.find(L"core (") != std::wstring::npos;
            if (!cpuLike) {
                continue;
            }
            if (!best || value > *best) {
                best = value;
            }
        }
        enumerator->Release();
        return best;
    }

    // ACPI thermal zones report tenths of Kelvin.
    std::optional<double> QueryAcpiTemperature() {
        if (!services_) {
            return std::nullopt;
        }
        IEnumWbemClassObject* enumerator = nullptr;
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
        HRESULT hr = services_->ExecQuery(
            language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &enumerator);
        SysFreeString(language);
        SysFreeString(query);
        if (FAILED(hr) || !enumerator) {
            Close();
            MarkFailed();
            return std::nullopt;
        }

        std::optional<double> best;
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        while (!g_workerStop && enumerator->Next(500, 1, &object, &returned) == S_OK &&
               returned == 1) {
            VARIANT valueVar;
            VariantInit(&valueVar);
            object->Get(L"CurrentTemperature", 0, &valueVar, nullptr, nullptr);
            double celsius = -1;
            if (valueVar.vt == VT_I4) {
                celsius = valueVar.lVal / 10.0 - 273.15;
            } else if (valueVar.vt == VT_UI4) {
                celsius = valueVar.ulVal / 10.0 - 273.15;
            }
            VariantClear(&valueVar);
            object->Release();
            if (celsius > 0 && celsius < 150 &&
                (!best || celsius > *best)) {
                best = celsius;
            }
        }
        enumerator->Release();
        return best;
    }

    void ResetTimeout() {
        failed_ = false;
        nextRetry_ = {};
    }

   private:
    void MarkFailed() {
        failed_ = true;
        nextRetry_ =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
    }

    void Close() {
        if (services_) {
            services_->Release();
            services_ = nullptr;
        }
    }

    IWbemServices* services_ = nullptr;
    bool failed_ = false;
    std::chrono::steady_clock::time_point nextRetry_{};
};

struct TemperatureSources {
    WmiConnection ohm;
    WmiConnection acpi;

    double Read() {
        if (ohm.Ensure(L"ROOT\\OpenHardwareMonitor")) {
            if (auto t = ohm.QueryCpuTemperature()) {
                return *t;
            }
        }
        if (acpi.Ensure(L"ROOT\\WMI")) {
            if (auto t = acpi.QueryAcpiTemperature()) {
                return *t;
            }
        }
        return -1;
    }
};

// Worker thread
// ---------------------------------------------------------------------------

void WorkerMain() {
    struct ComApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;

    NetCounters network;
    NvmlGpu nvml;
    PdhGpuLoad pdhGpu;
    TemperatureSources temps;
    SensorReader sensor;
    double lastCpuTemp = -1;
    unsigned tick = 0;
    std::chrono::steady_clock::time_point nextWmi{};

    while (!g_workerStop.load()) {
        Snapshot snapshot;
        const auto settings = CurrentSettings();

        if (auto cpu = ReadCpuLoad()) {
            snapshot.cpuPct = *cpu;
        }
        snapshot.ramPct = ReadRamPct();

        if (settings.showNet) {
            const auto [down, up] = ReadNetwork(network, snapshot.netAvailable);
            snapshot.netDown = down;
            snapshot.netUp = up;
        } else if (!network.previous.empty()) {
            network = {};
        }

        if (nvml.Ensure()) {
            double util = -1, temp = -1;
            if (nvml.Sample(util, temp, settings.showTemps)) {
                if (util >= 0) {
                    snapshot.gpuPct = util;
                }
                if (temp > 0) {
                    snapshot.gpuTemp = temp;
                }
                snapshot.gpuAvailable = true;
            }
        }
        if (snapshot.gpuPct < 0) {
            if (auto util = pdhGpu.Sample()) {
                snapshot.gpuPct = *util;
                snapshot.gpuAvailable = true;
            }
        }

        if (settings.showTemps) {
            if (auto sharedTemp = sensor.Read()) {
                lastCpuTemp = *sharedTemp;
                nextWmi = {};
            } else if (SUCCEEDED(com.result) && std::chrono::steady_clock::now() >= nextWmi) {
                lastCpuTemp = temps.Read();
                nextWmi = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            snapshot.cpuTemp = lastCpuTemp;
        }

        if (tick % 10 == 0) {
            DebugLog(
                L"metrics cpu=%.0f cpuTemp=%.1f gpu=%.0f gpuTemp=%.1f "
                L"ram=%.0f down=%.0f up=%.0f",
                snapshot.cpuPct, snapshot.cpuTemp, snapshot.gpuPct,
                snapshot.gpuTemp, snapshot.ramPct, snapshot.netDown,
                snapshot.netUp);
        }

        {
            std::lock_guard lock(g_snapMutex);
            g_snap = snapshot;
        }

        tick++;
        std::unique_lock lock(g_workerWakeMutex);
        g_workerWake.wait_for(lock, std::chrono::seconds(settings.intervalSeconds),
                              [] { return g_workerStop.load(); });
    }

}

void StartWorker() {
    g_workerStop = false;
    g_worker = std::thread(WorkerMain);
}

void StopWorker() {
    if (!g_worker.joinable()) {
        return;
    }
    {
        std::lock_guard lock(g_workerWakeMutex);
        g_workerStop = true;
    }
    g_workerWake.notify_all();
    g_worker.join();
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

std::wstring FormatPercent(double value) {
    if (value < 0) {
        return L"-";
    }
    wchar_t buffer[16];
    swprintf(buffer, 16, L"%.0f%%", value);
    return buffer;
}

std::wstring FormatTemp(double value) {
    if (value < 0) {
        return L"-";
    }
    wchar_t buffer[16];
    swprintf(buffer, 16, L"\u00B7\u2002%.0f\u00B0C", value);
    return buffer;
}

std::wstring FormatRate(double bytesPerSecond) {
    wchar_t buffer[32];
    if (bytesPerSecond < 1024) {
        swprintf(buffer, 32, L"%.0fB/s", bytesPerSecond);
    } else if (bytesPerSecond < 1024.0 * 1024) {
        swprintf(buffer, 32, L"%.0fKB/s", bytesPerSecond / 1024.0);
    } else if (bytesPerSecond < 1024.0 * 1024 * 1024) {
        swprintf(buffer, 32, L"%.1fMB/s", bytesPerSecond / (1024.0 * 1024));
    } else {
        swprintf(buffer, 32, L"%.1fGB/s",
                 bytesPerSecond / (1024.0 * 1024 * 1024));
    }
    return buffer;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

enum class IconKind { Cpu, Gpu, Ram, ArrowDown, ArrowUp };

struct Palette {
    Color text;
    Color subtle;
    Color warn;
    Color crit;
};

Palette CurrentPalette(bool dark) {
    Palette p;
    if (dark) {
        p.text = Color{255, 238, 238, 238};
        p.subtle = Color{235, 200, 200, 200};
        p.warn = Color{255, 255, 196, 87};
        p.crit = Color{255, 255, 107, 107};
    } else {
        p.text = Color{255, 24, 24, 24};
        p.subtle = Color{235, 88, 88, 88};
        p.warn = Color{255, 150, 84, 0};
        p.crit = Color{255, 196, 43, 28};
    }
    return p;
}

[[clang::no_destroy]] wuxc::Grid g_rootGrid{nullptr};
[[clang::no_destroy]] wuxc::Grid g_host{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_row{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_cpuGroup{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_gpuGroup{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_ramGroup{nullptr};
[[clang::no_destroy]] wuxc::StackPanel g_netGroup{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_cpuValue{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_cpuTemp{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_gpuValue{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_gpuTemp{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_ramValue{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_netDown{nullptr};
[[clang::no_destroy]] wuxc::TextBlock g_netUp{nullptr};
[[clang::no_destroy]] wuxm::SolidColorBrush g_textBrush{nullptr};
[[clang::no_destroy]] wuxm::SolidColorBrush g_warnBrush{nullptr};
[[clang::no_destroy]] wuxm::SolidColorBrush g_critBrush{nullptr};
[[clang::no_destroy]] wux::DispatcherTimer g_timer{nullptr};
[[clang::no_destroy]] wux::DispatcherTimer g_retryTimer{nullptr};
[[clang::no_destroy]] wux::FrameworkElement g_pendingFrame{nullptr};
[[clang::no_destroy]] wux::FrameworkElement g_measuredFrame{nullptr};
std::atomic<bool> g_attached{false};
// Only the primary taskbar thread subscribes and revokes these handlers.
[[clang::no_destroy]] std::vector<std::pair<winrt::weak_ref<wux::FrameworkElement>,
                                           winrt::event_token>> g_loadedHandlers;
[[clang::no_destroy]] wux::XamlRoot g_xamlRoot{nullptr};

bool g_dark = true;
std::atomic<bool> g_unloading{false};
double g_rightMargin = 250;
int g_retryCount = 0;
std::atomic<HWND> g_msgWindow{nullptr};
HWND g_taskbarWindow = nullptr;

void EnsureMessageWindow();

HWND FindPrimaryTaskbarOnCurrentThread() {
    struct Search {
        HWND found;
        DWORD threadId;
    } search{nullptr, GetCurrentThreadId()};

    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            wchar_t className[64];
            if (!GetClassNameW(window, className, 64)) {
                return TRUE;
            }
            if (wcscmp(className, L"Shell_TrayWnd") != 0) {
                return TRUE;
            }
            if (GetWindowThreadProcessId(window, nullptr) != s->threadId) {
                return TRUE;
            }
            s->found = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool IsDarkTheme() {
    if (g_host) {
        const auto theme = g_host.ActualTheme();
        if (theme == wux::ElementTheme::Dark) {
            return true;
        }
        if (theme == wux::ElementTheme::Light) {
            return false;
        }
    }
    auto application = wux::Application::Current();
    return !application ||
           application.RequestedTheme() == wux::ApplicationTheme::Dark;
}

wuxc::TextBlock MakeTextBlock(double size, Color color) {
    wuxc::TextBlock text;
    text.FontSize(size);
    text.FontFamily(
        wuxm::FontFamily(L"Segoe UI Variable Text"));
    text.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
    text.TextLineBounds(wux::TextLineBounds::Tight);
    text.Foreground(wuxm::SolidColorBrush(color));
    text.VerticalAlignment(wux::VerticalAlignment::Center);
    return text;
}

// Icons are redrawn from "game-icon-pack" by Nieobie (CC0-1.0) with XAML
// primitives and geometry built in code:
// https://github.com/Nieobie/game-icon-pack
constexpr double kIconBox = 16.0;

void AppendRoundedRect(wuxm::PathGeometry const& geometry, double left,
                       double top, double width, double height, double radius) {
    wuxm::PathFigure figure;
    figure.StartPoint(wf::Point{left + radius, top});
    auto segments = figure.Segments();

    auto lineTo = [&](double x, double y) {
        wuxm::LineSegment segment;
        segment.Point(wf::Point{x, y});
        segments.Append(segment);
    };
    auto arcTo = [&](double x, double y) {
        wuxm::ArcSegment segment;
        segment.Point(wf::Point{x, y});
        segment.Size(wf::Size{radius, radius});
        segment.SweepDirection(wuxm::SweepDirection::Clockwise);
        segment.IsLargeArc(false);
        segments.Append(segment);
    };

    lineTo(left + width - radius, top);
    arcTo(left + width, top + radius);
    lineTo(left + width, top + height - radius);
    arcTo(left + width - radius, top + height);
    lineTo(left + radius, top + height);
    arcTo(left, top + height - radius);
    lineTo(left, top + radius);
    arcTo(left + radius, top);
    figure.IsClosed(true);
    geometry.Figures().Append(figure);
}

wuxs::Path MakePath(wuxm::PathGeometry geometry,
                    wuxm::SolidColorBrush const& brush) {
    wuxs::Path path;
    path.Data(geometry);
    path.Fill(brush);
    geometry.FillRule(wuxm::FillRule::EvenOdd);
    path.Stretch(wuxm::Stretch::None);
    path.Width(kIconBox);
    path.Height(kIconBox);
    path.VerticalAlignment(wux::VerticalAlignment::Center);
    return path;
}

wuxs::Rectangle MakeFilledRect(const wuxm::SolidColorBrush& brush, double left,
                               double top, double width, double height,
                               double radius) {
    wuxs::Rectangle shape;
    shape.Width(width);
    shape.Height(height);
    shape.RadiusX(radius);
    shape.RadiusY(radius);
    shape.Fill(brush);
    wuxc::Canvas::SetLeft(shape, left);
    wuxc::Canvas::SetTop(shape, top);
    return shape;
}

wuxs::Polygon MakeFilledPolygon(
    const wuxm::SolidColorBrush& brush,
    std::initializer_list<wf::Point> points) {
    wuxs::Polygon shape;
    shape.Fill(brush);
    for (const auto& point : points) {
        shape.Points().Append(point);
    }
    return shape;
}

wux::FrameworkElement MakeCpuIcon(const wuxm::SolidColorBrush& brush) {
    wuxc::Canvas canvas;
    canvas.Width(kIconBox);
    canvas.Height(kIconBox);
    canvas.VerticalAlignment(wux::VerticalAlignment::Center);

    // Chip body: a rounded ring with a die in the middle.
    wuxm::PathGeometry ring;
    AppendRoundedRect(ring, 3.4, 3.4, 9.2, 9.2, 2.0);
    AppendRoundedRect(ring, 5.4, 5.4, 5.2, 5.2, 1.2);
    canvas.Children().Append(MakePath(ring, brush));
    canvas.Children().Append(
        MakeFilledRect(brush, 6.7, 6.7, 2.6, 2.6, 0.6));

    // Pins on four sides.
    for (double x : {5.6, 8.0, 10.4}) {
        canvas.Children().Append(
            MakeFilledRect(brush, x - 0.7, 1.4, 1.4, 2.1, 0.7));
        canvas.Children().Append(
            MakeFilledRect(brush, x - 0.7, 12.5, 1.4, 2.1, 0.7));
    }
    for (double y : {5.6, 8.0, 10.4}) {
        canvas.Children().Append(
            MakeFilledRect(brush, 1.4, y - 0.7, 2.1, 1.4, 0.7));
        canvas.Children().Append(
            MakeFilledRect(brush, 12.5, y - 0.7, 2.1, 1.4, 0.7));
    }
    return canvas;
}

wux::FrameworkElement MakeGpuIcon(const wuxm::SolidColorBrush& brush) {
    wuxc::Canvas canvas;
    canvas.Width(kIconBox);
    canvas.Height(kIconBox);
    canvas.VerticalAlignment(wux::VerticalAlignment::Center);

    // Monitor: screen ring, stand and base.
    wuxm::PathGeometry ring;
    AppendRoundedRect(ring, 1.6, 2.3, 12.8, 8.0, 1.6);
    AppendRoundedRect(ring, 2.8, 3.5, 10.4, 5.0, 0.8);
    canvas.Children().Append(MakePath(ring, brush));
    canvas.Children().Append(MakeFilledRect(brush, 7.2, 10.3, 1.6, 1.9, 0.3));
    canvas.Children().Append(MakeFilledRect(brush, 5.6, 12.2, 4.8, 1.4, 0.7));
    return canvas;
}

wux::FrameworkElement MakeRamIcon(const wuxm::SolidColorBrush& brush) {
    wuxc::Canvas canvas;
    canvas.Width(kIconBox);
    canvas.Height(kIconBox);
    canvas.VerticalAlignment(wux::VerticalAlignment::Center);

    // Memory card with a chamfered corner and three contact slots.
    wuxm::PathGeometry geometry;

    wuxm::PathFigure card;
    card.StartPoint(wf::Point{5.0, 2.3});
    auto segments = card.Segments();
    auto lineTo = [&](double x, double y) {
        wuxm::LineSegment segment;
        segment.Point(wf::Point{x, y});
        segments.Append(segment);
    };
    auto arcTo = [&](double x, double y, double radius) {
        wuxm::ArcSegment segment;
        segment.Point(wf::Point{x, y});
        segment.Size(wf::Size{radius, radius});
        segment.SweepDirection(wuxm::SweepDirection::Clockwise);
        segment.IsLargeArc(false);
        segments.Append(segment);
    };
    lineTo(12.8, 2.3);
    arcTo(14.6, 4.1, 1.8);
    lineTo(14.6, 10.5);
    arcTo(12.8, 12.3, 1.8);
    lineTo(5.2, 12.3);
    arcTo(3.4, 10.5, 1.8);
    lineTo(3.4, 3.9);
    lineTo(5.0, 2.3);
    card.IsClosed(true);
    geometry.Figures().Append(card);

    AppendRoundedRect(geometry, 5.3, 3.7, 1.5, 2.5, 0.3);
    AppendRoundedRect(geometry, 7.6, 3.7, 1.5, 2.5, 0.3);
    AppendRoundedRect(geometry, 9.9, 3.7, 1.5, 2.5, 0.3);
    canvas.Children().Append(MakePath(geometry, brush));

    // Contacts at the bottom edge.
    for (double x : {4.0, 6.1, 8.2, 10.3, 12.4}) {
        canvas.Children().Append(MakeFilledRect(brush, x, 12.4, 1.2, 1.2, 0.3));
    }
    return canvas;
}

wux::FrameworkElement MakeArrowIcon(const wuxm::SolidColorBrush& brush, bool up) {
    wuxc::Canvas canvas;
    canvas.Width(kIconBox);
    canvas.Height(kIconBox);
    canvas.VerticalAlignment(wux::VerticalAlignment::Center);

    if (up) {
        canvas.Children().Append(
            MakeFilledPolygon(brush, {{4.2, 7.3}, {8.0, 3.1}, {11.8, 7.3}}));
        canvas.Children().Append(MakeFilledRect(brush, 7.15, 6.1, 1.7, 7.4, 0.2));
    } else {
        canvas.Children().Append(
            MakeFilledPolygon(brush, {{4.2, 8.5}, {8.0, 12.7}, {11.8, 8.5}}));
        canvas.Children().Append(MakeFilledRect(brush, 7.15, 1.7, 1.7, 7.4, 0.2));
    }
    canvas.Children().Append(MakeFilledRect(brush, 3.8, 13.0, 8.4, 1.4, 0.7));
    return canvas;
}

wux::FrameworkElement MakeIcon(IconKind kind, Color color) {
    auto brush = wuxm::SolidColorBrush(color);
    switch (kind) {
        case IconKind::Cpu:
            return MakeCpuIcon(brush);
        case IconKind::Gpu:
            return MakeGpuIcon(brush);
        case IconKind::Ram:
            return MakeRamIcon(brush);
        case IconKind::ArrowDown:
            return MakeArrowIcon(brush, false);
        case IconKind::ArrowUp:
            return MakeArrowIcon(brush, true);
    }
    return MakeCpuIcon(brush);
}

void SetText(wuxc::TextBlock const& block, const std::wstring& value) {
    if (!block) {
        return;
    }
    auto text = winrt::hstring(value);
    if (block.Text() != text) {
        block.Text(text);
    }
}

void UpdateRightMargin() {
    if (!g_host) {
        return;
    }
    double scale = 1;
    if (g_xamlRoot) {
        scale = g_xamlRoot.RasterizationScale();
    }
    if (scale <= 0) {
        scale = 1;
    }

    if (!g_taskbarWindow || !IsWindow(g_taskbarWindow)) {
        g_taskbarWindow = FindWindowW(L"Shell_TrayWnd", nullptr);
    }

    double margin = 250;
    if (g_taskbarWindow) {
        if (HWND tray = FindWindowExW(g_taskbarWindow, nullptr,
                                      L"TrayNotifyWnd", nullptr)) {
            RECT taskbarRect{};
            RECT trayRect{};
            if (GetWindowRect(g_taskbarWindow, &taskbarRect) &&
                GetWindowRect(tray, &trayRect)) {
                // Align the widget's right edge with the tray's left edge in
                // the RootGrid's coordinate space, whatever its padding is.
                const double rootWidth =
                    g_rootGrid ? g_rootGrid.ActualWidth() : 0;
                const double trayLeft =
                    (trayRect.left - taskbarRect.left) / scale;
                margin = rootWidth > 0 ? rootWidth - trayLeft + 6
                                       : (trayRect.right - trayRect.left) / scale + 6;
            }
        }
    }
    if (margin < 0) {
        margin = 0;
    }
    g_rightMargin = margin;
    const auto current = g_host.Margin();
    if (std::abs(current.Right - margin) >= 0.5) {
        g_host.Margin(wux::Thickness{0, 0, margin, 0});
    }
}

void ApplyWidthLimits(const Snapshot& snapshot, const Settings& settings) {
    if (!g_rootGrid || !g_host || !g_row) {
        return;
    }
    const double rootWidth = g_rootGrid.ActualWidth();
    if (rootWidth <= 0) {
        return;
    }
    const double available = std::max(80.0, rootWidth - g_rightMargin - 14);

    if (g_netGroup) {
        g_netGroup.Visibility(settings.showNet && snapshot.netAvailable ? wux::Visibility::Visible
                                                 : wux::Visibility::Collapsed);
    }
    if (g_cpuTemp) {
        g_cpuTemp.Visibility(settings.showTemps && snapshot.cpuTemp >= 0 ? wux::Visibility::Visible
                                                  : wux::Visibility::Collapsed);
    }
    if (g_gpuTemp) {
        g_gpuTemp.Visibility(settings.showTemps && snapshot.gpuTemp >= 0 ? wux::Visibility::Visible
                                                  : wux::Visibility::Collapsed);
    }
    if (g_gpuGroup) {
        g_gpuGroup.Visibility(snapshot.gpuAvailable ? wux::Visibility::Visible : wux::Visibility::Collapsed);
    }

    auto desiredWidth = [&]() {
        g_row.Measure(wf::Size{std::numeric_limits<float>::infinity(), 64});
        return g_row.DesiredSize().Width;
    };

    if (desiredWidth() <= available) {
        return;
    }
    if (g_netGroup) {
        g_netGroup.Visibility(wux::Visibility::Collapsed);
    }
    if (desiredWidth() <= available) {
        return;
    }
    if (g_cpuTemp) {
        g_cpuTemp.Visibility(wux::Visibility::Collapsed);
    }
    if (g_gpuTemp) {
        g_gpuTemp.Visibility(wux::Visibility::Collapsed);
    }
    if (desiredWidth() <= available) {
        return;
    }
    if (g_gpuGroup) {
        g_gpuGroup.Visibility(wux::Visibility::Collapsed);
    }
}

void UpdateTexts() {
    if (!g_host) {
        return;
    }

    Snapshot snapshot;
    {
        std::lock_guard lock(g_snapMutex);
        snapshot = g_snap;
    }

    SetText(g_cpuValue, FormatPercent(snapshot.cpuPct));
    SetText(g_ramValue, FormatPercent(snapshot.ramPct));
    const auto settings = CurrentSettings();

    if (g_cpuTemp) {
        SetText(g_cpuTemp, settings.showTemps ? FormatTemp(snapshot.cpuTemp)
                                                : L"");
        if (settings.showTemps && snapshot.cpuTemp >= 0) {
            g_cpuTemp.Foreground(
                snapshot.cpuTemp >= settings.critTemp
                    ? g_critBrush
                    : (snapshot.cpuTemp >= settings.warnTemp ? g_warnBrush
                                                              : g_textBrush));
        } else {
            g_cpuTemp.Foreground(g_textBrush);
        }
    }

    if (snapshot.gpuAvailable) {
        SetText(g_gpuValue, FormatPercent(snapshot.gpuPct));
        if (g_gpuTemp) {
            SetText(g_gpuTemp,
                    settings.showTemps ? FormatTemp(snapshot.gpuTemp) : L"");
            if (settings.showTemps && snapshot.gpuTemp >= 0) {
                g_gpuTemp.Foreground(
                    snapshot.gpuTemp >= settings.critTemp
                        ? g_critBrush
                        : (snapshot.gpuTemp >= settings.warnTemp
                               ? g_warnBrush
                               : g_textBrush));
            } else {
                g_gpuTemp.Foreground(g_textBrush);
            }
        }
    }

    if (g_netGroup && settings.showNet && snapshot.netAvailable) {
        SetText(g_netDown, FormatRate(snapshot.netDown));
        SetText(g_netUp, FormatRate(snapshot.netUp));
    }

    ApplyWidthLimits(snapshot, settings);
}

void BuildContent();
void Tick() {
    if (g_unloading || !g_host) {
        return;
    }
    const bool dark = IsDarkTheme();
    if (dark != g_dark) {
        BuildContent();
    }
    if (!g_row) {
        return;
    }
    UpdateRightMargin();
    UpdateTexts();
}

void StartTimer() {
    if (g_timer) {
        return;
    }
    g_timer = wux::DispatcherTimer{};
    g_timer.Interval(std::chrono::seconds(CurrentSettings().intervalSeconds));
    g_timer.Tick([](auto&&, auto&&) { Tick(); });
    g_timer.Start();
}

void BuildContent() {
    if (!g_host) {
        return;
    }
    if (g_timer) {
        g_timer.Stop();
        g_timer = nullptr;
    }
    if (g_retryTimer) {
        g_retryTimer.Stop();
        g_retryTimer = nullptr;
    }
    g_host.Children().Clear();
    g_row = nullptr;
    g_cpuGroup = nullptr;
    g_gpuGroup = nullptr;
    g_ramGroup = nullptr;
    g_netGroup = nullptr;
    g_cpuValue = nullptr;
    g_cpuTemp = nullptr;
    g_gpuValue = nullptr;
    g_gpuTemp = nullptr;
    g_ramValue = nullptr;
    g_netDown = nullptr;
    g_netUp = nullptr;

    g_dark = IsDarkTheme();
    const auto palette = CurrentPalette(g_dark);
    const auto settings = CurrentSettings();
    const double size = static_cast<double>(settings.fontSize);

    g_textBrush = wuxm::SolidColorBrush(palette.text);
    g_warnBrush = wuxm::SolidColorBrush(palette.warn);
    g_critBrush = wuxm::SolidColorBrush(palette.crit);

    g_row = wuxc::StackPanel{};
    g_row.Orientation(wuxc::Orientation::Horizontal);
    g_row.VerticalAlignment(wux::VerticalAlignment::Center);
    auto makeIcon = [&](IconKind icon) {
        wuxc::Viewbox box;
        box.Child(MakeIcon(icon, palette.subtle));
        box.Width(size * kIconBox / 12);
        box.Height(box.Width());
        box.VerticalAlignment(wux::VerticalAlignment::Center);
        box.Margin(wux::Thickness{0, 0, 4, 0});
        return box;
    };
    auto makeGroup = [&](IconKind icon, const wchar_t* name) {
        wuxc::StackPanel group;
        group.Orientation(wuxc::Orientation::Horizontal);
        group.VerticalAlignment(wux::VerticalAlignment::Center);
        group.Margin(wux::Thickness{0, 0, 13, 0});
        if (settings.showIcons) {
            group.Children().Append(makeIcon(icon));
        }
        wux::Automation::AutomationProperties::SetName(group, name);
        return group;
    };

    // CPU
    g_cpuGroup = makeGroup(IconKind::Cpu, L"CPU");
    g_cpuValue = MakeTextBlock(size, palette.text);
    g_cpuGroup.Children().Append(g_cpuValue);
    if (settings.showTemps) {
        g_cpuTemp = MakeTextBlock(size, palette.text);
        g_cpuTemp.Margin(wux::Thickness{size / 2, 0, 0, 0});
        g_cpuGroup.Children().Append(g_cpuTemp);
    }
    g_row.Children().Append(g_cpuGroup);

    // GPU
    g_gpuGroup = makeGroup(IconKind::Gpu, L"GPU");
    g_gpuValue = MakeTextBlock(size, palette.text);
    g_gpuGroup.Children().Append(g_gpuValue);
    if (settings.showTemps) {
        g_gpuTemp = MakeTextBlock(size, palette.text);
        g_gpuTemp.Margin(wux::Thickness{size / 2, 0, 0, 0});
        g_gpuGroup.Children().Append(g_gpuTemp);
    }
    g_row.Children().Append(g_gpuGroup);

    // RAM
    g_ramGroup = makeGroup(IconKind::Ram, L"RAM");
    g_ramValue = MakeTextBlock(size, palette.text);
    g_ramGroup.Children().Append(g_ramValue);
    g_ramGroup.Margin(wux::Thickness{0, 0, 13, 0});
    g_row.Children().Append(g_ramGroup);

    // Network
    if (settings.showNet) {
        g_netGroup = wuxc::StackPanel{};
        g_netGroup.Orientation(wuxc::Orientation::Horizontal);
        g_netGroup.VerticalAlignment(wux::VerticalAlignment::Center);

        auto downPanel = wuxc::StackPanel{};
        downPanel.Orientation(wuxc::Orientation::Horizontal);
        downPanel.VerticalAlignment(wux::VerticalAlignment::Center);
        downPanel.Margin(wux::Thickness{0, 0, 9, 0});
        downPanel.Children().Append(makeIcon(IconKind::ArrowDown));
        g_netDown = MakeTextBlock(size, palette.text);
        downPanel.Children().Append(g_netDown);

        auto upPanel = wuxc::StackPanel{};
        upPanel.Orientation(wuxc::Orientation::Horizontal);
        upPanel.VerticalAlignment(wux::VerticalAlignment::Center);
        upPanel.Children().Append(makeIcon(IconKind::ArrowUp));
        g_netUp = MakeTextBlock(size, palette.text);
        upPanel.Children().Append(g_netUp);

        g_netGroup.Children().Append(downPanel);
        g_netGroup.Children().Append(upPanel);
        g_netGroup.Margin(wux::Thickness{0, 0, 6, 0});
        g_row.Children().Append(g_netGroup);
    }

    g_host.Children().Append(g_row);
    SetText(g_cpuValue, L"-");
    SetText(g_gpuValue, L"-");
    SetText(g_ramValue, L"-");
    SetText(g_cpuTemp, L"-");
    SetText(g_gpuTemp, L"-");
    StartTimer();
}

void CreateUi() {
    if (!g_rootGrid || g_unloading) {
        return;
    }

    if (!g_host) {
        g_host = wuxc::Grid{};
        g_host.Name(kHostName);
        g_host.IsHitTestVisible(false);
        g_host.HorizontalAlignment(wux::HorizontalAlignment::Right);
        g_host.VerticalAlignment(wux::VerticalAlignment::Center);
        wuxc::Canvas::SetZIndex(g_host, 10000);
        wuxc::Grid::SetColumnSpan(
            g_host, static_cast<int>(std::max(
                        1u, g_rootGrid.ColumnDefinitions().Size())));
    }

    uint32_t index = 0;
    if (!g_rootGrid.Children().IndexOf(g_host, index)) {
        g_rootGrid.Children().Append(g_host);
    }

    BuildContent();
    UpdateRightMargin();
    UpdateTexts();
}

wux::FrameworkElement FindDirectChildByName(wux::DependencyObject const& parent,
                                            const wchar_t* name) {
    const int count = wuxm::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(parent, i);
        auto element = child.try_as<wux::FrameworkElement>();
        if (element && element.Name() == name) {
            return element;
        }
    }
    return nullptr;
}

void AttachToTaskbarFrame(wux::FrameworkElement frame) {
    if (!frame || g_unloading) {
        return;
    }
    HWND primary = FindPrimaryTaskbarOnCurrentThread();
    if (!primary) {
        return;  // secondary taskbar
    }
    g_taskbarWindow = primary;
    EnsureMessageWindow();

    auto rootElement = FindDirectChildByName(frame, L"RootGrid");
    auto root = rootElement ? rootElement.try_as<wuxc::Grid>() : nullptr;
    if (!root) {
        g_pendingFrame = frame;
        if (!g_retryTimer) {
            g_retryCount = 0;
            g_retryTimer = wux::DispatcherTimer{};
            g_retryTimer.Interval(std::chrono::milliseconds(400));
            g_retryTimer.Tick([](auto&&, auto&&) {
                if (g_unloading) {
                    if (g_retryTimer) {
                        g_retryTimer.Stop();
                        g_retryTimer = nullptr;
                    }
                    return;
                }
                if (++g_retryCount > 25 || !g_pendingFrame) {
                    Wh_Log(L"SysBar: RootGrid not found, giving up");
                    if (g_retryTimer) {
                        g_retryTimer.Stop();
                        g_retryTimer = nullptr;
                    }
                    g_pendingFrame = nullptr;
                    return;
                }
                AttachToTaskbarFrame(g_pendingFrame);
            });
            g_retryTimer.Start();
        }
        return;
    }

    g_pendingFrame = nullptr;
    if (g_retryTimer) {
        g_retryTimer.Stop();
        g_retryTimer = nullptr;
    }

    if (g_rootGrid && winrt::get_abi(g_rootGrid) != winrt::get_abi(root)) {
        // A new taskbar tree was created (explorer restart or theme reload).
        g_host = nullptr;
        g_row = nullptr;
        g_cpuGroup = nullptr;
        g_gpuGroup = nullptr;
        g_ramGroup = nullptr;
        g_netGroup = nullptr;
        g_cpuValue = nullptr;
        g_cpuTemp = nullptr;
        g_gpuValue = nullptr;
        g_gpuTemp = nullptr;
        g_ramValue = nullptr;
        g_netDown = nullptr;
        g_netUp = nullptr;
        if (g_timer) {
            g_timer.Stop();
            g_timer = nullptr;
        }
    }

    g_rootGrid = root;
    g_xamlRoot = frame.XamlRoot();
    CreateUi();
    g_attached = true;
    DebugLog(L"widget attached: root=%.0fx%.0f margin=%.0f scale=%.2f",
             g_rootGrid.ActualWidth(), g_rootGrid.ActualHeight(), g_rightMargin,
             g_xamlRoot ? g_xamlRoot.RasterizationScale() : -1.0);
    Wh_Log(L"SysBar: widget attached");
}

// ---------------------------------------------------------------------------
// Taskbar-frame hook
// ---------------------------------------------------------------------------

using TaskbarFrame_Constructor_t = void*(WINAPI*)(void*);
TaskbarFrame_Constructor_t TaskbarFrame_Constructor_Original = nullptr;

void* WINAPI TaskbarFrame_Constructor_Hook(void* pThis) {
    void* result = TaskbarFrame_Constructor_Original(pThis);
    if (g_unloading) {
        return result;
    }

    try {
        wux::FrameworkElement frame{nullptr};
        reinterpret_cast<::IUnknown**>(pThis)[1]->QueryInterface(
            winrt::guid_of<wux::FrameworkElement>(), winrt::put_abi(frame));
        if (frame && FindPrimaryTaskbarOnCurrentThread()) {
            EnsureMessageWindow();
            if (!g_msgWindow || g_unloading) return result;
            std::erase_if(g_loadedHandlers, [](const auto& entry) { return !entry.first.get(); });
            auto token = frame.Loaded([](wf::IInspectable const& sender, auto&&) {
                if (g_unloading) {
                    return;
                }
                auto element = sender.try_as<wux::FrameworkElement>();
                if (element) {
                    AttachToTaskbarFrame(element);
                }
            });
            g_loadedHandlers.emplace_back(winrt::make_weak(frame), token);
        }
    } catch (...) {
        Wh_Log(L"SysBar: Loaded hook failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
    }
    return result;
}

// The constructor and Loaded event can both precede Windhawk at sign-in.
// Recover on the next layout pass, but modify the tree outside MeasureOverride.
using MeasureFrame_t = int(WINAPI*)(void*, wf::Size, wf::Size*);
MeasureFrame_t MeasureFrame_Original = nullptr;
int WINAPI MeasureFrame_Hook(void* self, wf::Size size, wf::Size* resultSize) {
    int result = MeasureFrame_Original(self, size, resultSize);
    if (SUCCEEDED(result) && !g_unloading && !g_attached &&
        FindPrimaryTaskbarOnCurrentThread() && !g_measuredFrame && !g_retryTimer) {
        wux::FrameworkElement frame{nullptr};
        static_cast<IUnknown*>(self)->QueryInterface(
            winrt::guid_of<wux::FrameworkElement>(), winrt::put_abi(frame));
        if (frame) {
            EnsureMessageWindow();
            if (HWND window = g_msgWindow.load()) {
                g_measuredFrame = frame;
                if (!PostMessageW(window, kRunOnTaskbarThreadMessage, 2, 0)) {
                    g_measuredFrame = nullptr;
                }
            }
        }
    }
    return result;
}

bool HookTaskbarViewSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {{
        {LR"(public: __cdecl winrt::Taskbar::implementation::TaskbarFrame::TaskbarFrame(void))"},
        &TaskbarFrame_Constructor_Original,
        TaskbarFrame_Constructor_Hook,
    }, {
        {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))"},
        &MeasureFrame_Original,
        MeasureFrame_Hook,
        true,  // Keep constructor support on builds without this symbol.
    }};
    return WindhawkUtils::HookSymbols(module, hooks, std::size(hooks));
}

HMODULE GetTaskbarViewModule() {
    HMODULE module = GetModuleHandleW(L"Taskbar.View.dll");
    if (!module) {
        module = GetModuleHandleW(L"ExplorerExtensions.dll");
    }
    return module;
}

bool g_taskbarViewHooked = false;

bool HandleLoadedModuleIfTaskbarView(HMODULE module, LPCWSTR fileName,
                                    bool applyImmediately) {
    if (!module || GetTaskbarViewModule() != module) {
        return false;
    }
    if (g_taskbarViewHooked) {
        return true;
    }
    Wh_Log(L"SysBar: taskbar view loaded (%s)",
           fileName ? fileName : L"already loaded");
    if (!HookTaskbarViewSymbols(module)) {
        Wh_Log(L"SysBar: taskbar view symbols unavailable");
        return false;
    }
    g_taskbarViewHooked = true;
    if (applyImmediately) {
        Wh_ApplyHookOperations();
    }
    return true;
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original = nullptr;

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR fileName, HANDLE file,
                                   DWORD flags) {
    HMODULE module = LoadLibraryExW_Original(fileName, file, flags);
    const DWORD lastError = GetLastError();
    if (module && !g_taskbarViewHooked) {
        HandleLoadedModuleIfTaskbarView(module, fileName, true);
    }
    SetLastError(lastError);
    return module;
}

// ---------------------------------------------------------------------------
// Cross-thread runner for the taskbar UI thread
// ---------------------------------------------------------------------------

// When Explorer already owns a fully laid out taskbar, no constructor or
// measure callback is guaranteed. Resolve the existing TaskbarHost as in
// Windhawk's taskbar-notification-icon-spacing mod (Windows x64).
void* g_siteVtable = nullptr;
void* (WINAPI* g_getHost)(void*, void**) = nullptr;
BYTE* g_frameHeight = nullptr;
void (WINAPI* g_releaseHost)(void*) = nullptr;
UINT g_attachMessage = 0;

void AttachExistingFrame(HWND tray) {
    if (g_attached || g_unloading) return;
    HWND band = static_cast<HWND>(GetPropW(tray, L"TaskbandHWND"));
    auto site = band ? reinterpret_cast<void**>(GetWindowLongPtrW(band, 0)) : nullptr;
    if (!site) return;
    int i = 0;
    while (i < 20 && site[i] != g_siteVtable) ++i;
    if (i == 20) return;
    // FrameHeight accesses the frame member via `sub rsp,N; add rcx,offset`.
    const BYTE* b = g_frameHeight;
    if (b[0] != 0x48 || b[1] != 0x83 || b[2] != 0xec ||
        b[4] != 0x48 || b[5] != 0x83 || b[6] != 0xc1 || b[7] > 0x7f) return;
    void* host[2]{};
    g_getHost(site + i, host);
    wux::FrameworkElement element{nullptr};
    if (host[0] && host[1]) {
        auto unk = *reinterpret_cast<IUnknown**>(static_cast<BYTE*>(host[0]) + b[7]);
        if (unk) unk->QueryInterface(winrt::guid_of<wux::FrameworkElement>(),
                                     winrt::put_abi(element));
    }
    if (host[1]) g_releaseHost(host[1]);
    if (element) {
        auto root = element.XamlRoot();
        auto frame = root && root.Content() ?
            root.Content().try_as<wux::FrameworkElement>() : nullptr;
        if (frame && winrt::get_class_name(frame) == L"Taskbar.TaskbarFrame")
            AttachToTaskbarFrame(frame);
        else if (winrt::get_class_name(element) == L"Taskbar.TaskbarFrame")
            AttachToTaskbarFrame(element);
    }
}

void RecoverExistingTaskbar() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(tray, &pid);
    HMODULE module = GetModuleHandleW(L"taskbar.dll");
    if (!tid || pid != GetCurrentProcessId() || !module || g_attached) return;
    WindhawkUtils::SYMBOL_HOOK symbols[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"}, &g_siteVtable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"}, &g_getHost},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"}, &g_frameHeight},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"}, &g_releaseHost},
    };
    if (!WindhawkUtils::HookSymbols(module, symbols, std::size(symbols))) return;
    g_attachMessage = RegisterWindowMessageW(L"SysBar.AttachExistingTaskbar");
    if (!g_attachMessage) return;
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC,
        [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            if (code == HC_ACTION) {
                auto msg = reinterpret_cast<const CWPSTRUCT*>(lp);
                if (msg->message == g_attachMessage) {
                    try { AttachExistingFrame(msg->hwnd); }
                    catch (...) { Wh_Log(L"SysBar: existing taskbar unavailable"); }
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }, nullptr, tid);
    if (hook) {
        SendMessageW(tray, g_attachMessage, 0, 0);
        UnhookWindowsHookEx(hook);
    }
}

void DestroyUi();

LRESULT CALLBACK MessageWindowProc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
    if (message == kRunOnTaskbarThreadMessage) {
        if (g_unloading) {
            if (wparam == 1) DestroyUi();
        } else if (wparam == 2 && g_measuredFrame) {
            auto frame = std::move(g_measuredFrame);
            try {
                if (!g_attached && frame.XamlRoot()) AttachToTaskbarFrame(frame);
            } catch (...) {
                Wh_Log(L"SysBar: layout attach failed: %08X",
                       static_cast<unsigned>(winrt::to_hresult()));
            }
        } else if (wparam == 0 && g_host) {
            BuildContent();
            UpdateTexts();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void EnsureMessageWindow() {
    if (g_msgWindow) {
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = MessageWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"SysBarMonitorMessageWindow";
        registered = RegisterClassW(&windowClass) != 0;
    }
    if (!registered) {
        return;
    }
    g_msgWindow = CreateWindowExW(0, L"SysBarMonitorMessageWindow", L"", 0, 0,
                                  0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    Wh_Log(L"SysBar: init");
    g_unloading = false;
    g_taskbarViewHooked = false;
    LoadSettings();
    DebugLog(L"init");

    if (HMODULE module = GetTaskbarViewModule()) {
        if (!HandleLoadedModuleIfTaskbarView(module, nullptr, false)) {
            Wh_Log(L"SysBar: taskbar view already loaded, symbols missing");
            return FALSE;
        }
    } else {
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        if (!kernelBase) {
            kernelBase = GetModuleHandleW(L"kernel32.dll");
        }
        auto loadLibraryEx =
            kernelBase ? reinterpret_cast<LoadLibraryExW_t>(
                             GetProcAddress(kernelBase, "LoadLibraryExW"))
                       : nullptr;
        if (!loadLibraryEx ||
            !WindhawkUtils::SetFunctionHook(loadLibraryEx, LoadLibraryExW_Hook,
                                            &LoadLibraryExW_Original)) {
            Wh_Log(L"SysBar: LoadLibraryExW hook failed");
            return FALSE;
        }
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    DebugLog(L"after init");
    StartWorker();
    if (!g_taskbarViewHooked) {
        if (HMODULE module = GetTaskbarViewModule()) {
            HandleLoadedModuleIfTaskbarView(module, nullptr, true);
        }
    }
    RecoverExistingTaskbar();
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    if (HWND window = g_msgWindow.load()) {
        PostMessageW(window, kRunOnTaskbarThreadMessage, 0, 0);
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"SysBar: before uninit");
    g_unloading = true;
    StopWorker();

    if (HWND window = g_msgWindow.load()) {
        SendMessageW(window, kRunOnTaskbarThreadMessage, 1, 0);
    }
}

namespace {
void DestroyUi() {
        for (auto& [weakFrame, token] : g_loadedHandlers) {
            if (auto frame = weakFrame.get()) frame.Loaded(token);
        }
        g_loadedHandlers.clear();
        if (g_timer) {
            g_timer.Stop();
            g_timer = nullptr;
        }
        if (g_retryTimer) {
            g_retryTimer.Stop();
            g_retryTimer = nullptr;
        }
        if (g_rootGrid && g_host) {
            uint32_t index = 0;
            if (g_rootGrid.Children().IndexOf(g_host, index)) {
                g_rootGrid.Children().RemoveAt(index);
            }
        }
        g_host = nullptr;
        g_row = nullptr;
        g_cpuGroup = nullptr;
        g_gpuGroup = nullptr;
        g_ramGroup = nullptr;
        g_netGroup = nullptr;
        g_cpuValue = nullptr;
        g_cpuTemp = nullptr;
        g_gpuValue = nullptr;
        g_gpuTemp = nullptr;
        g_ramValue = nullptr;
        g_netDown = nullptr;
        g_netUp = nullptr;
        g_textBrush = nullptr;
        g_warnBrush = nullptr;
        g_critBrush = nullptr;
        g_rootGrid = nullptr;
        g_attached = false;
        g_measuredFrame = nullptr;
        g_pendingFrame = nullptr;
        g_xamlRoot = nullptr;
        if (g_msgWindow) {
            DestroyWindow(g_msgWindow);
            g_msgWindow = nullptr;
            UnregisterClassW(L"SysBarMonitorMessageWindow",
                             GetModuleHandleW(nullptr));
        }
}
}  // namespace

void Wh_ModUninit() {
    Wh_Log(L"SysBar: uninit");
    StopWorker();
}



