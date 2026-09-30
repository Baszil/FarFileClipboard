#include "far3sdk_min.hpp"
#include "messages.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Keep the original plugin GUID so FarFileClipboard is an in-place successor of CopyPaste / FarPaste.
constexpr UUID PluginGuid =
{ 0x6fe2c62e, 0x43e8, 0x45cb, { 0xa7, 0x9a, 0x4f, 0xe4, 0x85, 0x47, 0xb2, 0x91 } };

constexpr UUID MainMenuGuid =
{ 0x5e46f184, 0xdf1a, 0x4d28, { 0x97, 0xb0, 0x51, 0x53, 0x28, 0x74, 0x6a, 0x61 } };

constexpr UUID OperationsMenuGuid =
{ 0x853c895d, 0xc2ec, 0x4931, { 0x9b, 0xf5, 0x30, 0x0e, 0x55, 0xc5, 0x84, 0x18 } };

constexpr UUID PluginConfigGuid =
{ 0x7feaf457, 0x87df, 0x41a5, { 0x94, 0xec, 0x3c, 0xc8, 0x19, 0x3b, 0x18, 0x60 } };

constexpr UUID ConfigDialogGuid =
{ 0x2baac2c7, 0x70fc, 0x47cc, { 0x91, 0x6f, 0x58, 0x36, 0xd4, 0x43, 0x22, 0x31 } };

constexpr UUID ConflictDialogGuid =
{ 0x1fbdbf17, 0x2d88, 0x4da7, { 0xb2, 0x5e, 0xd8, 0x72, 0xe6, 0x03, 0x7f, 0x09 } };

constexpr UUID RenameInputGuid =
{ 0xc20299da, 0x2325, 0x412f, { 0x92, 0xd6, 0x82, 0x59, 0xa0, 0xec, 0x49, 0xc1 } };

constexpr UUID MessageGuid =
{ 0xe31fed3a, 0xad30, 0x47e1, { 0xb2, 0xab, 0x27, 0x7c, 0x3c, 0xd6, 0xc7, 0xc5 } };

constexpr wchar_t DefaultCopyHotkey[]      = L"CtrlShiftC";
constexpr wchar_t DefaultCutHotkey[]       = L"CtrlShiftX";
constexpr wchar_t DefaultPasteHotkey[]     = L"CtrlShiftV";
constexpr wchar_t DefaultPasteIntoHotkey[] = L"CtrlShiftD";
constexpr wchar_t DefaultUndoHotkey[]      = L"CtrlShiftZ";
constexpr wchar_t DefaultRedoHotkey[]      = L"CtrlShiftE";
constexpr wchar_t CopyHotkeySetting[]      = L"CopyHotkey";
constexpr wchar_t CutHotkeySetting[]       = L"CutHotkey";
constexpr wchar_t PasteHotkeySetting[]     = L"PasteHotkey";
constexpr wchar_t PasteIntoHotkeySetting[] = L"PasteIntoHotkey";
constexpr wchar_t UndoHotkeySetting[]      = L"UndoHotkey";
constexpr wchar_t RedoHotkeySetting[]      = L"RedoHotkey";
constexpr wchar_t LegacyHotkeySetting[] = L"Hotkey";
constexpr wchar_t PrecheckInvalidSetting[] = L"PrecheckInvalidOperations";
constexpr wchar_t ConflictModeSetting[] = L"ConflictMode";
constexpr wchar_t AutoRenameTemplateSetting[] = L"AutoRenameTemplate";
constexpr wchar_t DefaultAutoRenameTemplate[] = L"{name} ({n}){ext}";

enum class ConflictMode : unsigned long long
{
    Ask = 0,
    System = 1,
    AutoRename = 2
};

PluginStartupInfo g_Info{};
std::wstring g_CopyHotkey = DefaultCopyHotkey;
std::wstring g_CutHotkey = DefaultCutHotkey;
std::wstring g_PasteHotkey = DefaultPasteHotkey;
std::wstring g_PasteIntoHotkey = DefaultPasteIntoHotkey;
std::wstring g_UndoHotkey = DefaultUndoHotkey;
std::wstring g_RedoHotkey = DefaultRedoHotkey;
bool g_PrecheckInvalidOperations = true;
ConflictMode g_ConflictMode = ConflictMode::Ask;
std::wstring g_AutoRenameTemplate = DefaultAutoRenameTemplate;

struct HotkeySpec
{
    bool Ctrl  = false;
    bool Shift = false;
    bool Alt   = false;
    WORD Vk    = 0;
};

HotkeySpec g_CopyHotkeySpec{};
HotkeySpec g_CutHotkeySpec{};
HotkeySpec g_PasteHotkeySpec{};
HotkeySpec g_PasteIntoHotkeySpec{};
HotkeySpec g_UndoHotkeySpec{};
HotkeySpec g_RedoHotkeySpec{};

enum class FileAction
{
    None,
    Copy,
    Move
};

struct FileIdentity
{
    bool Valid = false;
    DWORD VolumeSerialNumber = 0;
    DWORD FileIndexHigh = 0;
    DWORD FileIndexLow = 0;
};

struct FileActionItem
{
    std::wstring Source;
    std::wstring Destination;
    FileIdentity SourceIdentity{};
    FileIdentity DestinationIdentity{};
};

struct FileActionRecord
{
    FileAction Action = FileAction::None;
    std::vector<FileActionItem> Items;
};

FileActionRecord g_UndoRecord{};
FileActionRecord g_RedoRecord{};

bool IEquals(const std::wstring& a, const wchar_t* b)
{
    const std::wstring wb = b;
    if (a.size() != wb.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::towupper(a[i]) != std::towupper(wb[i]))
            return false;
    }
    return true;
}

bool StartsWithNoCase(const std::wstring& value, size_t pos, const wchar_t* token)
{
    const std::wstring t = token;
    if (value.size() - pos < t.size())
        return false;

    for (size_t i = 0; i < t.size(); ++i)
    {
        if (std::towupper(value[pos + i]) != std::towupper(t[i]))
            return false;
    }
    return true;
}

bool ParseHotkey(const std::wstring& source, HotkeySpec& out)
{
    std::wstring s;
    s.reserve(source.size());

    for (wchar_t ch : source)
    {
        if (ch == L'+' || ch == L'-' || std::iswspace(ch))
            continue;
        s.push_back(ch);
    }

    HotkeySpec result{};
    size_t pos = 0;
    bool consumed = true;

    while (consumed)
    {
        consumed = false;
        if (!result.Ctrl && StartsWithNoCase(s, pos, L"CTRL"))
        {
            result.Ctrl = true;
            pos += 4;
            consumed = true;
        }
        else if (!result.Shift && StartsWithNoCase(s, pos, L"SHIFT"))
        {
            result.Shift = true;
            pos += 5;
            consumed = true;
        }
        else if (!result.Alt && StartsWithNoCase(s, pos, L"ALT"))
        {
            result.Alt = true;
            pos += 3;
            consumed = true;
        }
    }

    if (pos >= s.size())
        return false;

    std::wstring key = s.substr(pos);
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });

    if (key.size() == 1)
    {
        const wchar_t ch = key[0];
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
            result.Vk = static_cast<WORD>(ch);
    }
    else if (!key.empty() && key[0] == L'F' && key.size() <= 3)
    {
        int n = 0;
        for (size_t i = 1; i < key.size(); ++i)
        {
            if (!std::iswdigit(key[i]))
                return false;
            n = n * 10 + (key[i] - L'0');
        }
        if (n >= 1 && n <= 24)
            result.Vk = static_cast<WORD>(VK_F1 + n - 1);
    }
    else if (IEquals(key, L"INSERT") || IEquals(key, L"INS")) result.Vk = VK_INSERT;
    else if (IEquals(key, L"DELETE") || IEquals(key, L"DEL")) result.Vk = VK_DELETE;
    else if (IEquals(key, L"HOME")) result.Vk = VK_HOME;
    else if (IEquals(key, L"END")) result.Vk = VK_END;
    else if (IEquals(key, L"PGUP") || IEquals(key, L"PAGEUP")) result.Vk = VK_PRIOR;
    else if (IEquals(key, L"PGDN") || IEquals(key, L"PAGEDOWN")) result.Vk = VK_NEXT;
    else if (IEquals(key, L"SPACE")) result.Vk = VK_SPACE;
    else if (IEquals(key, L"ENTER")) result.Vk = VK_RETURN;
    else if (IEquals(key, L"TAB")) result.Vk = VK_TAB;
    else if (IEquals(key, L"BACKSPACE") || IEquals(key, L"BS")) result.Vk = VK_BACK;
    else if (IEquals(key, L"ESC") || IEquals(key, L"ESCAPE")) result.Vk = VK_ESCAPE;

    if (!result.Vk)
        return false;

    out = result;
    return true;
}

bool SameHotkey(const HotkeySpec& a, const HotkeySpec& b)
{
    return a.Ctrl == b.Ctrl && a.Shift == b.Shift && a.Alt == b.Alt && a.Vk == b.Vk;
}

bool IsLatinLetter(wchar_t ch)
{
    ch = static_cast<wchar_t>(std::towupper(ch));
    return ch >= L'A' && ch <= L'Z';
}

std::wstring MakeInternalHotkey(wchar_t letter)
{
    std::wstring result = L"CtrlShift";
    result.push_back(static_cast<wchar_t>(std::towupper(letter)));
    return result;
}

std::wstring MakeDisplayHotkey(wchar_t letter)
{
    std::wstring result = L"Ctrl+Shift+";
    result.push_back(static_cast<wchar_t>(std::towupper(letter)));
    return result;
}

bool ParseFixedHotkey(const std::wstring& source, wchar_t& letter, HotkeySpec& spec)
{
    HotkeySpec parsed{};
    if (!ParseHotkey(source, parsed) || !parsed.Ctrl || !parsed.Shift || parsed.Alt ||
        parsed.Vk < L'A' || parsed.Vk > L'Z')
        return false;

    letter = static_cast<wchar_t>(parsed.Vk);
    spec = parsed;
    return true;
}

wchar_t HotkeyLetterOrDefault(const std::wstring& source, wchar_t fallback)
{
    wchar_t letter = 0;
    HotkeySpec spec{};
    return ParseFixedHotkey(source, letter, spec) ? letter : fallback;
}

bool HotkeyMatches(const KEY_EVENT_RECORD& key, const HotkeySpec& spec)
{
    if (!key.bKeyDown || key.wVirtualKeyCode != spec.Vk)
        return false;

    const DWORD state = key.dwControlKeyState;
    const bool ctrl  = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool shift = (state & SHIFT_PRESSED) != 0;
    const bool alt   = (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    return ctrl == spec.Ctrl && shift == spec.Shift && alt == spec.Alt;
}

const wchar_t* Msg(MessageId id)
{
    if (g_Info.GetMsg)
        return g_Info.GetMsg(&PluginGuid, static_cast<intptr_t>(id));
    return L"FarFileClipboard";
}

void ShowMessage(const std::wstring& text, bool warning = false)
{
    if (!g_Info.Message)
        return;

    const std::wstring body = std::wstring(Msg(MPluginName)) + L"\n" + text;
    const auto flags = FMSG_ALLINONE | FMSG_MB_OK | (warning ? FMSG_WARNING : 0ULL);
    g_Info.Message(
        &PluginGuid,
        &MessageGuid,
        flags,
        nullptr,
        reinterpret_cast<const wchar_t* const*>(body.c_str()),
        0,
        0);
}

bool IsPanelsWindow()
{
    if (!g_Info.AdvControl)
        return true;

    WindowType wt{};
    wt.StructSize = sizeof(wt);
    wt.Type = WTYPE_UNKNOWN;

    if (!g_Info.AdvControl(&PluginGuid, ACTL_GETWINDOWTYPE, 0, &wt))
        return false;

    return wt.Type == WTYPE_PANELS;
}

bool GetPanelInfo(PanelInfo& info)
{
    if (!g_Info.PanelControl)
        return false;

    info = {};
    info.StructSize = sizeof(info);
    if (!g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &info))
        return false;

    return info.PanelType == PTYPE_FILEPANEL && (info.Flags & PFLAGS_PLUGIN) == 0;
}

