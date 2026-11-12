# Memory Clean add-on: sources and licenses

The add-on as a whole (this repository and its release zip) is distributed under the
GNU General Public License, version 3 only (`LICENSE`). It combines the parts below.

## The mod and the helper (GPL-3.0-only)

`mod/memory-clean.wh.cpp` (built into `MemoryClean.dll`), `helper/helper.cpp`
(built into `MemoryCleanHelper.exe`) and `common/memclean_protocol.h` were written for
this add-on.

The code that finds the taskbar's XAML follows PillTaskbar
(https://github.com/hyeon1035298/PillTaskbar_BloomAddon, GPL-3.0-only), which in turn
follows the taskbar mods of the windhawk-mods collection
(https://github.com/ramensoftware/windhawk-mods).

The values of the native system information classes the helper uses
(`SystemMemoryListInformation` and its commands, `SystemCombinePhysicalMemoryInformation`,
`SystemRegistryReconciliationInformation`) are taken from phnt
(https://github.com/winsiderss/phnt, MIT) and the Windows documentation.

What the add-on cleans was chosen after looking at how Mem Reduct
(https://github.com/henrypp/memreduct, GPL-3.0) behaves. No code, text, icon or name
of Mem Reduct is used.

## The add-on runtime and host (GPL-3.0-only)

`MemoryCleanRuntime.dll`, `MemoryCleanHost.exe` and the add-on entry code built into
`MemoryClean.dll` come from PillTaskbar
(https://github.com/hyeon1035298/PillTaskbar_BloomAddon), GPL-3.0-only, built with this
add-on's names from `addon.conf`. A release zip's `SOURCE.txt` names the commit of this
repository and of PillTaskbar_BloomAddon it was built from.

The runtime implements the part of the Windhawk mod API that the mod uses. It was
written for PillTaskbar and follows the behavior of Windhawk
(https://github.com/ramensoftware/windhawk, GPL-3.0).

The runtime, host and helper contain or link these libraries. Their license texts are
in `THIRD_PARTY_NOTICES.txt` next to the binaries.

| Library | License | Used for |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (with HDE64) | BSD-2-Clause | function hooks |
| [Zydis](https://github.com/zyantific/zydis) / Zycore 4.1.0 amalgamation | MIT | `Wh_Disasm` (runtime) and the symbol porter's instruction decoder (host) |
| mingw-w64 runtime | see `COPYING.MinGW-w64-runtime.txt` | C runtime startup code |
| libc++, libunwind, compiler-rt (LLVM 20) | Apache-2.0 WITH LLVM-exception | the statically linked C++ runtime |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | MIT | headers the mod includes |

## Microsoft symbols

The host downloads Microsoft's public symbol file for `taskbar.dll` from the
Microsoft symbol server on first start and keeps only the addresses the mod needs.
Nothing from Microsoft is included in this repository or in the release zip.

## Fonts

The add-on asks Windows for Noto Sans and falls back to Segoe UI. No font is included.
