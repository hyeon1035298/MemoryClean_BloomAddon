// SPDX-License-Identifier: GPL-3.0-only
// MemoryCleanHelper.exe: the administrator half of the Memory Clean add-on.
//
// The mod runs inside explorer.exe at medium integrity. Whatever needs administrator
// rights it hands to this helper through a per-user scheduled task that runs with the
// user's highest privileges. Each run does exactly one job, writes a result file into
// the add-on's data folder and exits.
//
//   --install            copy this exe under Program Files and register the task
//   --uninstall          remove the task, and the installed exe once no user's task is left
//   --run v<n> clean <mask> <id>
//   --run v<n> kill <id> <pid>:<creationTimeHex>[,<pid>:<creationTimeHex>...]
//
// The task runs "--run $(Arg0)": only requests reach the elevated helper through it.
// Any medium-integrity process of the user can start the task with any argument, so
// every request is validated strictly and nothing named by an argument is ever opened,
// loaded or run. The only files written by a task run live in the data folder below
// %LOCALAPPDATA% of the helper's own token, reached through a verified directory handle.

#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <taskschd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../common/memclean_protocol.h"

using namespace memclean;

// ---------------------------------------------------------------------------------------
// Native API declarations not covered by the SDK headers.
// Values from phnt (ntexapi.h, MIT license) and the Windows documentation.

extern "C" {
NTSTATUS NTAPI NtSetSystemInformation(ULONG SystemInformationClass,
                                      PVOID SystemInformation,
                                      ULONG SystemInformationLength);
NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW VersionInformation);
}

namespace {

constexpr ULONG kSystemMemoryListInformation = 80;              // 0x50
constexpr ULONG kSystemCombinePhysicalMemoryInformation = 130;  // 0x82
constexpr ULONG kSystemRegistryReconciliationInformation = 155; // 0x9B

// SYSTEM_MEMORY_LIST_COMMAND values.
constexpr ULONG kMemoryEmptyWorkingSets = 2;
constexpr ULONG kMemoryFlushModifiedList = 3;
constexpr ULONG kMemoryPurgeStandbyList = 4;
constexpr ULONG kMemoryPurgeLowPriorityStandbyList = 5;

struct MemoryCombineInformationEx {
    HANDLE Handle;
    ULONG_PTR PagesCombined;
    ULONG Flags;
};

constexpr DWORD kBuildWindows81 = 9600;
constexpr DWORD kBuildWindows10 = 10240;

constexpr size_t kMaxIdLength = 40;
constexpr size_t kMaxKillTargets = 256;
constexpr size_t kMaxExclusions = 200;
constexpr size_t kMaxExeNameLength = 260;
constexpr LONGLONG kMaxPrefsBytes = 256 * 1024;
constexpr DWORD kKillWaitMs = 2000;

// Image names the kill request never ends, compared case-insensitively.
constexpr PCWSTR kProtectedImages[] = {
    L"system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
    L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe", L"dwm.exe",
    L"fontdrvhost.exe", L"sihost.exe", L"explorer.exe", L"taskhostw.exe",
    L"ctfmon.exe", L"memorycleanhost.exe", L"memorycleanhelper.exe", L"bloom.exe",
    L"registry", L"memcompression", L"securityhealthservice.exe", L"msmpeng.exe",
    L"audiodg.exe", L"conhost.exe", L"runtimebroker.exe",
    L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"searchhost.exe",
    L"textinputhost.exe", L"lockapp.exe", L"logonui.exe", L"userinit.exe",
};

// ---------------------------------------------------------------------------------------
// Small RAII helpers.

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void Reset(HANDLE h = nullptr) {
        if (h_) CloseHandle(h_);
        h_ = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    }
    HANDLE Get() const { return h_; }
    HANDLE* Put() { Reset(); return &h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

template <class T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { Reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    void Reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** Put() { Reset(); return &p_; }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

class Bstr {
public:
    explicit Bstr(PCWSTR s) : b_(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR Get() const { return b_; }
    explicit operator bool() const { return b_ != nullptr; }

private:
    BSTR b_;
};

VARIANT EmptyVariant() {
    VARIANT v;
    VariantInit(&v);
    return v;
}

// A VARIANT that borrows a BSTR; the Bstr keeps ownership.
VARIANT BstrVariant(const Bstr& b) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = b.Get();
    return v;
}

bool EqualsNoCase(const std::wstring& a, PCWSTR b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b, -1, TRUE) ==
           CSTR_EQUAL;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path)
        result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring ModulePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

// ---------------------------------------------------------------------------------------
// Token queries.

bool IsElevated() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.Put())) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation),
                             &size))
        return false;
    return elevation.TokenIsElevated != 0;
}

// Returns the TOKEN_USER of a token in a buffer, or an empty buffer.
std::vector<BYTE> TokenUserOf(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<BYTE> buf(size);
    if (!GetTokenInformation(token, TokenUser, buf.data(), size, &size)) return {};
    return buf;
}