bool GetActiveDirectory(std::wstring& directory)
{
    if (!g_Info.PanelControl)
        return false;

    const intptr_t size = g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
    if (size <= 0)
        return false;

    std::vector<unsigned char> buffer(static_cast<size_t>(size));
    auto* dir = reinterpret_cast<FarPanelDirectory*>(buffer.data());
    dir->StructSize = sizeof(FarPanelDirectory);

    if (!g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, size, dir) || !dir->Name || !*dir->Name)
        return false;

    directory = dir->Name;

    const DWORD attrs = GetFileAttributesW(directory.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool GetPanelItemFileName(int command, intptr_t index, std::wstring& fileName)
{
    if (!g_Info.PanelControl)
        return false;

    const intptr_t size = g_Info.PanelControl(PANEL_ACTIVE, command, index, nullptr);
    if (size <= 0)
        return false;

    std::vector<unsigned char> buffer(static_cast<size_t>(size));
    FarGetPluginPanelItem getItem{};
    getItem.StructSize = sizeof(getItem);
    getItem.Size = static_cast<size_t>(size);
    getItem.Item = reinterpret_cast<PluginPanelItem*>(buffer.data());

    if (!g_Info.PanelControl(PANEL_ACTIVE, command, index, &getItem) || !getItem.Item || !getItem.Item->FileName)
        return false;

    fileName = getItem.Item->FileName;
    return !fileName.empty();
}

struct CurrentPanelItem
{
    std::wstring FileName;
    uintptr_t FileAttributes = 0;
};

bool GetCurrentPanelItem(CurrentPanelItem& item)
{
    if (!g_Info.PanelControl)
        return false;

    const intptr_t size = g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, nullptr);
    if (size <= 0)
        return false;

    std::vector<unsigned char> buffer(static_cast<size_t>(size));
    FarGetPluginPanelItem getItem{};
    getItem.StructSize = sizeof(getItem);
    getItem.Size = static_cast<size_t>(size);
    getItem.Item = reinterpret_cast<PluginPanelItem*>(buffer.data());

    if (!g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &getItem) ||
        !getItem.Item || !getItem.Item->FileName)
        return false;

    item.FileName = getItem.Item->FileName;
    item.FileAttributes = getItem.Item->FileAttributes;
    return !item.FileName.empty();
}

bool IsAbsolutePath(const std::wstring& path)
{
    if (path.size() >= 2 && path[1] == L':')
        return true;
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
{
    if (IsAbsolutePath(name))
        return name;

    if (directory.empty())
        return name;

    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + name;

    return directory + L"\\" + name;
}

std::wstring BaseName(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ParentDirectory(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return {};

    if (pos == 2 && path.size() >= 3 && path[1] == L':')
        return path.substr(0, 3);

    if (pos == 0)
        return path.substr(0, 1);

    return path.substr(0, pos);
}

bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool GetFileIdentity(const std::wstring& path, FileIdentity& identity)
{
    identity = {};

    HANDLE handle = CreateFileW(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        return false;

    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);

    if (!ok)
        return false;

    identity.Valid = true;
    identity.VolumeSerialNumber = info.dwVolumeSerialNumber;
    identity.FileIndexHigh = info.nFileIndexHigh;
    identity.FileIndexLow = info.nFileIndexLow;
    return true;
}

bool SameFileIdentity(const FileIdentity& a, const FileIdentity& b)
{
    return a.Valid && b.Valid &&
        a.VolumeSerialNumber == b.VolumeSerialNumber &&
        a.FileIndexHigh == b.FileIndexHigh &&
        a.FileIndexLow == b.FileIndexLow;
}

bool PathMatchesIdentity(const std::wstring& path, const FileIdentity& expected)
{
    FileIdentity actual{};
    return GetFileIdentity(path, actual) && SameFileIdentity(actual, expected);
}

bool PathsEqualNoCase(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring NormalizePathForCompare(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');

    while (path.size() > 3 && !path.empty() && path.back() == L'\\')
        path.pop_back();

    return path;
}

bool IsSameOrDescendantPath(const std::wstring& candidate, const std::wstring& ancestor)
{
    const std::wstring c = NormalizePathForCompare(candidate);
    const std::wstring a = NormalizePathForCompare(ancestor);

    if (PathsEqualNoCase(c, a))
        return true;

    if (c.size() <= a.size())
        return false;

    if (CompareStringOrdinal(c.c_str(), static_cast<int>(a.size()), a.c_str(), static_cast<int>(a.size()), TRUE) != CSTR_EQUAL)
        return false;

    if (!a.empty() && a.back() == L'\\')
        return true;

    return c[a.size()] == L'\\';
}

bool ValidateObviousOperation(
    const std::vector<std::wstring>& sources,
    const std::wstring& destinationDirectory,
    bool move,
    std::wstring& reason)
{
    reason.clear();

    for (const auto& source : sources)
    {
        const DWORD attrs = GetFileAttributesW(source.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
            continue;

        const std::wstring target = JoinPath(destinationDirectory, BaseName(source));

        if (move && PathsEqualNoCase(NormalizePathForCompare(source), NormalizePathForCompare(target)))
        {
            reason = Msg(MMoveSameLocation);
            return false;
        }

        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            IsSameOrDescendantPath(destinationDirectory, source))
        {
            reason = Msg(MInvalidRecursiveOperation);
            return false;
        }
    }

    return true;
}

void ClearRecord(FileActionRecord& record)
{
    record.Action = FileAction::None;
    record.Items.clear();
}

bool HasRecord(const FileActionRecord& record)
{
    return record.Action != FileAction::None && !record.Items.empty();
}

bool BuildSafeActionRecord(
    const std::vector<std::wstring>& sources,
    const std::wstring& destinationDirectory,
    FileAction action,
    FileActionRecord& record)
{
    ClearRecord(record);
    record.Action = action;
    record.Items.reserve(sources.size());

    for (const auto& source : sources)
    {
        const std::wstring name = BaseName(source);
        if (name.empty())
        {
            ClearRecord(record);
            return false;
        }

        const std::wstring destination = JoinPath(destinationDirectory, name);

        // If the destination already exists, Windows may merge/replace it.
        // That cannot be undone safely without keeping a full backup.
        if (PathExists(destination))
        {
            ClearRecord(record);
            return false;
        }

        for (const auto& existing : record.Items)
        {
            if (PathsEqualNoCase(existing.Destination, destination))
            {
                ClearRecord(record);
                return false;
            }
        }

        FileIdentity sourceIdentity{};
        if (!GetFileIdentity(source, sourceIdentity))
        {
            ClearRecord(record);
            return false;
        }

        FileActionItem item{};
        item.Source = source;
        item.Destination = destination;
        item.SourceIdentity = sourceIdentity;
        record.Items.push_back(std::move(item));
    }

    return HasRecord(record);
}

bool FinalizeCompletedAction(FileActionRecord& record)
{
    if (!HasRecord(record))
        return false;

    for (auto& item : record.Items)
    {
        if (!GetFileIdentity(item.Destination, item.DestinationIdentity))
            return false;

        if (record.Action == FileAction::Move && PathExists(item.Source))
            return false;
    }

    return true;
}

bool GetSourceFilesFromPanel(std::vector<std::wstring>& paths)
{
    paths.clear();

    PanelInfo panel{};
    if (!GetPanelInfo(panel))
    {
        ShowMessage(Msg(MPanelOnlyCopyCut), true);
        return false;
    }

    std::wstring directory;
    if (!GetActiveDirectory(directory))
    {
        ShowMessage(Msg(MActiveDirFailed), true);
        return false;
    }

    const bool hasSelection = panel.SelectedItemsNumber > 0;
    const size_t count = hasSelection ? panel.SelectedItemsNumber : 1;
    paths.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        std::wstring name;
        const int command = hasSelection ? FCTL_GETSELECTEDPANELITEM : FCTL_GETCURRENTPANELITEM;
        const intptr_t index = hasSelection ? static_cast<intptr_t>(i) : 0;

        if (!GetPanelItemFileName(command, index, name))
        {
            ShowMessage(Msg(MPanelItemFailed), true);
            return false;
        }

        if (name == L".." || name == L".")
        {
            if (!hasSelection)
            {
                ShowMessage(Msg(MCursorParentCopyCut));
                return false;
            }
            continue;
        }

        std::wstring fullPath = JoinPath(directory, name);
        const DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            ShowMessage(std::wstring(Msg(MObjectNotFound)) + L"\n" + fullPath, true);
            return false;
        }

        paths.push_back(std::move(fullPath));
    }

    if (paths.empty())
    {
        ShowMessage(Msg(MNoFilesToCopyCut));
        return false;
    }

    return true;
}

struct ClipboardFiles
{
    std::vector<std::wstring> Paths;
    bool Move = false;
};

struct ShellClipboardData
{
    IDataObject* DataObject = nullptr;
    IShellItemArray* Items = nullptr;
    bool Move = false;
    bool OleInitialized = false;

    ShellClipboardData() = default;
    ShellClipboardData(const ShellClipboardData&) = delete;
    ShellClipboardData& operator=(const ShellClipboardData&) = delete;

    ~ShellClipboardData()
    {
        if (Items)
            Items->Release();
        if (DataObject)
            DataObject->Release();
        if (OleInitialized)
            OleUninitialize();
    }
};

DWORD ReadPreferredDropEffect(IDataObject* dataObject)
{
    if (!dataObject)
        return DROPEFFECT_COPY;

    const UINT format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!format)
        return DROPEFFECT_COPY;

    FORMATETC formatEtc{};
    formatEtc.cfFormat = static_cast<CLIPFORMAT>(format);
    formatEtc.dwAspect = DVASPECT_CONTENT;
    formatEtc.lindex = -1;
    formatEtc.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&formatEtc, &medium)))
        return DROPEFFECT_COPY;

    DWORD effect = DROPEFFECT_COPY;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
    {
        const auto* value = static_cast<const DWORD*>(GlobalLock(medium.hGlobal));
        if (value)
        {
            effect = *value;
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return effect;
}

bool ReadShellClipboard(ShellClipboardData& out)
{
    const HRESULT oleResult = OleInitialize(nullptr);
    if (SUCCEEDED(oleResult))
        out.OleInitialized = true;
    else if (oleResult != RPC_E_CHANGED_MODE)
        return false;

    IDataObject* dataObject = nullptr;
    if (FAILED(OleGetClipboard(&dataObject)) || !dataObject)
        return false;

    IShellItemArray* items = nullptr;
    const HRESULT arrayResult = SHCreateShellItemArrayFromDataObject(
        dataObject, IID_PPV_ARGS(&items));
    if (FAILED(arrayResult) || !items)
    {
        dataObject->Release();
        return false;
    }

    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || count == 0)
    {
        items->Release();
        dataObject->Release();
        return false;
    }

    const DWORD preferredEffect = ReadPreferredDropEffect(dataObject);
    out.DataObject = dataObject;
    out.Items = items;
    out.Move = (preferredEffect & DROPEFFECT_MOVE) != 0 &&
        (preferredEffect & DROPEFFECT_COPY) == 0;
    return true;
}

