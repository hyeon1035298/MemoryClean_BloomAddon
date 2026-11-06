// SPDX-License-Identifier: GPL-3.0-only
// What the mod (in explorer) and MemoryCleanHelper.exe (the admin helper) agree on.
// Change one side, change both, and raise kHelperProtocol.
#pragma once

#include <windows.h>

namespace memclean {

// The helper refuses a request of another protocol (exit code kExitProtocol); the mod
// then offers to install the helper again.
constexpr int kHelperProtocol = 1;

// Where the helper lives once installed: under Program Files, so only an administrator
// can replace the file the elevated task runs.
constexpr PCWSTR kInstallFolder = L"Bloom Add-ons\\MemoryClean";  // under %ProgramFiles%
constexpr PCWSTR kHelperExe = L"MemoryCleanHelper.exe";

// The scheduled task, one per user: folder \Bloom, name "MemoryClean <user SID>".
constexpr PCWSTR kTaskFolder = L"\\Bloom";
constexpr PCWSTR kTaskPrefix = L"MemoryClean ";

// The add-on's data folder, below %LOCALAPPDATA%.
constexpr PCWSTR kDataFolder = L"Bloom\\addons\\memory-clean";
constexpr PCWSTR kPrefsFile = L"prefs.json";          // the mod writes, the helper reads "exclude"
constexpr PCWSTR kResultFile = L"helper-result.json";  // the helper writes, the mod reads

// Areas to clean, the bits of a clean request's mask.
enum Area : unsigned {
    kWorkingSets = 1u << 0,         // working sets of processes
    kSystemFileCache = 1u << 1,     // the system file cache's working set
    kLowPriorityStandby = 1u << 2,  // standby pages of priority 0
    kStandby = 1u << 3,             // the whole standby list
    kModified = 1u << 4,            // the modified page list, written out
    kCombine = 1u << 5,             // identical pages combined (Windows 10+)
    kRegistryCache = 1u << 6,       // the registry's cached hives (Windows 8.1+)
    kAllAreas = (1u << 7) - 1,
};
// The defaults: everything but the two lists whose clearing can stall the PC a moment.
constexpr unsigned kDefaultAreas = kAllAreas & ~(kStandby | kModified);

// Helper exit codes.
constexpr int kExitOk = 0;
constexpr int kExitPartly = 1;     // some area or process failed (the result file says which)
constexpr int kExitBadArgs = 2;
constexpr int kExitProtocol = 3;
constexpr int kExitNotElevated = 4;
constexpr int kExitFailed = 5;

}  // namespace memclean
