// ==WindhawkMod==
// @id              memory-clean
// @name            Memory Clean
// @description     Memory use next to the taskbar clock; frees memory and ends programs
// @version         0.1.1
// @author          Hyeon
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -ldwmapi -lgdi32 -lntdll -lole32 -loleaut32 -lpsapi -lruntimeobject -lshell32 -lshlwapi -ltaskschd -luuid -lversion
// ==/WindhawkMod==

// SPDX-License-Identifier: GPL-3.0-only
// Source code is published under The GNU General Public License v3.0.
// Third-party sources and licenses: THIRD_PARTY.md in the repository root.

// ==WindhawkModReadme==
/*
# Memory Clean

A RAM module next to the taskbar clock shows how much memory is in use. Clicking it
opens a window beside it: free memory now, see which programs use the most memory and
CPU and end them, and set automatic cleaning and programs to leave alone.

Clearing the standby list, the file cache and the other system-wide lists needs
administrator rights. An optional helper, installed once with one UAC prompt, does
that part through a scheduled task and exits as soon as it is done. Without it only
the working sets of your own programs are trimmed.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- widget:
  - refreshSeconds: 2
    $name: 새로 고침 간격(초)
  $name: 위젯
- pill:
  - theme: system
    $name: 테마
    $description: 위젯과 창의 라이트·다크. Bloom 애드온으로 쓰면 Bloom의 테마를 따릅니다
    $options:
    - system: Windows 작업표시줄 테마
    - dark: 다크
    - light: 라이트
  - glass: true
    $name: 유리 효과
    $description: 끄면 창이 블러 없이 테마 색으로 칠해집니다
  $name: 모양
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <dwmapi.h>
#include <psapi.h>
#include <robuffer.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <taskschd.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

#include <windhawk_utils.h>

#include "../common/memclean_protocol.h"

namespace wf = winrt::Windows::Foundation;
namespace wdj = winrt::Windows::Data::Json;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;
using winrt::box_value;
using namespace memclean;

namespace {

// ============================================================================
// Basics: unloading, language, paths, files
// ============================================================================

std::atomic<bool> g_unloading;
std::atomic<int> g_pending;  // queued taskbar work and worker threads still running

bool g_korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
PCWSTR T(PCWSTR ko, PCWSTR en) {
    return g_korean ? ko : en;
}

std::wstring Format(PCWSTR format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring DataDir() {
    return KnownFolder(FOLDERID_LocalAppData) + L"\\" + kDataFolder;
}

// The folder this DLL was loaded from: the add-on's run copy, which also holds the helper.
std::wstring ModuleDir() {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&ModuleDir, &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH))
        return {};
    PathRemoveFileSpecW(path);
    return path;
}

std::optional<std::wstring> ReadText(const std::wstring& path, DWORD maxBytes = 256 * 1024) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= maxBytes;
    if (ok) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
    }
    CloseHandle(file);
    if (!ok) return std::nullopt;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    return text;
}

// Whole or not at all: a temporary file, then a rename over the old one.
bool WriteText(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

uint64_t NowFiletime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::optional<wdj::JsonObject> ParseObject(const std::optional<std::wstring>& text) {
    wdj::JsonObject object{nullptr};
    if (!text || !wdj::JsonObject::TryParse(*text, object)) return std::nullopt;
    return object;
}

// ============================================================================
// Preferences (prefs.json in the data folder; the helper reads "exclude")
// ============================================================================

enum class IconStyle { kLine, kFill, kSolid };

struct Prefs {
    IconStyle icon = IconStyle::kFill;
    unsigned areas = kDefaultAreas;
    bool autoPercentOn = false;
    int autoPercent = 90;
    bool autoIntervalOn = false;
    int autoMinutes = 30;
    bool sortCpu = false;
    // Lower-case file names, "chrome.exe". Bloom is left alone from the start, so the
    // launcher opens at once after a clean.
    std::vector<std::wstring> exclude{L"bloom.exe"};
    uint64_t lastCleanAt = 0;           // FILETIME, UTC
    uint64_t lastFreed = 0;             // bytes
};
Prefs g_prefs;  // taskbar thread

bool PlainExeName(const std::wstring& name) {
    return !name.empty() && name.size() <= 260 && name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos &&
           name.size() > 4 && Lower(name).ends_with(L".exe");
}

// Version 2 added Bloom to the exclusions; an older file gets it once. A missing or older
// file is written at once, so the helper reads the same list.
constexpr int kPrefsVersion = 2;

void SavePrefs();

void LoadPrefs() {
    Prefs p;
    bool older = true;
    if (auto o = ParseObject(ReadText(DataDir() + L"\\" + kPrefsFile))) {
        auto& obj = *o;
        p.icon = IconStyle(std::clamp(int(obj.GetNamedNumber(L"icon", 1)), 0, 2));
        p.areas = unsigned(obj.GetNamedNumber(L"areas", kDefaultAreas)) & kAllAreas;
        p.autoPercentOn = obj.GetNamedBoolean(L"autoPercentOn", false);
        p.autoPercent = std::clamp(int(obj.GetNamedNumber(L"autoPercent", 90)), 50, 99);
        p.autoIntervalOn = obj.GetNamedBoolean(L"autoIntervalOn", false);
        p.autoMinutes = std::clamp(int(obj.GetNamedNumber(L"autoMinutes", 30)), 1, 1440);
        p.sortCpu = obj.GetNamedBoolean(L"sortCpu", false);
        p.lastCleanAt = uint64_t(obj.GetNamedNumber(L"lastCleanAt", 0));
        p.lastFreed = uint64_t(obj.GetNamedNumber(L"lastFreed", 0));
        older = int(obj.GetNamedNumber(L"v", 1)) < kPrefsVersion;
        if (auto list = obj.TryLookup(L"exclude"); list && list.ValueType() == wdj::JsonValueType::Array) {
            if (!older) p.exclude.clear();
            for (auto v : list.GetArray()) {
                if (v.ValueType() != wdj::JsonValueType::String) continue;
                std::wstring name = Lower(std::wstring(v.GetString()));
                if (PlainExeName(name) && p.exclude.size() < 200 &&
                    std::ranges::find(p.exclude, name) == p.exclude.end())
                    p.exclude.push_back(name);
            }
        }
    }
    g_prefs = p;
    if (older) SavePrefs();
}

void SavePrefs() {
    wdj::JsonObject obj;
    auto& p = g_prefs;
    obj.SetNamedValue(L"v", wdj::JsonValue::CreateNumberValue(kPrefsVersion));
    obj.SetNamedValue(L"icon", wdj::JsonValue::CreateNumberValue(int(p.icon)));
    obj.SetNamedValue(L"areas", wdj::JsonValue::CreateNumberValue(p.areas));
    obj.SetNamedValue(L"autoPercentOn", wdj::JsonValue::CreateBooleanValue(p.autoPercentOn));
    obj.SetNamedValue(L"autoPercent", wdj::JsonValue::CreateNumberValue(p.autoPercent));
    obj.SetNamedValue(L"autoIntervalOn", wdj::JsonValue::CreateBooleanValue(p.autoIntervalOn));
    obj.SetNamedValue(L"autoMinutes", wdj::JsonValue::CreateNumberValue(p.autoMinutes));
    obj.SetNamedValue(L"sortCpu", wdj::JsonValue::CreateBooleanValue(p.sortCpu));
    obj.SetNamedValue(L"lastCleanAt", wdj::JsonValue::CreateNumberValue(double(p.lastCleanAt)));
    obj.SetNamedValue(L"lastFreed", wdj::JsonValue::CreateNumberValue(double(p.lastFreed)));
    wdj::JsonArray list;
    for (auto& name : p.exclude) list.Append(wdj::JsonValue::CreateStringValue(name));
    obj.SetNamedValue(L"exclude", list);
    CreateDirectoryW(DataDir().c_str(), nullptr);
    if (!WriteText(DataDir() + L"\\" + kPrefsFile, std::wstring(obj.Stringify())))
        Wh_Log(L"prefs not written: %u", GetLastError());
}

// ============================================================================
// Look: Bloom's light or dark and glass, as Bloom hands them to its add-ons
// ============================================================================

// Bloom's design tokens: zinc greys, one ink
// whose alpha makes every overlay, and Bloom's rose as the accent (dark #FF7A9E, light #E04E78).
struct Palette {
    uint32_t win, pop, btn, btnHover, fg, muted, faint, ok, warn, dangerText, dangerBtn, hilite, ink, accent;
    uint32_t ramInk, ramWarn, ramDanger;  // the taskbar icon, as the taskbar add-on colours its usage
    double tintOpacity, tintLuminosity;
};
constexpr Palette kDark{0xFF18181B, 0xFF27272A, 0xFF3F3F46, 0xFF52525B, 0xFFECEDEE, 0xFFA1A1AA, 0x99ECEDEE,
                        0xFF17C964, 0xFFF7B750, 0xFFE77578, 0xFFC62F35, 0x14FFFFFF, 0x00FFFFFF, 0xFFFF7A9E,
                        0xFFECEDEE, 0xFFF7B750, 0xFFE5484D, 0.75, 0.85};
constexpr Palette kLight{0xFFF4F4F5, 0xFFFAFAFA, 0xFFFFFFFF, 0xFFF4F4F5, 0xFF11181C, 0xFF3F3F46, 0xFF52525B,
                         0xFF0E793C, 0xFF62420E, 0xFFB3363A, 0xFFDC3E42, 0x99FAFAFA, 0x00111111, 0xFFE04E78,
                         0xFF11181C, 0xFFB7791F, 0xFFDC3E42, 0.75, 0.9};
constexpr double kWarnPercent = 80, kDangerPercent = 90;

struct Look {
    bool known = false;  // from Bloom; else the taskbar's own theme
    bool dark = true;
    bool glass = true;
};
Look g_look;

// The add-on's own settings (pill.theme, pill.glass), which Bloom sets to its theme. With
// "system" the widget follows the taskbar it sits on, PillTaskbar's or Windows' own.
Look ReadLook() {
    Look look;
    PCWSTR theme = Wh_GetStringSetting(L"pill.theme");
    if (theme && (!wcscmp(theme, L"dark") || !wcscmp(theme, L"light"))) {
        look.known = true;
        look.dark = !wcscmp(theme, L"dark");
    }
    Wh_FreeStringSetting(theme);
    look.glass = Wh_GetIntSetting(L"pill.glass") != 0;
    return look;
}

const Palette& PaletteFor(const wux::FrameworkElement& onTaskbar) {
    bool dark = g_look.known ? g_look.dark
                             : !onTaskbar || onTaskbar.ActualTheme() != wux::ElementTheme::Light;
    return dark ? kDark : kLight;
}

wuxm::SolidColorBrush Brush(uint32_t argb) {
    return wuxm::SolidColorBrush(winrt::Windows::UI::Color{BYTE(argb >> 24), BYTE(argb >> 16), BYTE(argb >> 8), BYTE(argb)});
}

uint32_t WithAlpha(uint32_t argb, BYTE alpha) {
    return (argb & 0x00FFFFFF) | (uint32_t(alpha) << 24);
}

// The ink at a percent of alpha: the tokens' w5, w8, w10, w15, w25.
uint32_t Ink(const Palette& c, int percent) {
    return WithAlpha(c.ink, BYTE(percent * 255 / 100));
}

wuxm::Brush GlassBrush(const Palette& c) {
    if (!g_look.glass) return Brush(c.pop);
    auto color = Brush(c.pop).Color();
    wuxm::AcrylicBrush glass;
    glass.BackgroundSource(wuxm::AcrylicBackgroundSource::HostBackdrop);
    glass.TintColor(color);
    glass.TintOpacity(c.tintOpacity);
    glass.TintLuminosityOpacity(c.tintLuminosity);
    glass.FallbackColor(color);
    return glass;
}

// Bars are the accent, solid; above 90 % the usage bar turns red.
uint32_t BarColor(const Palette& c, double percent) {
    return percent >= kDangerPercent ? c.dangerBtn : c.accent;
}

// ============================================================================
// Memory numbers
// ============================================================================

struct Mem {
    double percent = 0;
    uint64_t total = 0, used = 0, commit = 0, commitLimit = 0, cache = 0;
};

Mem ReadMem() {
    Mem m;
    MEMORYSTATUSEX status{sizeof(status)};
    if (GlobalMemoryStatusEx(&status) && status.ullTotalPhys) {
        m.total = status.ullTotalPhys;
        m.used = status.ullTotalPhys - status.ullAvailPhys;
        m.percent = 100.0 * double(m.used) / double(m.total);
    }
    PERFORMANCE_INFORMATION perf{sizeof(perf)};
    if (GetPerformanceInfo(&perf, sizeof(perf))) {
        m.commit = uint64_t(perf.CommitTotal) * perf.PageSize;
        m.commitLimit = uint64_t(perf.CommitLimit) * perf.PageSize;
        m.cache = uint64_t(perf.SystemCache) * perf.PageSize;
    }
    return m;
}

std::wstring Bytes(uint64_t bytes) {
    double gb = double(bytes) / (1024.0 * 1024 * 1024);
    if (gb >= 1) return Format(L"%.1f GB", gb);
    return Format(L"%.0f MB", double(bytes) / (1024.0 * 1024));
}

std::wstring Gb(uint64_t bytes) {
    return Format(L"%.1f", double(bytes) / (1024.0 * 1024 * 1024));
}

// ============================================================================
// Taskbar windows and their XAML (CTaskBand::GetTaskbarHost, as PillTaskbar finds it)
// ============================================================================

void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;
using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;
GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;
void* TaskbarHost_FrameHeight_Original;
using Decref_t = void(WINAPI*)(void* pThis);
Decref_t std__Ref_count_base__Decref_Original;

bool HookTaskbarDll() {
    HMODULE module = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"}, &TaskbarHost_FrameHeight_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"}, &std__Ref_count_base__Decref_Original},
    };
    return WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
}

bool IsSecondary(HWND hwnd) {
    wchar_t cls[32];
    return GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

std::vector<HWND> Taskbars() {
    std::vector<HWND> list;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t cls[32];
            DWORD pid = 0;
            if (GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId() &&
                GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
                (_wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0))
                ((std::vector<HWND>*)param)->push_back(hwnd);
            return TRUE;
        },
        (LPARAM)&list);
    std::ranges::stable_partition(list, [](HWND h) { return !IsSecondary(h); });
    return list;
}

HWND PrimaryTaskbar() {
    auto list = Taskbars();
    return list.empty() || IsSecondary(list[0]) ? nullptr : list[0];
}

// Taskbar thread.
wux::FrameworkElement TaskbarElement(HWND hwnd) {
    bool secondary = IsSecondary(hwnd);
    void* vftable = secondary ? CSecondaryTaskBand_ITaskListWndSite_vftable : CTaskBand_ITaskListWndSite_vftable;
    auto getHost = secondary ? CSecondaryTaskBand_GetTaskbarHost_Original : CTaskBand_GetTaskbarHost_Original;
    if (!vftable || !getHost || !TaskbarHost_FrameHeight_Original || !std__Ref_count_base__Decref_Original)
        return nullptr;
    HWND band = secondary ? FindWindowExW(hwnd, nullptr, L"WorkerW", nullptr) : (HWND)GetPropW(hwnd, L"TaskbandHWND");
    void* site = band ? (void*)GetWindowLongPtrW(band, 0) : nullptr;
    for (int i = 0; site && *(void**)site != vftable; i++) {
        if (i == 20) return nullptr;
        site = (void**)site + 1;
    }
    if (!site) return nullptr;
    void* host[2]{};  // std::shared_ptr<TaskbarHost>
    getHost(site, host);
    if (!host[0]) {
        if (host[1]) std__Ref_count_base__Decref_Original(host[1]);
        return nullptr;
    }
    // 48:83EC 28 | sub rsp,28 ; 48:83C1 48 | add rcx,48 <- the element's offset in TaskbarHost
    size_t offset = 0x10;
    const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
    if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F)
        offset = b[7];
    wux::FrameworkElement element{nullptr};
    winrt::copy_from_abi(element, *(void**)((BYTE*)host[0] + offset));
    std__Ref_count_base__Decref_Original(host[1]);
    return element;
}

wux::FrameworkElement Parent(const wux::FrameworkElement& e) {
    return wuxm::VisualTreeHelper::GetParent(e).try_as<wux::FrameworkElement>();
}

template <class F>
wux::FrameworkElement FindDescendant(const wux::FrameworkElement& e, F&& visit, int depth) {
    if (!e || depth <= 0) return nullptr;
    int n = wuxm::VisualTreeHelper::GetChildrenCount(e);
    for (int i = 0; i < n; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(e, i).try_as<wux::FrameworkElement>();
        if (!child) continue;
        if (visit(child)) return child;
        if (auto found = FindDescendant(child, visit, depth - 1)) return found;
    }
    return nullptr;
}

// The tray's horizontal panel: icons, then the clock (NotificationCenterButton).
wuxc::StackPanel TrayPanel(HWND hwnd) {
    auto element = TaskbarElement(hwnd);
    auto root = element ? element.XamlRoot() : nullptr;
    auto content = root ? root.Content().try_as<wux::FrameworkElement>() : nullptr;
    auto frame = FindDescendant(
        content, [](auto& e) { return winrt::get_class_name(e) == L"SystemTray.SystemTrayFrame"; }, 8);
    auto grid = FindDescendant(frame, [](auto& e) { return e.Name() == L"SystemTrayFrameGrid"; }, 2);
    return grid ? grid.try_as<wuxc::StackPanel>() : nullptr;
}

uint32_t ClockIndex(const wuxc::StackPanel& tray) {
    auto children = tray.Children();
    for (uint32_t i = 0; i < children.Size(); i++) {
        auto child = children.GetAt(i).try_as<wux::FrameworkElement>();
        if (child && (child.Name() == L"NotificationCenterButton" ||
                      FindDescendant(child, [](auto& e) { return e.Name() == L"NotificationCenterButton"; }, 3)))
            return i;
    }
    return children.Size();
}

bool TaskbarOnTop(HWND taskbar) {
    RECT rc{};
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(taskbar, &rc) || !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return (rc.top + rc.bottom) / 2 < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
}

// ---- running work on the taskbar thread ---------------------------------------

UINT RunMsg() {
    static UINT msg = RegisterWindowMessageW(L"MemoryClean_Run");
    return msg;
}

// An auto-hiding taskbar stays out while the window is open on it, as it does for
// Windows' own tray windows: the tray's hide check (timer 2, as TrayUI::_SetAutoHideTimer
// sets it) skips its ticks then. While the taskbar is in front Windows keeps it out by
// itself, so those ticks go on.
constexpr UINT_PTR kTrayHideTimer = 2;
HWND g_holdTaskbar;  // taskbar thread

LRESULT CALLBACK TaskbarSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR) {
    if (msg == WM_TIMER && wParam == kTrayHideTimer && hwnd == g_holdTaskbar && GetForegroundWindow() != hwnd)
        return 0;
    if (msg == RunMsg() && lParam) {
        try {
            (*(std::function<void()>*)lParam)();
        } catch (...) {
            Wh_Log(L"taskbar work failed: %08X", winrt::to_hresult());
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

HWND g_subclassed;

// Holds `taskbar` out (null lets go). Called on the taskbar thread.
void HoldTaskbar(HWND taskbar) {
    if (g_holdTaskbar == taskbar) return;
    if (g_holdTaskbar && g_holdTaskbar != g_subclassed)
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_holdTaskbar, TaskbarSubclass);
    g_holdTaskbar = nullptr;
    if (taskbar && (taskbar == g_subclassed ||
                    WindhawkUtils::SetWindowSubclassFromAnyThread(taskbar, TaskbarSubclass, 0)))
        g_holdTaskbar = taskbar;
}

// Runs `work` on the window's thread and waits for it.
bool RunOn(HWND hwnd, std::function<void()> work) {
    if (!hwnd) return false;
    if (g_subclassed != hwnd) {
        if (g_subclassed) WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_subclassed, TaskbarSubclass);
        g_subclassed = nullptr;
        if (!WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, TaskbarSubclass, 0)) return false;
        g_subclassed = hwnd;
    }
    SendMessageW(hwnd, RunMsg(), 0, (LPARAM)&work);
    return true;
}

winrt::Windows::System::DispatcherQueue g_queue{nullptr};  // the taskbar thread's, set there

// Runs `work` on the taskbar thread later, from any thread.
bool Post(std::function<void()> work) {
    auto queue = g_queue;
    if (g_unloading || !queue) return false;
    g_pending++;
    bool queued = false;
    try {
        queued = queue.TryEnqueue([work = std::move(work)] {
            if (!g_unloading) {
                try {
                    work();
                } catch (...) {
                    Wh_Log(L"posted work failed: %08X", winrt::to_hresult());
                }
            }
            g_pending--;
        });
    } catch (...) {
    }
    if (!queued) g_pending--;
    return queued;
}

// A worker thread the unload waits for.
void Worker(std::function<void()> work) {
    g_pending++;
    std::thread([work = std::move(work)] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            work();
        } catch (...) {
            Wh_Log(L"worker failed");
        }
        if (SUCCEEDED(co)) CoUninitialize();
        g_pending--;
    }).detach();
}

// ============================================================================
// Processes: one snapshot of NtQuerySystemInformation(SystemProcessInformation)
// ============================================================================

// The fields this add-on reads; the layout is the documented 64-bit one.
struct ProcessInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
};
static_assert(offsetof(ProcessInfo, ImageName) == 0x38 && offsetof(ProcessInfo, SessionId) == 0x64);

struct Proc {
    DWORD pid;
    uint64_t created, cpu, privateWs;
    std::wstring name;  // as Windows has it, "chrome.exe"
};

DWORD SessionOf(DWORD pid) {
    DWORD session = 0;
    ProcessIdToSessionId(pid, &session);
    return session;
}

// This session's processes. `buffer` is kept between calls.
std::vector<Proc> Snapshot(std::vector<BYTE>& buffer) {
    static DWORD session = SessionOf(GetCurrentProcessId());
    if (buffer.size() < 512 * 1024) buffer.resize(512 * 1024);
    ULONG needed = 0;
    NTSTATUS status;
    while ((status = NtQuerySystemInformation(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed)) ==
           NTSTATUS(0xC0000004L) /* STATUS_INFO_LENGTH_MISMATCH */) {
        if (buffer.size() > 64 * 1024 * 1024) return {};
        buffer.resize(std::max<size_t>(needed + 64 * 1024, buffer.size() * 2));
    }
    std::vector<Proc> list;
    if (status < 0) return list;
    for (BYTE* p = buffer.data();;) {
        auto* info = (ProcessInfo*)p;
        DWORD pid = DWORD(ULONG_PTR(info->UniqueProcessId));
        if (pid > 4 && info->SessionId == session && info->ImageName.Buffer)
            list.push_back({pid, uint64_t(info->CreateTime.QuadPart),
                            uint64_t(info->UserTime.QuadPart + info->KernelTime.QuadPart),
                            uint64_t(info->WorkingSetPrivateSize.QuadPart),
                            std::wstring(info->ImageName.Buffer, info->ImageName.Length / sizeof(wchar_t))});
        if (!info->NextEntryOffset) break;
        p += info->NextEntryOffset;
    }
    return list;
}