PSID SidOf(std::vector<BYTE>& tokenUser) {
    return reinterpret_cast<TOKEN_USER*>(tokenUser.data())->User.Sid;
}

bool TokenSessionOf(HANDLE token, DWORD* session) {
    DWORD size = 0;
    return GetTokenInformation(token, TokenSessionId, session, sizeof(*session), &size) != 0;
}

std::wstring SelfSidString() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.Put())) return {};
    std::vector<BYTE> user = TokenUserOf(token.Get());
    if (user.empty()) return {};
    PWSTR str = nullptr;
    if (!ConvertSidToStringSidW(SidOf(user), &str)) return {};
    std::wstring result = str;
    LocalFree(str);
    return result;
}

bool EnablePrivilege(HANDLE token, PCWSTR name) {
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) return false;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr))
        return false;
    return GetLastError() == ERROR_SUCCESS;
}

void EnablePrivileges(std::initializer_list<PCWSTR> names) {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          token.Put()))
        return;
    // A missing privilege shows up later as a failed call; nothing to do here.
    for (PCWSTR name : names) EnablePrivilege(token.Get(), name);
}

DWORD OsBuild() {
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!NT_SUCCESS(RtlGetVersion(&info))) return 0;
    return info.dwBuildNumber;
}

// ---------------------------------------------------------------------------------------
// The data folder, opened as a verified handle.
//
// A medium-integrity process of the same user can rename folders or plant junctions in
// its own profile. The helper therefore opens the data folder once, checks that the
// handle really is <LocalAppData>\<kDataFolder> inside the user's profile, and creates
// or opens files only relative to that handle.

std::wstring FinalPathOf(HANDLE h) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(n + 1);
    }
    // Drop the \\?\ prefix; turn \\?\UNC\server\share into \\server\share.
    if (buf.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + buf.substr(8);
    if (buf.rfind(L"\\\\?\\", 0) == 0) return buf.substr(4);
    return buf;
}

HANDLE OpenDirectory(const std::wstring& path, bool noReparse) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (noReparse ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
    return CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags,
                       nullptr);
}

bool IsPlainDirectory(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return false;
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

// Opens (creating as needed) the data folder; returns nullptr when it cannot be trusted.
HANDLE OpenDataFolder() {
    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    std::wstring profile = KnownFolder(FOLDERID_Profile);
    if (local.empty() || profile.empty()) return nullptr;

    Handle localDir(OpenDirectory(local, false));
    Handle profileDir(OpenDirectory(profile, false));
    if (!localDir || !profileDir) return nullptr;
    std::wstring base = FinalPathOf(localDir.Get());
    std::wstring profileFinal = FinalPathOf(profileDir.Get());
    if (base.empty() || profileFinal.empty()) return nullptr;
    // LocalAppData comes from a user-writable registry key; keep it inside the profile,
    // whose location only an administrator can change.
    profileFinal += L'\\';
    if (base.size() <= profileFinal.size() ||
        CompareStringOrdinal(base.c_str(), static_cast<int>(profileFinal.size()),
                             profileFinal.c_str(), static_cast<int>(profileFinal.size()),
                             TRUE) != CSTR_EQUAL)
        return nullptr;

    // Create the folder one component at a time, refusing junctions and links.
    std::wstring path = base;
    std::wstring rest = kDataFolder;
    size_t start = 0;
    while (start <= rest.size()) {
        size_t end = rest.find(L'\\', start);
        if (end == std::wstring::npos) end = rest.size();
        path += L'\\';
        path += rest.substr(start, end - start);
        if (!CreateDirectoryW(path.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return nullptr;
        Handle part(OpenDirectory(path, true));
        if (!part || !IsPlainDirectory(part.Get())) return nullptr;
        start = end + 1;
    }

    HANDLE dir = OpenDirectory(path, true);
    if (dir == INVALID_HANDLE_VALUE) return nullptr;
    std::wstring opened = FinalPathOf(dir);
    if (!IsPlainDirectory(dir) || opened.empty() ||
        CompareStringOrdinal(opened.c_str(), -1, path.c_str(), -1, TRUE) != CSTR_EQUAL) {
        CloseHandle(dir);
        return nullptr;
    }
    return dir;
}

// Opens or creates a file by name relative to a directory handle.
NTSTATUS OpenRelative(HANDLE dir, const std::wstring& name, ACCESS_MASK access,
                      ULONG share, ULONG disposition, HANDLE* out) {
    UNICODE_STRING us;
    us.Buffer = const_cast<PWSTR>(name.c_str());
    us.Length = static_cast<USHORT>(name.size() * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, dir, nullptr);
    IO_STATUS_BLOCK iosb{};
    return NtCreateFile(out, access | SYNCHRONIZE, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL,
                        share, disposition,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                            FILE_OPEN_REPARSE_POINT,
                        nullptr, 0);
}

// Writes the result file atomically: a new temp file, then a rename over the old one.
bool WriteResultFile(HANDLE dir, const std::string& json) {
    wchar_t suffix[48];
    swprintf(suffix, 48, L".%lu.%llx.tmp", GetCurrentProcessId(),
             static_cast<unsigned long long>(GetTickCount64()));
    std::wstring tempName = std::wstring(kResultFile) + suffix;

    Handle file;
    if (!NT_SUCCESS(OpenRelative(dir, tempName, GENERIC_WRITE | DELETE, 0, FILE_CREATE,
                                 file.Put())))
        return false;

    DWORD written = 0;
    bool ok = WriteFile(file.Get(), json.data(), static_cast<DWORD>(json.size()), &written,
                        nullptr) &&
              written == json.size();
    if (ok) {
        std::wstring target = kResultFile;
        std::vector<BYTE> buf(sizeof(FILE_RENAME_INFORMATION) + target.size() * sizeof(WCHAR));
        auto* rename = reinterpret_cast<FILE_RENAME_INFORMATION*>(buf.data());
        rename->ReplaceIfExists = TRUE;
        rename->RootDirectory = dir;
        rename->FileNameLength = static_cast<ULONG>(target.size() * sizeof(WCHAR));
        memcpy(rename->FileName, target.data(), rename->FileNameLength);
        IO_STATUS_BLOCK iosb{};
        ok = NT_SUCCESS(NtSetInformationFile(file.Get(), &iosb, rename,
                                             static_cast<ULONG>(buf.size()),
                                             FileRenameInformation));
    }
    if (!ok) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        SetFileInformationByHandle(file.Get(), FileDispositionInfo, &disposition,
                                   sizeof(disposition));
    }
    return ok;
}

// Reads a small file relative to the data folder; returns false when missing or unsafe.
bool ReadSmallFile(HANDLE dir, PCWSTR name, std::string* out) {
    Handle file;
    if (!NT_SUCCESS(OpenRelative(dir, name, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 FILE_OPEN, file.Put())))
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.Get(), &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart > kMaxPrefsBytes) return false;
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!out->empty() &&
        (!ReadFile(file.Get(), out->data(), static_cast<DWORD>(out->size()), &read, nullptr) ||
         read != out->size()))
        return false;
    return true;
}