bool ReadClipboardFiles(ClipboardFiles& out)
{
    out = {};

    if (!IsClipboardFormatAvailable(CF_HDROP))
        return false;

    if (!OpenClipboard(GetConsoleWindow()))
        return false;

    HDROP drop = reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop)
    {
        CloseClipboard();
        return false;
    }

    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    out.Paths.reserve(count);

    for (UINT i = 0; i < count; ++i)
    {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (!len)
            continue;

        std::wstring path(len + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        path.resize(len);
        out.Paths.push_back(std::move(path));
    }

    DWORD preferredEffect = DROPEFFECT_COPY;
    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (effectFormat)
    {
        HANDLE effectHandle = GetClipboardData(effectFormat);
        if (effectHandle)
        {
            const auto* effect = static_cast<const DWORD*>(GlobalLock(effectHandle));
            if (effect)
            {
                preferredEffect = *effect;
                GlobalUnlock(effectHandle);
            }
        }
    }

    CloseClipboard();

    out.Move = (preferredEffect & DROPEFFECT_MOVE) != 0 && (preferredEffect & DROPEFFECT_COPY) == 0;
    return !out.Paths.empty();
}

HGLOBAL CreateHDrop(const std::vector<std::wstring>& paths)
{
    size_t chars = 1; // final extra NUL
    for (const auto& path : paths)
        chars += path.size() + 1;

    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!handle)
        return nullptr;

    auto* memory = static_cast<unsigned char*>(GlobalLock(handle));
    if (!memory)
    {
        GlobalFree(handle);
        return nullptr;
    }

    auto* drop = reinterpret_cast<DROPFILES*>(memory);
    drop->pFiles = sizeof(DROPFILES);
    drop->pt = { 0, 0 };
    drop->fNC = FALSE;
    drop->fWide = TRUE;

    wchar_t* write = reinterpret_cast<wchar_t*>(memory + sizeof(DROPFILES));
    for (const auto& path : paths)
    {
        const size_t count = path.size() + 1;
        std::memcpy(write, path.c_str(), count * sizeof(wchar_t));
        write += count;
    }
    *write = L'\0';

    GlobalUnlock(handle);
    return handle;
}

HGLOBAL CreateDropEffect(DWORD effect)
{
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!handle)
        return nullptr;

    auto* value = static_cast<DWORD*>(GlobalLock(handle));
    if (!value)
    {
        GlobalFree(handle);
        return nullptr;
    }

    *value = effect;
    GlobalUnlock(handle);
    return handle;
}

bool WriteClipboardFiles(const std::vector<std::wstring>& paths, DWORD effect)
{
    if (paths.empty())
        return false;

    HGLOBAL hDrop = CreateHDrop(paths);
    HGLOBAL hEffect = CreateDropEffect(effect);
    if (!hDrop || !hEffect)
    {
        if (hDrop) GlobalFree(hDrop);
        if (hEffect) GlobalFree(hEffect);
        return false;
    }

    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!effectFormat || !OpenClipboard(GetConsoleWindow()))
    {
        GlobalFree(hDrop);
        GlobalFree(hEffect);
        return false;
    }

    bool ok = false;
    if (EmptyClipboard())
    {
        if (SetClipboardData(CF_HDROP, hDrop))
        {
            hDrop = nullptr; // clipboard owns it now
            if (SetClipboardData(effectFormat, hEffect))
            {
                hEffect = nullptr; // clipboard owns it now
                ok = true;
            }
        }
    }

    if (!ok)
        EmptyClipboard();

    CloseClipboard();

    if (hDrop) GlobalFree(hDrop);
    if (hEffect) GlobalFree(hEffect);
    return ok;
}

bool PutPanelFilesOnClipboard(DWORD effect)
{
    if (!IsPanelsWindow())
        return false;

    std::vector<std::wstring> paths;
    if (!GetSourceFilesFromPanel(paths))
        return true;

    if (!WriteClipboardFiles(paths, effect))
    {
        ShowMessage(Msg(MClipboardWriteFailed), true);
        return true;
    }

    return true;
}

bool CopyPanelFilesToClipboard()
{
    return PutPanelFilesOnClipboard(DROPEFFECT_COPY);
}

bool CutPanelFilesToClipboard()
{
    return PutPanelFilesOnClipboard(DROPEFFECT_MOVE);
}