// Never offered for ending: Windows itself, the shell this runs in, and the add-on.
bool Protected(const std::wstring& lowerName) {
    static const std::unordered_set<std::wstring> names = {
        L"system", L"registry", L"memcompression", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe", L"sihost.exe",
        L"explorer.exe", L"taskhostw.exe", L"ctfmon.exe", L"memorycleanhost.exe", L"memorycleanhelper.exe",
        L"securityhealthservice.exe", L"msmpeng.exe", L"audiodg.exe", L"conhost.exe", L"runtimebroker.exe",
        L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"searchhost.exe", L"textinputhost.exe",
        L"lockapp.exe", L"logonui.exe", L"userinit.exe", L"dllhost.exe", L"smartscreen.exe",
    };
    return names.contains(lowerName);
}

std::wstring ProcessPath(DWORD pid) {
    std::wstring path;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[MAX_PATH * 2];
        DWORD size = ARRAYSIZE(buffer);
        if (QueryFullProcessImageNameW(h, 0, buffer, &size)) path.assign(buffer, size);
        CloseHandle(h);
    }
    return path;
}

// True when this (medium) process may not end it: an elevated program of the user's.
bool NeedsAdmin(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) {
        CloseHandle(h);
        return false;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

uint64_t ProcessCreated(HANDLE h) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(h, &created, &exited, &kernel, &user)) return 0;
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// ============================================================================
// The admin helper: a scheduled task of this user's, run with the full token
// ============================================================================

enum class HelperState { kUnknown, kMissing, kReady, kOld };
std::atomic<HelperState> g_helper{HelperState::kUnknown};
std::atomic<bool> g_helperBusy;  // install or uninstall running

std::wstring UserSid() {
    std::wstring out;
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
    BYTE buffer[256];
    DWORD size = 0;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        PWSTR sid = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buffer)->User.Sid, &sid)) {
            out = sid;
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return out;
}

std::wstring InstalledHelper() {
    return KnownFolder(FOLDERID_ProgramFiles) + L"\\" + kInstallFolder + L"\\" + kHelperExe;
}

// Worker thread (COM initialized). The task of this user, or null.
winrt::com_ptr<IRegisteredTask> HelperTask() {
    winrt::com_ptr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, service.put_void())))
        return nullptr;
    VARIANT empty{};
    if (FAILED(service->Connect(empty, empty, empty, empty))) return nullptr;
    winrt::com_ptr<ITaskFolder> folder;
    BSTR folderName = SysAllocString(kTaskFolder);
    HRESULT hr = service->GetFolder(folderName, folder.put());
    SysFreeString(folderName);
    if (FAILED(hr)) return nullptr;
    winrt::com_ptr<IRegisteredTask> task;
    BSTR taskName = SysAllocString((std::wstring(kTaskPrefix) + UserSid()).c_str());
    hr = folder->GetTask(taskName, task.put());
    SysFreeString(taskName);
    return SUCCEEDED(hr) ? task : nullptr;
}

void CheckHelper() {
    bool file = GetFileAttributesW(InstalledHelper().c_str()) != INVALID_FILE_ATTRIBUTES;
    HelperState state = file && HelperTask() ? HelperState::kReady : HelperState::kMissing;
    if (state == HelperState::kReady && g_helper == HelperState::kOld) return;  // keeps the protocol verdict
    g_helper = state;
}

std::wstring NewId() {
    static std::atomic<unsigned> n;
    return Format(L"%llx-%u", NowFiletime(), unsigned(++n));
}

struct HelperResult {
    bool ran = false;  // a result came back
    bool ok = false;
    std::wstring error;
    std::vector<std::wstring> failed;
    int killed = 0;
};

// Worker thread. Runs the task with one argument and waits for its result file.
HelperResult RunHelper(const std::wstring& arg, const std::wstring& id) {
    HelperResult result;
    auto task = HelperTask();
    if (!task) {
        g_helper = HelperState::kMissing;
        return result;
    }
    VARIANT param{};
    param.vt = VT_BSTR;
    param.bstrVal = SysAllocString(arg.c_str());
    winrt::com_ptr<IRunningTask> running;
    HRESULT hr = task->Run(param, running.put());
    VariantClear(&param);
    if (FAILED(hr)) {
        Wh_Log(L"helper task did not start: %08X", hr);
        return result;
    }
    std::wstring path = DataDir() + L"\\" + kResultFile;
    for (int i = 0; i < 600 && !g_unloading; i++) {  // a minute
        Sleep(100);
        auto o = ParseObject(ReadText(path, 64 * 1024));
        if (!o || o->GetNamedString(L"id", L"") != id) {
            // Ended without a result (refused its arguments, or no rights): stop waiting.
            TASK_STATE state{};
            if (i > 20 && SUCCEEDED(task->get_State(&state)) && state != TASK_STATE_RUNNING &&
                state != TASK_STATE_QUEUED) {
                LONG code = 0;
                task->get_LastTaskResult(&code);
                Wh_Log(L"helper ended without a result: %d", code);
                if (code == kExitProtocol) g_helper = HelperState::kOld;
                return result;
            }
            continue;
        }
        result.ran = true;
        result.ok = o->GetNamedBoolean(L"ok", false);
        result.error = o->GetNamedString(L"error", L"");
        result.killed = int(o->GetNamedNumber(L"killed", 0));
        if (auto list = o->TryLookup(L"failed"); list && list.ValueType() == wdj::JsonValueType::Array)
            for (auto v : list.GetArray())
                if (v.ValueType() == wdj::JsonValueType::String) result.failed.push_back(std::wstring(v.GetString()));
        if (result.error == L"protocol") g_helper = HelperState::kOld;
        return result;
    }
    return result;
}

// Worker thread: the bundled helper with "--install" or "--uninstall", elevated (one UAC prompt).
bool RunElevated(PCWSTR verbArg) {
    std::wstring exe = ModuleDir() + L"\\" + kHelperExe;
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = verbArg;
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        Wh_Log(L"helper %s not started: %u", verbArg, GetLastError());
        return false;
    }
    WaitForSingleObject(info.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    Wh_Log(L"helper %s: %u", verbArg, code);
    return code == 0;
}

// ============================================================================
// Cleaning and ending programs (worker threads)
// ============================================================================

std::atomic<bool> g_cleaning;
void OnCleaned(uint64_t freed, bool helperUsed, bool ok);  // taskbar thread
void OnKilled(const std::wstring& name, int killed, int left, int blocked, uint64_t freed);

// Without the helper: the working sets of this user's programs this process may trim.
void TrimOwnWorkingSets(const std::vector<std::wstring>& exclude) {
    std::vector<BYTE> buffer;
    for (auto& p : Snapshot(buffer)) {
        if (p.pid == GetCurrentProcessId() || std::ranges::find(exclude, Lower(p.name)) != exclude.end()) continue;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, p.pid)) {
            EmptyWorkingSet(h);
            CloseHandle(h);
        }
    }
}