// ---------------------------------------------------------------------------------------
// prefs.json: a minimal JSON reader that validates the whole document and keeps only
// the string entries of the top-level "exclude" array.

class PrefsReader {
public:
    PrefsReader(const wchar_t* begin, const wchar_t* end) : p_(begin), end_(end) {}

    bool ReadExclusions(std::vector<std::wstring>* out) {
        std::vector<std::wstring> found;
        if (!Eat(L'{')) return false;
        if (!Eat(L'}')) {
            for (;;) {
                std::wstring key;
                SkipWs();
                if (!ReadString(&key) || !Eat(L':')) return false;
                SkipWs();
                if (key == L"exclude" && p_ < end_ && *p_ == L'[') {
                    if (!ReadExcludeArray(&found)) return false;
                } else if (!SkipValue(1)) {
                    return false;
                }
                if (Eat(L',')) continue;
                if (Eat(L'}')) break;
                return false;
            }
        }
        SkipWs();
        if (p_ != end_) return false;
        *out = std::move(found);
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    void SkipWs() {
        while (p_ < end_ && (*p_ == L' ' || *p_ == L'\t' || *p_ == L'\n' || *p_ == L'\r'))
            ++p_;
    }

    bool Eat(wchar_t c) {
        SkipWs();
        if (p_ < end_ && *p_ == c) {
            ++p_;
            return true;
        }
        return false;
    }

    static int HexValue(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }

    // Reads a string at the current position; out may be null to skip it.
    bool ReadString(std::wstring* out) {
        if (p_ >= end_ || *p_ != L'"') return false;
        ++p_;
        while (p_ < end_) {
            wchar_t c = *p_++;
            if (c == L'"') return true;
            if (c < 0x20) return false;
            if (c == L'\\') {
                if (p_ >= end_) return false;
                wchar_t e = *p_++;
                switch (e) {
                    case L'"': c = L'"'; break;
                    case L'\\': c = L'\\'; break;
                    case L'/': c = L'/'; break;
                    case L'b': c = L'\b'; break;
                    case L'f': c = L'\f'; break;
                    case L'n': c = L'\n'; break;
                    case L'r': c = L'\r'; break;
                    case L't': c = L'\t'; break;
                    case L'u': {
                        if (end_ - p_ < 4) return false;
                        unsigned v = 0;
                        for (int i = 0; i < 4; ++i) {
                            int h = HexValue(*p_++);
                            if (h < 0) return false;
                            v = v * 16 + static_cast<unsigned>(h);
                        }
                        c = static_cast<wchar_t>(v);
                        break;
                    }
                    default: return false;
                }
            }
            if (out && out->size() <= kMaxExeNameLength) out->push_back(c);
        }
        return false;
    }

    bool ReadLiteral(PCWSTR literal) {
        for (; *literal; ++literal, ++p_) {
            if (p_ >= end_ || *p_ != *literal) return false;
        }
        return true;
    }

    bool ReadNumber() {
        const wchar_t* start = p_;
        while (p_ < end_ && ((*p_ >= L'0' && *p_ <= L'9') || *p_ == L'-' || *p_ == L'+' ||
                             *p_ == L'.' || *p_ == L'e' || *p_ == L'E'))
            ++p_;
        return p_ != start;
    }

    bool SkipValue(int depth) {
        if (depth > kMaxDepth) return false;
        SkipWs();
        if (p_ >= end_) return false;
        switch (*p_) {
            case L'{':
                ++p_;
                if (Eat(L'}')) return true;
                for (;;) {
                    SkipWs();
                    if (!ReadString(nullptr) || !Eat(L':') || !SkipValue(depth + 1))
                        return false;
                    if (Eat(L',')) continue;
                    return Eat(L'}');
                }
            case L'[':
                ++p_;
                if (Eat(L']')) return true;
                for (;;) {
                    if (!SkipValue(depth + 1)) return false;
                    if (Eat(L',')) continue;
                    return Eat(L']');
                }
            case L'"': return ReadString(nullptr);
            case L't': return ReadLiteral(L"true");
            case L'f': return ReadLiteral(L"false");
            case L'n': return ReadLiteral(L"null");
            default: return ReadNumber();
        }
    }

    static bool IsPlainExeName(const std::wstring& s) {
        if (s.size() < 5 || s.size() > kMaxExeNameLength) return false;
        for (wchar_t c : s) {
            if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) return false;
        }
        return CompareStringOrdinal(s.c_str() + s.size() - 4, 4, L".exe", 4, TRUE) ==
               CSTR_EQUAL;
    }