std::wstring BuildDoubleNullList(const std::vector<std::wstring>& values)
{
    std::wstring result;
    size_t total = 1;
    for (const auto& value : values)
        total += value.size() + 1;
    result.reserve(total);

    for (const auto& value : values)
    {
        result.append(value);
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

std::wstring BuildDoubleNullSingle(const std::wstring& value)
{
    std::wstring result = value;
    result.push_back(L'\0');
    result.push_back(L'\0');
    return result;
}

bool RunShellOperation(
    UINT function,
    const std::vector<std::wstring>& fromPaths,
    const std::vector<std::wstring>& toPaths,
    FILEOP_FLAGS flags,
    int& errorCode,
    bool& aborted)
{
    errorCode = 0;
    aborted = false;

    if (fromPaths.empty())
        return false;

    const std::wstring from = BuildDoubleNullList(fromPaths);
    std::wstring to;
    if (!toPaths.empty())
        to = BuildDoubleNullList(toPaths);

    SHFILEOPSTRUCTW op{};
    op.hwnd = GetConsoleWindow();
    op.wFunc = function;
    op.pFrom = from.c_str();
    op.pTo = toPaths.empty() ? nullptr : to.c_str();
    op.fFlags = flags;

    errorCode = SHFileOperationW(&op);
    aborted = op.fAnyOperationsAborted != FALSE;
    return errorCode == 0 && !aborted;
}

bool ValidateUndoMove(const FileActionRecord& record)
{
    for (const auto& item : record.Items)
    {
        if (!PathMatchesIdentity(item.Destination, item.DestinationIdentity) || PathExists(item.Source))
            return false;

        const std::wstring parent = ParentDirectory(item.Source);
        if (parent.empty() || !DirectoryExists(parent))
            return false;
    }
    return true;
}

bool ValidateRedo(const FileActionRecord& record)
{
    for (const auto& item : record.Items)
    {
        if (!PathMatchesIdentity(item.Source, item.SourceIdentity) || PathExists(item.Destination))
            return false;

        const std::wstring parent = ParentDirectory(item.Destination);
        if (parent.empty() || !DirectoryExists(parent))
            return false;
    }
    return true;
}

void ClearClipboardAfterMove()
{
    if (OpenClipboard(GetConsoleWindow()))
    {
        EmptyClipboard();
        CloseClipboard();
    }
}

void RefreshPanel()
{
    if (!g_Info.PanelControl)
        return;

    g_Info.PanelControl(PANEL_ACTIVE, FCTL_UPDATEPANEL, 0, nullptr);
    g_Info.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, nullptr);
}

bool HasTopLevelCollision(const std::vector<std::wstring>& sources, const std::wstring& destination)
{
    for (const auto& source : sources)
    {
        const std::wstring name = BaseName(source);
        if (!name.empty() && PathExists(JoinPath(destination, name)))
            return true;
    }
    return false;
}

void UpdateClipboardAfterPartialMove(const std::vector<std::wstring>& originalSources)
{
    std::vector<std::wstring> remaining;
    remaining.reserve(originalSources.size());

    for (const auto& source : originalSources)
    {
        if (PathExists(source))
            remaining.push_back(source);
    }

    if (remaining.empty())
        ClearClipboardAfterMove();
    else
        WriteClipboardFiles(remaining, DROPEFFECT_MOVE);
}

bool PasteClipboardFilesUsingShell(
    const ClipboardFiles& clip,
    const std::wstring& destination)
{
    FileActionRecord candidate{};
    const bool candidateSafe = BuildSafeActionRecord(
        clip.Paths,
        destination,
        clip.Move ? FileAction::Move : FileAction::Copy,
        candidate);

    const std::wstring from = BuildDoubleNullList(clip.Paths);
    const std::wstring to   = BuildDoubleNullSingle(destination);

    SHFILEOPSTRUCTW op{};
    op.hwnd = GetConsoleWindow();
    op.wFunc = clip.Move ? FO_MOVE : FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS;

    const int rc = SHFileOperationW(&op);
    RefreshPanel();

    if (rc == 0 && !op.fAnyOperationsAborted)
    {
        ClearRecord(g_UndoRecord);
        ClearRecord(g_RedoRecord);

        if (candidateSafe && FinalizeCompletedAction(candidate))
            g_UndoRecord = std::move(candidate);

        if (clip.Move)
            ClearClipboardAfterMove();
        return true;
    }

    // A Shell operation can be partially completed before it is cancelled or
    // fails. The previous history can no longer be trusted in that case.
    ClearRecord(g_UndoRecord);
    ClearRecord(g_RedoRecord);

    if (clip.Move)
        UpdateClipboardAfterPartialMove(clip.Paths);

    if (op.fAnyOperationsAborted)
    {
        return true;
    }

    ShowMessage(std::wstring(Msg(MOperationFailedPrefix)) + L" " + std::to_wstring(rc), true);
    return true;
}

bool PasteClipboardFilesWithFarConflicts(const ClipboardFiles& clip, const std::wstring& destination);
bool PasteShellClipboardToDirectory(const std::wstring& destination);
bool IsValidSimpleName(const std::wstring& name);
bool RunExactCopyOrMove(
    bool move,
    const std::wstring& source,
    const std::wstring& target,
    int& errorCode,
    bool& aborted);

void ReplaceAll(std::wstring& text, const std::wstring& from, const std::wstring& to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos)
    {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::wstring AutoRenameNameForSource(const std::wstring& source, unsigned long long n)
{
    const std::wstring original = BaseName(source);
    const DWORD attrs = GetFileAttributesW(source.c_str());
    const bool directory = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

    std::wstring name = original;
    std::wstring ext;
    if (!directory)
    {
        const size_t dot = original.find_last_of(L'.');
        if (dot != std::wstring::npos && dot != 0)
        {
            name = original.substr(0, dot);
            ext = original.substr(dot);
        }
    }

    std::wstring result = g_AutoRenameTemplate;
    ReplaceAll(result, L"{name}", name);
    ReplaceAll(result, L"{ext}", ext);
    ReplaceAll(result, L"{n}", std::to_wstring(n));
    return result;
}

bool FindAutoRenameTarget(
    const std::wstring& source,
    const std::wstring& destination,
    std::wstring& target)
{
    const std::wstring originalTarget = JoinPath(destination, BaseName(source));
    if (!PathExists(originalTarget))
    {
        target = originalTarget;
        return true;
    }

    for (unsigned long long n = 1; n < 1000000ULL; ++n)
    {
        const std::wstring candidateName = AutoRenameNameForSource(source, n);
        if (!IsValidSimpleName(candidateName))
            return false;

        const std::wstring candidate = JoinPath(destination, candidateName);
        if (!PathExists(candidate))
        {
            target = candidate;
            return true;
        }
    }

    return false;
}

bool PasteClipboardFilesWithAutoRename(const ClipboardFiles& clip, const std::wstring& destination)
{
    bool anyPerformed = false;
    bool failed = false;
    bool cancelled = false;
    int failureCode = 0;
    bool historySafe = true;

    FileActionRecord candidate{};
    candidate.Action = clip.Move ? FileAction::Move : FileAction::Copy;

    for (const auto& source : clip.Paths)
    {
        std::wstring target;
        if (!FindAutoRenameTarget(source, destination, target))
        {
            ShowMessage(Msg(MAutoRenameFailed), true);
            failed = true;
            historySafe = false;
            break;
        }

        FileActionItem historyItem{};
        historyItem.Source = source;
        historyItem.Destination = target;
        if (!GetFileIdentity(source, historyItem.SourceIdentity))
            historySafe = false;

        int rc = 0;
        bool aborted = false;
        const bool ok = RunExactCopyOrMove(clip.Move, source, target, rc, aborted);
        if (!ok)
        {
            failed = true;
            cancelled = aborted;
            failureCode = rc;
            historySafe = false;
            break;
        }

        anyPerformed = true;
        if (historySafe)
        {
            if (GetFileIdentity(target, historyItem.DestinationIdentity))
                candidate.Items.push_back(std::move(historyItem));
            else
                historySafe = false;
        }
    }

    RefreshPanel();

    if (anyPerformed)
    {
        ClearRecord(g_UndoRecord);
        ClearRecord(g_RedoRecord);
        if (historySafe && !candidate.Items.empty())
            g_UndoRecord = std::move(candidate);
    }

    if (clip.Move && (anyPerformed || cancelled || failed))
        UpdateClipboardAfterPartialMove(clip.Paths);

    if (cancelled)
    {
        return true;
    }

    if (failed && failureCode)
        ShowMessage(std::wstring(Msg(MOperationFailedPrefix)) + L" " + std::to_wstring(failureCode), true);

    return true;
}

bool PasteClipboardFilesToDirectory(const std::wstring& destination)
{
    ClipboardFiles clip;
    if (!ReadClipboardFiles(clip))
        return PasteShellClipboardToDirectory(destination);

    if (g_PrecheckInvalidOperations)
    {
        std::wstring reason;
        if (!ValidateObviousOperation(clip.Paths, destination, clip.Move, reason))
        {
            ShowMessage(reason, true);
            return true;
        }
    }

    if (g_ConflictMode == ConflictMode::Ask && HasTopLevelCollision(clip.Paths, destination))
        return PasteClipboardFilesWithFarConflicts(clip, destination);

    if (g_ConflictMode == ConflictMode::AutoRename)
        return PasteClipboardFilesWithAutoRename(clip, destination);

    return PasteClipboardFilesUsingShell(clip, destination);
}

bool PasteClipboardFiles()
{
    if (!IsPanelsWindow())
        return false;

    PanelInfo panel{};
    if (!GetPanelInfo(panel))
    {
        ShowMessage(Msg(MPastePanelOnly), true);
        return true;
    }

    // Ordinary Paste is intentionally tied only to the directory currently
    // open in the active panel. The item under the cursor never changes it.
    std::wstring destination;
    if (!GetActiveDirectory(destination))
    {
        ShowMessage(Msg(MActivePanelNotFs), true);
        return true;
    }

    return PasteClipboardFilesToDirectory(destination);
}

bool PasteClipboardFilesIntoFolderUnderCursor();
bool UndoLastOperation();
bool RedoLastOperation();

bool CanPasteIntoFolderUnderCursor()
{
    if (!IsPanelsWindow())
        return false;

    PanelInfo panel{};
    if (!GetPanelInfo(panel))
        return false;

    std::wstring panelDirectory;
    if (!GetActiveDirectory(panelDirectory))
        return false;

    CurrentPanelItem item{};
    if (!GetCurrentPanelItem(item))
        return false;

    if (item.FileName == L".." || item.FileName == L"." ||
        (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return false;

    const std::wstring destination = JoinPath(panelDirectory, item.FileName);
    const DWORD attrs = GetFileAttributesW(destination.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void ShowOperationsMenu()
{
    if (!g_Info.Menu)
        return;

    const wchar_t copyLetter = HotkeyLetterOrDefault(g_CopyHotkey, L'C');
    const wchar_t cutLetter = HotkeyLetterOrDefault(g_CutHotkey, L'X');
    const wchar_t pasteLetter = HotkeyLetterOrDefault(g_PasteHotkey, L'V');
    const wchar_t pasteIntoLetter = HotkeyLetterOrDefault(g_PasteIntoHotkey, L'D');
    const wchar_t undoLetter = HotkeyLetterOrDefault(g_UndoHotkey, L'Z');
    const wchar_t redoLetter = HotkeyLetterOrDefault(g_RedoHotkey, L'E');

    std::array<std::wstring, 6> labels = {
        std::wstring(Msg(MMenuCopy)) + L"  " + MakeDisplayHotkey(copyLetter),
        std::wstring(Msg(MMenuCut)) + L"  " + MakeDisplayHotkey(cutLetter),
        std::wstring(Msg(MMenuPaste)) + L"  " + MakeDisplayHotkey(pasteLetter),
        std::wstring(Msg(MMenuPasteInto)) + L"  " + MakeDisplayHotkey(pasteIntoLetter),
        std::wstring(Msg(MMenuUndo)) + L"  " + MakeDisplayHotkey(undoLetter),
        std::wstring(Msg(MMenuRedo)) + L"  " + MakeDisplayHotkey(redoLetter)
    };

    FarMenuItem items[7]{};
    items[0].Text = labels[0].c_str();
    items[1].Text = labels[1].c_str();
    items[2].Text = labels[2].c_str();
    items[3].Text = labels[3].c_str();
    items[4].Flags = MIF_SEPARATOR;
    items[4].Text = L"";
    items[5].Text = labels[4].c_str();
    items[6].Text = labels[5].c_str();

    if (!CanPasteIntoFolderUnderCursor())
        items[3].Flags |= MIF_DISABLE;
    if (!HasRecord(g_UndoRecord))
        items[5].Flags |= MIF_DISABLE;
    if (!HasRecord(g_RedoRecord))
        items[6].Flags |= MIF_DISABLE;

    const intptr_t selected = g_Info.Menu(
        &PluginGuid,
        &OperationsMenuGuid,
        -1,
        -1,
        0,
        FMENU_WRAPMODE | FMENU_AUTOHIGHLIGHT,
        Msg(MPluginName),
        nullptr,
        L"Commands",
        nullptr,
        nullptr,
        items,
        _countof(items));

    switch (selected)
    {
        case 0: CopyPanelFilesToClipboard(); break;
        case 1: CutPanelFilesToClipboard(); break;
        case 2: PasteClipboardFiles(); break;
        case 3: PasteClipboardFilesIntoFolderUnderCursor(); break;
        case 5: UndoLastOperation(); break;
        case 6: RedoLastOperation(); break;
        default: break;
    }
}

bool PasteClipboardFilesIntoFolderUnderCursor()
{
    if (!IsPanelsWindow())
        return false;

    PanelInfo panel{};
    if (!GetPanelInfo(panel))
    {
        ShowMessage(Msg(MPastePanelOnly), true);
        return true;
    }

    std::wstring panelDirectory;
    if (!GetActiveDirectory(panelDirectory))
    {
        ShowMessage(Msg(MActivePanelNotFs), true);
        return true;
    }

    CurrentPanelItem item{};
    if (!GetCurrentPanelItem(item))
    {
        ShowMessage(Msg(MCurrentItemFailed), true);
        return true;
    }

    if (item.FileName == L".." || item.FileName == L"." ||
        (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        ShowMessage(Msg(MNeedDirectoryUnderCursor));
        return true;
    }

    const std::wstring destination = JoinPath(panelDirectory, item.FileName);
    const DWORD attrs = GetFileAttributesW(destination.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        ShowMessage(std::wstring(Msg(MDirectoryUnavailable)) + L"\n" + destination, true);
        return true;
    }

    return PasteClipboardFilesToDirectory(destination);
}


bool UndoLastOperation()
{
    if (!IsPanelsWindow())
        return false;

    if (!HasRecord(g_UndoRecord))
    {
        ShowMessage(Msg(MUndoUnavailable));
        return true;
    }

    FileActionRecord record = g_UndoRecord;
    int rc = 0;
    bool aborted = false;
    bool ok = false;

    if (record.Action == FileAction::Copy)
    {
        std::vector<std::wstring> targets;
        targets.reserve(record.Items.size());

        for (const auto& item : record.Items)
        {
            if (!PathMatchesIdentity(item.Destination, item.DestinationIdentity))
            {
                ShowMessage(Msg(MUndoStateChanged), true);
                return true;
            }
            targets.push_back(item.Destination);
        }

        // Copy undo removes only top-level objects that did not exist before
        // the original paste. Ask the Shell to preserve undo information in
        // the Windows session / Recycle Bin when possible.
        ok = RunShellOperation(
            FO_DELETE,
            targets,
            {},
            FOF_ALLOWUNDO,
            rc,
            aborted);
    }
    else if (record.Action == FileAction::Move)
    {
        if (!ValidateUndoMove(record))
        {
            ShowMessage(Msg(MUndoStateChanged), true);
            return true;
        }

        std::vector<std::wstring> from;
        std::vector<std::wstring> to;
        from.reserve(record.Items.size());
        to.reserve(record.Items.size());

        for (const auto& item : record.Items)
        {
            from.push_back(item.Destination);
            to.push_back(item.Source);
        }

        ok = RunShellOperation(
            FO_MOVE,
            from,
            to,
            FOF_MULTIDESTFILES | FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS,
            rc,
            aborted);
    }

    RefreshPanel();

    if (ok)
    {
        if (record.Action == FileAction::Move)
        {
            for (auto& item : record.Items)
            {
                if (!GetFileIdentity(item.Source, item.SourceIdentity))
                {
                    ClearRecord(g_UndoRecord);
                    ClearRecord(g_RedoRecord);
                    ShowMessage(Msg(MUndoStateChanged), true);
                    return true;
                }
                item.DestinationIdentity = {};
            }
        }

        g_RedoRecord = std::move(record);
        ClearRecord(g_UndoRecord);
        return true;
    }

    // SHFileOperation can partially complete an operation before returning an
    // error/cancellation. Do not keep history that may no longer match disk.
    ClearRecord(g_UndoRecord);
    ClearRecord(g_RedoRecord);

    if (aborted)
        ShowMessage(Msg(MUndoCancelled));
    else
        ShowMessage(std::wstring(Msg(MUndoFailedPrefix)) + L" " + std::to_wstring(rc), true);

    return true;
}

bool RedoLastOperation()
{
    if (!IsPanelsWindow())
        return false;

    if (!HasRecord(g_RedoRecord))
    {
        ShowMessage(Msg(MRedoUnavailable));
        return true;
    }

    FileActionRecord record = g_RedoRecord;
    if (!ValidateRedo(record))
    {
        ShowMessage(Msg(MRedoStateChanged), true);
        return true;
    }

    std::vector<std::wstring> from;
    std::vector<std::wstring> to;
    from.reserve(record.Items.size());
    to.reserve(record.Items.size());

    for (const auto& item : record.Items)
    {
        from.push_back(item.Source);
        to.push_back(item.Destination);
    }

    int rc = 0;
    bool aborted = false;
    const UINT function = record.Action == FileAction::Move ? FO_MOVE : FO_COPY;
    const bool ok = RunShellOperation(
        function,
        from,
        to,
        FOF_MULTIDESTFILES | FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS,
        rc,
        aborted);

    RefreshPanel();

    if (ok)
    {
        for (auto& item : record.Items)
        {
            if (!GetFileIdentity(item.Destination, item.DestinationIdentity))
            {
                ClearRecord(g_UndoRecord);
                ClearRecord(g_RedoRecord);
                ShowMessage(Msg(MRedoStateChanged), true);
                return true;
            }
        }

        g_UndoRecord = std::move(record);
        ClearRecord(g_RedoRecord);
        return true;
    }

    ClearRecord(g_UndoRecord);
    ClearRecord(g_RedoRecord);

    if (aborted)
        ShowMessage(Msg(MRedoCancelled));
    else
        ShowMessage(std::wstring(Msg(MRedoFailedPrefix)) + L" " + std::to_wstring(rc), true);

    return true;
}

bool OpenSettings(FarSettingsCreate& settings)
{
    if (!g_Info.SettingsControl)
        return false;

    settings = {};
    settings.StructSize = sizeof(settings);
    settings.Guid = PluginGuid;
    settings.Handle = nullptr;

    return g_Info.SettingsControl(INVALID_HANDLE_VALUE, SCTL_CREATE, PSL_ROAMING, &settings) != 0;
}

bool ReadStringSetting(HANDLE handle, const wchar_t* name, std::wstring& value)
{
    FarSettingsItem item{};
    item.StructSize = sizeof(item);
    item.Root = 0;
    item.Name = name;
    item.Type = FST_STRING;

    if (!g_Info.SettingsControl(handle, SCTL_GET, 0, &item) || !item.String || !*item.String)
        return false;

    value = item.String;
    return true;
}

bool WriteStringSetting(HANDLE handle, const wchar_t* name, const std::wstring& value)
{
    FarSettingsItem item{};
    item.StructSize = sizeof(item);
    item.Root = 0;
    item.Name = name;
    item.Type = FST_STRING;
    item.String = value.c_str();
    return g_Info.SettingsControl(handle, SCTL_SET, 0, &item) != 0;
}

bool ReadNumberSetting(HANDLE handle, const wchar_t* name, unsigned long long& value)
{
    FarSettingsItem item{};
    item.StructSize = sizeof(item);
    item.Root = 0;
    item.Name = name;
    item.Type = FST_QWORD;

    if (!g_Info.SettingsControl(handle, SCTL_GET, 0, &item))
        return false;

    value = item.Number;
    return true;
}

bool WriteNumberSetting(HANDLE handle, const wchar_t* name, unsigned long long value)
{
    FarSettingsItem item{};
    item.StructSize = sizeof(item);
    item.Root = 0;
    item.Name = name;
    item.Type = FST_QWORD;
    item.Number = value;
    return g_Info.SettingsControl(handle, SCTL_SET, 0, &item) != 0;
}

void SetFixedHotkey(std::wstring& target, HotkeySpec& spec, wchar_t letter)
{
    target = MakeInternalHotkey(letter);
    ParseHotkey(target, spec);
}

bool ApplyStoredFixedHotkey(const std::wstring& value, std::wstring& target, HotkeySpec& spec)
{
    wchar_t letter = 0;
    HotkeySpec parsed{};
    if (!ParseFixedHotkey(value, letter, parsed))
        return false;

    target = MakeInternalHotkey(letter);
    spec = parsed;
    return true;
}

void LoadSettings()
{
    SetFixedHotkey(g_CopyHotkey, g_CopyHotkeySpec, L'C');
    SetFixedHotkey(g_CutHotkey, g_CutHotkeySpec, L'X');
    SetFixedHotkey(g_PasteHotkey, g_PasteHotkeySpec, L'V');
    SetFixedHotkey(g_PasteIntoHotkey, g_PasteIntoHotkeySpec, L'D');
    SetFixedHotkey(g_UndoHotkey, g_UndoHotkeySpec, L'Z');
    SetFixedHotkey(g_RedoHotkey, g_RedoHotkeySpec, L'E');

    FarSettingsCreate settings{};
    if (!OpenSettings(settings))
        return;

    std::wstring value;
    if (ReadStringSetting(settings.Handle, CopyHotkeySetting, value))
        ApplyStoredFixedHotkey(value, g_CopyHotkey, g_CopyHotkeySpec);

    value.clear();
    if (ReadStringSetting(settings.Handle, CutHotkeySetting, value))
        ApplyStoredFixedHotkey(value, g_CutHotkey, g_CutHotkeySpec);

    value.clear();
    bool loadedPaste = ReadStringSetting(settings.Handle, PasteHotkeySetting, value);
    if (!loadedPaste)
    {
        // Migrate FarPaste 0.1.x setting transparently when it fits the
        // current Ctrl+Shift+letter model.
        loadedPaste = ReadStringSetting(settings.Handle, LegacyHotkeySetting, value);
    }
    if (loadedPaste)
        ApplyStoredFixedHotkey(value, g_PasteHotkey, g_PasteHotkeySpec);

    value.clear();
    if (ReadStringSetting(settings.Handle, PasteIntoHotkeySetting, value))
        ApplyStoredFixedHotkey(value, g_PasteIntoHotkey, g_PasteIntoHotkeySpec);

    value.clear();
    if (ReadStringSetting(settings.Handle, UndoHotkeySetting, value))
        ApplyStoredFixedHotkey(value, g_UndoHotkey, g_UndoHotkeySpec);

    value.clear();
    if (ReadStringSetting(settings.Handle, RedoHotkeySetting, value))
        ApplyStoredFixedHotkey(value, g_RedoHotkey, g_RedoHotkeySpec);

    unsigned long long number = 0;
    if (ReadNumberSetting(settings.Handle, PrecheckInvalidSetting, number))
        g_PrecheckInvalidOperations = number != 0;

    number = 0;
    if (ReadNumberSetting(settings.Handle, ConflictModeSetting, number) &&
        number <= static_cast<unsigned long long>(ConflictMode::AutoRename))
        g_ConflictMode = static_cast<ConflictMode>(number);

    value.clear();
    if (ReadStringSetting(settings.Handle, AutoRenameTemplateSetting, value))
        g_AutoRenameTemplate = value;
    else
        g_AutoRenameTemplate = DefaultAutoRenameTemplate;

    g_Info.SettingsControl(settings.Handle, SCTL_FREE, 0, nullptr);
}

bool SaveSettings(
    const std::wstring& copyHotkey,
    const std::wstring& cutHotkey,
    const std::wstring& pasteHotkey,
    const std::wstring& pasteIntoHotkey,
    const std::wstring& undoHotkey,
    const std::wstring& redoHotkey,
    bool precheckInvalidOperations,
    ConflictMode conflictMode,
    const std::wstring& autoRenameTemplate)
{
    FarSettingsCreate settings{};
    if (!OpenSettings(settings))
        return false;

    const bool copyOk = WriteStringSetting(settings.Handle, CopyHotkeySetting, copyHotkey);
    const bool cutOk = WriteStringSetting(settings.Handle, CutHotkeySetting, cutHotkey);
    const bool pasteOk = WriteStringSetting(settings.Handle, PasteHotkeySetting, pasteHotkey);
    const bool pasteIntoOk = WriteStringSetting(settings.Handle, PasteIntoHotkeySetting, pasteIntoHotkey);
    const bool undoOk = WriteStringSetting(settings.Handle, UndoHotkeySetting, undoHotkey);
    const bool redoOk = WriteStringSetting(settings.Handle, RedoHotkeySetting, redoHotkey);
    const bool precheckOk = WriteNumberSetting(settings.Handle, PrecheckInvalidSetting, precheckInvalidOperations ? 1ULL : 0ULL);
    const bool conflictOk = WriteNumberSetting(settings.Handle, ConflictModeSetting, static_cast<unsigned long long>(conflictMode));
    const bool templateOk = WriteStringSetting(settings.Handle, AutoRenameTemplateSetting, autoRenameTemplate);
    g_Info.SettingsControl(settings.Handle, SCTL_FREE, 0, nullptr);
    return copyOk && cutOk && pasteOk && pasteIntoOk && undoOk && redoOk && precheckOk && conflictOk && templateOk;
}

FarDialogItem MakeDialogItem(
    FARDIALOGITEMTYPES type,
    intptr_t x1,
    intptr_t y1,
    intptr_t x2,
    intptr_t y2,
    FARDIALOGITEMFLAGS flags,
    const wchar_t* data,
    size_t maxLength = 0)
{
    FarDialogItem item{};
    item.Type = type;
    item.X1 = x1;
    item.Y1 = y1;
    item.X2 = x2;
    item.Y2 = y2;
    item.Flags = flags;
    item.Data = data;
    item.MaxLength = maxLength;
    return item;
}


enum class ConflictChoice
{
    Replace,
    Skip,
    Rename,
    Cancel
};

struct ConflictDecision
{
    ConflictChoice Choice = ConflictChoice::Cancel;
    bool ApplyToAll = false;
};

bool IsValidSimpleName(const std::wstring& name)
{
    if (name.empty() || name == L"." || name == L"..")
        return false;

    if (name.back() == L' ' || name.back() == L'.')
        return false;

    constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
    for (wchar_t ch : name)
    {
        if (ch < 32 || std::wcschr(invalid, ch))
            return false;
    }

    return true;
}

bool PromptNewObjectName(const std::wstring& currentName, std::wstring& newName)
{
    if (!g_Info.InputBox)
        return false;

    std::array<wchar_t, 1024> buffer{};
    wcsncpy_s(buffer.data(), buffer.size(), currentName.c_str(), _TRUNCATE);

    while (true)
    {
        const intptr_t result = g_Info.InputBox(
            &PluginGuid,
            &RenameInputGuid,
            Msg(MPluginName),
            Msg(MConflictNewName),
            nullptr,
            buffer.data(),
            buffer.data(),
            buffer.size(),
            L"Conflicts",
            FIB_NOUSELASTHISTORY | FIB_BUTTONS);

        if (!result)
            return false;

        newName = buffer.data();
        if (IsValidSimpleName(newName))
            return true;

        ShowMessage(Msg(MConflictInvalidName), true);
    }
}

ConflictDecision ShowConflictDialog(const std::wstring& destinationPath, bool replaceAllowed)
{
    ConflictDecision decision{};

    if (!g_Info.DialogInit || !g_Info.DialogRun || !g_Info.DialogFree ||
        !g_Info.SendDlgMessage || !g_Info.DefDlgProc)
        return decision;

    enum ConflictItem : intptr_t
    {
        ConflictBox = 0,
        ConflictText,
        ConflictName,
        ConflictApplyAll,
        ConflictSeparator,
        ConflictReplace,
        ConflictSkip,
        ConflictRename,
        ConflictCancel,
        ConflictCount
    };

    const std::wstring name = BaseName(destinationPath);
    const std::wstring displayName = name.size() > 60 ? name.substr(0, 57) + L"..." : name;
    FarDialogItem items[ConflictCount]{};
    items[ConflictBox] = MakeDialogItem(DI_DOUBLEBOX, 3, 1, 72, 7, DIF_NONE, Msg(MWarningTitle));
    items[ConflictText] = MakeDialogItem(DI_TEXT, 5, 2, 70, 2, DIF_NONE, Msg(MConflictExists));
    items[ConflictName] = MakeDialogItem(DI_TEXT, 7, 3, 70, 3, DIF_NONE, displayName.c_str());
    items[ConflictApplyAll] = MakeDialogItem(DI_CHECKBOX, 5, 4, 0, 4, DIF_NONE, Msg(MConflictApplyAll));
    items[ConflictSeparator] = MakeDialogItem(DI_TEXT, 5, 5, 70, 5, DIF_SEPARATOR, L"");

    const FARDIALOGITEMFLAGS replaceFlags = replaceAllowed ? DIF_DEFAULTBUTTON : DIF_DISABLE;
    items[ConflictReplace] = MakeDialogItem(DI_BUTTON, 7, 6, 0, 6, replaceFlags, Msg(MConflictReplace));
    items[ConflictSkip] = MakeDialogItem(DI_BUTTON, 23, 6, 0, 6, DIF_NONE, Msg(MConflictSkip));
    items[ConflictRename] = MakeDialogItem(
        DI_BUTTON, 38, 6, 0, 6, replaceAllowed ? DIF_NONE : DIF_DEFAULTBUTTON, Msg(MConflictRename));
    items[ConflictCancel] = MakeDialogItem(DI_BUTTON, 57, 6, 0, 6, DIF_NONE, Msg(MConflictCancel));

    HANDLE dialog = g_Info.DialogInit(
        &PluginGuid,
        &ConflictDialogGuid,
        -1,
        -1,
        76,
        9,
        L"Conflicts",
        items,
        ConflictCount,
        0,
        FDLG_WARNING,
        reinterpret_cast<FARWINDOWPROC>(g_Info.DefDlgProc),
        nullptr);

    if (!dialog || dialog == INVALID_HANDLE_VALUE)
        return decision;

    const intptr_t result = g_Info.DialogRun(dialog);
    const bool applyToAll =
        g_Info.SendDlgMessage(dialog, DM_GETCHECK, ConflictApplyAll, nullptr) != 0;
    g_Info.DialogFree(dialog);

    if (result == ConflictReplace && replaceAllowed)
        decision.Choice = ConflictChoice::Replace;
    else if (result == ConflictSkip)
        decision.Choice = ConflictChoice::Skip;
    else if (result == ConflictRename)
        decision.Choice = ConflictChoice::Rename;
    else
        decision.Choice = ConflictChoice::Cancel;

    decision.ApplyToAll = applyToAll &&
        (decision.Choice == ConflictChoice::Replace || decision.Choice == ConflictChoice::Skip);
    return decision;
}

bool RunExactCopyOrMove(
    bool move,
    const std::wstring& source,
    const std::wstring& target,
    int& errorCode,
    bool& aborted)
{
    return RunShellOperation(
        move ? FO_MOVE : FO_COPY,
        { source },
        { target },
        FOF_MULTIDESTFILES | FOF_NOCONFIRMATION |
            FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS,
        errorCode,
        aborted);
}

bool GetShellItemInfo(IShellItem* item, std::wstring& name, bool& directory, std::wstring& fileSystemPath)
{
    name.clear();
    directory = false;
    fileSystemPath.clear();
    if (!item)
        return false;

    PWSTR rawName = nullptr;
    HRESULT hr = item->GetDisplayName(SIGDN_PARENTRELATIVEEDITING, &rawName);
    if (FAILED(hr) || !rawName || !*rawName)
    {
        if (rawName)
            CoTaskMemFree(rawName);
        rawName = nullptr;
        hr = item->GetDisplayName(SIGDN_NORMALDISPLAY, &rawName);
    }

    if (FAILED(hr) || !rawName || !*rawName)
    {
        if (rawName)
            CoTaskMemFree(rawName);
        return false;
    }

    name = rawName;
    CoTaskMemFree(rawName);
    if (!IsValidSimpleName(name))
        return false;

    SFGAOF attrs = 0;
    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attrs)))
        directory = (attrs & SFGAO_FOLDER) != 0;

    PWSTR rawPath = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath)
    {
        fileSystemPath = rawPath;
        CoTaskMemFree(rawPath);
    }

    return true;
}

std::wstring AutoRenameNameForObject(
    const std::wstring& original,
    bool directory,
    unsigned long long n)
{
    std::wstring name = original;
    std::wstring ext;
    if (!directory)
    {
        const size_t dot = original.find_last_of(L'.');
        if (dot != std::wstring::npos && dot != 0)
        {
            name = original.substr(0, dot);
            ext = original.substr(dot);
        }
    }

    std::wstring result = g_AutoRenameTemplate;
    ReplaceAll(result, L"{name}", name);
    ReplaceAll(result, L"{ext}", ext);
    ReplaceAll(result, L"{n}", std::to_wstring(n));
    return result;
}

bool FindAutoRenameTargetForObject(
    const std::wstring& original,
    bool directory,
    const std::wstring& destination,
    std::wstring& targetName)
{
    if (!PathExists(JoinPath(destination, original)))
    {
        targetName.clear(); // Preserve the original Shell name.
        return true;
    }

    for (unsigned long long n = 1; n < 1000000ULL; ++n)
    {
        const std::wstring candidateName = AutoRenameNameForObject(original, directory, n);
        if (!IsValidSimpleName(candidateName))
            return false;

        if (!PathExists(JoinPath(destination, candidateName)))
        {
            targetName = candidateName;
            return true;
        }
    }
    return false;
}

bool PerformShellClipboardOperation(
    ShellClipboardData& clip,
    const std::wstring& destination,
    bool useSystemConflictUi)
{
    IShellItem* destinationItem = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(
        destination.c_str(), nullptr, IID_PPV_ARGS(&destinationItem));
    if (FAILED(hr) || !destinationItem)
        return false;

    IFileOperation* operation = nullptr;
    hr = CoCreateInstance(
        CLSID_FileOperation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));
    if (FAILED(hr) || !operation)
    {
        destinationItem->Release();
        return false;
    }

    FILEOP_FLAGS flags = FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS;
    if (!useSystemConflictUi)
        flags |= FOF_NOCONFIRMATION;
    operation->SetOperationFlags(flags);

    hr = clip.Move
        ? operation->MoveItems(clip.DataObject, destinationItem)
        : operation->CopyItems(clip.DataObject, destinationItem);
    if (SUCCEEDED(hr))
        hr = operation->PerformOperations();

    BOOL aborted = FALSE;
    if (SUCCEEDED(hr))
        operation->GetAnyOperationsAborted(&aborted);

    operation->Release();
    destinationItem->Release();
    RefreshPanel();

    if (FAILED(hr))
    {
        ShowMessage(std::wstring(Msg(MOperationFailedPrefix)) + L" HRESULT=" +
            std::to_wstring(static_cast<unsigned long>(hr)), true);
        return true;
    }

    if (aborted)
    {
        return true;
    }

    ClearRecord(g_UndoRecord);
    ClearRecord(g_RedoRecord);
    if (clip.Move)
        ClearClipboardAfterMove();
    return true;
}

bool PasteShellClipboardWithPluginConflicts(
    ShellClipboardData& clip,
    const std::wstring& destination)
{
    IShellItem* destinationItem = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(
        destination.c_str(), nullptr, IID_PPV_ARGS(&destinationItem));
    if (FAILED(hr) || !destinationItem)
        return false;

    IFileOperation* operation = nullptr;
    hr = CoCreateInstance(
        CLSID_FileOperation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));
    if (FAILED(hr) || !operation)
    {
        destinationItem->Release();
        return false;
    }

    operation->SetOperationFlags(
        FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS);

    DWORD count = 0;
    clip.Items->GetCount(&count);
    bool stickyDecisionActive = false;
    ConflictChoice stickyChoice = ConflictChoice::Cancel;
    bool queuedAny = false;
    bool cancelled = false;

    for (DWORD i = 0; i < count; ++i)
    {
        IShellItem* item = nullptr;
        if (FAILED(clip.Items->GetItemAt(i, &item)) || !item)
            continue;

        std::wstring originalName;
        std::wstring sourcePath;
        bool directory = false;
        if (!GetShellItemInfo(item, originalName, directory, sourcePath))
        {
            item->Release();
            continue;
        }

        std::wstring newName;
        const std::wstring originalTarget = JoinPath(destination, originalName);
        const bool collision = PathExists(originalTarget);
        bool skipItem = false;

        if (g_ConflictMode == ConflictMode::AutoRename)
        {
            if (!FindAutoRenameTargetForObject(
                    originalName, directory, destination, newName))
            {
                item->Release();
                ShowMessage(Msg(MAutoRenameFailed), true);
                operation->Release();
                destinationItem->Release();
                return true;
            }
        }
        else if (g_ConflictMode == ConflictMode::Ask && collision)
        {
            std::wstring conflictTarget = originalTarget;
            while (true)
            {
                bool samePath = false;
                if (!sourcePath.empty())
                {
                    samePath = PathsEqualNoCase(
                        NormalizePathForCompare(sourcePath),
                        NormalizePathForCompare(conflictTarget));
                }

                ConflictDecision decision{};
                if (stickyDecisionActive && !(stickyChoice == ConflictChoice::Replace && samePath))
                {
                    decision.Choice = stickyChoice;
                    decision.ApplyToAll = true;
                }
                else
                {
                    decision = ShowConflictDialog(conflictTarget, !samePath);
                }

                if (decision.ApplyToAll)
                {
                    stickyDecisionActive = true;
                    stickyChoice = decision.Choice;
                }

                if (decision.Choice == ConflictChoice::Skip)
                {
                    skipItem = true;
                    break;
                }
                if (decision.Choice == ConflictChoice::Cancel)
                {
                    cancelled = true;
                    break;
                }
                if (decision.Choice == ConflictChoice::Replace)
                    break;

                std::wstring renamed;
                if (!PromptNewObjectName(originalName, renamed))
                    continue; // Return to the conflict dialog.

                conflictTarget = JoinPath(destination, renamed);
                if (PathExists(conflictTarget))
                {
                    ShowMessage(Msg(MConflictNameExists), true);
                    continue;
                }

                newName = renamed;
                break;
            }
        }

        if (cancelled)
        {
            item->Release();
            break;
        }
        if (skipItem)
        {
            item->Release();
            continue;
        }

        const wchar_t* requestedName = newName.empty() ? nullptr : newName.c_str();
        hr = clip.Move
            ? operation->MoveItem(item, destinationItem, requestedName, nullptr)
            : operation->CopyItem(item, destinationItem, requestedName, nullptr);
        item->Release();
        if (FAILED(hr))
        {
            operation->Release();
            destinationItem->Release();
            ShowMessage(std::wstring(Msg(MOperationFailedPrefix)) + L" HRESULT=" +
                std::to_wstring(static_cast<unsigned long>(hr)), true);
            return true;
        }
        queuedAny = true;
    }

    if (cancelled || !queuedAny)
    {
        operation->Release();
        destinationItem->Release();
        if (cancelled)
            return true;
    }

    hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr))
        operation->GetAnyOperationsAborted(&aborted);

    operation->Release();
    destinationItem->Release();
    RefreshPanel();

    // A Shell namespace source can be virtual (ZIP, Libraries, search results,
    // etc.), so the filesystem-path based Undo / Redo journal is not safe here.
    ClearRecord(g_UndoRecord);
    ClearRecord(g_RedoRecord);

    if (FAILED(hr))
    {
        ShowMessage(std::wstring(Msg(MOperationFailedPrefix)) + L" HRESULT=" +
            std::to_wstring(static_cast<unsigned long>(hr)), true);
        return true;
    }
    if (aborted)
    {
        return true;
    }

    if (clip.Move)
        ClearClipboardAfterMove();
    return true;
}

