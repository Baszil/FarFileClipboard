# Changelog

## Unreleased

## 1.0.2 — 2026-10-05

- Fixed progress/input starvation: `ACTL_SYNCHRO` returns 0 by contract, but the previous coalescing code treated that 0 as a failure and immediately cleared its "sync queued" guard. During directory copies this allowed every `CopyFileEx` progress callback to enqueue another main-thread redraw, competing directly with Far's `GetInputRecord` path. Progress redraws are now coalesced correctly, capped at about 20 Hz, and never have more than one synchro request outstanding.
- Cancel from the progress dialog now follows Far's normal copy UX: one Esc/F10/Enter/Space press or one click opens a Far warning with a localized Yes/No confirmation instead of immediately aborting or leaking the input to the panel.
- While the confirmation is open, native file transfer pauses at the next progress/item boundary; No resumes the same operation and Yes arms the real `CopyFileEx` / `MoveFileWithProgress` cancellation path.
- Overall progress remains byte-proportional across directories and multiple files, including the in-flight bytes of the current file.
- Unfinished progress-bar cells use the light dotted `░` texture requested for Far-style progress instead of blending into the dialog background.
- Fixed x86 plugin discovery in Far by exporting the Far API entry points under their exact undecorated names; 32-bit `WINAPI` symbols are decorated internally by MSVC.
- Added a real x86 Far functional CI test: exact export verification, plugin discovery in F11, and Copy/Paste between two panel directories in actual 32-bit Far.
- Release tags are now gated by the same real 32-bit Far discovery test, so an x86 DLL that builds but cannot actually load is not published.
- Switched MSVC builds to the static runtime so the plugin does not require a separately installed architecture-specific VC runtime merely to load.
- File transfer work runs on a worker thread, while the default Far progress dialog is modal and owns keyboard/mouse input for the duration of the operation.
- Redesigned the copy/move progress to follow Far's native copy-dialog style: the box title is now the operation itself rather than the plugin name, the bracketed pseudo-bar is gone, the current-file and overall sections have explicit localized labels, and multi-item or directory operations add a separate overall bar. The Far dialog is modal so Enter/Esc/F10/mouse cannot leak to the panels. Ordinary filesystem transfers use `CopyFileEx` / `MoveFileWithProgress`, whose progress callback returns `PROGRESS_CANCEL`, so Cancel interrupts the file that is currently being transferred instead of merely stopping before the next Shell work item.
- Widened the configuration dialog so the Russian "Windows progress" option stays clear of the right border, and removed the useless empty row below the action buttons.
- Virtual Shell files (including Explorer ZIP items) now use the Shell `Size` property when no filesystem source path exists, so the current-file indicator can remain distinct from the overall-operation estimate whenever Windows exposes that size.
- Explorer-style Windows progress UI is disabled by default and can be enabled explicitly instead of the Far progress window.
- Added safe main-thread completion through Far synchronization, panel refresh after background completion, and protection against starting a second plugin file operation concurrently.
- Installer now prints the exact target Far.exe, detected architecture, selected build source and destination before copying, and refuses an architecture mismatch.
- Added a universal `-all.zip` package containing x86, x64 and ARM64 builds; `install.ps1` selects the correct one from the target `Far.exe` PE machine type.

## 1.0.1 — 2026-10-02

- Added first-class x86, x64 and ARM64 build targets.
- Added architecture-specific local build/package scripts and CI artifacts.
- GitHub release workflow now produces x86, x64, ARM64 and source ZIPs.
- Added x86 Far API ABI layout checks while preserving the existing 64-bit checks.
- Added a native Windows ARM64 runtime smoke test using ARM64 Far on GitHub Actions.
- Added a native Windows ARM64 functional test that verifies plugin discovery and performs Copy/Paste through the Far plugin menu between two real panel directories.
- Added explicit ARM64 DLL load/export diagnostics and PE architecture verification to make CI failures actionable.


## 1.0.0 — 2026-09-30

First public release under the **FarFileClipboard** name.

- Copy and cut files/directories from Far into the Windows file clipboard.
- Paste Windows clipboard objects into the current panel directory.
- Separate paste command for the directory under the cursor.
- Windows Shell clipboard fallback for virtual objects, including files copied directly from Explorer ZIP folders.
- One-level safe Undo/Redo for filesystem operations performed by FarFileClipboard itself.
- Default hotkeys: `Ctrl+Shift+C`, `Ctrl+Shift+X`, `Ctrl+Shift+V`, `Ctrl+Shift+D`, `Ctrl+Shift+Z`, `Ctrl+Shift+E`.
- Compact Far-style configuration dialog with one-letter hotkey fields.
- Optional preliminary validation of obviously invalid recursive operations.
- Three name-conflict modes: FarFileClipboard dialog, Windows behavior, or automatic rename.
- Configurable automatic-rename template (`{name}`, `{ext}`, mandatory `{n}`), default `{name} ({n}){ext}`.
- Conflict dialog supports Replace, Skip, explicit Rename, Cancel, and Apply to all for Replace/Skip.
- Conflict dialog uses Far's native Warning color scheme and a localized Warning title.
- Deliberate cancellation is silent; cancelling an operation does not produce a redundant "operation cancelled" message.
- Russian and English `.lng` / `.hlf` localization.
- Self-elevating installer with Far auto-detection, UAC support, and safe waiting for a loaded plugin DLL during upgrades.
- DLL metadata and author information: **Qwaduda**.
- Compatibility migration from earlier FarPaste / CopyPaste prototypes by keeping the plugin GUID.
