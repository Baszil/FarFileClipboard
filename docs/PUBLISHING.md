# Publishing FarFileClipboard

## 1. Local release test

1. Close Far Manager instances that load an older FarPaste / CopyPaste / FarFileClipboard DLL.
2. Run `build.cmd`.
3. Run `install.cmd` and verify UAC elevation when Far is under Program Files. For an update with the DLL already loaded, verify the detached installer waits for Far to close and resumes automatically.
4. Remove old `Far\Plugins\FarPaste` and `Far\Plugins\CopyPaste` directories.
5. Start Far and test the matrix below.

### Test matrix

#### Clipboard interoperability

- Copy one file from Far -> Explorer.
- Copy several Insert-selected files from Far -> Explorer.
- Copy a directory from Far -> Explorer.
- Cut one file from Far -> Explorer and confirm the source disappears only after Paste in Explorer.
- Cut several Insert-selected objects from Far -> Explorer.
- Copy from Explorer -> `Ctrl+Shift+V` in Far; verify destination is the current panel directory regardless of cursor item.
- Cut from Explorer -> `Ctrl+Shift+V` in Far; verify move.
- `Ctrl+Shift+D` on a real directory; verify paste goes inside it without changing the current Far directory.
- `Ctrl+Shift+D` on a file and on `..`; verify it refuses the operation.

#### Undo / Redo

- Copy from Explorer -> Paste in Far -> `Ctrl+Shift+Z`; verify the newly created destination object disappears and the source remains.
- Immediately press `Ctrl+Shift+E`; verify the copy appears again.
- Cut from Explorer -> Paste in Far -> `Ctrl+Shift+Z`; verify the object returns to its original path.
- Immediately press `Ctrl+Shift+E`; verify it moves to Far again.
- Repeat Undo/Redo for several selected files and for a directory.
- After Undo, create another object using the destination name and press Redo; verify FarFileClipboard refuses rather than overwriting it.
- After Paste, remove the destination and create a different object with the same name, then press Undo; verify FarFileClipboard refuses because file identity changed.
- Paste over/into an already existing destination name, then press Undo; verify the collision/merge operation was not recorded as undoable.
- Perform a new successful Paste after an Undo; verify the old Redo becomes unavailable.
- Copy/Cut files to the clipboard without pasting; verify this does not create or replace Undo history.
- Perform Far -> Explorer paste and verify FarFileClipboard Undo does not claim to undo the Explorer operation.

#### UI / localization / settings

- Switch Far interface language between Russian and English; verify that F11 shows one FarFileClipboard entry and its internal menu is localized.
- Verify the internal menu contains Copy, Cut, Paste, Paste into directory under cursor, a separator, Undo and Redo.
- Verify Paste into directory under cursor is disabled when the cursor is not on a real directory.
- Verify Undo/Redo menu items are disabled when unavailable.
- Open plugin configuration and verify all six commands are visible in one dialog.
- Verify every row shows a fixed `Ctrl+Shift+` prefix and the editable field accepts one Latin letter only.
- Enter lowercase letters and verify they are normalized to uppercase after saving.
- Assign the same letter to two commands and verify the dialog rejects the duplicate.
- Press **Defaults** and verify `C / X / V / D / Z / E` are restored.
- Press F1 from the configuration dialog; verify localized help opens.
- Change all six letters, restart Far and verify settings persist.

## 2. GitHub

Suggested repository name: `FarFileClipboard`.

Push the repository and create the tag:

```text
v1.0.0
```

The included GitHub Actions workflows build x64 on Windows, upload a build artifact for every push/PR, and create binary/source ZIP assets for version tags.

## 3. Far forum

Use `docs/forum-post-ru.md` as the first post and add links to the GitHub repository and the latest release ZIP.

## 4. PlugRinG

Use the short descriptions in:

- `docs/plugring-description-ru.txt`
- `docs/plugring-description-en.txt`

Point the project/homepage link to GitHub and the download link to the latest GitHub release.