bool PasteShellClipboardToDirectory(const std::wstring& destination)
{
    ShellClipboardData clip;
    if (!ReadShellClipboard(clip))
    {
        ShowMessage(Msg(MClipboardEmpty));
        return true;
    }

    // Shell namespace items (for example files copied from Explorer's ZIP
    // view) do not necessarily have real filesystem source paths. Let the
    // Windows Shell materialize them through IFileOperation.
    if (g_ConflictMode == ConflictMode::System)
        return PerformShellClipboardOperation(clip, destination, true);

    return PasteShellClipboardWithPluginConflicts(clip, destination);
}

bool PasteClipboardFilesWithFarConflicts(const ClipboardFiles& clip, const std::wstring& destination)
{
    bool stickyDecisionActive = false;
    ConflictChoice stickyChoice = ConflictChoice::Cancel;
    bool anyPerformed = false;
    bool cancelled = false;
    bool failed = false;
    bool historySafe = true;
    int failureCode = 0;

    FileActionRecord candidate{};
    candidate.Action = clip.Move ? FileAction::Move : FileAction::Copy;

    for (const auto& source : clip.Paths)
    {
        if (!PathExists(source))
        {
            ShowMessage(std::wstring(Msg(MObjectNotFound)) + L"\n" + source, true);
            failed = true;
            historySafe = false;
            break;
        }

        const std::wstring originalName = BaseName(source);
        if (originalName.empty())
        {
            failed = true;
            historySafe = false;
            break;
        }

        std::wstring target = JoinPath(destination, originalName);
        const bool hadCollision = PathExists(target);
        bool replacingExisting = false;

        if (hadCollision)
        {
            while (true)
            {
                const bool samePath = PathsEqualNoCase(
                    NormalizePathForCompare(source), NormalizePathForCompare(target));

                ConflictDecision decision{};
                if (stickyDecisionActive && !(stickyChoice == ConflictChoice::Replace && samePath))
                {
                    decision.Choice = stickyChoice;
                    decision.ApplyToAll = true;
                }
                else
                {
                    decision = ShowConflictDialog(target, !samePath);
                }

                if (decision.ApplyToAll)
                {
                    stickyDecisionActive = true;
                    stickyChoice = decision.Choice;
                }

                if (decision.Choice == ConflictChoice::Skip)
                    break;

                if (decision.Choice == ConflictChoice::Cancel)
                {
                    cancelled = true;
                    break;
                }

                if (decision.Choice == ConflictChoice::Replace)
                {
                    replacingExisting = true;
                    historySafe = false;
                    break;
                }

                std::wstring renamed;
                if (!PromptNewObjectName(originalName, renamed))
                    continue; // Back to the conflict dialog.

                const std::wstring renamedTarget = JoinPath(destination, renamed);
                if (PathExists(renamedTarget))
                {
                    ShowMessage(Msg(MConflictNameExists), true);
                    target = renamedTarget;
                    continue;
                }

                target = renamedTarget;
                break;
            }

            if (cancelled)
                break;

            if (!replacingExisting && PathExists(target))
            {
                // The only remaining case is Skip.
                continue;
            }
        }

        FileActionItem historyItem{};
        historyItem.Source = source;
        historyItem.Destination = target;
        if (!GetFileIdentity(source, historyItem.SourceIdentity))
            historySafe = false;

        int rc = 0;
        bool aborted = false;
        const bool ok = RunExactCopyOrMove(clip.Move, source, target, rc, aborted);
        if (!ok)
        {
            failed = true;
            failureCode = rc;
            historySafe = false;
            if (aborted)
                cancelled = true;
            break;
        }

        anyPerformed = true;

        if (!replacingExisting && historySafe)
        {
            if (GetFileIdentity(target, historyItem.DestinationIdentity))
                candidate.Items.push_back(std::move(historyItem));
            else
                historySafe = false;
        }
    }

    RefreshPanel();

    if (anyPerformed)
    {
        ClearRecord(g_UndoRecord);
        ClearRecord(g_RedoRecord);

        if (historySafe && !candidate.Items.empty())
            g_UndoRecord = std::move(candidate);
    }

    if (clip.Move && (anyPerformed || cancelled || failed))
        UpdateClipboardAfterPartialMove(clip.Paths);

    if (cancelled)
    {
        return true;
    }

    if (failed)
    {
        if (failureCode)
            ShowMessage(std::wstring(Msg(MOperationFailedPrefix)) + L" " + std::to_wstring(failureCode), true);
        return true;
    }

    return true;
}