void StartClean(unsigned areas) {
    if (g_cleaning.exchange(true)) return;
    std::vector<std::wstring> exclude = g_prefs.exclude;
    bool helper = g_helper == HelperState::kReady;
    Worker([areas, exclude, helper] {
        uint64_t before = ReadMem().used;
        bool ok = true, used = false;
        if (helper) {
            std::wstring id = NewId();
            auto r = RunHelper(Format(L"v%d clean %u %s", kHelperProtocol, areas, id.c_str()), id);
            used = r.ran;
            ok = r.ran && r.ok;
        }
        if (!used && (areas & kWorkingSets)) TrimOwnWorkingSets(exclude);
        Sleep(300);  // the counts settle
        uint64_t after = ReadMem().used;
        uint64_t freed = before > after ? before - after : 0;
        Wh_Log(L"cleaned %llu MB (helper %d, ok %d)", freed >> 20, used, ok);
        if (!Post([freed, used, ok] { OnCleaned(freed, used, ok); })) g_cleaning = false;
    });
}

struct KillTarget {
    DWORD pid;
    uint64_t created;
    uint64_t privateWs;
};

// "Close first": the windows get WM_CLOSE, then what is still running after 3 s ends,
// unless it shows a window of its own on top (a "save changes?" question).
void StartKill(const std::wstring& name, std::vector<KillTarget> targets) {
    bool helper = g_helper == HelperState::kReady;
    Worker([name, targets, helper] {
        struct Open {
            KillTarget t;
            HANDLE h;
        };
        std::vector<Open> open;
        for (auto& t : targets) {
            HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, t.pid);
            if (h && ProcessCreated(h) == t.created)
                open.push_back({t, h});
            else if (h)
                CloseHandle(h);
        }
        std::unordered_set<DWORD> pids;
        for (auto& o : open) pids.insert(o.t.pid);
        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (((std::unordered_set<DWORD>*)param)->contains(pid) && IsWindowVisible(hwnd) &&
                    !GetWindow(hwnd, GW_OWNER))
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            },
            (LPARAM)&pids);
        for (int i = 0; i < 30 && !g_unloading; i++) {
            if (std::ranges::all_of(open, [](auto& o) { return WaitForSingleObject(o.h, 0) == WAIT_OBJECT_0; })) break;
            Sleep(100);
        }
        // Still running: a question on screen keeps it; the rest end.
        std::unordered_set<DWORD> asking;
        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) && IsWindowEnabled(hwnd))
                    ((std::unordered_set<DWORD>*)param)->insert(pid);
                return TRUE;
            },
            (LPARAM)&asking);
        int killed = 0, blocked = 0;
        uint64_t freed = 0;
        std::wstring adminArg;
        for (auto& o : open) {
            if (WaitForSingleObject(o.h, 0) == WAIT_OBJECT_0) {
                killed++;
                freed += o.t.privateWs;
            } else if (asking.contains(o.t.pid)) {
                blocked++;
            } else if (HANDLE t = OpenProcess(PROCESS_TERMINATE, FALSE, o.t.pid)) {
                if (TerminateProcess(t, 1) && WaitForSingleObject(o.h, 2000) == WAIT_OBJECT_0) {
                    killed++;
                    freed += o.t.privateWs;
                }
                CloseHandle(t);
            } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                adminArg += Format(L"%s%lu:%016llx", adminArg.empty() ? L"" : L",", o.t.pid, o.t.created);
            }
        }
        if (!adminArg.empty() && helper) {
            std::wstring id = NewId();
            auto r = RunHelper(Format(L"v%d kill %s ", kHelperProtocol, id.c_str()) + adminArg, id);
            for (auto& o : open)
                if (WaitForSingleObject(o.h, 0) == WAIT_OBJECT_0 && r.ran) {
                    // counted once: only those not counted above
                }
            killed += r.killed;
        }
        int left = 0;
        for (auto& o : open) {
            if (WaitForSingleObject(o.h, 0) != WAIT_OBJECT_0) left++;
            CloseHandle(o.h);
        }
        Post([name, killed, left, blocked, freed] { OnKilled(name, killed, left, blocked, freed); });
    });
}

// ============================================================================
// The widget: a RAM module with the percent in it, before each taskbar's clock
// ============================================================================

// 25:18, the same width as the brightness add-on's monitor next to it.
constexpr double kW = 25, kH = 18;
constexpr double kBodyX = 1.15, kBodyY = 0.95, kBodyW = kW - 2.3, kBodyH = 13.3, kRadius = 3.6, kStroke = 1.3;
constexpr double kFeetH = 2.9, kFootW = 1.7;
constexpr double kFeet[] = {kW / 2 - 7, kW / 2 - 4.4, kW / 2 - 1.8,  // the middle one left out: the key notch
                            kW / 2 + 1.8, kW / 2 + 4.4, kW / 2 + 7};
constexpr PCWSTR kFont = L"Segoe UI, Malgun Gothic";  // Bloom's own pair
// Other Bloom add-ons in the tray find this one by name (the brightness add-on sits left of it).
constexpr PCWSTR kWidgetName = L"BloomAddon.memory-clean";

winrt::Windows::UI::Text::FontWeight Weight(int weight) {
    return {uint16_t(weight)};
}

// One drawing of the icon, used on the taskbar and in the settings.
struct RamIcon {
    wuxc::Canvas canvas{nullptr};
    wuxs::Rectangle body{nullptr}, fill{nullptr};
    std::vector<wuxs::Rectangle> feet;
    wuxc::TextBlock text{nullptr};

    void Build(double scale) {
        canvas = wuxc::Canvas();
        canvas.Width(kW * scale);
        canvas.Height(kH * scale);
        wuxm::ScaleTransform st;
        st.ScaleX(scale);
        st.ScaleY(scale);
        wuxc::Canvas inner;
        inner.RenderTransform(st);
        fill = wuxs::Rectangle();
        body = wuxs::Rectangle();
        for (auto* r : {&fill, &body}) {
            r->Width(kBodyW);
            r->Height(kBodyH);
            r->RadiusX(kRadius);
            r->RadiusY(kRadius);
            wuxc::Canvas::SetLeft(*r, kBodyX);
            wuxc::Canvas::SetTop(*r, kBodyY);
        }
        body.StrokeThickness(kStroke);
        inner.Children().Append(fill);
        for (double x : kFeet) {
            wuxs::Rectangle foot;
            foot.Width(kFootW);
            foot.Height(kFeetH + kStroke / 2);
            wuxc::Canvas::SetLeft(foot, x - kFootW / 2);
            wuxc::Canvas::SetTop(foot, kBodyY + kBodyH - kStroke / 2);
            inner.Children().Append(foot);
            feet.push_back(foot);
        }
        inner.Children().Append(body);
        wuxc::Grid textBox;
        textBox.Width(kBodyW);
        textBox.Height(kBodyH);
        wuxc::Canvas::SetLeft(textBox, kBodyX);
        wuxc::Canvas::SetTop(textBox, kBodyY);
        text = wuxc::TextBlock();
        text.FontFamily(wuxm::FontFamily(kFont));
        text.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
        text.HorizontalAlignment(wux::HorizontalAlignment::Center);
        text.VerticalAlignment(wux::VerticalAlignment::Center);
        text.TextLineBounds(wux::TextLineBounds::Tight);
        textBox.Children().Append(text);
        inner.Children().Append(textBox);
        canvas.Children().Append(inner);
    }

    void Paint(int percent, IconStyle style, const Palette& c) {
        uint32_t color = percent >= kDangerPercent ? c.ramDanger : percent >= kWarnPercent ? c.ramWarn : c.ramInk;
        auto brush = Brush(color);
        body.Stroke(brush);
        for (auto& f : feet) f.Fill(brush);
        if (style == IconStyle::kLine) {
            fill.Visibility(wux::Visibility::Collapsed);
        } else {
            fill.Visibility(wux::Visibility::Visible);
            bool solid = style == IconStyle::kSolid;
            fill.Fill(Brush(solid ? color : WithAlpha(color, 0x47)));
            wuxm::RectangleGeometry clip;
            clip.Rect({0, 0, float(solid ? kBodyW : kBodyW * std::clamp(percent, 0, 100) / 100.0), float(kBodyH)});
            fill.Clip(clip);
        }
        text.Text(std::to_wstring(percent));
        text.FontSize(percent >= 100 ? 7.8 : 10);
        text.Foreground(Brush(style == IconStyle::kSolid ? c.win : color));
    }
};

struct Widget {
    HWND taskbar = nullptr;
    wuxc::Grid root{nullptr};
    RamIcon icon;
    int shown = -1;
    IconStyle style{};
    const Palette* palette = nullptr;
    bool hover = false;
};
std::vector<std::unique_ptr<Widget>> g_widgets;  // taskbar thread
Mem g_mem;
int g_percent = 0;
void TogglePanel(Widget& w);

void PaintWidget(Widget& w, bool force = false) {
    const Palette& c = PaletteFor(w.root);
    if (!force && w.shown == g_percent && w.style == g_prefs.icon && w.palette == &c) return;
    w.shown = g_percent;
    w.style = g_prefs.icon;
    w.palette = &c;
    w.icon.Paint(g_percent, g_prefs.icon, c);
    w.root.Background(Brush(w.hover ? 0x2E7F7F7F : 0));
    std::wstring tip = Format(T(L"메모리 %d %% · %s / %s GB", L"Memory %d%% · %s / %s GB"), g_percent,
                              Gb(g_mem.used).c_str(), Gb(g_mem.total).c_str());
    wuxc::ToolTipService::SetToolTip(w.root, box_value(tip));
    wux::Automation::AutomationProperties::SetName(w.root, tip);
}

std::unique_ptr<Widget> BuildWidget(HWND taskbar) {
    auto w = std::make_unique<Widget>();
    w->taskbar = taskbar;
    w->root = wuxc::Grid();
    w->root.Name(kWidgetName);
    w->root.Padding({5, 4, 5, 4});
    w->root.Margin({2, 0, 2, 0});
    w->root.CornerRadius({6, 6, 6, 6});
    w->root.VerticalAlignment(wux::VerticalAlignment::Center);
    w->root.Background(Brush(0));
    w->icon.Build(1.0);
    w->root.Children().Append(w->icon.canvas);
    Widget* raw = w.get();
    w->root.PointerEntered([raw](auto&&, auto&&) {
        raw->hover = true;
        PaintWidget(*raw, true);
    });
    w->root.PointerExited([raw](auto&&, auto&&) {
        raw->hover = false;
        PaintWidget(*raw, true);
    });
    w->root.Tapped([raw](auto&&, wux::Input::TappedRoutedEventArgs const& args) {
        args.Handled(true);
        TogglePanel(*raw);
    });
    return w;
}

void RemoveWidget(Widget& w) {
    if (!w.root) return;
    if (auto panel = Parent(w.root).try_as<wuxc::Panel>()) {
        uint32_t index;
        if (panel.Children().IndexOf(w.root, index)) panel.Children().RemoveAt(index);
    }
}

// Every taskbar gets one, before its clock; ones whose tray was rebuilt go in again.
void PlaceWidgets() {
    auto taskbars = Taskbars();
    std::erase_if(g_widgets, [&](auto& w) {
        bool gone = std::ranges::find(taskbars, w->taskbar) == taskbars.end();
        if (gone) RemoveWidget(*w);
        return gone;
    });
    for (HWND hwnd : taskbars) {
        auto it = std::ranges::find_if(g_widgets, [&](auto& w) { return w->taskbar == hwnd; });
        if (it != g_widgets.end() && Parent((*it)->root) && (*it)->root.IsLoaded()) continue;
        auto tray = TrayPanel(hwnd);
        if (!tray) continue;
        if (it == g_widgets.end()) {
            g_widgets.push_back(BuildWidget(hwnd));
            it = g_widgets.end() - 1;
        } else {
            RemoveWidget(**it);
        }
        // A widget of an earlier load of this mod would be a stranger now.
        for (uint32_t i = tray.Children().Size(); i-- > 0;)
            if (auto e = tray.Children().GetAt(i).try_as<wux::FrameworkElement>(); e && e.Name() == kWidgetName)
                tray.Children().RemoveAt(i);
        tray.Children().InsertAt(ClockIndex(tray), (*it)->root);
        PaintWidget(**it, true);
        Wh_Log(L"widget placed on %p", hwnd);
    }
}

// ============================================================================
// Parts: Bloom-style buttons, segments, keys, switch, number box, panels
// ============================================================================

constexpr double kInner = 326;  // the window's 348 less its border and padding

const Palette* g_c = &kDark;  // the palette the open window is drawn with

constexpr PCWSTR kGlyphSettings = L"\uE713",
                 kGlyphBack = L"\uE72B",
                 kGlyphPower = L"\uE7E8",
                 kGlyphLock = L"\uE72E",
                 kGlyphShield = L"\uEA18",
                 kGlyphClose = L"\uE711",
                 kGlyphAdd = L"\uE710",
                 kGlyphSearch = L"\uE721",
                 kGlyphCheck = L"\uE73E",
                 kGlyphChevron = L"\uE76C",
                 kGlyphWarn = L"\uE7BA",
                 kGlyphAuto = L"\uE916",
                 kGlyphBlock = L"\uE8F8";

wuxc::TextBlock Text(const std::wstring& s, double size, uint32_t color, int weight = 400) {
    wuxc::TextBlock t;
    t.Text(s);
    t.FontFamily(wuxm::FontFamily(kFont));
    t.FontSize(size);
    t.Foreground(Brush(color));
    if (weight != 400) t.FontWeight(Weight(weight));
    t.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    t.VerticalAlignment(wux::VerticalAlignment::Center);
    return t;
}

wuxc::TextBlock Wrapped(const std::wstring& s, double size, uint32_t color) {
    auto t = Text(s, size, color);
    t.TextWrapping(wux::TextWrapping::Wrap);
    t.TextTrimming(wux::TextTrimming::None);
    return t;
}