    bool ReadExcludeArray(std::vector<std::wstring>* found) {
        found->clear();
        ++p_;  // '['
        if (Eat(L']')) return true;
        for (;;) {
            SkipWs();
            if (p_ < end_ && *p_ == L'"') {
                std::wstring name;
                if (!ReadString(&name)) return false;
                if (IsPlainExeName(name) && found->size() < kMaxExclusions)
                    found->push_back(std::move(name));
            } else if (!SkipValue(2)) {
                return false;
            }
            if (Eat(L',')) continue;
            return Eat(L']');
        }
    }

    const wchar_t* p_;
    const wchar_t* end_;
};

std::vector<std::wstring> ReadExclusions(HANDLE dataDir) {
    std::string bytes;
    if (!ReadSmallFile(dataDir, kPrefsFile, &bytes) || bytes.empty()) return {};
    size_t skip = (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
    int len = static_cast<int>(bytes.size() - skip);
    if (len == 0) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip, len,
                                   nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring text(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip, len, text.data(),
                        wlen);
    std::vector<std::wstring> result;
    PrefsReader reader(text.data(), text.data() + text.size());
    if (!reader.ReadExclusions(&result)) return {};
    return result;
}

// ---------------------------------------------------------------------------------------
// Result JSON.

std::string UtcNow() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// The id is validated to [0-9A-Za-z-], so it needs no escaping.
std::string ResultHead(const std::string& id, bool ok) {
    return "{\"v\":1,\"id\":\"" + id + "\",\"ok\":" + (ok ? "true" : "false");
}

std::string ResultTail() { return ",\"at\":\"" + UtcNow() + "\"}"; }

// ---------------------------------------------------------------------------------------
// Request parsing.

bool ParseDecimal(const std::wstring& s, size_t maxDigits, unsigned long long* out) {
    if (s.empty() || s.size() > maxDigits || (s.size() > 1 && s[0] == L'0')) return false;
    unsigned long long v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<unsigned>(c - L'0');
    }
    *out = v;
    return true;
}