std::array<std::wstring, 6> CurrentHotkeyLetters()
{
    return {
        std::wstring(1, HotkeyLetterOrDefault(g_CopyHotkey, L'C')),
        std::wstring(1, HotkeyLetterOrDefault(g_CutHotkey, L'X')),
        std::wstring(1, HotkeyLetterOrDefault(g_PasteHotkey, L'V')),
        std::wstring(1, HotkeyLetterOrDefault(g_PasteIntoHotkey, L'D')),
        std::wstring(1, HotkeyLetterOrDefault(g_UndoHotkey, L'Z')),
        std::wstring(1, HotkeyLetterOrDefault(g_RedoHotkey, L'E'))
    };
}

void SetDefaultHotkeyLetters(std::array<std::wstring, 6>& letters)
{
    letters = { L"C", L"X", L"V", L"D", L"Z", L"E" };
}

bool NormalizeHotkeyLetters(std::array<std::wstring, 6>& letters)
{
    for (auto& value : letters)
    {
        if (value.size() != 1 || !IsLatinLetter(value[0]))
            return false;
        value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    }
    return true;
}

bool HotkeyLettersAreUnique(const std::array<std::wstring, 6>& letters)
{
    for (size_t i = 0; i < letters.size(); ++i)
    {
        for (size_t j = i + 1; j < letters.size(); ++j)
        {
            if (letters[i] == letters[j])
                return false;
        }
    }
    return true;
}