// A glyph from Segoe Fluent Icons (Segoe MDL2 Assets on older systems).
wuxc::TextBlock Glyph(PCWSTR glyph, double size, uint32_t color) {
    wuxc::TextBlock t;
    t.Text(glyph);
    t.FontFamily(wuxm::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    t.FontSize(size);
    t.Foreground(Brush(color));
    t.VerticalAlignment(wux::VerticalAlignment::Center);
    t.HorizontalAlignment(wux::HorizontalAlignment::Center);
    return t;
}

// The clean button's mark: two four-pointed stars.
wux::FrameworkElement Sparkle(uint32_t color) {
    wuxc::Canvas canvas;
    canvas.Width(14);
    canvas.Height(14);
    canvas.VerticalAlignment(wux::VerticalAlignment::Center);
    auto star = [&](double cx, double cy, double r, double in) {
        wuxs::Polygon polygon;
        wuxm::PointCollection points;
        for (int i = 0; i < 8; i++) {
            double a = i * 3.14159265358979 / 4 - 3.14159265358979 / 2, d = i % 2 ? in : r;
            points.Append({float(cx + d * cos(a)), float(cy + d * sin(a))});
        }
        polygon.Points(points);
        polygon.Fill(Brush(color));
        canvas.Children().Append(polygon);
    };
    star(5.5, 5.5, 5.2, 1.2);
    star(11, 11, 2.8, 0.7);
    return canvas;
}

wuxc::Border Box(double radius, uint32_t background) {
    wuxc::Border b;
    b.CornerRadius({radius, radius, radius, radius});
    b.Background(Brush(background));
    return b;
}

wuxc::StackPanel HStack(double spacing) {
    wuxc::StackPanel s;
    s.Orientation(wuxc::Orientation::Horizontal);
    s.Spacing(spacing);
    return s;
}

wuxc::StackPanel VStack(double spacing) {
    wuxc::StackPanel s;
    s.Spacing(spacing);
    return s;
}

wux::GridLength Px(double v) {
    return {v, wux::GridUnitType::Pixel};
}

// Columns: > 0 pixels, 0 auto, < 0 star.
wuxc::Grid Columns(std::initializer_list<double> widths, double spacing) {
    wuxc::Grid g;
    g.ColumnSpacing(spacing);
    for (double w : widths) {
        wuxc::ColumnDefinition col;
        col.Width(w > 0 ? Px(w) : w == 0 ? wux::GridLengthHelper::Auto() : wux::GridLength{-w, wux::GridUnitType::Star});
        g.ColumnDefinitions().Append(col);
    }
    return g;
}

template <class E>
E InColumn(E e, int column) {
    wuxc::Grid::SetColumn(e, column);
    return e;
}

// Handlers of the window's elements, revoked when the page is rebuilt or the window
// closes: a handler holding its own element would otherwise keep it alive for good.
std::vector<std::shared_ptr<void>> g_keep;

template <class R>
void Keep(R&& revoker) {
    g_keep.push_back(std::make_shared<std::decay_t<R>>(std::forward<R>(revoker)));
}

// Drops the page's handlers once the event that asked for it has finished.
void ReleaseHandlers() {
    if (g_keep.empty()) return;
    auto old = std::make_shared<std::vector<std::shared_ptr<void>>>(std::move(g_keep));
    g_keep.clear();
    if (!Post([old] { old->clear(); })) old->clear();
}

template <class E, class F>
void OnClick(const E& e, F&& action) {
    Keep(e.Tapped(winrt::auto_revoke, [action = std::forward<F>(action)](auto&&, wux::Input::TappedRoutedEventArgs const& args) {
        args.Handled(true);
        action();
    }));
}

template <class E, class In, class Out>
void OnHover(const E& e, In&& enter, Out&& leave) {
    Keep(e.PointerEntered(winrt::auto_revoke, [enter = std::forward<In>(enter)](auto&&, auto&&) { enter(); }));
    Keep(e.PointerExited(winrt::auto_revoke, [leave = std::forward<Out>(leave)](auto&&, auto&&) { leave(); }));
}

// ---- buttons ------------------------------------------------------------------------

enum class Kind { kSolid, kGhost, kOutline, kDanger };

struct Button {
    wuxc::Border root{nullptr};
    wuxc::TextBlock glyph{nullptr}, label{nullptr};
};

// Height 32 (28 small), radius 8. An empty label makes a 28 square icon button.
Button MakeButton(Kind kind, PCWSTR glyph, const std::wstring& label, bool small, std::function<void()> action) {
    const Palette& c = *g_c;
    Button b;
    b.root = Box(8, 0);
    bool iconOnly = label.empty();
    b.root.Height(iconOnly || small ? 28 : 32);
    if (iconOnly) b.root.Width(28);
    else b.root.Padding({small ? 10.0 : 12.0, 0, small ? 10.0 : 12.0, 0});
    uint32_t text = kind == Kind::kGhost ? c.muted : kind == Kind::kDanger ? 0xFFFFFFFF : c.fg;
    uint32_t bg = kind == Kind::kSolid ? c.btn : kind == Kind::kDanger ? c.dangerBtn : 0;
    uint32_t hover = kind == Kind::kSolid ? c.btnHover : kind == Kind::kDanger ? c.dangerBtn : Ink(c, 8);
    b.root.Background(Brush(bg));
    if (kind == Kind::kOutline || (kind == Kind::kSolid && &c == &kLight)) {
        b.root.BorderThickness({1, 1, 1, 1});
        b.root.BorderBrush(Brush(Ink(c, kind == Kind::kOutline ? 25 : 10)));
    }
    auto content = HStack(6);
    content.HorizontalAlignment(wux::HorizontalAlignment::Center);
    content.VerticalAlignment(wux::VerticalAlignment::Center);
    if (glyph) {
        b.glyph = Glyph(glyph, small ? 12 : 14, text);
        content.Children().Append(b.glyph);
    }
    if (!iconOnly) {
        b.label = Text(label, small ? 12 : 13, text, 500);
        content.Children().Append(b.label);
    }
    b.root.Child(content);
    auto root = b.root;
    auto parts = b;
    OnHover(
        b.root,
        [root, parts, hover, kind, cp = &c] {
            root.Background(Brush(hover));
            if (kind == Kind::kDanger) root.Opacity(0.9);
            if (kind == Kind::kGhost) {
                if (parts.glyph) parts.glyph.Foreground(Brush(cp->fg));
                if (parts.label) parts.label.Foreground(Brush(cp->fg));
            }
        },
        [root, parts, bg, text] {
            root.Background(Brush(bg));
            root.Opacity(1);
            if (parts.glyph) parts.glyph.Foreground(Brush(text));
            if (parts.label) parts.label.Foreground(Brush(text));
        });
    OnClick(b.root, std::move(action));
    return b;
}

// ---- segments -------------------------------------------------------------------------

struct Segment {
    struct State {
        std::vector<wuxc::Border> items;
        std::vector<wuxc::TextBlock> labels;
        int on = 0;
    };
    std::shared_ptr<State> s;
    wuxc::Border root{nullptr};

    void Select(int index) {
        const Palette& c = *g_c;
        s->on = index;
        for (size_t i = 0; i < s->items.size(); i++) {
            bool sel = int(i) == index;
            s->items[i].Background(Brush(sel ? Ink(c, 15) : 0));
            s->labels[i].Foreground(Brush(sel ? c.fg : c.faint));
        }
    }
};

// w5 track, radius 8, 2 inside; items radius 6, 12 px 500. `fill` spreads the items evenly.
Segment MakeSegment(std::initializer_list<std::wstring> names, bool fill, double height, int on,
                    std::function<void(int)> picked) {
    const Palette& c = *g_c;
    Segment seg;
    seg.s = std::make_shared<Segment::State>();
    seg.root = Box(8, Ink(c, 5));
    seg.root.Padding({2, 2, 2, 2});
    wuxc::Grid row;
    row.ColumnSpacing(2);
    int i = 0;
    for (auto& name : names) {
        wuxc::ColumnDefinition col;
        col.Width(fill ? wux::GridLength{1, wux::GridUnitType::Star} : wux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(col);
        auto item = Box(6, 0);
        item.Height(height);
        item.Padding({10, 0, 10, 0});
        auto label = Text(name, 12, c.faint, 500);
        label.HorizontalAlignment(wux::HorizontalAlignment::Center);
        item.Child(label);
        wuxc::Grid::SetColumn(item, i);
        row.Children().Append(item);
        seg.s->items.push_back(item);
        seg.s->labels.push_back(label);
        auto state = seg.s;
        OnHover(
            item, [label, cp = &c] { label.Foreground(Brush(cp->fg)); },
            [label, state, i, cp = &c] { label.Foreground(Brush(state->on == i ? cp->fg : cp->faint)); });
        OnClick(item, [seg, i, picked] {
            auto copy = seg;
            copy.Select(i);
            picked(i);
        });
        i++;
    }
    seg.root.Child(row);
    seg.Select(on);
    return seg;
}

// ---- keys: on = filled with a check, off = outline only ---------------------------

wuxc::Border MakeKey(const std::wstring& label, bool on, std::function<void(bool)> changed) {
    const Palette& c = *g_c;
    auto key = Box(8, 0);
    key.Height(28);
    auto content = HStack(6);
    content.VerticalAlignment(wux::VerticalAlignment::Center);
    auto check = Glyph(kGlyphCheck, 11, c.fg);
    auto text = Text(label, 12, c.faint, 500);
    content.Children().Append(check);
    content.Children().Append(text);
    key.Child(content);
    auto state = std::make_shared<bool>(on);
    auto paint = [key, check, text, state, cp = &c] {
        bool v = *state;
        key.Background(Brush(v ? cp->btn : 0));
        key.BorderThickness({1, 1, 1, 1});
        key.BorderBrush(Brush(v ? (cp == &kLight ? Ink(*cp, 10) : 0) : Ink(*cp, 15)));
        key.Padding({v ? 9.0 : 12.0, 0, 12, 0});
        check.Visibility(v ? wux::Visibility::Visible : wux::Visibility::Collapsed);
        text.Foreground(Brush(v ? cp->fg : cp->faint));
    };
    paint();
    OnHover(
        key, [key, state, cp = &c] { if (!*state) key.Background(Brush(Ink(*cp, 5))); }, [paint] { paint(); });
    OnClick(key, [state, paint, changed] {
        *state = !*state;
        paint();
        changed(*state);
    });
    return key;
}

// ---- the switch: 40 x 22, lit line inside and a lit knob when on ------------------

wuxc::Border MakeSwitch(bool on, std::function<void(bool)> changed) {
    const Palette& c = *g_c;
    auto track = Box(11, 0);
    track.Width(40);
    track.Height(22);
    track.BorderThickness({1, 1, 1, 1});
    wuxc::Canvas canvas;
    wuxs::Rectangle glowLine, line;
    for (auto* r : {&glowLine, &line}) {
        r->Fill(Brush(r == &line ? c.accent : WithAlpha(c.accent, 0x40)));
        r->RadiusX(1);
        r->RadiusY(1);
        canvas.Children().Append(*r);
    }
    line.Height(2);
    wuxc::Canvas::SetLeft(line, 6);
    wuxc::Canvas::SetTop(line, 9);
    glowLine.Height(6);
    wuxc::Canvas::SetLeft(glowLine, 4);
    wuxc::Canvas::SetTop(glowLine, 7);
    wuxs::Ellipse halo, knob;
    halo.Width(22);
    halo.Height(22);
    halo.Fill(Brush(WithAlpha(c.accent, 0x30)));
    halo.IsHitTestVisible(false);
    knob.Width(14);
    knob.Height(14);
    knob.StrokeThickness(1.5);
    wuxc::Canvas::SetTop(halo, -1);
    wuxc::Canvas::SetTop(knob, 3);
    canvas.Children().Append(halo);
    canvas.Children().Append(knob);
    track.Child(canvas);
    auto state = std::make_shared<bool>(on);
    auto paint = [track, line, glowLine, halo, knob, state, cp = &c] {
        bool v = *state;
        track.BorderBrush(Brush(Ink(*cp, v ? 15 : 25)));
        line.Width(v ? 18 : 0);
        glowLine.Width(v ? 22 : 0);
        halo.Visibility(v ? wux::Visibility::Visible : wux::Visibility::Collapsed);
        wuxc::Canvas::SetLeft(halo, v ? 17 : -1);
        wuxc::Canvas::SetLeft(knob, v ? 21 : 3);
        knob.Fill(Brush(v ? cp->accent : 0));
        knob.Stroke(Brush(v ? cp->accent : cp->faint));
    };
    paint();
    OnClick(track, [state, paint, changed] {
        *state = !*state;
        paint();
        changed(*state);
    });
    return track;
}

// ---- the number box: press to type, the wheel over it steps by 1 --------------------------

void StyleTextBox(const wuxc::TextBox& box, const Palette& c) {
    box.FontFamily(wuxm::FontFamily(kFont));
    box.FontSize(12);
    box.Padding({0, 0, 0, 0});
    box.MinWidth(0);
    box.MinHeight(0);
    box.BorderThickness({0, 0, 0, 0});
    box.VerticalAlignment(wux::VerticalAlignment::Center);
    box.IsSpellCheckEnabled(false);
    auto res = box.Resources();
    for (auto [key, argb] : std::initializer_list<std::pair<PCWSTR, uint32_t>>{
             {L"TextControlBackground", 0}, {L"TextControlBackgroundPointerOver", 0},
             {L"TextControlBackgroundFocused", 0}, {L"TextControlBorderBrush", 0},
             {L"TextControlBorderBrushPointerOver", 0}, {L"TextControlBorderBrushFocused", 0},
             {L"TextControlForeground", c.fg}, {L"TextControlForegroundPointerOver", c.fg},
             {L"TextControlForegroundFocused", c.fg}, {L"TextControlPlaceholderForeground", c.faint},
             {L"TextControlPlaceholderForegroundPointerOver", c.faint},
             {L"TextControlPlaceholderForegroundFocused", c.faint}, {L"TextControlSelectionHighlightColor", c.accent}})
        res.Insert(box_value(key), Brush(argb));
    res.Insert(box_value(L"TextControlThemeMinHeight"), box_value(0.0));
    res.Insert(box_value(L"TextControlThemeMinWidth"), box_value(0.0));
    res.Insert(box_value(L"TextControlThemePadding"), box_value(wux::Thickness{0, 0, 0, 0}));
    res.Insert(box_value(L"TextControlBorderThemeThickness"), box_value(wux::Thickness{0, 0, 0, 0}));
}

// w8 field, height 28, radius 8: the number, then its unit in the faint colour.
wuxc::Border NumberBox(int value, int lo, int hi, PCWSTR unit, std::function<void(int)> changed) {
    const Palette& c = *g_c;
    auto field = Box(8, Ink(c, 8));
    field.Height(28);
    field.Padding({10, 0, 10, 0});
    field.BorderThickness({1, 1, 1, 1});
    field.BorderBrush(Brush(0));
    auto row = HStack(3);
    row.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::TextBox box;
    StyleTextBox(box, c);
    int digits = hi >= 1000 ? 4 : hi >= 100 ? 3 : 2;
    box.Width(7.0 * digits);
    box.TextAlignment(wux::TextAlignment::Right);
    box.InputScope([] {
        wux::Input::InputScope scope;
        wux::Input::InputScopeName name;
        name.NameValue(wux::Input::InputScopeNameValue::Number);
        scope.Names().Append(name);
        return scope;
    }());
    box.Text(std::to_wstring(value));
    row.Children().Append(box);
    row.Children().Append(Text(unit, 12, c.faint));
    field.Child(row);
    auto current = std::make_shared<int>(value);
    auto set = [box, current, lo, hi, changed](int v) {
        v = std::clamp(v, lo, hi);
        box.Text(std::to_wstring(v));
        if (v == *current) return;
        *current = v;
        changed(v);
    };
    auto commit = [box, current, set] {
        std::wstring text(box.Text());
        wchar_t* end = nullptr;
        long v = wcstol(text.c_str(), &end, 10);
        set(end && end != text.c_str() ? int(v) : *current);
    };
    OnHover(
        field, [field, cp = &c] { field.BorderBrush(Brush(Ink(*cp, 25))); }, [field] { field.BorderBrush(Brush(0)); });
    OnClick(field, [box] {
        box.Focus(wux::FocusState::Programmatic);
        box.SelectAll();
    });
    Keep(box.GotFocus(winrt::auto_revoke, [box](auto&&, auto&&) { box.SelectAll(); }));
    Keep(box.LostFocus(winrt::auto_revoke, [commit](auto&&, auto&&) { commit(); }));
    Keep(box.KeyDown(winrt::auto_revoke, [commit](auto&&, wux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
            args.Handled(true);
            commit();
        }
    }));
    Keep(field.PointerWheelChanged(winrt::auto_revoke, [field, current, set](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        args.Handled(true);
        int delta = args.GetCurrentPoint(field).Properties().MouseWheelDelta();
        if (delta) set(*current + (delta > 0 ? 1 : -1));
    }));
    return field;
}

// ---- panels, rows, section heads ----------------------------------------------------------

// A w5 panel, radius 12; its rows are parted by a w5 line.
wuxc::Border PanelOf(const std::vector<wux::FrameworkElement>& rows) {
    const Palette& c = *g_c;
    auto panel = Box(12, Ink(c, 5));
    auto stack = VStack(0);
    for (size_t i = 0; i < rows.size(); i++) {
        wuxc::Border wrap;
        if (i) {
            wrap.BorderThickness({0, 1, 0, 0});
            wrap.BorderBrush(Brush(Ink(c, 5)));
        }
        wrap.Child(rows[i]);
        stack.Children().Append(wrap);
    }
    panel.Child(stack);
    return panel;
}

// Title (13, 500) with a quieter line under it, then controls at the right.
wuxc::Grid OptionRow(const wux::FrameworkElement& lead, const std::wstring& title, const std::wstring& sub,
                     const std::vector<wux::FrameworkElement>& right) {
    const Palette& c = *g_c;
    wuxc::Grid g;
    g.ColumnSpacing(12);
    g.Padding({12, 8, 12, 8});
    g.MinHeight(48);
    int column = 0;
    auto add = [&](const wux::FrameworkElement& e, bool star) {
        wuxc::ColumnDefinition col;
        col.Width(star ? wux::GridLength{1, wux::GridUnitType::Star} : wux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(col);
        e.VerticalAlignment(wux::VerticalAlignment::Center);
        wuxc::Grid::SetColumn(e, column++);
        g.Children().Append(e);
    };
    if (lead) add(lead, false);
    auto texts = VStack(1);
    texts.Children().Append(Text(title, 13, c.fg, 500));
    if (!sub.empty()) texts.Children().Append(Wrapped(sub, 12, c.faint));
    add(texts, true);
    for (auto& e : right) add(e, false);
    return g;
}

wuxc::Grid SectionHead(const std::wstring& title, const std::wstring& end = {}) {
    const Palette& c = *g_c;
    auto g = Columns({-1, 0}, 8);
    g.Margin({2, 16, 2, 6});
    g.Children().Append(Text(title, 12, c.muted, 600));
    if (!end.empty()) g.Children().Append(InColumn(Text(end, 12, c.faint), 1));
    return g;
}

wuxc::TextBlock Help(const std::wstring& s) {
    auto t = Wrapped(s, 12, g_c->faint);
    t.Margin({2, 6, 2, 0});
    return t;
}

struct Bar {
    wuxc::Grid track{nullptr};
    wuxc::Border fill{nullptr};
    double width = 0;
    void Set(double fraction, uint32_t color) {
        fill.Width(std::max(0.0, width * std::clamp(fraction, 0.0, 1.0)));
        fill.Background(Brush(color));
    }
};

// A w8 track (4 px, or 2 px thin) with the accent fill.
Bar MakeBar(double width, double height) {
    Bar bar;
    bar.width = width;
    bar.track = wuxc::Grid();
    bar.track.Width(width);
    bar.track.Height(height);
    bar.track.CornerRadius({height / 2, height / 2, height / 2, height / 2});
    bar.track.Background(Brush(Ink(*g_c, 8)));
    bar.track.HorizontalAlignment(wux::HorizontalAlignment::Left);
    bar.fill = wuxc::Border();
    bar.fill.HorizontalAlignment(wux::HorizontalAlignment::Left);
    bar.fill.CornerRadius({height / 2, height / 2, height / 2, height / 2});
    bar.track.Children().Append(bar.fill);
    return bar;
}

// A program's tile: its icon once loaded, else the first letter.
struct Tile {
    wuxc::Border root{nullptr};
    wuxc::Image image{nullptr};
    wuxc::TextBlock letter{nullptr};
};

Tile MakeTile(double size, double radius, const std::wstring& name) {
    const Palette& c = *g_c;
    Tile t;
    t.root = Box(radius, Ink(c, 8));
    t.root.Width(size);
    t.root.Height(size);
    wuxc::Grid inside;
    t.letter = Text(name.empty() ? L"?" : std::wstring(1, towupper(name[0])), size > 24 ? 12 : 11, c.fg, 600);
    t.letter.HorizontalAlignment(wux::HorizontalAlignment::Center);
    t.image = wuxc::Image();
    t.image.Width(size - 8);
    t.image.Height(size - 8);
    inside.Children().Append(t.letter);
    inside.Children().Append(t.image);
    t.root.Child(inside);
    return t;
}

// ============================================================================
// The window: [clean | programs] tabs and the settings, as a flyout under the widget
// ============================================================================

enum class Page { kClean, kPrograms, kSettings, kPick };

struct Row {
    std::wstring key;  // lower-case file name
    std::wstring name, path;
    std::vector<KillTarget> targets;
    uint64_t mem = 0;
    double cpu = -1;
    bool locked = false, admin = false, hung = false, iconAsked = false;
    bool hover = false, confirm = false, busy = false;
    wuxc::Border root{nullptr};  // holds the line, or the question once "end" is pressed
    wuxc::Grid line{nullptr};
    wuxc::Border ask{nullptr};
    Tile tile, askTile;
    wuxc::TextBlock title{nullptr}, cpuText{nullptr}, memText{nullptr}, askText{nullptr}, yesText{nullptr};
    wuxc::StackPanel badges{nullptr};
    Bar bar;
    Button kill;
    wuxc::TextBlock lock{nullptr};
};

struct PanelState {
    HWND taskbar = nullptr;
    const Palette* c = &kDark;
    wuxc::Flyout flyout{nullptr};
    wuxc::Border bubble{nullptr}, headHost{nullptr}, pageHost{nullptr};
    Page page = Page::kClean;
    // clean page
    wuxc::TextBlock hero{nullptr}, heroSub{nullptr}, commitText{nullptr}, cacheText{nullptr};
    wuxc::TextBlock result{nullptr}, resultMark{nullptr}, autoEnd{nullptr}, exclEnd{nullptr}, noticeText{nullptr};
    Button cleanButton;
    wuxc::Border notice{nullptr};
    Bar big, commitBar, cacheBar;
    // programs page
    wuxc::StackPanel list{nullptr};
    wuxc::TextBlock foot{nullptr}, toast{nullptr}, colCpu{nullptr}, colMem{nullptr};
    wuxc::Border toastBox{nullptr};
    std::wstring filter;
    std::map<std::wstring, std::unique_ptr<Row>> rows;
    std::vector<std::wstring> order;
    std::vector<BYTE> buffer;
    std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> lastCpu;  // pid -> (created, cpu time)
    uint64_t lastCpuAt = 0;
    std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> known;  // key -> (path, name)
    std::unordered_map<uint64_t, bool> admin;  // (pid ^ created) -> needs admin
};
std::unique_ptr<PanelState> g_panel;
uint64_t g_panelClosedAt = 0;
std::wstring g_toastText;

void ShowPage(Page page);
void RefreshClean();
void RefreshPrograms();

std::wstring BaseName(const std::wstring& file) {
    std::wstring s = file;
    if (Lower(s).ends_with(L".exe")) s.resize(s.size() - 4);
    if (!s.empty()) s[0] = towupper(s[0]);
    return s;
}

// ---- clean page -------------------------------------------------------------------

std::wstring Ago(uint64_t at) {
    uint64_t now = NowFiletime();
    uint64_t minutes = now > at ? (now - at) / (10'000'000ull * 60) : 0;
    if (minutes < 1) return T(L"방금", L"just now");
    if (minutes < 60) return Format(T(L"%llu분 전", L"%llu min ago"), minutes);
    if (minutes < 60 * 24) return Format(T(L"%llu시간 전", L"%llu h ago"), minutes / 60);
    return Format(T(L"%llu일 전", L"%llu d ago"), minutes / (60 * 24));
}

void InstallHelper(bool remove) {
    if (g_helperBusy.exchange(true)) return;
    Worker([remove] {
        RunElevated(remove ? L"--uninstall" : L"--install");
        g_helper = HelperState::kUnknown;
        CheckHelper();
        g_helperBusy = false;
        Post([] {
            if (g_panel) ShowPage(g_panel->page);
        });
    });
}

wux::UIElement BuildClean() {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto page = VStack(0);

    auto hero = HStack(10);
    hero.Margin({2, 16, 2, 8});
    p.hero = Text(L"", 26, c.fg, 500);
    p.hero.TextTrimming(wux::TextTrimming::None);
    p.hero.VerticalAlignment(wux::VerticalAlignment::Bottom);
    p.heroSub = Text(L"", 12, c.muted);
    p.heroSub.VerticalAlignment(wux::VerticalAlignment::Bottom);
    p.heroSub.Margin({0, 0, 0, 3});
    hero.Children().Append(p.hero);
    hero.Children().Append(p.heroSub);
    page.Children().Append(hero);
    p.big = MakeBar(kInner - 4, 4);
    p.big.track.Margin({2, 0, 2, 0});
    page.Children().Append(p.big.track);

    auto metric = [&](PCWSTR label, wuxc::TextBlock& value, Bar& bar) {
        auto box = VStack(6);
        box.Padding({12, 10, 12, 10});
        auto head = Columns({-1, 0}, 8);
        head.Children().Append(Text(label, 12, c.muted));
        value = Text(L"", 12, c.fg, 500);
        head.Children().Append(InColumn(value, 1));
        box.Children().Append(head);
        bar = MakeBar(kInner - 24, 2);
        box.Children().Append(bar.track);
        return box;
    };
    auto metrics = PanelOf({metric(T(L"커밋 (페이지 파일 포함)", L"Committed (with page file)"), p.commitText, p.commitBar),
                            metric(T(L"시스템 캐시", L"System cache"), p.cacheText, p.cacheBar)});
    metrics.Margin({0, 14, 0, 0});
    page.Children().Append(metrics);

    // Without the helper only part of it runs: say so, with the way to get the rest.
    p.notice = Box(12, Ink(c, 5));
    p.notice.Margin({0, 8, 0, 0});
    p.notice.Padding({12, 10, 10, 10});
    auto noticeRow = Columns({0, -1, 0}, 10);
    noticeRow.Children().Append(Glyph(kGlyphShield, 14, c.muted));
    p.noticeText = Wrapped(L"", 12, c.muted);
    noticeRow.Children().Append(InColumn(p.noticeText, 1));
    auto install = MakeButton(Kind::kSolid, nullptr, T(L"설치", L"Install"), true, [] { InstallHelper(false); });
    install.root.VerticalAlignment(wux::VerticalAlignment::Center);
    noticeRow.Children().Append(InColumn(install.root, 2));
    p.notice.Child(noticeRow);
    page.Children().Append(p.notice);

    p.cleanButton = MakeButton(Kind::kSolid, nullptr, T(L"지금 정리", L"Clean now"), false, [] {
        if (g_cleaning) return;
        StartClean(g_prefs.areas);
        RefreshClean();
    });
    if (auto content = p.cleanButton.root.Child().try_as<wuxc::StackPanel>()) content.Children().InsertAt(0, Sparkle(c.fg));
    p.cleanButton.root.Margin({0, 12, 0, 0});
    page.Children().Append(p.cleanButton.root);

    auto result = HStack(6);
    result.HorizontalAlignment(wux::HorizontalAlignment::Center);
    result.Margin({0, 8, 0, 0});
    p.resultMark = Glyph(kGlyphCheck, 12, c.ok);
    p.result = Text(L"", 12, c.faint);
    result.Children().Append(p.resultMark);
    result.Children().Append(p.result);
    page.Children().Append(result);

    wuxc::Border links;
    links.Margin({0, 12, 0, 0});
    links.Padding({0, 6, 0, 0});
    links.BorderThickness({0, 1, 0, 0});
    links.BorderBrush(Brush(Ink(c, 8)));
    auto linkStack = VStack(0);
    auto link = [&](PCWSTR glyph, PCWSTR label, wuxc::TextBlock& end) {
        auto row = Box(8, 0);
        row.Height(32);
        row.Padding({8, 0, 8, 0});
        auto g = Columns({0, -1, 0}, 10);
        g.Children().Append(Glyph(glyph, 14, c.muted));
        g.Children().Append(InColumn(Text(label, 13, c.fg), 1));
        auto right = HStack(6);
        end = Text(L"", 12, c.faint);
        right.Children().Append(end);
        right.Children().Append(Glyph(kGlyphChevron, 10, c.faint));
        g.Children().Append(InColumn(right, 2));
        row.Child(g);
        OnHover(row, [row, cp = &c] { row.Background(Brush(Ink(*cp, 8))); }, [row] { row.Background(Brush(0)); });
        OnClick(row, [] { ShowPage(Page::kSettings); });
        linkStack.Children().Append(row);
    };
    link(kGlyphAuto, T(L"자동 정리", L"Automatic cleaning"), p.autoEnd);
    link(kGlyphBlock, T(L"예외 프로그램", L"Programs left alone"), p.exclEnd);
    links.Child(linkStack);
    page.Children().Append(links);
    return page;
}

void RefreshClean() {
    if (!g_panel || g_panel->page != Page::kClean || !g_panel->hero) return;
    auto& p = *g_panel;
    const Palette& c = *p.c;
    const Mem& m = g_mem;
    bool alert = m.percent >= kDangerPercent;
    p.hero.Inlines().Clear();
    if (alert) {
        wux::Documents::Run mark;
        mark.Text(std::wstring(kGlyphWarn) + L" ");
        mark.FontFamily(wuxm::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
        mark.FontSize(18);
        p.hero.Inlines().Append(mark);
    }
    wux::Documents::Run number, unit;
    number.Text(std::to_wstring(g_percent));
    unit.Text(L"%");
    unit.FontSize(15);
    unit.Foreground(Brush(c.muted));
    p.hero.Inlines().Append(number);
    p.hero.Inlines().Append(unit);
    p.hero.Foreground(Brush(alert ? c.dangerText : c.fg));
    p.hero.FontWeight(Weight(alert ? 600 : 500));
    p.heroSub.Text(Format(T(L"%s GB / %s GB 사용 중", L"%s GB / %s GB in use"), Gb(m.used).c_str(), Gb(m.total).c_str()));
    p.big.Set(m.percent / 100, BarColor(c, m.percent));
    double commit = m.commitLimit ? double(m.commit) / double(m.commitLimit) : 0;
    p.commitText.Text(Format(L"%s / %s GB", Gb(m.commit).c_str(), Gb(m.commitLimit).c_str()));
    p.commitBar.Set(commit, c.accent);
    p.cacheText.Text(Bytes(m.cache));
    p.cacheBar.Set(m.total ? double(m.cache) / double(m.total) : 0, c.accent);
    p.notice.Visibility(g_helper == HelperState::kMissing || g_helper == HelperState::kOld ? wux::Visibility::Visible
                                                                                            : wux::Visibility::Collapsed);
    p.noticeText.Text(g_helper == HelperState::kOld
                          ? T(L"관리자 도우미를 다시 설치해야 합니다.", L"The admin helper needs installing again.")
                          : T(L"관리자 도우미가 없어 내 프로그램의 작업 집합만 비웁니다.",
                              L"Without the admin helper only your programs' working sets are trimmed."));
    p.cleanButton.label.Text(g_cleaning ? T(L"정리 중…", L"Cleaning…") : T(L"지금 정리", L"Clean now"));
    p.cleanButton.root.Opacity(g_cleaning ? 0.6 : 1.0);
    bool cleaned = g_prefs.lastCleanAt != 0;
    p.resultMark.Visibility(cleaned ? wux::Visibility::Visible : wux::Visibility::Collapsed);
    p.result.Text(cleaned ? Format(T(L"%s 정리 · %s 비움", L"Cleaned %s · %s freed"), Ago(g_prefs.lastCleanAt).c_str(),
                                   Bytes(g_prefs.lastFreed).c_str())
                          : std::wstring(T(L"아직 정리하지 않음", L"Not cleaned yet")));
    if (g_prefs.autoPercentOn)
        p.autoEnd.Text(Format(T(L"%d %% 넘으면", L"Above %d%%"), g_prefs.autoPercent));
    else if (g_prefs.autoIntervalOn)
        p.autoEnd.Text(Format(T(L"%d분마다", L"Every %d min"), g_prefs.autoMinutes));
    else
        p.autoEnd.Text(T(L"꺼짐", L"Off"));
    p.exclEnd.Text(g_prefs.exclude.empty() ? std::wstring(T(L"없음", L"None"))
                                           : Format(T(L"%d개", L"%d"), int(g_prefs.exclude.size())));
}

// ---- programs page ---------------------------------------------------------------

constexpr double kColCpu = 44, kColMem = 58, kRowBar = 150;

void PaintRow(Row& r) {
    const Palette& c = *g_panel->c;
    bool sortCpu = g_prefs.sortCpu;
    r.root.Child(r.confirm ? (wux::UIElement)r.ask : (wux::UIElement)r.line);
    r.line.Background(Brush(r.hover && !r.locked ? Ink(c, 8) : 0));
    r.line.Opacity(r.locked ? 0.55 : r.busy ? 0.6 : 1.0);
    bool showKill = r.hover && !r.locked && !r.busy;
    auto shown = [](bool v) { return v ? wux::Visibility::Visible : wux::Visibility::Collapsed; };
    r.cpuText.Visibility(shown(!showKill && !r.locked));
    r.memText.Visibility(shown(!showKill && !r.locked));
    r.kill.root.Visibility(shown(showKill));
    r.lock.Visibility(shown(r.locked));
    r.title.Text(r.name);
    r.badges.Children().Clear();
    auto badge = [&](const std::wstring& text, bool warn) {
        auto b = Box(6, warn ? WithAlpha(c.warn, 0x2E) : Ink(c, 8));
        b.Height(18);
        b.Padding({5, 0, 5, 0});
        auto row = HStack(3);
        row.VerticalAlignment(wux::VerticalAlignment::Center);
        if (warn) row.Children().Append(Glyph(kGlyphWarn, 10, c.warn));
        row.Children().Append(Text(text, 11, warn ? c.warn : c.muted, 500));
        b.Child(row);
        r.badges.Children().Append(b);
    };
    if (r.targets.size() > 1) badge(Format(T(L"%d개", L"×%d"), int(r.targets.size())), false);
    if (r.hung) badge(T(L"응답 없음", L"Not responding"), true);
    if (r.admin) r.badges.Children().Append(Glyph(kGlyphShield, 11, c.faint));
    bool hot = r.cpu >= 30;
    r.cpuText.Text(r.cpu < 0 ? L"—" : r.cpu < 10 ? Format(L"%.1f %%", r.cpu) : Format(L"%.0f %%", r.cpu));
    r.cpuText.Foreground(Brush(hot ? c.warn : sortCpu ? c.fg : c.muted));
    r.cpuText.FontWeight(Weight(hot ? 600 : sortCpu ? 500 : 400));
    r.memText.Text(Bytes(r.mem));
    r.memText.Foreground(Brush(sortCpu ? c.muted : c.fg));
    r.memText.FontWeight(Weight(sortCpu ? 400 : 500));
    r.askText.Text(r.targets.size() > 1
                       ? Format(T(L"%s %d개를 끝낼까요?", L"End %s (%d)?"), r.name.c_str(), int(r.targets.size()))
                       : Format(T(L"%s를 끝낼까요?", L"End %s?"), r.name.c_str()));
    r.yesText.Text(r.targets.size() > 1 ? T(L"모두 끝내기", L"End all") : T(L"끝내기", L"End"));
}

std::unique_ptr<Row> BuildRow(const std::wstring& key, const std::wstring& name) {
    const Palette& c = *g_panel->c;
    auto r = std::make_unique<Row>();
    Row* raw = r.get();
    r->key = key;
    r->name = name;
    r->root = wuxc::Border();

    r->line = Columns({28, -1, kColCpu, kColMem}, 8);
    r->line.Height(44);
    r->line.Padding({6, 0, 8, 0});
    r->line.CornerRadius({8, 8, 8, 8});
    r->tile = MakeTile(28, 8, name);
    r->tile.root.VerticalAlignment(wux::VerticalAlignment::Center);
    r->line.Children().Append(r->tile.root);

    auto middle = VStack(5);
    middle.VerticalAlignment(wux::VerticalAlignment::Center);
    auto nameLine = HStack(5);
    r->title = Text(name, 13, c.fg, 500);
    r->title.MaxWidth(kRowBar);
    r->badges = HStack(4);
    nameLine.Children().Append(r->title);
    nameLine.Children().Append(r->badges);
    middle.Children().Append(nameLine);
    r->bar = MakeBar(kRowBar, 2);
    middle.Children().Append(r->bar.track);
    r->line.Children().Append(InColumn(middle, 1));

    r->cpuText = Text(L"", 12, c.muted);
    r->cpuText.HorizontalAlignment(wux::HorizontalAlignment::Right);
    r->line.Children().Append(InColumn(r->cpuText, 2));
    r->memText = Text(L"", 12, c.fg, 500);
    r->memText.HorizontalAlignment(wux::HorizontalAlignment::Right);
    r->line.Children().Append(InColumn(r->memText, 3));

    r->kill = MakeButton(Kind::kSolid, kGlyphPower, T(L"끝내기", L"End"), true, [raw] {
        raw->confirm = true;
        PaintRow(*raw);
    });
    r->kill.root.HorizontalAlignment(wux::HorizontalAlignment::Right);
    r->kill.root.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::Grid::SetColumn(r->kill.root, 2);
    wuxc::Grid::SetColumnSpan(r->kill.root, 2);
    r->line.Children().Append(r->kill.root);
    r->lock = Glyph(kGlyphLock, 12, c.faint);
    r->lock.HorizontalAlignment(wux::HorizontalAlignment::Right);
    r->line.Children().Append(InColumn(r->lock, 3));
    OnHover(
        r->line,
        [raw] {
            raw->hover = true;
            PaintRow(*raw);
        },
        [raw] {
            raw->hover = false;
            PaintRow(*raw);
        });

    // The question: cancel has the default look; ending cannot be undone, so it is red.
    r->ask = Box(10, Ink(c, 8));
    r->ask.Margin({0, 2, 0, 2});
    r->ask.Padding({12, 10, 10, 10});
    auto askStack = VStack(8);
    auto question = Columns({0, -1}, 10);
    r->askTile = MakeTile(28, 8, name);
    question.Children().Append(r->askTile.root);
    auto words = VStack(1);
    r->askText = Text(L"", 13, c.fg);
    words.Children().Append(r->askText);
    words.Children().Append(Wrapped(T(L"창을 먼저 닫고, 3초 뒤에도 남은 것만 끝냅니다. 저장을 묻는 창이 뜨면 두고 알려 드립니다.",
                                      L"Its windows close first; whatever still runs after 3 s ends. A program asking "
                                      L"to save is left open."),
                                    12, c.faint));
    question.Children().Append(InColumn(words, 1));
    askStack.Children().Append(question);
    auto buttons = HStack(6);
    buttons.HorizontalAlignment(wux::HorizontalAlignment::Right);
    auto no = MakeButton(Kind::kOutline, nullptr, T(L"취소", L"Cancel"), true, [raw] {
        raw->confirm = false;
        PaintRow(*raw);
    });
    auto yes = MakeButton(Kind::kDanger, nullptr, T(L"끝내기", L"End"), true, [raw] {
        raw->confirm = false;
        raw->busy = true;
        PaintRow(*raw);
        StartKill(raw->name, raw->targets);
    });
    r->yesText = yes.label;
    buttons.Children().Append(no.root);
    buttons.Children().Append(yes.root);
    askStack.Children().Append(buttons);
    r->ask.Child(askStack);
    return r;
}

void SetRowBar(Row& r, double fraction) {
    const Palette& c = *g_panel->c;
    r.bar.Set(fraction, g_prefs.sortCpu && r.cpu >= 30 ? c.warn : c.accent);
}

wux::UIElement BuildPrograms() {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto page = VStack(0);

    auto tools = Columns({-1, 0}, 6);
    tools.Margin({0, 10, 0, 0});
    auto search = Box(8, Ink(c, 8));
    search.Height(28);
    search.Padding({10, 0, 10, 0});
    auto searchRow = Columns({0, -1}, 8);
    searchRow.Children().Append(Glyph(kGlyphSearch, 12, c.faint));
    wuxc::TextBox box;
    StyleTextBox(box, c);
    box.PlaceholderText(T(L"찾기", L"Find"));
    box.Text(p.filter);
    Keep(box.TextChanged(winrt::auto_revoke, [box](auto&&, auto&&) {
        if (!g_panel) return;
        g_panel->filter = Lower(std::wstring(box.Text()));
        RefreshPrograms();
    }));
    searchRow.Children().Append(InColumn(box, 1));
    search.Child(searchRow);
    OnClick(search, [box] { box.Focus(wux::FocusState::Programmatic); });
    tools.Children().Append(search);
    auto sort = MakeSegment({T(L"메모리순", L"Memory"), T(L"CPU순", L"CPU")}, false, 24, g_prefs.sortCpu ? 1 : 0,
                            [](int i) {
                                g_prefs.sortCpu = i == 1;
                                SavePrefs();
                                RefreshPrograms();
                            });
    tools.Children().Append(InColumn(sort.root, 1));
    page.Children().Append(tools);

    auto cols = Columns({-1, kColCpu, kColMem}, 8);
    cols.Padding({42, 10, 8, 4});
    cols.Children().Append(Text(T(L"프로그램", L"Program"), 11, c.faint));
    p.colCpu = Text(L"CPU", 11, c.faint);
    p.colCpu.HorizontalAlignment(wux::HorizontalAlignment::Right);
    p.colMem = Text(T(L"메모리", L"Memory"), 11, c.faint);
    p.colMem.HorizontalAlignment(wux::HorizontalAlignment::Right);
    cols.Children().Append(InColumn(p.colCpu, 1));
    cols.Children().Append(InColumn(p.colMem, 2));
    page.Children().Append(cols);

    wuxc::ScrollViewer scroll;
    scroll.MaxHeight(440);
    scroll.HorizontalScrollBarVisibility(wuxc::ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(wuxc::ScrollBarVisibility::Hidden);
    p.list = VStack(1);
    scroll.Content(p.list);
    page.Children().Append(scroll);

    p.toastBox = Box(10, Ink(c, 8));
    p.toastBox.Padding({12, 9, 12, 9});
    p.toastBox.Margin({0, 8, 0, 0});
    auto toastRow = Columns({0, -1}, 10);
    toastRow.Children().Append(Glyph(kGlyphCheck, 12, c.ok));
    p.toast = Wrapped(L"", 12, c.fg);
    toastRow.Children().Append(InColumn(p.toast, 1));
    p.toastBox.Child(toastRow);
    p.toastBox.Visibility(wux::Visibility::Collapsed);
    page.Children().Append(p.toastBox);

    wuxc::Border footLine;
    footLine.BorderThickness({0, 1, 0, 0});
    footLine.BorderBrush(Brush(Ink(c, 8)));
    footLine.Padding({4, 8, 4, 0});
    footLine.Margin({0, 8, 0, 0});
    p.foot = Text(L"", 12, c.faint);
    footLine.Child(p.foot);
    page.Children().Append(footLine);

    p.rows.clear();
    p.order.clear();
    return page;
}

// Worker: icon pixels and the file's description for one program.
using Pixels = std::vector<uint32_t>;

bool IconPixels(const std::wstring& path, int size, Pixels* pixels) {
    winrt::com_ptr<IShellItem> item;
    winrt::com_ptr<IShellItemImageFactory> factory;
    HBITMAP bitmap = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))) ||
        FAILED(item->QueryInterface(IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->GetImage({size, size}, SIIGBF_ICONONLY, &bitmap)))
        return false;
    BITMAPINFO info{{sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB}};
    pixels->assign(size * size, 0);
    HDC dc = GetDC(nullptr);
    bool ok = GetDIBits(dc, bitmap, 0, size, pixels->data(), &info, DIB_RGB_COLORS) == size;
    ReleaseDC(nullptr, dc);
    DeleteObject(bitmap);
    if (!ok) return false;
    if (std::none_of(pixels->begin(), pixels->end(), [](uint32_t p) { return p >> 24; })) {
        for (uint32_t& p : *pixels) p |= 0xFF000000;
    } else {
        // Straight alpha drawn as premultiplied leaves a light fringe.
        auto straight = [](uint32_t p) {
            uint32_t a = p >> 24;
            return ((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a;
        };
        if (std::any_of(pixels->begin(), pixels->end(), straight))
            for (uint32_t& p : *pixels) {
                uint32_t a = p >> 24, out = a << 24;
                for (int s = 0; s < 24; s += 8) out |= (((p >> s) & 255) * a + 127) / 255 << s;
                p = out;
            }
    }
    return true;
}

std::wstring Description(const std::wstring& path) {
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size || size > 1024 * 1024) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Lang {
        WORD language, codepage;
    }* langs = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &bytes) || bytes < sizeof(Lang))
        return {};
    wchar_t* text = nullptr;
    UINT chars = 0;
    std::wstring key = Format(L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].language, langs[0].codepage);
    if (!VerQueryValueW(data.data(), key.c_str(), (void**)&text, &chars) || !chars) return {};
    std::wstring out(text, wcsnlen(text, chars));
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out.size() <= 40 ? out : std::wstring();
}