bool ParseHex16(const std::wstring& s, unsigned long long* out) {
    if (s.size() != 16) return false;
    unsigned long long v = 0;
    for (wchar_t c : s) {
        unsigned d;
        if (c >= L'0' && c <= L'9') d = static_cast<unsigned>(c - L'0');
        else if (c >= L'a' && c <= L'f') d = static_cast<unsigned>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') d = static_cast<unsigned>(c - L'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

bool ParseId(const std::wstring& s, std::string* out) {
    if (s.empty() || s.size() > kMaxIdLength) return false;
    std::string id;
    for (wchar_t c : s) {
        bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                  (c >= L'A' && c <= L'Z') || c == L'-';
        if (!ok) return false;
        id.push_back(static_cast<char>(c));
    }
    *out = std::move(id);
    return true;
}

bool ParseVersion(const std::wstring& s, unsigned long long* out) {
    if (s.size() < 2 || s[0] != L'v') return false;
    return ParseDecimal(s.substr(1), 9, out);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t end = s.find(sep, start);
        if (end == std::wstring::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
}

struct KillTarget {
    DWORD pid;
    unsigned long long created;
};

bool ParseKillTargets(const std::wstring& s, std::vector<KillTarget>* out) {
    std::vector<std::wstring> items = Split(s, L',');
    if (items.empty() || items.size() > kMaxKillTargets) return false;
    for (const std::wstring& item : items) {
        size_t colon = item.find(L':');
        if (colon == std::wstring::npos) return false;
        unsigned long long pid = 0, created = 0;
        if (!ParseDecimal(item.substr(0, colon), 10, &pid) || pid == 0 || pid > 0xFFFFFFFFull ||
            !ParseHex16(item.substr(colon + 1), &created))
            return false;
        out->push_back({static_cast<DWORD>(pid), created});
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// clean

NTSTATUS MemoryListCommand(ULONG command) {
    return NtSetSystemInformation(kSystemMemoryListInformation, &command, sizeof(command));
}

bool IsExcluded(PCWSTR image, const std::vector<std::wstring>& exclusions) {
    for (const std::wstring& name : exclusions) {
        if (EqualsNoCase(name, image)) return true;
    }
    return false;
}

// Empties every process's working set except the excluded images. Processes that
// cannot be opened (protected ones, ones that just exited) are skipped as normal.
bool EmptyWorkingSetsExcept(const std::vector<std::wstring>& exclusions) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD self = GetCurrentProcessId();
    int emptied = 0;
    for (BOOL more = Process32FirstW(snapshot.Get(), &entry); more;
         more = Process32NextW(snapshot.Get(), &entry)) {
        DWORD pid = entry.th32ProcessID;
        if (pid == 0 || pid == 4 || pid == self) continue;
        if (IsExcluded(entry.szExeFile, exclusions)) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE,
                                   pid));
        if (!process) continue;
        if (EmptyWorkingSet(process.Get())) ++emptied;
    }
    return emptied > 0;
}

bool CleanWorkingSets(HANDLE dataDir) {
    std::vector<std::wstring> exclusions = ReadExclusions(dataDir);
    if (exclusions.empty())
        return NT_SUCCESS(MemoryListCommand(kMemoryEmptyWorkingSets));
    return EmptyWorkingSetsExcept(exclusions);
}

bool CleanSystemFileCache() {
    // Documented: both sizes (SIZE_T)-1 flush as many pages as possible from the
    // system file cache's working set.
    return SetSystemFileCacheSize(static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0) != 0;
}

bool CombineMemory() {
    MemoryCombineInformationEx info{};
    return NT_SUCCESS(NtSetSystemInformation(kSystemCombinePhysicalMemoryInformation, &info,
                                             sizeof(info)));
}

bool ReconcileRegistry() {
    return NT_SUCCESS(
        NtSetSystemInformation(kSystemRegistryReconciliationInformation, nullptr, 0));
}

int RunClean(unsigned mask, const std::string& id, HANDLE dataDir) {
    EnablePrivileges({SE_PROF_SINGLE_PROCESS_NAME, SE_INCREASE_QUOTA_NAME, SE_DEBUG_NAME});
    DWORD build = OsBuild();

    struct Step {
        unsigned bit;
        const char* name;
    };
    static constexpr Step kSteps[] = {
        {kWorkingSets, "workingSets"},     {kSystemFileCache, "systemFileCache"},
        {kLowPriorityStandby, "lowPriorityStandby"}, {kStandby, "standby"},
        {kModified, "modified"},           {kCombine, "combine"},
        {kRegistryCache, "registryCache"},
    };

    std::string failed;
    for (const Step& step : kSteps) {
        if (!(mask & step.bit)) continue;
        bool ok = true;
        switch (step.bit) {
            case kWorkingSets: ok = CleanWorkingSets(dataDir); break;
            case kSystemFileCache: ok = CleanSystemFileCache(); break;
            case kLowPriorityStandby:
                ok = NT_SUCCESS(MemoryListCommand(kMemoryPurgeLowPriorityStandbyList));
                break;
            case kStandby: ok = NT_SUCCESS(MemoryListCommand(kMemoryPurgeStandbyList)); break;
            case kModified: ok = NT_SUCCESS(MemoryListCommand(kMemoryFlushModifiedList)); break;
            // Areas the running Windows lacks are skipped, not reported as failed.
            case kCombine: ok = build < kBuildWindows10 || CombineMemory(); break;
            case kRegistryCache: ok = build < kBuildWindows81 || ReconcileRegistry(); break;
            default: break;
        }
        if (!ok) {
            if (!failed.empty()) failed += ',';
            failed += '"';
            failed += step.name;
            failed += '"';
        }
    }

    bool allOk = failed.empty();
    std::string json = ResultHead(id, allOk) + ",\"failed\":[" + failed + "]" + ResultTail();
    if (!WriteResultFile(dataDir, json)) return kExitFailed;
    return allOk ? kExitOk : kExitPartly;
}

// ---------------------------------------------------------------------------------------
// kill

bool IsProtectedImage(const std::wstring& fileName) {
    for (PCWSTR name : kProtectedImages) {
        if (EqualsNoCase(fileName, name)) return true;
    }
    return false;
}

std::wstring ImageFileName(HANDLE process) {
    std::wstring buf(32768, L'\0');
    DWORD size = static_cast<DWORD>(buf.size());
    if (!QueryFullProcessImageNameW(process, 0, buf.data(), &size)) return {};
    buf.resize(size);
    size_t slash = buf.find_last_of(L"\\/");
    return slash == std::wstring::npos ? buf : buf.substr(slash + 1);
}

// Ends one process, but only when it is still the same process (creation time), runs
// as this user in this session, and is not on the protected list.
bool KillOne(const KillTarget& target, PSID selfSid, DWORD selfSession) {
    if (target.pid == GetCurrentProcessId()) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                                   SYNCHRONIZE,
                               FALSE, target.pid));
    if (!process) return false;

    FILETIME created, exited, kernelTime, userTime;
    if (!GetProcessTimes(process.Get(), &created, &exited, &kernelTime, &userTime)) return false;
    unsigned long long createdValue =
        (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    if (createdValue != target.created) return false;

    Handle token;
    if (!OpenProcessToken(process.Get(), TOKEN_QUERY, token.Put())) return false;
    std::vector<BYTE> user = TokenUserOf(token.Get());
    if (user.empty() || !EqualSid(SidOf(user), selfSid)) return false;
    DWORD session = 0;
    if (!TokenSessionOf(token.Get(), &session) || session != selfSession) return false;

    std::wstring image = ImageFileName(process.Get());
    if (image.empty() || IsProtectedImage(image)) return false;

    if (!TerminateProcess(process.Get(), 1)) return false;
    return WaitForSingleObject(process.Get(), kKillWaitMs) == WAIT_OBJECT_0;
}

int RunKill(const std::vector<KillTarget>& targets, const std::string& id, HANDLE dataDir) {
    EnablePrivileges({SE_DEBUG_NAME});

    Handle selfToken;
    std::vector<BYTE> selfUser;
    DWORD selfSession = 0;
    bool selfKnown = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, selfToken.Put()) &&
                     !(selfUser = TokenUserOf(selfToken.Get())).empty() &&
                     TokenSessionOf(selfToken.Get(), &selfSession);

    int killed = 0, failed = 0;
    for (const KillTarget& target : targets) {
        if (selfKnown && KillOne(target, SidOf(selfUser), selfSession)) ++killed;
        else ++failed;
    }

    std::string json = ResultHead(id, failed == 0) + ",\"killed\":" + std::to_string(killed) +
                       ",\"failed\":" + std::to_string(failed) + ResultTail();
    if (!WriteResultFile(dataDir, json)) return kExitFailed;
    return failed == 0 ? kExitOk : kExitPartly;
}

