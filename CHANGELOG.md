# Changelog

## Unreleased

- Added first-class x86, x64 and ARM64 build targets.
- Added architecture-specific local build/package scripts and CI artifacts.
- GitHub release workflow now produces x86, x64, ARM64 and source ZIPs.
- Added x86 Far API ABI layout checks while preserving the existing 64-bit checks.


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