wuxm::ImageSource ToBitmap(const Pixels& pixels, int size) {
    wuxm::Imaging::WriteableBitmap bitmap(size, size);
    auto buffer = bitmap.PixelBuffer();
    winrt::com_ptr<::Windows::Storage::Streams::IBufferByteAccess> access;
    BYTE* bytes = nullptr;
    if (buffer.Capacity() < pixels.size() * 4 ||
        FAILED(reinterpret_cast<::IUnknown*>(winrt::get_abi(buffer))
                   ->QueryInterface(winrt::guid_of<::Windows::Storage::Streams::IBufferByteAccess>(), access.put_void())) ||
        FAILED(access->Buffer(&bytes)))
        return nullptr;
    memcpy(bytes, pixels.data(), pixels.size() * 4);
    buffer.Length(uint32_t(pixels.size() * 4));
    bitmap.Invalidate();
    return bitmap;
}

void ShowIcon(Tile& t, const wuxm::ImageSource& source) {
    t.image.Source(source);
    t.letter.Visibility(wux::Visibility::Collapsed);
}

void LoadIcon(Row& r) {
    if (r.iconAsked || r.path.empty()) return;
    r.iconAsked = true;
    std::wstring key = r.key, path = r.path;
    Worker([key, path] {
        constexpr int kSize = 32;
        Pixels pixels;
        bool icon = IconPixels(path, kSize, &pixels);
        std::wstring name = Description(path);
        Post([key, pixels = std::move(pixels), icon, name] {
            if (!g_panel) return;
            if (!name.empty()) g_panel->known[key].second = name;
            auto it = g_panel->rows.find(key);
            if (it == g_panel->rows.end()) return;
            Row& r = *it->second;
            if (!name.empty()) r.name = name;
            if (icon)
                if (auto source = ToBitmap(pixels, kSize)) {
                    ShowIcon(r.tile, source);
                    ShowIcon(r.askTile, source);
                }
            PaintRow(r);
        });
    });
}