// ---------------------------------------------------------------------------------------
// Task requests: "v<n> clean <mask> <id>" or "v<n> kill <id> <targets>".
// Task Scheduler substitutes the request into the command line unquoted, so it arrives
// either as four arguments or, when quoted, as one argument holding four fields.

int RunRequest(const std::vector<std::wstring>& fields) {
    if (fields.size() != 4) return kExitBadArgs;
    unsigned long long version = 0;
    if (!ParseVersion(fields[0], &version)) return kExitBadArgs;
    const std::wstring& verb = fields[1];
    bool isClean = verb == L"clean";
    bool isKill = verb == L"kill";
    if (!isClean && !isKill) return kExitBadArgs;

    std::string id;
    bool idOk = ParseId(isClean ? fields[3] : fields[2], &id);

    if (version != static_cast<unsigned long long>(kHelperProtocol)) {
        if (idOk) {
            Handle dataDir(OpenDataFolder());
            if (dataDir)
                WriteResultFile(dataDir.Get(),
                                ResultHead(id, false) + ",\"error\":\"protocol\"" + ResultTail());
        }
        return kExitProtocol;
    }
    if (!idOk) return kExitBadArgs;

    unsigned long long mask = 0;
    std::vector<KillTarget> targets;
    if (isClean) {
        if (!ParseDecimal(fields[2], 3, &mask) || mask < 1 || mask > kAllAreas)
            return kExitBadArgs;
    } else if (!ParseKillTargets(fields[3], &targets)) {
        return kExitBadArgs;
    }

    if (!IsElevated()) return kExitNotElevated;
    Handle dataDir(OpenDataFolder());
    if (!dataDir) return kExitFailed;

    return isClean ? RunClean(static_cast<unsigned>(mask), id, dataDir.Get())
                   : RunKill(targets, id, dataDir.Get());
}

// ---------------------------------------------------------------------------------------
// --install / --uninstall

std::wstring InstallFolder() {
    std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
    if (programFiles.empty()) return {};
    return programFiles + L"\\" + kInstallFolder;
}