bool AutoRenameTemplateIsValid(const std::wstring& value)
{
    if (value.empty() || value.find(L"{n}") == std::wstring::npos)
        return false;

    // Validate the template using representative substitutions.  The counter
    // token is mandatory so repeated collisions can always produce a new name.
    std::wstring sample = value;
    auto replaceAll = [](std::wstring& text, const std::wstring& from, const std::wstring& to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::wstring::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(sample, L"{name}", L"file");
    replaceAll(sample, L"{ext}", L".txt");
    replaceAll(sample, L"{n}", L"1");

    // Unknown brace tokens are almost certainly a typo rather than an intended
    // literal filename, so reject them at configuration time.
    if (sample.find(L'{') != std::wstring::npos || sample.find(L'}') != std::wstring::npos)
        return false;

    return IsValidSimpleName(sample);
}

enum ConfigItem : intptr_t
{
    CfgBox = 0,
    CfgCopyLabel, CfgCopyPrefix, CfgCopyEdit,
    CfgCutLabel, CfgCutPrefix, CfgCutEdit,
    CfgPasteLabel, CfgPastePrefix, CfgPasteEdit,
    CfgPasteIntoLabel, CfgPasteIntoPrefix, CfgPasteIntoEdit,
    CfgUndoLabel, CfgUndoPrefix, CfgUndoEdit,
    CfgRedoLabel, CfgRedoPrefix, CfgRedoEdit,
    CfgOperationsSeparator,
    CfgInvalidPrecheck,
    CfgConflictsSeparator,
    CfgConflictAsk,
    CfgConflictSystem,
    CfgConflictAutoRename,
    CfgAutoRenameTemplateLabel,
    CfgAutoRenameTemplateEdit,
    CfgBottomSeparator,
    CfgDefaults,
    CfgOk,
    CfgCancel,
    CfgCount
};

void EnableAutoRenameTemplate(HANDLE dialog, bool enabled)
{
    if (!g_Info.SendDlgMessage)
        return;

    g_Info.SendDlgMessage(
        dialog, DM_ENABLE, CfgAutoRenameTemplateLabel,
        reinterpret_cast<void*>(static_cast<intptr_t>(enabled ? 1 : 0)));
    g_Info.SendDlgMessage(
        dialog, DM_ENABLE, CfgAutoRenameTemplateEdit,
        reinterpret_cast<void*>(static_cast<intptr_t>(enabled ? 1 : 0)));
}

intptr_t WINAPI ConfigDialogProc(HANDLE dialog, intptr_t msg, intptr_t param1, void* param2)
{
    if (msg == DN_INITDIALOG)
    {
        const bool enabled =
            g_Info.SendDlgMessage(dialog, DM_GETCHECK, CfgConflictAutoRename, nullptr) != 0;
        EnableAutoRenameTemplate(dialog, enabled);
    }
    else if (msg == DN_BTNCLICK &&
             (param1 == CfgConflictAsk || param1 == CfgConflictSystem || param1 == CfgConflictAutoRename) &&
             reinterpret_cast<intptr_t>(param2) != 0)
    {
        EnableAutoRenameTemplate(dialog, param1 == CfgConflictAutoRename);
    }

    return g_Info.DefDlgProc ? g_Info.DefDlgProc(dialog, msg, param1, param2) : 0;
}

bool ShowConfigurationDialog(
    std::array<std::wstring, 6>& letters,
    bool& precheckInvalidOperations,
    ConflictMode& conflictMode,
    std::wstring& autoRenameTemplate)
{
    if (!g_Info.DialogInit || !g_Info.DialogRun || !g_Info.DialogFree ||
        !g_Info.SendDlgMessage || !g_Info.DefDlgProc)
        return false;

    constexpr std::array<intptr_t, 6> editIds = {
        CfgCopyEdit, CfgCutEdit, CfgPasteEdit,
        CfgPasteIntoEdit, CfgUndoEdit, CfgRedoEdit
    };

    while (true)
    {
        FarDialogItem items[CfgCount]{};
        // Compact Far-style layout: one outer frame, plain separators only,
        // and no empty spacer rows between logical groups.
        items[CfgBox] = MakeDialogItem(DI_DOUBLEBOX, 3, 1, 66, 17, DIF_NONE, Msg(MPluginName));

        const MessageId labels[6] = {
            MConfigCopy, MConfigCut, MConfigPaste,
            MConfigPasteInto, MConfigUndo, MConfigRedo
        };

        for (size_t i = 0; i < 6; ++i)
        {
            const intptr_t y = 2 + static_cast<intptr_t>(i);
            const intptr_t base = 1 + static_cast<intptr_t>(i) * 3;
            items[base] = MakeDialogItem(DI_TEXT, 7, y, 35, y, DIF_NONE, Msg(labels[i]));
            items[base + 1] = MakeDialogItem(DI_TEXT, 39, y, 49, y, DIF_NONE, Msg(MConfigPrefix));
            items[base + 2] = MakeDialogItem(
                DI_FIXEDIT,
                51,
                y,
                51,
                y,
                (i == 0 ? DIF_FOCUS : DIF_NONE) | DIF_SELECTONENTRY | DIF_NOAUTOCOMPLETE,
                letters[i].c_str(),
                1);
        }

        items[CfgOperationsSeparator] = MakeDialogItem(
            DI_TEXT, 5, 8, 64, 8, DIF_SEPARATOR, L"");
        items[CfgInvalidPrecheck] = MakeDialogItem(
            DI_CHECKBOX, 7, 9, 0, 9, DIF_NONE, Msg(MConfigPrecheckInvalid));
        items[CfgInvalidPrecheck].Selected = precheckInvalidOperations ? 1 : 0;

        items[CfgConflictsSeparator] = MakeDialogItem(
            DI_TEXT, 5, 10, 64, 10, DIF_SEPARATOR, L"");
        items[CfgConflictAsk] = MakeDialogItem(
            DI_RADIOBUTTON, 7, 11, 0, 11, DIF_GROUP, Msg(MConfigConflictAsk));
        items[CfgConflictAsk].Selected = conflictMode == ConflictMode::Ask ? 1 : 0;
        items[CfgConflictSystem] = MakeDialogItem(
            DI_RADIOBUTTON, 7, 12, 0, 12, DIF_NONE, Msg(MConfigConflictSystem));
        items[CfgConflictSystem].Selected = conflictMode == ConflictMode::System ? 1 : 0;
        items[CfgConflictAutoRename] = MakeDialogItem(
            DI_RADIOBUTTON, 7, 13, 0, 13, DIF_NONE, Msg(MConfigConflictAutoRename));
        items[CfgConflictAutoRename].Selected = conflictMode == ConflictMode::AutoRename ? 1 : 0;
        const FARDIALOGITEMFLAGS templateDisabled =
            conflictMode == ConflictMode::AutoRename ? DIF_NONE : DIF_DISABLE;
        items[CfgAutoRenameTemplateLabel] = MakeDialogItem(
            DI_TEXT, 10, 14, 0, 14, templateDisabled, Msg(MConfigAutoRenameTemplate));
        items[CfgAutoRenameTemplateEdit] = MakeDialogItem(
            DI_EDIT, 24, 14, 61, 14,
            DIF_SELECTONENTRY | DIF_NOAUTOCOMPLETE | templateDisabled,
            autoRenameTemplate.c_str(), 128);

        items[CfgBottomSeparator] = MakeDialogItem(DI_TEXT, 5, 15, 64, 15, DIF_SEPARATOR, L"");
        items[CfgDefaults] = MakeDialogItem(DI_BUTTON, 10, 16, 0, 16, DIF_NONE, Msg(MConfigDefaults));
        items[CfgOk] = MakeDialogItem(DI_BUTTON, 40, 16, 0, 16, DIF_DEFAULTBUTTON, Msg(MConfigOk));
        items[CfgCancel] = MakeDialogItem(DI_BUTTON, 51, 16, 0, 16, DIF_NONE, Msg(MConfigCancel));

        HANDLE dialog = g_Info.DialogInit(
            &PluginGuid,
            &ConfigDialogGuid,
            -1,
            -1,
            70,
            19,
            L"Config",
            items,
            CfgCount,
            0,
            FDLG_NONE,
            ConfigDialogProc,
            nullptr);

        if (!dialog || dialog == INVALID_HANDLE_VALUE)
            return false;

        const intptr_t result = g_Info.DialogRun(dialog);

        if (result == CfgOk)
        {
            for (size_t i = 0; i < editIds.size(); ++i)
            {
                const auto* text = reinterpret_cast<const wchar_t*>(
                    g_Info.SendDlgMessage(dialog, DM_GETCONSTTEXTPTR, editIds[i], nullptr));
                letters[i] = text ? text : L"";
            }

            precheckInvalidOperations =
                g_Info.SendDlgMessage(dialog, DM_GETCHECK, CfgInvalidPrecheck, nullptr) != 0;

            if (g_Info.SendDlgMessage(dialog, DM_GETCHECK, CfgConflictSystem, nullptr) != 0)
                conflictMode = ConflictMode::System;
            else if (g_Info.SendDlgMessage(dialog, DM_GETCHECK, CfgConflictAutoRename, nullptr) != 0)
                conflictMode = ConflictMode::AutoRename;
            else
                conflictMode = ConflictMode::Ask;

            const auto* templateText = reinterpret_cast<const wchar_t*>(
                g_Info.SendDlgMessage(dialog, DM_GETCONSTTEXTPTR, CfgAutoRenameTemplateEdit, nullptr));
            autoRenameTemplate = templateText ? templateText : L"";
        }

        g_Info.DialogFree(dialog);

        if (result == CfgDefaults)
        {
            SetDefaultHotkeyLetters(letters);
            precheckInvalidOperations = true;
            conflictMode = ConflictMode::Ask;
            autoRenameTemplate = DefaultAutoRenameTemplate;
            continue;
        }

        if (result != CfgOk)
            return false;

        if (!NormalizeHotkeyLetters(letters))
        {
            ShowMessage(Msg(MConfigLetterError), true);
            continue;
        }

        if (!HotkeyLettersAreUnique(letters))
        {
            ShowMessage(Msg(MHotkeysMustDiffer), true);
            continue;
        }

        if (!AutoRenameTemplateIsValid(autoRenameTemplate))
        {
            ShowMessage(Msg(MConfigAutoRenameTemplateError), true);
            continue;
        }

        return true;
    }
}

} // namespace