void RefreshPrograms() {
    if (!g_panel || g_panel->page != Page::kPrograms || !g_panel->list) return;
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto procs = Snapshot(p.buffer);
    uint64_t now = NowFiletime();
    double elapsed = p.lastCpuAt ? double(now - p.lastCpuAt) : 0;
    static double cores = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> nowCpu;
    std::unordered_set<DWORD> hung;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            if (IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) && IsHungAppWindow(hwnd)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                ((std::unordered_set<DWORD>*)param)->insert(pid);
            }
            return TRUE;
        },
        (LPARAM)&hung);

    struct Group {
        std::wstring file;
        std::vector<KillTarget> targets;
        uint64_t mem = 0;
        double cpu = 0;
        bool cpuKnown = true, admin = false, hung = false;
    };
    std::map<std::wstring, Group> groups;
    for (auto& proc : procs) {
        std::wstring key = Lower(proc.name);
        auto& g = groups[key];
        g.file = proc.name;
        g.targets.push_back({proc.pid, proc.created, proc.privateWs});
        g.mem += proc.privateWs;
        nowCpu[proc.pid] = {proc.created, proc.cpu};
        auto last = p.lastCpu.find(proc.pid);
        if (elapsed > 0 && last != p.lastCpu.end() && last->second.first == proc.created)
            g.cpu += 100.0 * double(proc.cpu - last->second.second) / (elapsed * cores);
        else if (!(elapsed > 0 && last == p.lastCpu.end()))  // a new process shows its share next time
            g.cpuKnown = false;
        g.hung |= hung.contains(proc.pid);
        uint64_t adminKey = (uint64_t(proc.pid) << 32) ^ proc.created;
        auto known = p.admin.find(adminKey);
        if (known == p.admin.end()) known = p.admin.emplace(adminKey, NeedsAdmin(proc.pid)).first;
        g.admin |= known->second;
    }
    p.lastCpu = std::move(nowCpu);
    p.lastCpuAt = now;

    std::vector<std::wstring> order;
    for (auto& [key, g] : groups) {
        auto& known = p.known[key];
        if (known.first.empty()) known.first = ProcessPath(g.targets[0].pid);
        std::wstring name = known.second.empty() ? BaseName(g.file) : known.second;
        if (!p.filter.empty() && Lower(name).find(p.filter) == std::wstring::npos && key.find(p.filter) == std::wstring::npos)
            continue;
        auto& row = p.rows[key];
        if (!row) row = BuildRow(key, name);
        row->name = name;
        row->path = known.first;
        row->targets = g.targets;
        row->mem = g.mem;
        row->cpu = elapsed > 0 && g.cpuKnown ? g.cpu : -1;
        row->locked = Protected(key);
        row->admin = g.admin && !row->locked;
        row->hung = g.hung;
        order.push_back(key);
    }
    // Rows of programs that ended go.
    for (auto it = p.rows.begin(); it != p.rows.end();)
        it = std::ranges::find(order, it->first) == order.end() ? p.rows.erase(it) : std::next(it);
    bool cpu = g_prefs.sortCpu;
    std::ranges::stable_sort(order, [&](auto& a, auto& b) {
        auto &ra = *p.rows[a], &rb = *p.rows[b];
        return cpu ? (ra.cpu != rb.cpu ? ra.cpu > rb.cpu : ra.mem > rb.mem) : ra.mem > rb.mem;
    });
    if (order.size() > 60) order.resize(60);
    double top = 0;
    for (auto& key : order) top = std::max(top, cpu ? p.rows[key]->cpu : double(p.rows[key]->mem));
    for (auto& key : order) {
        Row& r = *p.rows[key];
        PaintRow(r);
        SetRowBar(r, top > 0 ? (cpu ? std::max(0.0, r.cpu) : double(r.mem)) / top : 0);
        LoadIcon(r);
    }
    if (order != p.order) {
        p.list.Children().Clear();
        for (auto& key : order) p.list.Children().Append(p.rows[key]->root);
        p.order = order;
    }
    p.colCpu.Foreground(Brush(cpu ? c.fg : c.faint));
    p.colCpu.FontWeight(Weight(cpu ? 500 : 400));
    p.colMem.Foreground(Brush(cpu ? c.faint : c.fg));
    p.colMem.FontWeight(Weight(cpu ? 400 : 500));
    p.foot.Text(Format(T(L"실행 중 %d개 · 커밋 %s / %s GB", L"%d running · committed %s / %s GB"), int(procs.size()),
                       Gb(g_mem.commit).c_str(), Gb(g_mem.commitLimit).c_str()));
    p.toastBox.Visibility(g_toastText.empty() ? wux::Visibility::Collapsed : wux::Visibility::Visible);
    p.toast.Text(g_toastText);
}