std::wstring TaskName(const std::wstring& sid) { return std::wstring(kTaskPrefix) + sid; }

bool ConnectTaskService(ComRef<ITaskService>& service) {
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, service.PutVoid())))
        return false;
    VARIANT empty = EmptyVariant();
    return SUCCEEDED(service->Connect(empty, empty, empty, empty));
}

bool IsNotFound(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// Schedules a file or empty folder for deletion at the next restart.
bool DeleteLater(const std::wstring& path) {
    return MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != 0;
}

// Copies this exe to the install folder. A running older copy cannot be overwritten,
// but it can be renamed aside and removed at the next restart.
bool CopySelf(const std::wstring& target) {
    std::wstring self = ModulePath();
    if (self.empty()) return false;
    if (CompareStringOrdinal(self.c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL)
        return true;
    if (CopyFileW(self.c_str(), target.c_str(), FALSE)) return true;
    DWORD error = GetLastError();
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) return false;

    wchar_t suffix[32];
    swprintf(suffix, 32, L".%llx.old", static_cast<unsigned long long>(GetTickCount64()));
    std::wstring aside = target + suffix;
    if (!MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    if (!DeleteFileW(aside.c_str())) DeleteLater(aside);
    return CopyFileW(self.c_str(), target.c_str(), FALSE) != 0;
}

bool RegisterTask(const std::wstring& exePath, const std::wstring& sid) {
    ComRef<ITaskService> service;
    if (!ConnectTaskService(service)) return false;

    ComRef<ITaskFolder> root, folder;
    Bstr rootPath(L"\\"), folderPath(kTaskFolder);
    if (!rootPath || !folderPath || FAILED(service->GetFolder(rootPath.Get(), root.Put())))
        return false;
    if (FAILED(service->GetFolder(folderPath.Get(), folder.Put()))) {
        VARIANT noSddl = EmptyVariant();
        HRESULT hr = root->CreateFolder(folderPath.Get(), noSddl, folder.Put());
        if (FAILED(hr) && FAILED(service->GetFolder(folderPath.Get(), folder.Put())))
            return false;
    }

    ComRef<ITaskDefinition> task;
    if (FAILED(service->NewTask(0, task.Put()))) return false;

    Bstr author(L"Bloom Memory Clean");
    Bstr description(L"Memory Clean add-on for Bloom: cleans memory areas or ends chosen "
                     L"processes when the add-on asks.");
    ComRef<IRegistrationInfo> info;
    if (SUCCEEDED(task->get_RegistrationInfo(info.Put()))) {
        info->put_Author(author.Get());
        info->put_Description(description.Get());
    }

    Bstr userId(sid.c_str());
    ComRef<IPrincipal> principal;
    if (FAILED(task->get_Principal(principal.Put())) ||
        FAILED(principal->put_UserId(userId.Get())) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST)))
        return false;

    Bstr timeLimit(L"PT2M");
    ComRef<ITaskSettings> settings;
    if (FAILED(task->get_Settings(settings.Put())) ||
        FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE)) ||
        FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_ExecutionTimeLimit(timeLimit.Get())) ||
        FAILED(settings->put_MultipleInstances(TASK_INSTANCES_QUEUE)) ||
        FAILED(settings->put_AllowDemandStart(VARIANT_TRUE)))
        return false;

    ComRef<IActionCollection> actions;
    ComRef<IAction> action;
    ComRef<IExecAction> exec;
    std::wstring quotedPath = L"\"" + exePath + L"\"";
    Bstr path(quotedPath.c_str()), arguments(L"--run $(Arg0)");
    if (FAILED(task->get_Actions(actions.Put())) ||
        FAILED(actions->Create(TASK_ACTION_EXEC, action.Put())) ||
        FAILED(action->QueryInterface(IID_IExecAction, exec.PutVoid())) ||
        FAILED(exec->put_Path(path.Get())) || FAILED(exec->put_Arguments(arguments.Get())))
        return false;

    // Administrators and SYSTEM manage the task; the user may only read and run it.
    std::wstring sddl = L"D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;" + sid + L")";
    Bstr name(TaskName(sid).c_str()), sddlBstr(sddl.c_str());
    VARIANT user = BstrVariant(userId);
    VARIANT password = EmptyVariant();
    VARIANT security = BstrVariant(sddlBstr);
    ComRef<IRegisteredTask> registered;
    return SUCCEEDED(folder->RegisterTaskDefinition(name.Get(), task.Get(),
                                                    TASK_CREATE_OR_UPDATE, user, password,
                                                    TASK_LOGON_INTERACTIVE_TOKEN, security,
                                                    registered.Put()));
}