extern "C" __declspec(dllexport) void WINAPI GetGlobalInfoW(GlobalInfo* info)
{
    if (!info)
        return;

    info->StructSize = sizeof(GlobalInfo);
    info->MinFarVersion = { 3, 0, 0, 4326, VS_RELEASE };
    info->Version = { 1, 0, 0, 0, VS_RELEASE };
    info->Guid = PluginGuid;
    info->Title = L"FarFileClipboard";
    info->Description = L"Windows file clipboard integration for Far Manager";
    info->Author = L"Qwaduda";
    info->Instance = nullptr;
}

extern "C" __declspec(dllexport) void WINAPI SetStartupInfoW(const PluginStartupInfo* info)
{
    if (!info)
        return;

    g_Info = *info;
    LoadSettings();
}

extern "C" __declspec(dllexport) void WINAPI GetPluginInfoW(PluginInfo* info)
{
    if (!info)
        return;

    static const wchar_t* menuStrings[1]{};
    menuStrings[0] = Msg(MPluginName);
    static const UUID menuGuids[] = { MainMenuGuid };

    static const wchar_t* configStrings[1]{};
    configStrings[0] = Msg(MPluginName);
    static const UUID configGuids[] = { PluginConfigGuid };

    info->StructSize = sizeof(PluginInfo);
    info->Flags = PF_PRELOAD;
    info->DiskMenu = {};
    info->PluginMenu = { menuGuids, menuStrings, 1 };
    info->PluginConfig = { configGuids, configStrings, 1 };
    info->CommandPrefix = nullptr;
    info->Instance = nullptr;
}

extern "C" __declspec(dllexport) HANDLE WINAPI OpenW(const OpenInfo* info)
{
    if (info && info->Guid && IsEqualGUID(*info->Guid, MainMenuGuid))
        ShowOperationsMenu();

    return nullptr;
}

extern "C" __declspec(dllexport) intptr_t WINAPI ConfigureW(const ConfigureInfo* info)
{
    if (!info || !info->Guid || !IsEqualGUID(*info->Guid, PluginConfigGuid))
        return 0;

    auto letters = CurrentHotkeyLetters();
    bool precheckInvalidOperations = g_PrecheckInvalidOperations;
    ConflictMode conflictMode = g_ConflictMode;
    std::wstring autoRenameTemplate = g_AutoRenameTemplate;
    if (!ShowConfigurationDialog(letters, precheckInvalidOperations, conflictMode, autoRenameTemplate))
        return 0;

    std::array<std::wstring, 6> hotkeys;
    std::array<HotkeySpec, 6> specs{};

    for (size_t i = 0; i < letters.size(); ++i)
    {
        hotkeys[i] = MakeInternalHotkey(letters[i][0]);
        if (!ParseHotkey(hotkeys[i], specs[i]))
        {
            ShowMessage(Msg(MConfigLetterError), true);
            return 0;
        }
    }

    for (size_t i = 0; i < specs.size(); ++i)
    {
        for (size_t j = i + 1; j < specs.size(); ++j)
        {
            if (SameHotkey(specs[i], specs[j]))
            {
                ShowMessage(Msg(MHotkeysMustDiffer), true);
                return 0;
            }
        }
    }

    if (!SaveSettings(
            hotkeys[0], hotkeys[1], hotkeys[2], hotkeys[3], hotkeys[4], hotkeys[5],
            precheckInvalidOperations, conflictMode, autoRenameTemplate))
    {
        ShowMessage(Msg(MSaveSettingsFailed), true);
        return 0;
    }

    g_CopyHotkey = hotkeys[0];
    g_CutHotkey = hotkeys[1];
    g_PasteHotkey = hotkeys[2];
    g_PasteIntoHotkey = hotkeys[3];
    g_UndoHotkey = hotkeys[4];
    g_RedoHotkey = hotkeys[5];
    g_PrecheckInvalidOperations = precheckInvalidOperations;
    g_ConflictMode = conflictMode;
    g_AutoRenameTemplate = autoRenameTemplate;
    g_CopyHotkeySpec = specs[0];
    g_CutHotkeySpec = specs[1];
    g_PasteHotkeySpec = specs[2];
    g_PasteIntoHotkeySpec = specs[3];
    g_UndoHotkeySpec = specs[4];
    g_RedoHotkeySpec = specs[5];
    return 1;
}

extern "C" __declspec(dllexport) intptr_t WINAPI ProcessConsoleInputW(ProcessConsoleInputInfo* info)
{
    if (!info || info->Rec.EventType != KEY_EVENT)
        return 0;

    const KEY_EVENT_RECORD& key = info->Rec.Event.KeyEvent;
    if (!key.bKeyDown || !IsPanelsWindow())
        return 0;

    if (HotkeyMatches(key, g_CopyHotkeySpec))
    {
        CopyPanelFilesToClipboard();
        return 1;
    }

    if (HotkeyMatches(key, g_CutHotkeySpec))
    {
        CutPanelFilesToClipboard();
        return 1;
    }

    if (HotkeyMatches(key, g_PasteHotkeySpec))
    {
        PasteClipboardFiles();
        return 1;
    }

    if (HotkeyMatches(key, g_PasteIntoHotkeySpec))
    {
        PasteClipboardFilesIntoFolderUnderCursor();
        return 1;
    }

    if (HotkeyMatches(key, g_UndoHotkeySpec))
    {
        UndoLastOperation();
        return 1;
    }

    if (HotkeyMatches(key, g_RedoHotkeySpec))
    {
        RedoLastOperation();
        return 1;
    }

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
