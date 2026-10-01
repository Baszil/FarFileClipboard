# FarFileClipboard

**FarFileClipboard** is a Far Manager 3 plugin for x86, x64 and ARM64 by **Qwaduda** that connects a normal Far file panel to the Windows file clipboard.

[Русская версия](README.ru.md)

## Commands

| Default hotkey | Command | Behavior |
|---|---|---|
| `Ctrl+Shift+C` | Copy | Puts Insert-selected files/directories on the Windows clipboard; if nothing is selected, uses the item under the cursor. |
| `Ctrl+Shift+X` | Cut | Same selection rule, but marks the clipboard operation as Move/Cut. |
| `Ctrl+Shift+V` | Paste | Pastes **strictly into the directory currently open on the active panel**. The item under the cursor never changes the destination. |
| `Ctrl+Shift+D` | Paste into directory under cursor | Separate command. Works only when the cursor is on a real directory; does not accept a file, `.` or `..`. |
| `Ctrl+Shift+Z` | Undo | Undoes the last safely reversible file operation performed by FarFileClipboard itself. |
| `Ctrl+Shift+E` | Redo | Repeats the operation that was just undone by FarFileClipboard. |

The Far plugin menu (`F11`) contains one `FarFileClipboard` entry that opens the plugin's own menu. Undo/Redo are disabled when unavailable. All six hotkeys are configured on one screen in `Options -> Plugins configuration -> FarFileClipboard`; `Ctrl+Shift+` is fixed and only the A-Z letter is editable.

## Windows clipboard interoperability

For Far -> Windows, the plugin publishes ordinary file-system paths through `CF_HDROP` and uses `CFSTR_PREFERREDDROPEFFECT` to distinguish Copy from Cut/Move. This makes `Ctrl+Shift+C` / `Ctrl+Shift+X` from Far pasteable with ordinary `Ctrl+V` in Explorer and other applications that understand the Windows file clipboard.

For Windows -> Far, ordinary `CF_HDROP` is used first. If Explorer puts virtual Shell objects on the clipboard without real file-system paths — for example files copied directly from an opened ZIP folder — FarFileClipboard obtains the clipboard `IDataObject` / `IShellItemArray` and lets Windows `IFileOperation` materialize them. Such objects can therefore be pasted directly with `Ctrl+Shift+V` or `Ctrl+Shift+D` without manually extracting the archive first.

Both directions are supported:

- Explorer -> Far: Copy/Cut ordinary files and directories, plus Copy virtual objects from Explorer ZIP folders; then use `Ctrl+Shift+V` or `Ctrl+Shift+D` in Far.
- Far -> Explorer: `Ctrl+Shift+C` / `Ctrl+Shift+X` in Far, then ordinary `Ctrl+V` in Explorer.

Safe Undo/Redo history is not created for a paste from a virtual Shell source because such an object may not have a stable original file-system path. A new virtual-source paste clears the plugin's previous one-level history.

## Undo / Redo

History is one-level and covers **only file operations executed by FarFileClipboard during Paste**. Copying or cutting files to the clipboard does not itself change the file system, so it creates no undo entry. A paste later performed by Explorer is also outside FarFileClipboard's history.

For safety, an undo entry is created only when none of the destination names existed before the paste. If Windows has to merge a directory or replace an existing object, the operation can still proceed but it is not recorded as undoable.

Undo of Copy removes the newly created objects through Windows Shell with `FOF_ALLOWUNDO`. Undo of Move returns the objects to their original paths. Before Undo/Redo, the plugin checks file identity and destination conflicts to avoid deleting or overwriting a different object.

Every new successful Paste replaces the previous one-level history. Redo is available only after Undo.

## Languages and help

The distribution contains standard Far language and help files:

- `FarFileClipboardEng.lng`
- `FarFileClipboardRus.lng`
- `FarFileClipboardEng.hlf`
- `FarFileClipboardRus.hlf`

Far chooses the matching language automatically. Press `F1` from plugin configuration for built-in help.

## Requirements

- Far Manager 3 matching the plugin DLL architecture: x86, x64 or ARM64
- Windows
- Visual Studio 2022 C++ toolchain and CMake to build from source