// Deletes this user's task, and the task folder when nothing else is left in it.
// *othersLeft: another user's Memory Clean task still runs the installed exe.
bool UnregisterTask(const std::wstring& sid, bool* othersLeft) {
    *othersLeft = false;
    ComRef<ITaskService> service;
    if (!ConnectTaskService(service)) return false;

    ComRef<ITaskFolder> folder;
    Bstr folderPath(kTaskFolder);
    HRESULT hr = service->GetFolder(folderPath.Get(), folder.Put());
    if (IsNotFound(hr)) return true;
    if (FAILED(hr)) return false;

    Bstr name(TaskName(sid).c_str());
    hr = folder->DeleteTask(name.Get(), 0);
    if (FAILED(hr) && !IsNotFound(hr)) return false;

    ComRef<IRegisteredTaskCollection> tasks;
    ComRef<ITaskFolderCollection> subfolders;
    LONG taskCount = 1, folderCount = 1;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.Put()))) {
        tasks->get_Count(&taskCount);
        size_t prefix = wcslen(kTaskPrefix);
        for (LONG i = 1; i <= taskCount && !*othersLeft; ++i) {
            VARIANT index = EmptyVariant();
            index.vt = VT_I4;
            index.lVal = i;
            ComRef<IRegisteredTask> other;
            BSTR otherName = nullptr;
            if (SUCCEEDED(tasks->get_Item(index, other.Put())) &&
                SUCCEEDED(other->get_Name(&otherName)) && otherName) {
                *othersLeft = wcsncmp(otherName, kTaskPrefix, prefix) == 0;
                SysFreeString(otherName);
            }
        }
    }
    if (SUCCEEDED(folder->GetFolders(0, subfolders.Put()))) subfolders->get_Count(&folderCount);
    if (taskCount == 0 && folderCount == 0) {
        ComRef<ITaskFolder> root;
        Bstr rootPath(L"\\");
        if (SUCCEEDED(service->GetFolder(rootPath.Get(), root.Put()))) {
            Bstr folderName(kTaskFolder + 1);  // without the leading backslash
            root->DeleteFolder(folderName.Get(), 0);
        }
    }
    return true;
}

// Removes the installed exe, copies renamed aside by CopySelf, and the folder.
bool RemoveInstalledFiles() {
    std::wstring folder = InstallFolder();
    if (folder.empty()) return false;
    bool ok = true;
    bool delayed = false;

    std::wstring exe = folder + L"\\" + kHelperExe;
    std::vector<std::wstring> files;
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(exe);
    WIN32_FIND_DATAW data;
    std::wstring pattern = exe + L".*.old";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(folder + L"\\" + data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    for (const std::wstring& file : files) {
        if (DeleteFileW(file.c_str())) continue;
        if (DeleteLater(file)) delayed = true;
        else ok = false;
    }

    if (!RemoveDirectoryW(folder.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        if (delayed) DeleteLater(folder);
    }
    // The parent folder is shared with other add-ons; remove it only when empty.
    std::wstring parent = folder.substr(0, folder.find_last_of(L'\\'));
    if (!RemoveDirectoryW(parent.c_str()) && delayed) DeleteLater(parent);
    return ok;
}

int RunInstall() {
    if (!IsElevated()) return kExitNotElevated;
    std::wstring sid = SelfSidString();
    std::wstring folder = InstallFolder();
    if (sid.empty() || folder.empty()) return kExitFailed;
    int result = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        return kExitFailed;
    std::wstring exe = folder + L"\\" + kHelperExe;
    if (!CopySelf(exe)) return kExitFailed;
    return RegisterTask(exe, sid) ? kExitOk : kExitFailed;
}

int RunUninstall() {
    if (!IsElevated()) return kExitNotElevated;
    std::wstring sid = SelfSidString();
    if (sid.empty()) return kExitFailed;
    bool othersLeft = false;
    bool taskOk = UnregisterTask(sid, &othersLeft);
    // Another user's task still runs the installed copy: leave it in place.
    bool filesOk = othersLeft || RemoveInstalledFiles();
    return taskOk && filesOk ? kExitOk : kExitFailed;
}

// The task always runs "--run $(Arg0)", so whatever a caller of the task passes lands
// after "--run" and can only be a request, never "--install" or "--uninstall".
int Dispatch(int argc, PWSTR* argv) {
    if (argc == 2 && wcscmp(argv[1], L"--install") == 0) return RunInstall();
    if (argc == 2 && wcscmp(argv[1], L"--uninstall") == 0) return RunUninstall();
    if (argc < 2 || wcscmp(argv[1], L"--run") != 0) return kExitBadArgs;

    std::vector<std::wstring> fields;
    if (argc == 3) {
        fields = Split(argv[2], L' ');
    } else if (argc == 6) {
        for (int i = 2; i < argc; ++i) fields.emplace_back(argv[i]);
    } else {
        return kExitBadArgs;
    }
    return RunRequest(fields);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Never load DLLs from the current or application folder beyond system ones.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return kExitBadArgs;

    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(com)) {
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                             RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, 0, nullptr);
    }
    int code = Dispatch(argc, argv);
    if (SUCCEEDED(com)) CoUninitialize();
    LocalFree(argv);
    return code;
}
