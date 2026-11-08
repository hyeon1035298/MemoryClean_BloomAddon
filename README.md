# Memory Clean (Bloom add-on)

[한국어](README.ko.md)

An add-on for the [Bloom](https://github.com/hyeon1035298/Bloom) launcher that puts a small RAM module next to the taskbar clock. It shows how much memory is in use, or the CPU use, or both as two icons; a right click on the icon switches memory and CPU. Clicking it opens a window with two tabs:

- **Clean**: free memory now, choose which areas to clean, and clean automatically when use passes a percent or every few minutes.
- **Programs**: the running programs sorted by memory or CPU, with a button to end one. The list and the CPU readings are taken only while this tab is open.

Programs on the exclusion list keep their memory when the add-on cleans; Bloom is on it from the start. The gear opens the settings: what the icon shows and its style, the areas, automatic cleaning, exclusions and the helper.

It works on the plain Windows 11 taskbar and on a taskbar restyled by [PillTaskbar](https://github.com/hyeon1035298/PillTaskbar_BloomAddon). The add-on runtime and host from PillTaskbar load it into Explorer; PillTaskbar itself does not have to be on.

With PillTaskbar and an auto-hide taskbar, PillTaskbar's islands can show the memory and CPU use as these icons too, in the middle island or one in the left or right corner (PillTaskbar's island settings).

## Requirements

- Windows 11, 64-bit
- Bloom 0.19.0 or later
- An internet connection on the first start (see below)

## Install

1. Download `MemoryClean_BloomAddon-<version>.zip` from [Releases](https://github.com/hyeon1035298/MemoryClean_BloomAddon/releases).
2. Unzip it into `%LOCALAPPDATA%\Bloom\user-addons\memory-clean\`. The folder name must be `memory-clean`.
3. Restart Bloom, open Settings › Add-ons and turn on "Memory Clean".

Bloom checks every file against the SHA-256 hashes in `addon.json` before it starts the add-on. To remove the add-on, uninstall the helper first if you installed it (gear › helper), then turn the add-on off and delete the folder.

### First start

The add-on finds the taskbar's XAML through internal functions of `taskbar.dll`, so the host needs that DLL's symbols. On the first start, and again after a Windows update changes the DLL, it downloads the matching public symbol file from Microsoft's symbol server and keeps only the offsets. Nothing from Microsoft ships with the add-on.

### The helper (optional)

Without administrator rights the add-on trims the working sets of your own programs. Clearing the standby list, the modified page list, the file cache and the other system-wide lists needs administrator rights. The helper does that part:

- Installing it asks once for administrator rights (one UAC prompt). It copies `MemoryCleanHelper.exe` to `%ProgramFiles%\Bloom Add-ons\MemoryClean\` and registers a scheduled task under `\Bloom\` that runs it with the highest privileges, only when the add-on asks.
- It runs for one request (clean these areas, or end these programs) and exits. It writes its result to `%LOCALAPPDATA%\Bloom\addons\memory-clean\helper-result.json`.
- Uninstalling it (one more UAC prompt) removes the task and, when no other user still has one, the folder.

## Settings

Bloom's settings page for the add-on has the look and the refresh interval:

| Key | Built-in | Other values |
|---|---|---|
| `pill.theme` | `system` (the taskbar's theme) | `dark`, `light` |
| `pill.glass` | `true` | `false` (an opaque window, no blur) |
| `widget.refreshSeconds` | `2` | 1 to 10 |

Bloom sets the theme and glass to its own. Everything else (areas, automatic cleaning, exclusions, icon style) is in the add-on's own window and is kept in `%LOCALAPPDATA%\Bloom\addons\memory-clean\prefs.json`.

## Build from source

You need a clang that targets `x86_64-w64-mingw32` (llvm-mingw, or the compiler that ships with Windhawk), Python 3, and a bash shell such as Git Bash. The build uses PillTaskbar's add-on tools, so clone both repositories side by side:

```
git clone https://github.com/hyeon1035298/MemoryClean_BloomAddon
git clone https://github.com/hyeon1035298/PillTaskbar_BloomAddon
cd MemoryClean_BloomAddon
export CXX=/path/to/llvm-mingw/bin/clang++.exe   # optional, defaults to Windhawk's compiler
bash build.sh
```

`PILL_ROOT` points `build.sh` at a PillTaskbar_BloomAddon checkout elsewhere. llvm-mingw has no C++/WinRT headers; set `CPPWINRT_INCLUDE` to a folder that the Windows SDK's `cppwinrt.exe` wrote, as the workflow does. The result lands in `build/addon`.

The workflow in `.github/workflows/build.yml` builds every push against the PillTaskbar_BloomAddon commit pinned in `PILL_REF`. A `v*` tag publishes a release with the zip, its SHA-256, and a `SOURCE.txt` that names the commits of both repositories.

## License

The add-on is free software under the GNU General Public License, version 3 only. See `LICENSE`.

The runtime and host bundle MinHook (BSD-2-Clause) and Zydis (MIT). Credits and license texts are in `THIRD_PARTY.md` and, in the release zip, `THIRD_PARTY_NOTICES.txt`.

This program comes with no warranty. It patches code inside explorer.exe and can end programs and clear system memory lists, so use it at your own risk. Windows is a trademark of Microsoft. This project is not affiliated with Microsoft or with Mem Reduct.