// ---- settings page and the program picker -------------------------------------------

// A page taller than the window scrolls with the wheel or touch, with no scroll bar
// drawn over its right edge.
wux::UIElement Scrolled(const wux::UIElement& content, double maxHeight) {
    wuxc::ScrollViewer scroll;
    scroll.MaxHeight(maxHeight);
    scroll.HorizontalScrollBarVisibility(wuxc::ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(wuxc::ScrollBarVisibility::Hidden);
    scroll.Content(content);
    return scroll;
}

wux::UIElement BuildSettings() {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto page = VStack(0);

    page.Children().Append(SectionHead(T(L"작업표시줄 아이콘", L"Taskbar icon")));
    auto styles = Columns({-1, -1, -1}, 6);
    for (int i = 0; i < 3; i++) {
        bool on = int(g_prefs.icon) == i;
        auto chip = Box(8, on ? c.btn : 0);
        chip.Height(40);
        chip.BorderThickness({on ? 1.5 : 1.0, on ? 1.5 : 1.0, on ? 1.5 : 1.0, on ? 1.5 : 1.0});
        chip.BorderBrush(Brush(Ink(c, on ? 25 : 15)));
        RamIcon icon;
        icon.Build(16.0 / kH);
        icon.Paint(46, IconStyle(i), c);
        icon.canvas.HorizontalAlignment(wux::HorizontalAlignment::Center);
        icon.canvas.VerticalAlignment(wux::VerticalAlignment::Center);
        chip.Child(icon.canvas);
        if (!on)
            OnHover(chip, [chip, cp = &c] { chip.Background(Brush(Ink(*cp, 5))); }, [chip] { chip.Background(Brush(0)); });
        OnClick(chip, [i] {
            g_prefs.icon = IconStyle(i);
            SavePrefs();
            for (auto& w : g_widgets) PaintWidget(*w, true);
            ShowPage(Page::kSettings);
        });
        styles.Children().Append(InColumn(chip, i));
    }
    page.Children().Append(styles);

    page.Children().Append(SectionHead(T(L"정리할 곳", L"What to clean"), T(L"지금 정리와 자동 정리", L"Now and automatic")));
    auto area = [](unsigned bit, PCWSTR ko, PCWSTR en) {
        return MakeKey(T(ko, en), (g_prefs.areas & bit) != 0, [bit](bool on) {
            g_prefs.areas = on ? g_prefs.areas | bit : g_prefs.areas & ~bit;
            if (!g_prefs.areas) g_prefs.areas = kWorkingSets;
            SavePrefs();
        });
    };
    // No wrap panel in the shell's XAML: the keys go in rows of their own.
    auto keyRow = [&](std::initializer_list<wuxc::Border> keys, double top) {
        auto row = HStack(6);
        row.Margin({0, top, 0, 0});
        for (auto& k : keys) row.Children().Append(k);
        page.Children().Append(row);
    };
    keyRow({area(kWorkingSets, L"작업 집합", L"Working sets"),
            area(kSystemFileCache, L"시스템 파일 캐시", L"System file cache")},
           0);
    keyRow({area(kLowPriorityStandby, L"낮은 우선순위 대기", L"Low-priority standby"),
            area(kCombine, L"메모리 합치기", L"Combine memory")},
           6);
    keyRow({area(kRegistryCache, L"레지스트리 캐시", L"Registry cache")}, 6);
    page.Children().Append(Help(T(L"잠깐 멈칫할 수 있음 · 자동 정리에서는 빠짐", L"May stall a moment · never in automatic cleaning")));
    keyRow({area(kStandby, L"대기 목록 전체", L"Whole standby list"), area(kModified, L"수정된 페이지", L"Modified pages")}, 6);

    page.Children().Append(SectionHead(T(L"자동 정리", L"Automatic cleaning")));
    auto percent = NumberBox(g_prefs.autoPercent, 50, 99, L"%", [](int v) {
        g_prefs.autoPercent = v;
        SavePrefs();
    });
    auto minutes = NumberBox(g_prefs.autoMinutes, 1, 1440, T(L"분", L"min"), [](int v) {
        g_prefs.autoMinutes = v;
        SavePrefs();
    });
    page.Children().Append(PanelOf({
        OptionRow(nullptr, T(L"사용률이 넘으면", L"When use passes"), T(L"30초에 한 번까지", L"At most every 30 s"),
                  {percent, MakeSwitch(g_prefs.autoPercentOn,
                                       [](bool on) {
                                           g_prefs.autoPercentOn = on;
                                           SavePrefs();
                                       })}),
        OptionRow(nullptr, T(L"일정한 간격마다", L"Every so often"), L"",
                  {minutes, MakeSwitch(g_prefs.autoIntervalOn,
                                       [](bool on) {
                                           g_prefs.autoIntervalOn = on;
                                           SavePrefs();
                                       })}),
    }));

    page.Children().Append(SectionHead(T(L"예외 프로그램", L"Programs left alone"),
                                       Format(T(L"%d개", L"%d"), int(g_prefs.exclude.size()))));
    if (!g_prefs.exclude.empty()) {
        std::vector<wux::FrameworkElement> rows;
        for (auto& file : g_prefs.exclude) {
            auto known = p.known.find(file);
            std::wstring name = known != p.known.end() && !known->second.second.empty() ? known->second.second : BaseName(file);
            auto row = Columns({22, -1, 0, 0}, 10);
            row.Height(40);
            row.Padding({10, 0, 6, 0});
            auto tile = MakeTile(22, 7, name);
            tile.root.VerticalAlignment(wux::VerticalAlignment::Center);
            row.Children().Append(tile.root);
            row.Children().Append(InColumn(Text(name, 13, c.fg, 500), 1));
            row.Children().Append(InColumn(Text(file, 12, c.faint), 2));
            std::wstring key = file;
            auto remove = MakeButton(Kind::kGhost, kGlyphClose, L"", true, [key] {
                std::erase(g_prefs.exclude, key);
                SavePrefs();
                ShowPage(Page::kSettings);
            });
            remove.root.VerticalAlignment(wux::VerticalAlignment::Center);
            row.Children().Append(InColumn(remove.root, 3));
            rows.push_back(row);
        }
        page.Children().Append(PanelOf(rows));
    }
    // The dashed "add" box: a dashed rectangle under the label.
    wuxc::Grid add;
    add.Height(36);
    add.Margin({0, 6, 0, 0});
    add.Background(Brush(0));
    wuxs::Rectangle dash;
    dash.RadiusX(12);
    dash.RadiusY(12);
    dash.Stroke(Brush(Ink(c, 25)));
    dash.StrokeThickness(1);
    wuxm::DoubleCollection dashes;
    dashes.Append(4);
    dashes.Append(3);
    dash.StrokeDashArray(dashes);
    add.Children().Append(dash);
    auto addLabel = HStack(6);
    addLabel.HorizontalAlignment(wux::HorizontalAlignment::Center);
    addLabel.VerticalAlignment(wux::VerticalAlignment::Center);
    auto addGlyph = Glyph(kGlyphAdd, 11, c.muted);
    auto addText = Text(T(L"실행 중인 프로그램에서 추가", L"Add from running programs"), 12, c.muted, 500);
    addLabel.Children().Append(addGlyph);
    addLabel.Children().Append(addText);
    add.Children().Append(addLabel);
    OnHover(
        add,
        [add, addGlyph, addText, cp = &c] {
            add.Background(Brush(Ink(*cp, 5)));
            addGlyph.Foreground(Brush(cp->fg));
            addText.Foreground(Brush(cp->fg));
        },
        [add, addGlyph, addText, cp = &c] {
            add.Background(Brush(0));
            addGlyph.Foreground(Brush(cp->muted));
            addText.Foreground(Brush(cp->muted));
        });
    OnClick(add, [] { ShowPage(Page::kPick); });
    page.Children().Append(add);
    page.Children().Append(Help(T(L"예외 프로그램은 작업 집합을 비우지 않습니다. 대기 목록과 파일 캐시는 시스템 전체라 예외 없이 비웁니다.",
                                  L"Programs left alone keep their working sets. The standby list and file cache belong "
                                  L"to the whole system and are cleared regardless.")));

    page.Children().Append(SectionHead(T(L"관리자 도우미", L"Admin helper")));
    bool ready = g_helper == HelperState::kReady;
    wux::FrameworkElement lead{nullptr};
    if (ready) {
        wuxs::Ellipse lamp;
        lamp.Width(8);
        lamp.Height(8);
        lamp.Fill(Brush(c.ok));
        lead = lamp;
    } else {
        lead = Glyph(kGlyphShield, 14, c.faint);
    }
    auto helperButton = MakeButton(Kind::kSolid, nullptr, ready ? T(L"제거", L"Remove") : T(L"설치", L"Install"), true,
                                   [ready] { InstallHelper(ready); });
    page.Children().Append(PanelOf({OptionRow(
        lead,
        ready ? T(L"설치됨", L"Installed")
              : g_helper == HelperState::kOld ? T(L"다시 설치 필요", L"Needs installing again") : T(L"없음", L"Not installed"),
        ready ? T(L"정리할 때만 잠깐 실행되고 끝납니다", L"Runs only while cleaning, then exits")
              : T(L"정리할 때만 잠깐 실행되고 끝납니다 · 설치할 때 UAC를 한 번 묻습니다",
                  L"Runs only while cleaning, then exits · installing asks UAC once"),
        {helperButton.root})}));
    return Scrolled(page, 520);
}

// Running programs to leave alone: one snapshot, a press adds.
wux::UIElement BuildPicker() {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto page = VStack(1);
    page.Margin({0, 8, 0, 0});
    std::map<std::wstring, std::wstring> names;
    for (auto& proc : Snapshot(p.buffer)) {
        std::wstring key = Lower(proc.name);
        if (!Protected(key) && PlainExeName(key) && std::ranges::find(g_prefs.exclude, key) == g_prefs.exclude.end())
            names[key] = proc.name;
    }
    for (auto& [key, file] : names) {
        auto known = p.known.find(key);
        std::wstring label = known != p.known.end() && !known->second.second.empty() ? known->second.second : BaseName(file);
        auto row = Box(8, 0);
        row.Height(36);
        row.Padding({8, 0, 8, 0});
        auto g = Columns({22, -1, 0}, 10);
        auto tile = MakeTile(22, 7, label);
        tile.root.VerticalAlignment(wux::VerticalAlignment::Center);
        g.Children().Append(tile.root);
        g.Children().Append(InColumn(Text(label, 13, c.fg), 1));
        g.Children().Append(InColumn(Text(key, 12, c.faint), 2));
        row.Child(g);
        OnHover(row, [row, cp = &c] { row.Background(Brush(Ink(*cp, 8))); }, [row] { row.Background(Brush(0)); });
        std::wstring k = key;
        OnClick(row, [k] {
            if (g_prefs.exclude.size() < 200) g_prefs.exclude.push_back(k);
            SavePrefs();
            ShowPage(Page::kSettings);
        });
        page.Children().Append(row);
    }
    return Scrolled(page, 480);
}

// ---- the frame ---------------------------------------------------------------------

// The top line: [clean | programs] and the gear, or a way back and the page's name.
wux::UIElement BuildHead(Page page) {
    const Palette& c = *g_panel->c;
    if (page == Page::kClean || page == Page::kPrograms) {
        auto row = Columns({-1, 0}, 6);
        auto tabs = MakeSegment({T(L"정리", L"Clean"), T(L"프로그램", L"Programs")}, true, 28,
                                page == Page::kPrograms ? 1 : 0, [](int i) {
                                    Page target = i ? Page::kPrograms : Page::kClean;
                                    if (g_panel && g_panel->page != target) ShowPage(target);
                                });
        row.Children().Append(tabs.root);
        auto gear = MakeButton(Kind::kGhost, kGlyphSettings, L"", false, [] { ShowPage(Page::kSettings); });
        wux::Automation::AutomationProperties::SetName(gear.root, T(L"정리 설정", L"Cleaning settings"));
        gear.root.VerticalAlignment(wux::VerticalAlignment::Center);
        row.Children().Append(InColumn(gear.root, 1));
        return row;
    }
    auto row = Columns({0, -1}, 6);
    row.Height(32);
    bool pick = page == Page::kPick;
    auto back = MakeButton(Kind::kGhost, kGlyphBack, L"", false, [pick] { ShowPage(pick ? Page::kSettings : Page::kClean); });
    back.root.VerticalAlignment(wux::VerticalAlignment::Center);
    row.Children().Append(back.root);
    row.Children().Append(InColumn(
        Text(pick ? T(L"예외로 둘 프로그램", L"Program to leave alone") : T(L"정리 설정", L"Cleaning settings"), 13, c.fg, 600), 1));
    return row;
}

void ShowPage(Page page) {
    if (!g_panel) return;
    auto& p = *g_panel;
    g_c = p.c;
    p.page = page;
    p.hero = nullptr;
    p.list = nullptr;
    ReleaseHandlers();
    wux::UIElement content{nullptr};
    switch (page) {
        case Page::kClean:
            content = BuildClean();
            break;
        case Page::kPrograms:
            p.lastCpuAt = 0;
            p.lastCpu.clear();
            content = BuildPrograms();
            break;
        case Page::kSettings:
            content = BuildSettings();
            break;
        case Page::kPick:
            content = BuildPicker();
            break;
    }
    p.headHost.Child(BuildHead(page));
    p.pageHost.Child(content);
    RefreshClean();
    RefreshPrograms();
}

// A popup window draws the desktop behind it only once DWM lets it.
void AllowHostBackdrop() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM) -> BOOL {
            wchar_t name[32];
            if (GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, L"Xaml_WindowedPopupClass") == 0) {
                BOOL allow = TRUE;
                DwmSetWindowAttribute(window, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &allow, sizeof(allow));
            }
            return TRUE;
        },
        0);
}