## Build

Run:

```cmd
build.cmd x86
build.cmd x64
build.cmd ARM64
```

Runtime directories are created under `dist\FarFileClipboard-x86`, `dist\FarFileClipboard-x64` and `dist\FarFileClipboard-ARM64`.

Build all three architectures:

```cmd
build-all.cmd
```

Create a distributable ZIP for one architecture:

```cmd
package.cmd x86
package.cmd x64
package.cmd ARM64
```

Create all three release ZIPs:

```cmd
package-all.cmd
```

## Install

Copy the whole `FarFileClipboard` directory into:

```text
<Far>\Plugins\FarFileClipboard\
```

or run:

```cmd
install.cmd "C:\Path\To\Far Manager"
```

When `install.cmd` is started from Far, the target path may be omitted: the installer first detects the running parent Far process, then checks `%FARHOME%`, then standard `Program Files` / `Program Files (x86)` locations. If the destination is protected, the installer requests elevation through Windows UAC automatically. If an existing `FarFileClipboard.dll` is loaded by a running Far instance, installation continues in a separate window: close that Far instance and the installer resumes automatically when the DLL is released.

The binary release ZIP includes `install.cmd` and its `install.ps1` worker next to the `FarFileClipboard` directory, so `install.cmd` can be run directly after extraction.

If you used the old **FarPaste** or **CopyPaste** prototype, remove its old plugin directory before starting Far. FarFileClipboard intentionally keeps the old plugin GUID. Existing hotkey settings migrate when they already match the current `Ctrl+Shift+letter` model; other legacy combinations fall back to defaults.

## Hotkey configuration

Open **Options -> Plugins configuration -> FarFileClipboard**. All six commands are configured on one screen.

The `Ctrl+Shift+` prefix is fixed; each field accepts one Latin letter `A-Z`. Lowercase input is normalized to uppercase. **Defaults** restores `C / X / V / D / Z / E`. Duplicate letters are rejected.

Far stores the value internally in its compact form (`CtrlShiftV`), while the plugin displays the conventional human-readable form `Ctrl+Shift+V`.

## File-operation behavior

The same configuration screen contains two independent behavior groups.

**Operation checks:**

- **Pre-check invalid operations** is enabled by default. The plugin rejects clearly impossible cases before calling Windows, for example placing a directory inside itself or one of its descendants.
- When the checkbox is cleared, FarFileClipboard passes the request straight to Windows. This does not bypass Windows restrictions; the system file-operation layer still has final authority.

**Name conflicts:**

- **Ask what to do** — the default. A Far-style dialog offers **Replace / Skip / Rename / Cancel**. Replace and Skip can be applied to all remaining conflicts. Rename asks for an explicit new name for the incoming file or directory.
- **Use Windows behavior** — the collision is delegated to Windows Shell.
- **Automatically rename** — FarFileClipboard chooses a free name using a configurable template. The default is `{name} ({n}){ext}`, e.g. `report.xlsx` -> `report (1).xlsx`, `report (2).xlsx`, while a directory `Photos` becomes `Photos (1)`.

The template is editable directly below the conflict modes. Supported placeholders are `{name}` (basename without the final extension; full name for directories), `{ext}` (the final extension including the dot; empty for directories), and mandatory counter `{n}`. **Defaults** restores `{name} ({n}){ext}`.

FarFileClipboard still uses standard Windows file operations in its own conflict mode; it only chooses the action before starting the operation. Replacements or directory merges are intentionally excluded from safe Undo history, while a copy/move to a new explicitly chosen name can be recorded normally.

## Limitations

FarFileClipboard works with normal Far file-system panels. Virtual plugin panels such as archive or FTP panels are intentionally not treated as file-system sources or destination directories. This does not prevent pasting virtual Shell objects copied, for example, from a ZIP opened in Windows Explorer into a normal Far panel.

File operations use Windows Shell. Undo/Redo history exists only in the current Far process and does not survive restart.

## License

BSD-3-Clause. Copyright (c) 2026 Qwaduda.