// The flyout's own presenter shows nothing: the bubble inside draws the glass.
wux::Style BarePresenter() {
    wux::Style style{winrt::xaml_typename<wuxc::FlyoutPresenter>()};
    auto setters = style.Setters();
    auto set = [&](const wux::DependencyProperty& property, const wf::IInspectable& value) {
        setters.Append(wux::Setter(property, value));
    };
    set(wuxc::Control::PaddingProperty(), box_value(wux::Thickness{}));
    set(wuxc::Control::BackgroundProperty(), Brush(0));
    set(wuxc::Control::BorderThicknessProperty(), box_value(wux::Thickness{}));
    set(wuxc::Control::CornerRadiusProperty(), box_value(wux::CornerRadius{}));
    set(wuxc::FlyoutPresenter::IsDefaultShadowEnabledProperty(), box_value(false));
    set(wux::FrameworkElement::MinWidthProperty(), box_value(0.0));
    set(wux::FrameworkElement::MinHeightProperty(), box_value(0.0));
    set(wux::FrameworkElement::MaxWidthProperty(), box_value(10000.0));
    set(wux::FrameworkElement::MaxHeightProperty(), box_value(10000.0));
    return style;
}

void ClosePanel() {
    if (!g_panel) return;
    auto panel = std::move(g_panel);
    g_panelClosedAt = GetTickCount64();
    HoldTaskbar(nullptr);
    ReleaseHandlers();
    try {
        if (panel->flyout) panel->flyout.Hide();
    } catch (...) {
    }
}

// ---- one add-on window open at a time --------------------------------------------
// Every Bloom add-on with a window keeps a message-only window titled Bloom.Addon.Flyout;
// one about to open its window asks the others to close theirs.

constexpr PCWSTR kFlyoutTitle = L"Bloom.Addon.Flyout";
constexpr PCWSTR kFlyoutClass = L"Bloom.Addon.memory-clean.Flyout";
HWND g_flyoutWnd;

UINT CloseFlyoutMsg() {
    static UINT msg = RegisterWindowMessageW(L"Bloom.Addon.CloseFlyout");
    return msg;
}

HINSTANCE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ThisModule, &module);
    return module;
}

// Created on the taskbar thread, so the request runs where the window lives.
LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == CloseFlyoutMsg()) {
        ClosePanel();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateFlyoutWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = FlyoutWndProc;
    wc.hInstance = ThisModule();
    wc.lpszClassName = kFlyoutClass;
    RegisterClassW(&wc);
    g_flyoutWnd = CreateWindowExW(0, kFlyoutClass, kFlyoutTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  wc.hInstance, nullptr);
    if (!g_flyoutWnd) Wh_Log(L"no flyout window: %u", GetLastError());
}

void DestroyFlyoutWindow() {
    if (g_flyoutWnd) DestroyWindow(g_flyoutWnd);
    g_flyoutWnd = nullptr;
    UnregisterClassW(kFlyoutClass, ThisModule());
}

void CloseOtherFlyouts() {
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, nullptr, kFlyoutTitle));)
        if (w != g_flyoutWnd) PostMessageW(w, CloseFlyoutMsg(), 0, 0);
}

void TogglePanel(Widget& w) {
    if (g_panel) {
        ClosePanel();
        return;
    }
    if (GetTickCount64() - g_panelClosedAt < 300) return;  // the press that just closed it
    CloseOtherFlyouts();
    HoldTaskbar(w.taskbar);
    g_panel = std::make_unique<PanelState>();
    auto& p = *g_panel;
    p.taskbar = w.taskbar;
    p.c = &PaletteFor(w.root);
    g_c = p.c;
    const Palette& c = *p.c;
    g_mem = ReadMem();

    // Width 348, padding 10, radius 12, a w8 edge.
    p.bubble = wuxc::Border();
    p.bubble.Width(348);
    p.bubble.Padding({10, 10, 10, 10});
    p.bubble.CornerRadius({12, 12, 12, 12});
    p.bubble.Background(GlassBrush(c));
    p.bubble.BorderThickness({1, 1, 1, 1});
    p.bubble.BorderBrush(Brush(Ink(c, 8)));
    p.bubble.RequestedTheme(&c == &kLight ? wux::ElementTheme::Light : wux::ElementTheme::Dark);
    auto frame = VStack(0);
    p.headHost = wuxc::Border();
    p.pageHost = wuxc::Border();
    frame.Children().Append(p.headHost);
    frame.Children().Append(p.pageHost);
    p.bubble.Child(frame);

    p.flyout = wuxc::Flyout();
    p.flyout.FlyoutPresenterStyle(BarePresenter());
    p.flyout.Content(p.bubble);
    p.flyout.ShouldConstrainToRootBounds(false);
    p.flyout.ShowMode(wuxc::Primitives::FlyoutShowMode::Standard);
    p.flyout.Opened([](auto&&, auto&&) { AllowHostBackdrop(); });
    p.flyout.Closed([](wf::IInspectable const& sender, auto&&) {
        if (g_panel && g_panel->flyout == sender.try_as<wuxc::Flyout>()) {
            g_panel.reset();
            g_panelClosedAt = GetTickCount64();
            HoldTaskbar(nullptr);
            ReleaseHandlers();
        }
    });
    ShowPage(Page::kClean);
    // The window opens kGap clear of the taskbar's edge, not of the widget in its middle:
    // the widget's distance to that edge is measured in the taskbar's XAML root.
    constexpr double kGap = 8;
    bool below = TaskbarOnTop(w.taskbar);
    double toEdge = 0;
    try {
        if (auto root = w.root.XamlRoot()) {
            auto box = w.root.TransformToVisual(nullptr).TransformBounds(
                {0, 0, float(w.root.ActualWidth()), float(w.root.ActualHeight())});
            toEdge = std::max(0.0, below ? root.Size().Height - (box.Y + box.Height) : double(box.Y));
        }
    } catch (...) {
    }
    wuxc::Primitives::FlyoutShowOptions options;
    options.Placement(below ? wuxc::Primitives::FlyoutPlacementMode::Bottom : wuxc::Primitives::FlyoutPlacementMode::Top);
    options.Position(wf::IReference<wf::Point>(wf::Point{
        float(w.root.ActualWidth() / 2),
        float(below ? w.root.ActualHeight() + toEdge + kGap : -(toEdge + kGap))}));
    p.flyout.ShowAt(w.root, options);
    Worker([] {
        CheckHelper();
        Post([] {
            if (g_panel && g_panel->page == Page::kClean) RefreshClean();
        });
    });
}

// ============================================================================
// Ticks: the number, the widget's place, the window's numbers, automatic cleaning
// ============================================================================

void OnCleaned(uint64_t freed, bool helperUsed, bool ok) {
    g_cleaning = false;
    g_prefs.lastCleanAt = NowFiletime();
    g_prefs.lastFreed = freed;
    SavePrefs();
    (void)helperUsed;
    (void)ok;
    g_mem = ReadMem();
    RefreshClean();
}

void OnKilled(const std::wstring& name, int killed, int left, int blocked, uint64_t freed) {
    if (blocked)
        g_toastText = Format(T(L"%s: 저장 확인 창이 떠 있어 닫지 않았습니다", L"%s asks a question on screen, so it was left open"),
                             name.c_str());
    else if (left)
        g_toastText = Format(T(L"%s: %d개를 끝내지 못했습니다 (관리자 도우미 필요)", L"%s: %d could not be ended (needs the admin helper)"),
                             name.c_str(), left);
    else
        g_toastText = Format(T(L"%s %d개를 끝냈습니다 · %s 돌아옴", L"Ended %d of %s · %s back"), name.c_str(), killed,
                             Bytes(freed).c_str());
    if (!g_korean)
        g_toastText = blocked || left ? g_toastText
                                      : Format(L"Ended %s (%d) · %s back", name.c_str(), killed, Bytes(freed).c_str());
    if (g_panel)
        for (auto& [key, row] : g_panel->rows)
            if (row->name == name) row->busy = false;
    RefreshPrograms();
}

winrt::Windows::System::DispatcherQueueTimer g_timer{nullptr};
int g_tick = 0;
int g_refreshSeconds = 2;

// An integer setting held to its range; make-schema.py reads the range from these calls.
int Clamped(PCWSTR name, int lo, int hi) {
    return std::clamp(Wh_GetIntSetting(name), lo, hi);
}
uint64_t g_lastAutoAt = 0;  // GetTickCount64

void Tick() {
    if (g_unloading) return;
    g_tick++;
    g_mem = ReadMem();
    g_percent = int(g_mem.percent + 0.5);
    if (g_tick % 5 == 1) PlaceWidgets();
    for (auto& w : g_widgets) {
        if (!Parent(w->root)) {
            PlaceWidgets();
            break;
        }
    }
    for (auto& w : g_widgets) PaintWidget(*w);
    if (g_panel) {
        RefreshClean();
        RefreshPrograms();
    }
    // Automatic cleaning, never twice within 30 s; never the lists that can stall.
    uint64_t now = GetTickCount64();
    bool due = false;
    if (g_prefs.autoPercentOn && g_mem.percent >= g_prefs.autoPercent) due = true;
    if (g_prefs.autoIntervalOn &&
        NowFiletime() - g_prefs.lastCleanAt >= uint64_t(g_prefs.autoMinutes) * 60 * 10'000'000ull)
        due = true;
    if (due && !g_cleaning && now - g_lastAutoAt >= 30'000) {
        g_lastAutoAt = now;
        Wh_Log(L"auto clean at %d%%", g_percent);
        StartClean(g_prefs.areas & ~(kStandby | kModified));
    }
}

void Start() {
    g_queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    if (!g_queue) {
        Wh_Log(L"no dispatcher queue on the taskbar thread");
        return;
    }
    LoadPrefs();
    g_look = ReadLook();
    CreateFlyoutWindow();
    g_mem = ReadMem();
    g_percent = int(g_mem.percent + 0.5);
    PlaceWidgets();
    g_timer = g_queue.CreateTimer();
    g_timer.Interval(std::chrono::seconds(g_refreshSeconds));
    g_timer.Tick([](auto&&, auto&&) { Tick(); });
    g_timer.Start();
}

void Stop() {
    if (g_timer) {
        g_timer.Stop();
        g_timer = nullptr;
    }
    ClosePanel();
    DestroyFlyoutWindow();
    for (auto& w : g_widgets) RemoveWidget(*w);
    g_widgets.clear();
    g_queue = nullptr;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");
    g_refreshSeconds = Clamped(L"widget.refreshSeconds", 1, 10);
    if (!HookTaskbarDll()) {
        Wh_Log(L"taskbar symbols missing");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    RunOn(PrimaryTaskbar(), Start);
    Worker([] { CheckHelper(); });
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");
    g_unloading = true;
    RunOn(PrimaryTaskbar(), Stop);
    if (g_subclassed) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_subclassed, TaskbarSubclass);
        g_subclassed = nullptr;
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");
    for (int i = 0; g_pending > 0 && i < 700; i++) Sleep(10);  // workers poll g_unloading
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    *bReload = FALSE;
    int seconds = Clamped(L"widget.refreshSeconds", 1, 10);
    Look look = ReadLook();
    RunOn(PrimaryTaskbar(), [seconds, look] {
        g_refreshSeconds = seconds;
        if (g_timer) g_timer.Interval(std::chrono::seconds(seconds));
        g_look = look;
        for (auto& w : g_widgets) PaintWidget(*w, true);
        ClosePanel();  // the window opens again in the new look
    });
    return TRUE;
}
