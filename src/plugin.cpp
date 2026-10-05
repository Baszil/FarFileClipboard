#include "far3sdk_min.hpp"
#include "messages.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <shobjidl.h>
#include <propkeydef.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cwctype>
#include <cwchar>
#include <climits>
#include <cstring>
#include <atomic>
#include <new>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
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

constexpr UUID CancelConfirmGuid =
{ 0xa825bc30, 0xe913, 0x4e0b, { 0xa7, 0x4f, 0xdf, 0x08, 0x4f, 0xdd, 0x4e, 0x53 } };

constexpr UUID ProgressDialogGuid =
{ 0x55d4329a, 0xd12a, 0x4a43, { 0x88, 0x91, 0x54, 0x65, 0x4f, 0x8b, 0x94, 0x27 } };

// System.Size (PKEY_Size): define it locally instead of pulling in the
// propkey library symbol, which would otherwise add an easy-to-miss linker
// dependency just for progress reporting of virtual Shell items.
constexpr PROPERTYKEY ProgressSizePropertyKey =
{ { 0xb725f130, 0x47ef, 0x101a, { 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac } }, 12 };

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
constexpr wchar_t SystemProgressUiSetting[] = L"SystemProgressUi";
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
bool g_SystemProgressUi = false;
std::atomic<bool> g_BackgroundOperationRunning{ false };

// The default progress UI belongs to Far itself, not to Explorer.  Worker
// threads never touch Dialog API directly; they only queue ACTL_SYNCHRO and
// the actual UI update is performed in Far's main thread.
HANDLE g_ProgressDialog = nullptr;
std::atomic<bool> g_NativeProgressActive{ false };
std::atomic<bool> g_ProgressDialogClosing{ false };
std::atomic<bool> g_ProgressDialogRunning{ false };
std::atomic<bool> g_ProgressCancelRequested{ false };
// While the Far-style confirmation is on screen, freeze the worker at the
// next progress / item boundary.  A negative answer resumes the same transfer;
// a positive answer arms the real Win32 cancellation path.
std::atomic<bool> g_ProgressCancelPromptActive{ false };
std::mutex g_ProgressCancelPromptMutex;
std::condition_variable g_ProgressCancelPromptCv;
// CopyFileEx can also observe an LPBOOL cancellation flag independently of
// our progress callback.  Keep both mechanisms: the raw Far input path sets
// this flag immediately, while the callback also returns PROGRESS_CANCEL.
volatile LONG g_ProgressWin32CancelFlag = FALSE;
std::atomic<unsigned int> g_ProgressTotal{ 0 };
std::atomic<unsigned int> g_ProgressDone{ 0 };
std::atomic<bool> g_ProgressShowTotal{ false };
std::atomic<unsigned long long> g_ProgressCurrentTotalBytes{ 0 };
std::atomic<unsigned long long> g_ProgressCurrentDoneBytes{ 0 };
std::atomic<unsigned long long> g_ProgressOverallTotalBytes{ 0 };
std::atomic<unsigned long long> g_ProgressOverallDoneBytes{ 0 };
std::atomic<bool> g_ProgressCurrentTargetObservedChange{ false };
std::atomic<bool> g_ProgressSyncQueued{ false };
std::atomic<unsigned long long> g_ProgressRedrawGeneration{ 0 };
std::atomic<unsigned long long> g_ProgressLastSyncTick{ 0 };
std::atomic<bool> g_ProgressForceRedrawPending{ false };
constexpr unsigned long long ProgressRedrawIntervalMs = 50;
std::mutex g_ProgressNameMutex;
std::wstring g_ProgressName;
std::wstring g_ProgressCurrentTargetPath;
unsigned long long g_ProgressCurrentTargetInitialSize = 0;
FILETIME g_ProgressCurrentTargetInitialWriteTime{};
bool g_ProgressCurrentTargetInitiallyExisted = false;
std::wstring g_ProgressOverallBarText;
std::wstring g_ProgressCurrentBarText;
std::wstring g_ProgressDetailText;
char g_ProgressSyncToken = 0;

// ---------------------------------------------------------------------------
// Optional cancellation diagnostics.
//
// Release builds compile this out completely. It can be enabled explicitly
// with -DFFC_CANCEL_DIAGNOSTICS=ON when reproducing input/cancellation issues.
// ---------------------------------------------------------------------------
#ifdef FFC_CANCEL_DIAGNOSTICS
std::mutex g_DebugLogMutex;
std::atomic<unsigned long long> g_DebugLogSequence{ 0 };

const std::wstring& DebugLogPath()
{
    static const std::wstring path = []()
    {
        wchar_t tempPath[MAX_PATH + 2]{};
        constexpr size_t tempPathCount = sizeof(tempPath) / sizeof(tempPath[0]);
        const DWORD length = GetTempPathW(static_cast<DWORD>(tempPathCount), tempPath);
        if (length > 0 && length < tempPathCount)
            return std::wstring(tempPath) + L"FarFileClipboard-cancel-debug.log";
        return std::wstring(L"FarFileClipboard-cancel-debug.log");
    }();
    return path;
}

void WriteDebugLogBytes(const char* data, DWORD size)
{
    HANDLE file = CreateFileW(
        DebugLogPath().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, data, size, &written, nullptr);
    // Do not FlushFileBuffers() for every diagnostic record.  This logger runs
    // on Far's input thread too; forcing a physical flush per key/mouse event
    // measurably harms the very responsiveness we are trying to diagnose.
    CloseHandle(file);
}

void DebugLog(const wchar_t* format, ...)
{
    wchar_t message[2048]{};
    constexpr size_t messageCount = sizeof(message) / sizeof(message[0]);
    va_list args;
    va_start(args, format);
    const int formatted = std::vswprintf(message, messageCount, format, args);
    va_end(args);
    if (formatted < 0)
        return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const unsigned long long sequence = g_DebugLogSequence.fetch_add(1) + 1;

    wchar_t line[2600]{};
    constexpr size_t lineCount = sizeof(line) / sizeof(line[0]);
    const int lineLength = swprintf_s(
        line,
        lineCount,
        L"%06llu %02u:%02u:%02u.%03u pid=%lu tid=%lu | %ls\r\n",
        sequence,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()),
        message);
    if (lineLength <= 0)
        return;

    const int utf8Length = WideCharToMultiByte(
        CP_UTF8, 0, line, lineLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return;

    std::string utf8(static_cast<size_t>(utf8Length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, line, lineLength, utf8.data(), utf8Length, nullptr, nullptr);

    std::lock_guard<std::mutex> lock(g_DebugLogMutex);
    WriteDebugLogBytes(utf8.data(), static_cast<DWORD>(utf8.size()));
}

void ResetDebugLog()
{
    std::lock_guard<std::mutex> lock(g_DebugLogMutex);
    HANDLE file = CreateFileW(
        DebugLogPath().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        static constexpr unsigned char Utf8Bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(file, Utf8Bom, sizeof(Utf8Bom), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    g_DebugLogSequence.store(0);
}

const wchar_t* DebugDialogMessageName(intptr_t msg)
{
    switch (msg)
    {
    case DN_BTNCLICK: return L"DN_BTNCLICK";
    case DN_INITDIALOG: return L"DN_INITDIALOG";
    case DN_INPUT: return L"DN_INPUT";
    case DN_CONTROLINPUT: return L"DN_CONTROLINPUT";
    case DN_CLOSE: return L"DN_CLOSE";
    default: return L"DN_OTHER";
    }
}

void DebugLogInputRecord(const wchar_t* source, const INPUT_RECORD& input)
{
    if (input.EventType == KEY_EVENT)
    {
        const auto& key = input.Event.KeyEvent;
        DebugLog(
            L"%ls KEY down=%d repeat=%u vk=0x%04X scan=0x%04X char=U+%04X ctrl=0x%08lX",
            source,
            key.bKeyDown ? 1 : 0,
            static_cast<unsigned>(key.wRepeatCount),
            static_cast<unsigned>(key.wVirtualKeyCode),
            static_cast<unsigned>(key.wVirtualScanCode),
            static_cast<unsigned>(key.uChar.UnicodeChar),
            static_cast<unsigned long>(key.dwControlKeyState));
        return;
    }

    if (input.EventType == MOUSE_EVENT)
    {
        const auto& mouse = input.Event.MouseEvent;
        // Do not flood the log with plain mouse-move notifications.  Button,
        // wheel and double-click events are what matter for cancellation.
        if (mouse.dwButtonState != 0 || mouse.dwEventFlags != MOUSE_MOVED)
        {
            DebugLog(
                L"%ls MOUSE x=%d y=%d buttons=0x%08lX ctrl=0x%08lX flags=0x%08lX",
                source,
                static_cast<int>(mouse.dwMousePosition.X),
                static_cast<int>(mouse.dwMousePosition.Y),
                static_cast<unsigned long>(mouse.dwButtonState),
                static_cast<unsigned long>(mouse.dwControlKeyState),
                static_cast<unsigned long>(mouse.dwEventFlags));
        }
        return;
    }

    DebugLog(L"%ls INPUT eventType=0x%04X", source, static_cast<unsigned>(input.EventType));
}
#else
#define DebugLog(...) ((void)0)
#define DebugLogInputRecord(...) ((void)0)
inline void ResetDebugLog() {}
#endif

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

enum class BackgroundJobType
{
    FileSystemPaste,
    ShellPaste
};

struct BackgroundJobBase
{
    BackgroundJobType Type = BackgroundJobType::FileSystemPaste;
};

struct BackgroundPasteItem
{
    std::wstring Source;
    std::wstring Target;
};

enum class BackgroundPasteKind
{
    ToDirectory,
    ExactTargets
};

struct BackgroundPasteJob : BackgroundJobBase
{
    BackgroundPasteJob() { Type = BackgroundJobType::FileSystemPaste; }
    BackgroundPasteKind Kind = BackgroundPasteKind::ExactTargets;
    bool Move = false;
    bool AllowSystemConflictUi = false;
    bool ShowSystemProgressUi = false;
    std::wstring DestinationDirectory;
    std::vector<BackgroundPasteItem> Items;
    std::vector<std::wstring> OriginalSources;
    DWORD ClipboardSequenceAtStart = 0;
    FileActionRecord Candidate{};
    bool CandidateSafe = false;

    bool AnyPerformed = false;
    bool Failed = false;
    bool Aborted = false;
    int ErrorCode = 0;
};

struct BackgroundShellPastePlanItem
{
    DWORD Index = 0;
    std::wstring NewName;
};

struct BackgroundShellPasteJob : BackgroundJobBase
{
    BackgroundShellPasteJob() { Type = BackgroundJobType::ShellPaste; }
    ~BackgroundShellPasteJob()
    {
        if (MarshaledDataObject)
            MarshaledDataObject->Release();
    }
    IStream* MarshaledDataObject = nullptr;
    std::wstring DestinationDirectory;
    std::vector<BackgroundShellPastePlanItem> Plan;
    bool UsePlan = false;
    bool Move = false;
    bool AllowSystemConflictUi = false;
    bool ShowSystemProgressUi = false;
    DWORD ClipboardSequenceAtStart = 0;
    HRESULT Result = S_OK;
    bool Aborted = false;
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

void WaitWhileProgressCancelPromptActive()
{
    if (!g_ProgressCancelPromptActive.load())
        return;

    std::unique_lock<std::mutex> lock(g_ProgressCancelPromptMutex);
    g_ProgressCancelPromptCv.wait(lock, []
    {
        return !g_ProgressCancelPromptActive.load();
    });
}

bool ShowProgressCancelConfirmation()
{
    if (!g_Info.Message)
        return true;

    const std::wstring body =
        std::wstring(Msg(MWarningTitle)) + L"\n" +
        Msg(MProgressCancelQuestion1) + L"\n" +
        Msg(MProgressCancelQuestion2);

    const intptr_t result = g_Info.Message(
        &PluginGuid,
        &CancelConfirmGuid,
        FMSG_WARNING | FMSG_ALLINONE | FMSG_MB_YESNO,
        nullptr,
        reinterpret_cast<const wchar_t* const*>(body.c_str()),
        0,
        0);
    DebugLog(L"cancel confirmation result=%lld (0=yes)", static_cast<long long>(result));
    return result == 0;
}


enum NativeProgressItem : intptr_t
{
    ProgressBox = 0,
    ProgressCurrentLabel,
    ProgressDetail,
    ProgressCurrentBar,
    ProgressTotalSeparator,
    ProgressTotalBar,
    ProgressSeparator,
    ProgressCancel,
    ProgressCount
};

intptr_t WINAPI ProgressDialogProc(HANDLE dialog, intptr_t msg, intptr_t param1, void* param2)
{
    auto commitCancel = [dialog]()
    {
        const bool alreadyRequested = g_ProgressCancelRequested.exchange(true);
        DebugLog(
            L"commitCancel dialog=%p alreadyRequested=%d win32Flag(before)=%ld",
            dialog,
            alreadyRequested ? 1 : 0,
            static_cast<long>(InterlockedCompareExchange(&g_ProgressWin32CancelFlag, FALSE, FALSE)));
        if (alreadyRequested)
            return;

        InterlockedExchange(&g_ProgressWin32CancelFlag, TRUE);
        DebugLog(
            L"commitCancel armed: cancelRequested=%d win32Flag(after)=%ld",
            g_ProgressCancelRequested.load() ? 1 : 0,
            static_cast<long>(InterlockedCompareExchange(&g_ProgressWin32CancelFlag, FALSE, FALSE)));

        if (g_Info.SendDlgMessage)
        {
            g_Info.SendDlgMessage(
                dialog,
                DM_SETTEXTPTR,
                ProgressDetail,
                const_cast<wchar_t*>(Msg(MProgressCancelling)));
            g_Info.SendDlgMessage(dialog, DM_ENABLE, ProgressCancel, nullptr);
            g_Info.SendDlgMessage(dialog, DM_REDRAW, 0, nullptr);
        }
    };

    auto confirmCancel = [dialog, &commitCancel]()
    {
        if (g_ProgressCancelRequested.load())
            return;

        bool expected = false;
        if (!g_ProgressCancelPromptActive.compare_exchange_strong(expected, true))
            return;

        DebugLog(L"cancel confirmation OPEN dialog=%p", dialog);
        const bool confirmed = ShowProgressCancelConfirmation();
        DebugLog(L"cancel confirmation CLOSE confirmed=%d", confirmed ? 1 : 0);

        if (confirmed)
            commitCancel();

        g_ProgressCancelPromptActive.store(false);
        g_ProgressCancelPromptCv.notify_all();

        if (!confirmed && g_Info.SendDlgMessage)
        {
            g_Info.SendDlgMessage(dialog, DM_SETFOCUS, ProgressCancel, nullptr);
            g_Info.SendDlgMessage(dialog, DM_REDRAW, 0, nullptr);
        }
    };

    auto keyMeansCancel = [](const INPUT_RECORD& input)
    {
        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown)
            return false;

        const WORD key = input.Event.KeyEvent.wVirtualKeyCode;
        return key == VK_ESCAPE || key == VK_F10 || key == VK_RETURN || key == VK_SPACE;
    };

    auto mouseHitsCancel = [dialog](const INPUT_RECORD& input)
    {
        if (input.EventType != MOUSE_EVENT || !g_Info.SendDlgMessage)
            return false;

        const auto& mouse = input.Event.MouseEvent;
        if ((mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) == 0)
            return false;

        SMALL_RECT dialogRect{};
        SMALL_RECT buttonRect{};
        const intptr_t dlgRectResult = g_Info.SendDlgMessage(dialog, DM_GETDLGRECT, 0, &dialogRect);
        const intptr_t buttonRectResult = g_Info.SendDlgMessage(
            dialog, DM_GETITEMPOSITION, ProgressCancel, &buttonRect);
        if (!dlgRectResult || !buttonRectResult)
        {
            DebugLog(
                L"mouseHitsCancel rect query failed dlgResult=%lld buttonResult=%lld",
                static_cast<long long>(dlgRectResult),
                static_cast<long long>(buttonRectResult));
            return false;
        }

        const SHORT left = static_cast<SHORT>(dialogRect.Left + buttonRect.Left);
        const SHORT right = static_cast<SHORT>(dialogRect.Left + buttonRect.Right);
        const SHORT top = static_cast<SHORT>(dialogRect.Top + buttonRect.Top);
        const SHORT bottom = static_cast<SHORT>(dialogRect.Top + buttonRect.Bottom);
        const COORD pos = mouse.dwMousePosition;
        const bool hit = pos.X >= left && pos.X <= right && pos.Y >= top && pos.Y <= bottom;
        DebugLog(
            L"mouseHitsCancel pos=(%d,%d) dlg=(%d,%d,%d,%d) buttonRel=(%d,%d,%d,%d) buttonAbs=(%d,%d,%d,%d) hit=%d",
            static_cast<int>(pos.X), static_cast<int>(pos.Y),
            static_cast<int>(dialogRect.Left), static_cast<int>(dialogRect.Top),
            static_cast<int>(dialogRect.Right), static_cast<int>(dialogRect.Bottom),
            static_cast<int>(buttonRect.Left), static_cast<int>(buttonRect.Top),
            static_cast<int>(buttonRect.Right), static_cast<int>(buttonRect.Bottom),
            static_cast<int>(left), static_cast<int>(top),
            static_cast<int>(right), static_cast<int>(bottom),
            hit ? 1 : 0);
        return hit;
    };

    if (msg == DN_INITDIALOG)
    {
        DebugLog(
            L"ProgressDialogProc %ls dialog=%p param1=%lld param2=%p",
            DebugDialogMessageName(msg), dialog,
            static_cast<long long>(param1), param2);
        // DN_INPUT arrives *before* Far's own input handling.  This matters:
        // swallowing Esc/F10 here prevents the key from leaking to the panel.
        // We deliberately show our own Far warning / Yes-No confirmation so
        // plugin copy behaves like ordinary Far F5/F6 copy.  DN_CONTROLINPUT
        // is kept below only as a fallback.
        if (g_Info.SendDlgMessage)
        {
            const intptr_t inputNotifyResult = g_Info.SendDlgMessage(
                dialog, DM_SETINPUTNOTIFY, 1, nullptr);
            const intptr_t focusResult = g_Info.SendDlgMessage(
                dialog, DM_SETFOCUS, ProgressCancel, nullptr);
            const intptr_t actualFocus = g_Info.SendDlgMessage(dialog, DM_GETFOCUS, 0, nullptr);
            DebugLog(
                L"DN_INITDIALOG inputNotifyResult=%lld focusResult=%lld actualFocus=%lld expectedFocus=%lld",
                static_cast<long long>(inputNotifyResult),
                static_cast<long long>(focusResult),
                static_cast<long long>(actualFocus),
                static_cast<long long>(ProgressCancel));
        }
        const intptr_t defResult = g_Info.DefDlgProc
            ? g_Info.DefDlgProc(dialog, msg, param1, param2)
            : 1;
        DebugLog(L"DN_INITDIALOG DefDlgProc -> %lld", static_cast<long long>(defResult));
        return defResult;
    }

    if (msg == DN_INPUT && param2)
    {
        const auto& input = *static_cast<const INPUT_RECORD*>(param2);
        if (input.EventType == 0)
            return 1;
        DebugLogInputRecord(L"DN_INPUT", input);
        const bool keyCancel = keyMeansCancel(input);
        const bool mouseCancel = mouseHitsCancel(input);
        DebugLog(L"DN_INPUT cancelMatch key=%d mouse=%d", keyCancel ? 1 : 0, mouseCancel ? 1 : 0);
        if (keyCancel || mouseCancel)
        {
            confirmCancel();
            // DN_INPUT uses the inverse convention from DN_CONTROLINPUT:
            // FALSE means "handled by plugin, do not let Far process it".
            return 0;
        }
        return 1;
    }

    if (msg == DN_BTNCLICK && param1 == ProgressCancel)
    {
        DebugLog(
            L"ProgressDialogProc DN_BTNCLICK CANCEL dialog=%p param1=%lld param2=%p",
            dialog, static_cast<long long>(param1), param2);
        confirmCancel();
        return 1;
    }

    if (msg == DN_BTNCLICK)
    {
        DebugLog(
            L"ProgressDialogProc DN_BTNCLICK OTHER dialog=%p param1=%lld param2=%p",
            dialog, static_cast<long long>(param1), param2);
    }

    if (msg == DN_CONTROLINPUT && param2)
    {
        const auto& input = *static_cast<const INPUT_RECORD*>(param2);
        if (input.EventType == 0)
            return g_Info.DefDlgProc ? g_Info.DefDlgProc(dialog, msg, param1, param2) : 0;
        DebugLogInputRecord(L"DN_CONTROLINPUT", input);
        const bool keyCancel = keyMeansCancel(input);
        const bool mouseCancel = mouseHitsCancel(input);
        DebugLog(
            L"DN_CONTROLINPUT cancelMatch key=%d mouse=%d",
            keyCancel ? 1 : 0,
            mouseCancel ? 1 : 0);
        if (keyCancel || mouseCancel)
        {
            confirmCancel();
            // DN_CONTROLINPUT: TRUE means the plugin consumed the event.
            return 1;
        }
    }

    if (msg == DN_CLOSE)
    {
        DebugLog(
            L"ProgressDialogProc DN_CLOSE dialog=%p param1=%lld closing=%d cancelRequested=%d",
            dialog,
            static_cast<long long>(param1),
            g_ProgressDialogClosing.load() ? 1 : 0,
            g_ProgressCancelRequested.load() ? 1 : 0);
        if (g_ProgressDialogClosing.load())
        {
            DebugLog(L"DN_CLOSE accepted: internal worker-completion close");
            return 1;
        }

        // User close attempts ask for confirmation, exactly like ordinary Far
        // copy.  Keep the progress dialog alive unless the worker itself ends.
        confirmCancel();
        DebugLog(L"DN_CLOSE rejected after converting it to cancel request");
        return 0;
    }

    return g_Info.DefDlgProc ? g_Info.DefDlgProc(dialog, msg, param1, param2) : 0;
}

bool GetProgressPathInfo(
    const std::wstring& path,
    unsigned long long& size,
    FILETIME& writeTime,
    bool& directory)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;

    directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ULARGE_INTEGER value{};
    value.HighPart = data.nFileSizeHigh;
    value.LowPart = data.nFileSizeLow;
    size = directory ? 0ULL : value.QuadPart;
    writeTime = data.ftLastWriteTime;
    return true;
}

std::wstring JoinProgressPath(const std::wstring& directory, const std::wstring& name)
{
    if (directory.empty())
        return name;
    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + name;
    return directory + L"\\" + name;
}

std::wstring ProgressBaseName(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool GetShellItemProgressPath(IShellItem* item, std::wstring& path)
{
    path.clear();
    if (!item)
        return false;

    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    if (FAILED(hr) || !raw || !*raw)
    {
        if (raw)
            CoTaskMemFree(raw);
        return false;
    }

    path = raw;
    CoTaskMemFree(raw);
    return true;
}

bool GetShellItemProgressSize(IShellItem* item, unsigned long long& size)
{
    size = 0;
    if (!item)
        return false;

    IShellItem2* item2 = nullptr;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2))) || !item2)
        return false;

    ULONGLONG value = 0;
    const HRESULT hr = item2->GetUInt64(ProgressSizePropertyKey, &value);
    item2->Release();
    if (FAILED(hr))
        return false;

    size = static_cast<unsigned long long>(value);
    return true;
}

void SetCurrentProgressItem(
    IShellItem* item,
    IShellItem* destinationFolder,
    LPCWSTR requestedName)
{
    std::wstring sourcePath;
    std::wstring destinationPath;
    std::wstring displayName;
    unsigned long long sourceSize = 0;
    FILETIME sourceWrite{};
    bool sourceDirectory = false;

    if (GetShellItemProgressPath(item, sourcePath))
    {
        displayName = ProgressBaseName(sourcePath);
        GetProgressPathInfo(sourcePath, sourceSize, sourceWrite, sourceDirectory);
    }
    else if (item)
    {
        PWSTR rawName = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &rawName)) && rawName)
        {
            displayName = rawName;
            CoTaskMemFree(rawName);
        }
    }

    if (item && sourcePath.empty())
    {
        SFGAOF attributes = 0;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)))
            sourceDirectory = (attributes & SFGAO_FOLDER) != 0;

        // Explorer ZIP and other virtual Shell items have no filesystem
        // source path, but they often expose PKEY_Size.  Using it lets the
        // current-file indicator remain genuinely per-file instead of merely
        // mirroring the overall IFileOperation work estimate.
        if (!sourceDirectory)
            GetShellItemProgressSize(item, sourceSize);
    }

    std::wstring destinationDirectory;
    if (GetShellItemProgressPath(destinationFolder, destinationDirectory))
    {
        std::wstring targetName = requestedName && *requestedName
            ? std::wstring(requestedName)
            : displayName;
        if (!targetName.empty())
            destinationPath = JoinProgressPath(destinationDirectory, targetName);
    }

    unsigned long long initialSize = 0;
    FILETIME initialWrite{};
    bool initialDirectory = false;
    const bool targetExisted = !destinationPath.empty() &&
        GetProgressPathInfo(destinationPath, initialSize, initialWrite, initialDirectory) &&
        !initialDirectory;

    {
        std::lock_guard<std::mutex> lock(g_ProgressNameMutex);
        g_ProgressName = displayName;
        g_ProgressCurrentTargetPath = destinationPath;
        g_ProgressCurrentTargetInitialSize = initialSize;
        g_ProgressCurrentTargetInitialWriteTime = initialWrite;
        g_ProgressCurrentTargetInitiallyExisted = targetExisted;
    }

    g_ProgressCurrentTargetObservedChange.store(!targetExisted);
    g_ProgressCurrentTotalBytes.store(sourceDirectory ? 0ULL : sourceSize);
    g_ProgressCurrentDoneBytes.store(0ULL);
}

void RefreshCurrentProgressFromTarget()
{
    const unsigned long long total = g_ProgressCurrentTotalBytes.load();
    if (!total)
        return;

    std::wstring targetPath;
    unsigned long long initialSize = 0;
    FILETIME initialWrite{};
    bool initiallyExisted = false;
    {
        std::lock_guard<std::mutex> lock(g_ProgressNameMutex);
        targetPath = g_ProgressCurrentTargetPath;
        initialSize = g_ProgressCurrentTargetInitialSize;
        initialWrite = g_ProgressCurrentTargetInitialWriteTime;
        initiallyExisted = g_ProgressCurrentTargetInitiallyExisted;
    }

    if (targetPath.empty())
        return;

    unsigned long long currentSize = 0;
    FILETIME currentWrite{};
    bool directory = false;
    if (!GetProgressPathInfo(targetPath, currentSize, currentWrite, directory) || directory)
        return;

    bool changed = g_ProgressCurrentTargetObservedChange.load();
    if (!changed && initiallyExisted)
    {
        changed = currentSize != initialSize ||
            CompareFileTime(&currentWrite, &initialWrite) != 0;
        if (changed)
            g_ProgressCurrentTargetObservedChange.store(true);
    }

    if (!initiallyExisted || changed)
        g_ProgressCurrentDoneBytes.store(std::min(currentSize, total));
}

void CompleteCurrentProgressItem()
{
    const unsigned long long total = g_ProgressCurrentTotalBytes.load();
    if (total)
        g_ProgressCurrentDoneBytes.store(total);
}

std::wstring MakeProgressBar(unsigned int percent, size_t width = 66)
{
    // Keep the indicator visually close to Far's own text-mode progress:
    // solid cells for completed work, a quiet shaded remainder and a compact
    // percentage at the right.  Deliberately no [] ruler and no plugin name.
    percent = std::min(percent, 100U);

    wchar_t percentText[16]{};
    swprintf_s(percentText, L" %3u%%", percent);
    const size_t percentLength = wcslen(percentText);
    const size_t barWidth = width > percentLength ? width - percentLength : 0;
    const size_t filled = (barWidth * percent) / 100;

    // Far-like text progress: solid completed cells and an explicit light
    // dotted / shaded remainder, rather than the dialog background.
    std::wstring text(filled, L'\x2588'); // █ completed
    text.append(barWidth - filled, L'\x2591'); // ░ remaining
    text += percentText;
    return text;
}

void BeginNativeProgress(bool move, bool showSystemProgressUi, bool showTotal)
{
    DebugLog(
        L"BeginNativeProgress move=%d showSystemProgressUi=%d showTotal=%d",
        move ? 1 : 0,
        showSystemProgressUi ? 1 : 0,
        showTotal ? 1 : 0);
    g_ProgressCancelRequested.store(false);
    g_ProgressCancelPromptActive.store(false);
    g_ProgressCancelPromptCv.notify_all();
    InterlockedExchange(&g_ProgressWin32CancelFlag, FALSE);
    g_ProgressTotal.store(0);
    g_ProgressDone.store(0);
    g_ProgressShowTotal.store(showTotal);
    g_ProgressCurrentTotalBytes.store(0);
    g_ProgressCurrentDoneBytes.store(0);
    g_ProgressOverallTotalBytes.store(0);
    g_ProgressOverallDoneBytes.store(0);
    g_ProgressCurrentTargetObservedChange.store(false);
    g_ProgressSyncQueued.store(false);
    g_ProgressRedrawGeneration.store(0);
    g_ProgressLastSyncTick.store(0);
    g_ProgressForceRedrawPending.store(false);
    g_ProgressDialogClosing.store(false);
    g_ProgressDialogRunning.store(false);
    {
        std::lock_guard<std::mutex> lock(g_ProgressNameMutex);
        g_ProgressName.clear();
        g_ProgressCurrentTargetPath.clear();
        g_ProgressCurrentTargetInitialSize = 0;
        g_ProgressCurrentTargetInitialWriteTime = {};
        g_ProgressCurrentTargetInitiallyExisted = false;
    }

    // The Explorer progress window is only an explicit compatibility option.
    // The default UI intentionally follows Far's own copy-progress layout.
    if (showSystemProgressUi || !g_Info.AdvControl || !g_Info.DialogInit ||
        !g_Info.DialogRun || !g_Info.DialogFree || !g_Info.SendDlgMessage ||
        !g_Info.DefDlgProc)
    {
        DebugLog(
            L"BeginNativeProgress native dialog unavailable: systemUi=%d AdvControl=%d DialogInit=%d DialogRun=%d DialogFree=%d SendDlgMessage=%d DefDlgProc=%d",
            showSystemProgressUi ? 1 : 0,
            g_Info.AdvControl ? 1 : 0,
            g_Info.DialogInit ? 1 : 0,
            g_Info.DialogRun ? 1 : 0,
            g_Info.DialogFree ? 1 : 0,
            g_Info.SendDlgMessage ? 1 : 0,
            g_Info.DefDlgProc ? 1 : 0);
        g_NativeProgressActive.store(false);
        g_ProgressDialog = nullptr;
        return;
    }

    constexpr intptr_t dialogWidth = 76;
    const intptr_t totalSeparatorY = showTotal ? 5 : 4;
    const intptr_t totalBarY = showTotal ? 6 : 4;
    const intptr_t separatorY = showTotal ? 7 : 5;
    const intptr_t buttonY = showTotal ? 8 : 6;
    const intptr_t boxBottom = showTotal ? 9 : 7;

    FarDialogItem items[ProgressCount]{};

    items[ProgressBox].Type = DI_DOUBLEBOX;
    items[ProgressBox].X1 = 3;
    items[ProgressBox].Y1 = 1;
    items[ProgressBox].X2 = dialogWidth - 4;
    items[ProgressBox].Y2 = boxBottom;
    items[ProgressBox].Flags = DIF_NONE;
    items[ProgressBox].Data = move ? Msg(MProgressMove) : Msg(MProgressCopy);

    items[ProgressCurrentLabel].Type = DI_TEXT;
    items[ProgressCurrentLabel].X1 = 5;
    items[ProgressCurrentLabel].Y1 = 2;
    items[ProgressCurrentLabel].X2 = dialogWidth - 6;
    items[ProgressCurrentLabel].Y2 = 2;
    items[ProgressCurrentLabel].Flags = DIF_NOFOCUS;
    items[ProgressCurrentLabel].Data = move ? Msg(MProgressCurrentMove) : Msg(MProgressCurrentCopy);

    items[ProgressDetail].Type = DI_TEXT;
    items[ProgressDetail].X1 = 5;
    items[ProgressDetail].Y1 = 3;
    items[ProgressDetail].X2 = dialogWidth - 6;
    items[ProgressDetail].Y2 = 3;
    items[ProgressDetail].Flags = DIF_NOFOCUS | DIF_SHOWAMPERSAND;
    items[ProgressDetail].Data = Msg(MProgressPreparing);

    items[ProgressCurrentBar].Type = DI_TEXT;
    items[ProgressCurrentBar].X1 = 5;
    items[ProgressCurrentBar].Y1 = 4;
    items[ProgressCurrentBar].X2 = dialogWidth - 6;
    items[ProgressCurrentBar].Y2 = 4;
    items[ProgressCurrentBar].Flags = DIF_NOFOCUS;
    g_ProgressCurrentBarText = MakeProgressBar(0);
    items[ProgressCurrentBar].Data = g_ProgressCurrentBarText.c_str();

    items[ProgressTotalSeparator].Type = DI_TEXT;
    items[ProgressTotalSeparator].X1 = -1;
    items[ProgressTotalSeparator].Y1 = totalSeparatorY;
    items[ProgressTotalSeparator].X2 = dialogWidth - 6;
    items[ProgressTotalSeparator].Y2 = totalSeparatorY;
    items[ProgressTotalSeparator].Flags = DIF_SEPARATOR | DIF_NOFOCUS |
        (showTotal ? DIF_NONE : DIF_HIDDEN);
    items[ProgressTotalSeparator].Data = showTotal ? Msg(MProgressTotal) : L"";

    items[ProgressTotalBar].Type = DI_TEXT;
    items[ProgressTotalBar].X1 = 5;
    items[ProgressTotalBar].Y1 = totalBarY;
    items[ProgressTotalBar].X2 = dialogWidth - 6;
    items[ProgressTotalBar].Y2 = totalBarY;
    items[ProgressTotalBar].Flags = DIF_NOFOCUS | (showTotal ? DIF_NONE : DIF_HIDDEN);
    g_ProgressOverallBarText = MakeProgressBar(0);
    items[ProgressTotalBar].Data = showTotal ? g_ProgressOverallBarText.c_str() : L"";

    items[ProgressSeparator].Type = DI_TEXT;
    items[ProgressSeparator].X1 = -1;
    items[ProgressSeparator].Y1 = separatorY;
    items[ProgressSeparator].X2 = dialogWidth - 6;
    items[ProgressSeparator].Y2 = separatorY;
    items[ProgressSeparator].Flags = DIF_SEPARATOR | DIF_NOFOCUS;
    items[ProgressSeparator].Data = L"";

    items[ProgressCancel].Type = DI_BUTTON;
    items[ProgressCancel].X1 = 0;
    items[ProgressCancel].Y1 = buttonY;
    items[ProgressCancel].X2 = 0;
    items[ProgressCancel].Y2 = buttonY;
    items[ProgressCancel].Flags =
        DIF_CENTERGROUP | DIF_FOCUS | DIF_DEFAULTBUTTON | DIF_BTNNOCLOSE;
    items[ProgressCancel].Data = Msg(MProgressCancel);

    HANDLE dialog = g_Info.DialogInit(
        &PluginGuid,
        &ProgressDialogGuid,
        -1,
        -1,
        dialogWidth,
        boxBottom + 2,
        nullptr,
        items,
        ProgressCount,
        0,
        // This MUST be a modal Far dialog.  The actual file operation runs
        // on the worker thread, while Far's dialog loop stays on the main
        // thread and owns keyboard / mouse input.  A non-modal dialog only
        // painted the progress UI over the panels: Enter / Esc / mouse still
        // went to the panel underneath and could even be replayed after the
        // copy finished.
        FDLG_KEEPCONSOLETITLE,
        ProgressDialogProc,
        nullptr);

    DebugLog(
        L"DialogInit returned dialog=%p width=%lld height=%lld showTotal=%d",
        dialog,
        static_cast<long long>(dialogWidth),
        static_cast<long long>(boxBottom + 2),
        showTotal ? 1 : 0);

    if (!dialog || dialog == INVALID_HANDLE_VALUE)
    {
        DebugLog(L"DialogInit FAILED dialog=%p lastError=%lu", dialog, static_cast<unsigned long>(GetLastError()));
        g_ProgressDialog = nullptr;
        g_NativeProgressActive.store(false);
        return;
    }

    g_ProgressDialog = dialog;
    g_NativeProgressActive.store(true);
}

void RunNativeProgressDialog()
{
    HANDLE dialog = g_ProgressDialog;
    if (!g_NativeProgressActive.load() || !dialog || dialog == INVALID_HANDLE_VALUE ||
        !g_Info.DialogRun || !g_Info.DialogFree)
    {
        DebugLog(
            L"RunNativeProgressDialog skipped active=%d dialog=%p DialogRun=%d DialogFree=%d",
            g_NativeProgressActive.load() ? 1 : 0,
            dialog,
            g_Info.DialogRun ? 1 : 0,
            g_Info.DialogFree ? 1 : 0);
        return;
    }

    g_ProgressDialogRunning.store(true);
    DebugLog(L"DialogRun ENTER dialog=%p", dialog);
    const intptr_t runResult = g_Info.DialogRun(dialog);
    DebugLog(
        L"DialogRun EXIT dialog=%p result=%lld cancelRequested=%d closing=%d",
        dialog,
        static_cast<long long>(runResult),
        g_ProgressCancelRequested.load() ? 1 : 0,
        g_ProgressDialogClosing.load() ? 1 : 0);
    g_ProgressDialogRunning.store(false);
    g_ProgressDialogClosing.store(false);

    // Modal dialogs are owned by the plugin and must be freed explicitly.
    // EndNativeProgress() may already have cleared the global handle while
    // closing us from ProcessSynchroEventW, therefore keep and free the local
    // handle exactly once here.
    if (g_ProgressDialog == dialog)
        g_ProgressDialog = nullptr;
    DebugLog(L"DialogFree dialog=%p", dialog);
    g_Info.DialogFree(dialog);
}

void EndNativeProgress()
{
    DebugLog(
        L"EndNativeProgress ENTER active=%d dialog=%p running=%d cancelRequested=%d",
        g_NativeProgressActive.load() ? 1 : 0,
        g_ProgressDialog,
        g_ProgressDialogRunning.load() ? 1 : 0,
        g_ProgressCancelRequested.load() ? 1 : 0);
    g_NativeProgressActive.store(false);
    g_ProgressSyncQueued.store(false);
    g_ProgressForceRedrawPending.store(false);

    HANDLE dialog = g_ProgressDialog;
    if (dialog && dialog != INVALID_HANDLE_VALUE)
    {
        if (g_ProgressDialogRunning.load() && g_Info.SendDlgMessage)
        {
            g_ProgressDialogClosing.store(true);
            const intptr_t closeResult = g_Info.SendDlgMessage(dialog, DM_CLOSE, -1, nullptr);
            DebugLog(
                L"EndNativeProgress DM_CLOSE dialog=%p result=%lld",
                dialog,
                static_cast<long long>(closeResult));
        }
        else if (g_Info.DialogFree)
        {
            // DialogInit succeeded but the worker thread could not be
            // started, so DialogRun was never entered.  Free it here.
            g_Info.DialogFree(dialog);
            g_ProgressDialogClosing.store(false);
            DebugLog(L"EndNativeProgress freed dialog without DialogRun dialog=%p", dialog);
        }
    }

    g_ProgressDialog = nullptr;
    g_ProgressDialogRunning.store(false);
    g_ProgressCancelRequested.store(false);
    g_ProgressCancelPromptActive.store(false);
    g_ProgressCancelPromptCv.notify_all();
    InterlockedExchange(&g_ProgressWin32CancelFlag, FALSE);
    g_ProgressShowTotal.store(false);
    g_ProgressCurrentTotalBytes.store(0);
    g_ProgressCurrentDoneBytes.store(0);
    g_ProgressOverallTotalBytes.store(0);
    g_ProgressOverallDoneBytes.store(0);
    DebugLog(L"EndNativeProgress EXIT");
}

void EnsureProgressRedrawQueued(bool force)
{
    if (!g_NativeProgressActive.load() || !g_Info.AdvControl)
        return;

    const unsigned long long now = GetTickCount64();
    const unsigned long long last = g_ProgressLastSyncTick.load();
    if (!force && last != 0 && now - last < ProgressRedrawIntervalMs)
        return;

    bool expected = false;
    if (!g_ProgressSyncQueued.compare_exchange_strong(expected, true))
        return;

    g_ProgressLastSyncTick.store(now);

    // ACTL_SYNCHRO is unusual: by Far API contract it returns 0 on success.
    // The old code treated 0 as failure and immediately cleared
    // g_ProgressSyncQueued, so *every* CopyFileEx progress callback queued a
    // new main-thread synchro event.  Since Far services ACTL_SYNCHRO from its
    // GetInputRecord path, that flood could starve keyboard and mouse input,
    // especially while recursively copying a directory.
    g_Info.AdvControl(&PluginGuid, ACTL_SYNCHRO, 0, &g_ProgressSyncToken);
}

void QueueProgressRedraw(bool force = false)
{
    g_ProgressRedrawGeneration.fetch_add(1, std::memory_order_relaxed);
    if (force)
        g_ProgressForceRedrawPending.store(true, std::memory_order_release);
    EnsureProgressRedrawQueued(force);
}

void QueueProgressUpdate(unsigned int total, unsigned int done, IShellItem* item = nullptr)
{
    if (!g_NativeProgressActive.load())
        return;

    g_ProgressTotal.store(total);
    g_ProgressDone.store(done);

    if (item)
    {
        PWSTR displayName = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) && displayName)
        {
            std::lock_guard<std::mutex> lock(g_ProgressNameMutex);
            g_ProgressName = displayName;
            CoTaskMemFree(displayName);
        }
    }

    QueueProgressRedraw();
}

void UpdateNativeProgress()
{
    // Keep the queued flag set for the *whole* main-thread redraw.  Clearing it
    // at entry lets the worker queue another ACTL_SYNCHRO while this one is
    // still executing and recreates the input-starvation loop.
    const unsigned long long renderedGeneration =
        g_ProgressRedrawGeneration.load(std::memory_order_acquire);
    g_ProgressForceRedrawPending.store(false, std::memory_order_release);

    if (!g_NativeProgressActive.load() || !g_ProgressDialog ||
        g_ProgressDialog == INVALID_HANDLE_VALUE || !g_Info.SendDlgMessage)
    {
        g_ProgressSyncQueued.store(false, std::memory_order_release);
        return;
    }

    const unsigned int total = g_ProgressTotal.load();
    const unsigned int done = g_ProgressDone.load();
    const unsigned long long overallBytesTotal = g_ProgressOverallTotalBytes.load();
    const unsigned long long overallBytesDone = g_ProgressOverallDoneBytes.load();
    const unsigned int overallPercent = overallBytesTotal
        ? static_cast<unsigned int>(std::min<unsigned long long>(
              100ULL, (100ULL * overallBytesDone) / overallBytesTotal))
        : (total
            ? static_cast<unsigned int>(std::min<unsigned long long>(100ULL, (100ULL * done) / total))
            : 0);

    const unsigned long long currentTotal = g_ProgressCurrentTotalBytes.load();
    const unsigned long long currentDone = g_ProgressCurrentDoneBytes.load();
    const unsigned int currentPercent = currentTotal
        ? static_cast<unsigned int>(std::min<unsigned long long>(100ULL, (100ULL * currentDone) / currentTotal))
        : overallPercent;

    g_ProgressCurrentBarText = MakeProgressBar(currentPercent);
    g_ProgressOverallBarText = MakeProgressBar(overallPercent);

    if (g_ProgressCancelRequested.load())
    {
        g_ProgressDetailText = Msg(MProgressCancelling);
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_ProgressNameMutex);
        g_ProgressDetailText = g_ProgressName.empty()
            ? std::wstring(Msg(MProgressPreparing))
            : g_ProgressName;
    }

    if (g_ProgressDetailText.size() > 66)
        g_ProgressDetailText = L"..." + g_ProgressDetailText.substr(g_ProgressDetailText.size() - 63);

    g_Info.SendDlgMessage(
        g_ProgressDialog,
        DM_SETTEXTPTR,
        ProgressDetail,
        const_cast<wchar_t*>(g_ProgressDetailText.c_str()));
    g_Info.SendDlgMessage(
        g_ProgressDialog,
        DM_SETTEXTPTR,
        ProgressCurrentBar,
        const_cast<wchar_t*>(g_ProgressCurrentBarText.c_str()));

    if (g_ProgressShowTotal.load())
    {
        g_Info.SendDlgMessage(
            g_ProgressDialog,
            DM_SETTEXTPTR,
            ProgressTotalBar,
            const_cast<wchar_t*>(g_ProgressOverallBarText.c_str()));
    }

    g_Info.SendDlgMessage(g_ProgressDialog, DM_REDRAW, 0, nullptr);

    // Let Far return to its input loop before another ordinary progress update.
    // If progress changed while we were painting, a follow-up is allowed, but
    // ordinary redraws remain capped at ~20 Hz.  A forced final redraw may skip
    // the cap, yet there is still never more than one ACTL_SYNCHRO outstanding.
    g_ProgressSyncQueued.store(false, std::memory_order_release);
    if (g_NativeProgressActive.load() &&
        g_ProgressRedrawGeneration.load(std::memory_order_acquire) != renderedGeneration)
    {
        const bool force = g_ProgressForceRedrawPending.load(std::memory_order_acquire);
        EnsureProgressRedrawQueued(force);
    }
}

// IFileOperation does not expose a separate Cancel() method.  Its callbacks
// remain useful for Shell-only fallback paths, but they are not relied on for
// guaranteed mid-file cancellation: ordinary filesystem copies use the
// CopyFileEx / MoveFileWithProgress engine below.  This invisible progress
// object merely gives the Shell fallback another chance to observe a pending
// cancellation request between work items.
class FarOperationsProgressDialog final : public IOperationsProgressDialog
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_POINTER;

        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IOperationsProgressDialog)
        {
            *object = static_cast<IOperationsProgressDialog*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG refs = InterlockedDecrement(&refs_);
        if (!refs)
            delete this;
        return static_cast<ULONG>(refs);
    }

    HRESULT STDMETHODCALLTYPE StartProgressDialog(HWND, OPPROGDLGF) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StopProgressDialog() override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetOperation(SPACTION) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetMode(PDMODE) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateProgress(
        ULONGLONG,
        ULONGLONG,
        ULONGLONG,
        ULONGLONG,
        ULONGLONG,
        ULONGLONG) override
    {
        WaitWhileProgressCancelPromptActive();
        return g_ProgressCancelRequested.load() ? E_ABORT : S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateLocations(IShellItem*, IShellItem*, IShellItem*) override
    {
        WaitWhileProgressCancelPromptActive();
        return g_ProgressCancelRequested.load() ? E_ABORT : S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetMilliseconds(ULONGLONG* elapsed, ULONGLONG* remaining) override
    {
        if (!elapsed || !remaining)
            return E_POINTER;

        *elapsed = 0;
        *remaining = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetOperationStatus(PDOPSTATUS* status) override
    {
        if (!status)
            return E_POINTER;

        // STOPPED means "terminate completely" rather than merely pausing the
        // progress surface.  This is what Far's Cancel button promises here.
        *status = g_ProgressCancelRequested.load() ? PDOPS_STOPPED : PDOPS_RUNNING;
        return S_OK;
    }

private:
    volatile LONG refs_ = 1;
};

class FileOperationProgressSink final : public IFileOperationProgressSink
{
public:
    FileOperationProgressSink() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink)
        {
            *object = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG refs = InterlockedDecrement(&refs_);
        if (!refs)
            delete this;
        return static_cast<ULONG>(refs);
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override
    {
        QueueProgressUpdate(0, 0);
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem* item, LPCWSTR) override
    {
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load(), item);
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override
    {
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PreMoveItem(
        DWORD,
        IShellItem* item,
        IShellItem* destinationFolder,
        LPCWSTR newName) override
    {
        SetCurrentProgressItem(item, destinationFolder, newName);
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load());
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override
    {
        CompleteCurrentProgressItem();
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load());
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PreCopyItem(
        DWORD,
        IShellItem* item,
        IShellItem* destinationFolder,
        LPCWSTR newName) override
    {
        SetCurrentProgressItem(item, destinationFolder, newName);
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load());
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override
    {
        CompleteCurrentProgressItem();
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load());
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem* item) override
    {
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load(), item);
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override
    {
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem* item, LPCWSTR) override
    {
        QueueProgressUpdate(g_ProgressTotal.load(), g_ProgressDone.load(), item);
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override
    {
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT total, UINT done) override
    {
        RefreshCurrentProgressFromTarget();
        QueueProgressUpdate(total, done);
        return CheckCancelled();
    }

    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    HRESULT CheckCancelled() const
    {
        WaitWhileProgressCancelPromptActive();
        return g_ProgressCancelRequested.load() ? E_ABORT : S_OK;
    }

    volatile LONG refs_ = 1;
};

// IFileOperation has no public Cancel() method.  Its progress sink can abort
// between Shell work items, but on some Windows builds that does not stop an
// already-running large CopyItem.  For normal filesystem clipboard paths we
// therefore use the Win32 copy/move APIs that *explicitly* support mid-file
// cancellation.  IFileOperation remains the fallback for virtual Shell items,
// reparse points and the optional Windows conflict/progress UI.

struct NativeTransferState
{
    unsigned long long TotalBytes = 0;
    unsigned long long CompletedBytes = 0;
};

struct NativeTransferProgressContext
{
    NativeTransferState* State = nullptr;
    unsigned long long OverallBase = 0;
};

DWORD CALLBACK NativeTransferProgressRoutine(
    LARGE_INTEGER totalFileSize,
    LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER,
    LARGE_INTEGER,
    DWORD,
    DWORD,
    HANDLE,
    HANDLE,
    LPVOID data)
{
    WaitWhileProgressCancelPromptActive();

    const auto* context = static_cast<const NativeTransferProgressContext*>(data);
    const unsigned long long total = totalFileSize.QuadPart > 0
        ? static_cast<unsigned long long>(totalFileSize.QuadPart)
        : 0ULL;
    const unsigned long long done = totalBytesTransferred.QuadPart > 0
        ? static_cast<unsigned long long>(totalBytesTransferred.QuadPart)
        : 0ULL;

    const unsigned long long currentDone = std::min(done, total);
    g_ProgressCurrentTotalBytes.store(total);
    g_ProgressCurrentDoneBytes.store(currentDone);
    if (context && context->State)
    {
        const unsigned long long base = std::min(
            context->OverallBase, context->State->TotalBytes);
        const unsigned long long remaining = context->State->TotalBytes - base;
        const unsigned long long overall = base + std::min(currentDone, remaining);
        g_ProgressOverallDoneBytes.store(overall);
    }
    QueueProgressRedraw();

    const bool cancelRequested = g_ProgressCancelRequested.load();
    const LONG win32Cancel = InterlockedCompareExchange(
        &g_ProgressWin32CancelFlag, FALSE, FALSE);
    if (cancelRequested || win32Cancel != FALSE)
    {
        DebugLog(
            L"NativeTransferProgressRoutine CANCEL total=%llu done=%llu cancelRequested=%d win32Flag=%ld",
            total,
            currentDone,
            cancelRequested ? 1 : 0,
            static_cast<long>(win32Cancel));
    }

    return cancelRequested ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

void SetNativeProgressPath(const std::wstring& source, unsigned long long size)
{
    {
        std::lock_guard<std::mutex> lock(g_ProgressNameMutex);
        g_ProgressName = ProgressBaseName(source);
        g_ProgressCurrentTargetPath.clear();
        g_ProgressCurrentTargetInitialSize = 0;
        g_ProgressCurrentTargetInitialWriteTime = {};
        g_ProgressCurrentTargetInitiallyExisted = false;
    }

    g_ProgressCurrentTargetObservedChange.store(true);
    g_ProgressCurrentTotalBytes.store(size);
    g_ProgressCurrentDoneBytes.store(0);
    QueueProgressRedraw();
}

bool MeasureNativeTransferPath(
    const std::wstring& path,
    unsigned long long& totalBytes,
    DWORD& errorCode,
    bool& unsupported)
{
    WaitWhileProgressCancelPromptActive();
    if (g_ProgressCancelRequested.load())
    {
        errorCode = ERROR_REQUEST_ABORTED;
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        errorCode = GetLastError();
        return false;
    }

    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        unsupported = true;
        errorCode = ERROR_NOT_SUPPORTED;
        return false;
    }

    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        ULARGE_INTEGER size{};
        size.HighPart = data.nFileSizeHigh;
        size.LowPart = data.nFileSizeLow;
        if (ULLONG_MAX - totalBytes < size.QuadPart)
            totalBytes = ULLONG_MAX;
        else
            totalBytes += size.QuadPart;
        return true;
    }

    WIN32_FIND_DATAW findData{};
    const std::wstring mask = JoinProgressPath(path, L"*");
    HANDLE find = FindFirstFileW(mask.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD findError = GetLastError();
        // FindFirstFile("dir\\*") reports FILE_NOT_FOUND for an empty
        // directory.  That is a valid zero-byte tree, not a transfer error.
        if (findError == ERROR_FILE_NOT_FOUND)
            return true;
        errorCode = findError;
        return false;
    }

    bool ok = true;
    do
    {
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
            continue;

        if (!MeasureNativeTransferPath(
                JoinProgressPath(path, findData.cFileName), totalBytes, errorCode, unsupported))
        {
            ok = false;
            break;
        }
    }
    while (FindNextFileW(find, &findData));

    if (ok)
    {
        const DWORD findError = GetLastError();
        if (findError != ERROR_NO_MORE_FILES)
        {
            errorCode = findError;
            ok = false;
        }
    }

    FindClose(find);
    return ok;
}

void CopyDirectoryMetadataBestEffort(const std::wstring& source, const std::wstring& target)
{
    const DWORD attrs = GetFileAttributesW(source.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(target.c_str(), attrs);

    HANDLE sourceHandle = CreateFileW(
        source.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (sourceHandle == INVALID_HANDLE_VALUE)
        return;

    HANDLE targetHandle = CreateFileW(
        target.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (targetHandle != INVALID_HANDLE_VALUE)
    {
        FILETIME created{}, accessed{}, written{};
        if (GetFileTime(sourceHandle, &created, &accessed, &written))
            SetFileTime(targetHandle, &created, &accessed, &written);
        CloseHandle(targetHandle);
    }
    CloseHandle(sourceHandle);
}

bool MakePartialTargetPath(const std::wstring& target, std::wstring& partial)
{
    const DWORD processId = GetCurrentProcessId();
    const DWORD threadId = GetCurrentThreadId();
    for (unsigned int attempt = 0; attempt < 1000; ++attempt)
    {
        wchar_t suffix[96]{};
        swprintf_s(
            suffix,
            L".farfileclipboard-part-%lu-%lu-%u",
            static_cast<unsigned long>(processId),
            static_cast<unsigned long>(threadId),
            attempt);
        const std::wstring candidate = target + suffix;

        HANDLE file = CreateFileW(
            candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
            partial = candidate;
            return true;
        }

        const DWORD createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS)
            return false;
    }

    SetLastError(ERROR_FILE_EXISTS);
    return false;
}

bool TransferNativePath(
    bool move,
    const std::wstring& source,
    const std::wstring& target,
    NativeTransferState& progress,
    DWORD& errorCode,
    bool& aborted,
    bool& unsupported)
{
    WaitWhileProgressCancelPromptActive();
    DebugLog(
        L"TransferNativePath ENTER move=%d source='%ls' target='%ls' cancelRequested=%d win32Flag=%ld",
        move ? 1 : 0,
        source.c_str(),
        target.c_str(),
        g_ProgressCancelRequested.load() ? 1 : 0,
        static_cast<long>(InterlockedCompareExchange(&g_ProgressWin32CancelFlag, FALSE, FALSE)));

    if (g_ProgressCancelRequested.load())
    {
        DebugLog(L"TransferNativePath abort before start: cancel already requested");
        aborted = true;
        errorCode = ERROR_REQUEST_ABORTED;
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA sourceData{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &sourceData))
    {
        errorCode = GetLastError();
        return false;
    }

    if ((sourceData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        unsupported = true;
        errorCode = ERROR_NOT_SUPPORTED;
        return false;
    }

    const bool sourceDirectory =
        (sourceData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    ULARGE_INTEGER sourceSizeValue{};
    sourceSizeValue.HighPart = sourceData.nFileSizeHigh;
    sourceSizeValue.LowPart = sourceData.nFileSizeLow;
    const unsigned long long sourceSize = sourceDirectory ? 0ULL : sourceSizeValue.QuadPart;

    if (!sourceDirectory)
    {
        const DWORD targetAttrs = GetFileAttributesW(target.c_str());
        if (targetAttrs != INVALID_FILE_ATTRIBUTES &&
            (targetAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            errorCode = ERROR_ALREADY_EXISTS;
            return false;
        }

        SetNativeProgressPath(source, sourceSize);
        const unsigned long long base = progress.CompletedBytes;
        NativeTransferProgressContext context{ &progress, base };

        BOOL ok = FALSE;
        const bool replacingExisting = targetAttrs != INVALID_FILE_ATTRIBUTES;
        std::wstring transferTarget = target;
        if (replacingExisting)
        {
            // Copy through a sibling temporary file.  CopyFileEx documents
            // that PROGRESS_CANCEL deletes the partial destination; writing
            // directly over an existing destination would therefore risk
            // destroying the old file just because the user pressed Cancel.
            if (!MakePartialTargetPath(target, transferTarget))
            {
                errorCode = GetLastError();
                return false;
            }
        }

        if (move && !replacingExisting)
        {
            DebugLog(
                L"MoveFileWithProgressW START source='%ls' target='%ls' size=%llu",
                source.c_str(), transferTarget.c_str(), sourceSize);
            ok = MoveFileWithProgressW(
                source.c_str(), transferTarget.c_str(),
                NativeTransferProgressRoutine, &context,
                MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            const DWORD moveError = ok ? ERROR_SUCCESS : GetLastError();
            DebugLog(
                L"MoveFileWithProgressW END ok=%d error=%lu cancelRequested=%d win32Flag=%ld",
                ok ? 1 : 0,
                static_cast<unsigned long>(moveError),
                g_ProgressCancelRequested.load() ? 1 : 0,
                static_cast<long>(InterlockedCompareExchange(&g_ProgressWin32CancelFlag, FALSE, FALSE)));
            if (!ok)
                SetLastError(moveError);
        }
        else
        {
            DebugLog(
                L"CopyFileExW START source='%ls' target='%ls' size=%llu replacing=%d",
                source.c_str(), transferTarget.c_str(), sourceSize, replacingExisting ? 1 : 0);
            ok = CopyFileExW(
                source.c_str(), transferTarget.c_str(),
                NativeTransferProgressRoutine, &context,
                reinterpret_cast<LPBOOL>(const_cast<LONG*>(&g_ProgressWin32CancelFlag)), 0);
            const DWORD copyError = ok ? ERROR_SUCCESS : GetLastError();
            DebugLog(
                L"CopyFileExW END ok=%d error=%lu cancelRequested=%d win32Flag=%ld",
                ok ? 1 : 0,
                static_cast<unsigned long>(copyError),
                g_ProgressCancelRequested.load() ? 1 : 0,
                static_cast<long>(InterlockedCompareExchange(&g_ProgressWin32CancelFlag, FALSE, FALSE)));
            if (!ok)
                SetLastError(copyError);
        }

        if (!ok)
        {
            errorCode = GetLastError();
            DebugLog(
                L"TransferNativePath transfer failed error=%lu replacing=%d cancelRequested=%d",
                static_cast<unsigned long>(errorCode),
                replacingExisting ? 1 : 0,
                g_ProgressCancelRequested.load() ? 1 : 0);
            if (replacingExisting)
                DeleteFileW(transferTarget.c_str());
            if (errorCode == ERROR_REQUEST_ABORTED || g_ProgressCancelRequested.load())
            {
                aborted = true;
                errorCode = ERROR_REQUEST_ABORTED;
            }
            return false;
        }

        if (replacingExisting)
        {
            if (g_ProgressCancelRequested.load())
            {
                DeleteFileW(transferTarget.c_str());
                aborted = true;
                errorCode = ERROR_REQUEST_ABORTED;
                return false;
            }

            if (!MoveFileExW(
                    transferTarget.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                errorCode = GetLastError();
                DeleteFileW(transferTarget.c_str());
                return false;
            }

            if (move && !DeleteFileW(source.c_str()))
            {
                errorCode = GetLastError();
                return false;
            }
        }

        const unsigned long long remaining = progress.TotalBytes - std::min(base, progress.TotalBytes);
        progress.CompletedBytes = std::min(base, progress.TotalBytes) +
            std::min(sourceSize, remaining);
        g_ProgressCurrentTotalBytes.store(sourceSize);
        g_ProgressCurrentDoneBytes.store(sourceSize);
        g_ProgressOverallDoneBytes.store(progress.CompletedBytes);
        QueueProgressRedraw();
        DebugLog(
            L"TransferNativePath SUCCESS source='%ls' completedBytes=%llu totalBytes=%llu",
            source.c_str(), progress.CompletedBytes, progress.TotalBytes);
        return true;
    }

    const DWORD targetAttrs = GetFileAttributesW(target.c_str());
    const bool targetExists = targetAttrs != INVALID_FILE_ATTRIBUTES;
    if (targetExists && (targetAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        errorCode = ERROR_ALREADY_EXISTS;
        return false;
    }

    // A non-colliding directory move on the same volume is just a rename and
    // should stay fast.  Cross-volume directory moves are not supported by
    // MoveFileWithProgress, so fall through to the recursive cancellable path.
    if (move && !targetExists)
    {
        unsigned long long subtreeBytes = 0;
        DWORD measureError = ERROR_SUCCESS;
        bool measureUnsupported = false;
        if (!MeasureNativeTransferPath(source, subtreeBytes, measureError, measureUnsupported))
        {
            if (measureUnsupported)
                unsupported = true;
            if (measureError == ERROR_REQUEST_ABORTED)
                aborted = true;
            errorCode = measureError;
            return false;
        }

        SetNativeProgressPath(source, subtreeBytes);
        const unsigned long long base = progress.CompletedBytes;
        NativeTransferProgressContext context{ &progress, base };
        if (MoveFileWithProgressW(
                source.c_str(), target.c_str(),
                NativeTransferProgressRoutine, &context,
                MOVEFILE_WRITE_THROUGH))
        {
            const unsigned long long remaining = progress.TotalBytes - std::min(base, progress.TotalBytes);
            progress.CompletedBytes = std::min(base, progress.TotalBytes) +
                std::min(subtreeBytes, remaining);
            g_ProgressCurrentTotalBytes.store(subtreeBytes);
            g_ProgressCurrentDoneBytes.store(subtreeBytes);
            g_ProgressOverallDoneBytes.store(progress.CompletedBytes);
            QueueProgressRedraw();
            return true;
        }

        const DWORD moveError = GetLastError();
        if (moveError == ERROR_REQUEST_ABORTED || g_ProgressCancelRequested.load())
        {
            aborted = true;
            errorCode = ERROR_REQUEST_ABORTED;
            return false;
        }
        if (moveError != ERROR_NOT_SAME_DEVICE)
        {
            errorCode = moveError;
            return false;
        }
    }

    bool createdTarget = false;
    if (!targetExists)
    {
        if (!CreateDirectoryW(target.c_str(), nullptr))
        {
            const DWORD createError = GetLastError();
            if (createError != ERROR_ALREADY_EXISTS)
            {
                errorCode = createError;
                return false;
            }
        }
        else
        {
            createdTarget = true;
        }
    }

    WIN32_FIND_DATAW findData{};
    const std::wstring mask = JoinProgressPath(source, L"*");
    HANDLE find = FindFirstFileW(mask.c_str(), &findData);
    bool ok = true;
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD findError = GetLastError();
        if (findError != ERROR_FILE_NOT_FOUND)
        {
            errorCode = findError;
            return false;
        }
    }
    else
    {
        do
        {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                continue;

            if (g_ProgressCancelRequested.load())
            {
                aborted = true;
                errorCode = ERROR_REQUEST_ABORTED;
                ok = false;
                break;
            }

            const std::wstring childSource = JoinProgressPath(source, findData.cFileName);
            const std::wstring childTarget = JoinProgressPath(target, findData.cFileName);
            if (!TransferNativePath(
                    move, childSource, childTarget, progress,
                    errorCode, aborted, unsupported))
            {
                ok = false;
                break;
            }
        }
        while (FindNextFileW(find, &findData));

        if (ok)
        {
            const DWORD findError = GetLastError();
            if (findError != ERROR_NO_MORE_FILES)
            {
                errorCode = findError;
                ok = false;
            }
        }
        FindClose(find);
    }

    if (!ok)
        return false;

    if (createdTarget)
        CopyDirectoryMetadataBestEffort(source, target);

    if (move)
    {
        if (g_ProgressCancelRequested.load())
        {
            aborted = true;
            errorCode = ERROR_REQUEST_ABORTED;
            return false;
        }

        if (!RemoveDirectoryW(source.c_str()))
        {
            errorCode = GetLastError();
            return false;
        }
    }

    return true;
}

bool RunCancellableFileSystemOperation(
    UINT function,
    const std::vector<std::wstring>& fromPaths,
    const std::vector<std::wstring>& toPaths,
    FILEOP_FLAGS flags,
    int& errorCode,
    bool& aborted,
    bool& unsupported)
{
    errorCode = 0;
    aborted = false;
    unsupported = false;

    if (fromPaths.empty() || toPaths.empty())
    {
        errorCode = ERROR_INVALID_PARAMETER;
        return false;
    }

    unsigned long long totalBytes = 0;
    DWORD scanError = ERROR_SUCCESS;
    for (const auto& source : fromPaths)
    {
        if (!MeasureNativeTransferPath(source, totalBytes, scanError, unsupported))
        {
            if (scanError == ERROR_REQUEST_ABORTED)
                aborted = true;
            errorCode = static_cast<int>(scanError);
            return false;
        }
    }

    NativeTransferState progress{};
    progress.TotalBytes = totalBytes;
    progress.CompletedBytes = 0;

    g_ProgressOverallTotalBytes.store(progress.TotalBytes);
    g_ProgressOverallDoneBytes.store(progress.CompletedBytes);
    QueueProgressRedraw();

    const bool multiDestination = (flags & FOF_MULTIDESTFILES) != 0;
    const bool move = function == FO_MOVE;

    for (size_t i = 0; i < fromPaths.size(); ++i)
    {
        if (g_ProgressCancelRequested.load())
        {
            aborted = true;
            errorCode = ERROR_REQUEST_ABORTED;
            return false;
        }

        std::wstring target;
        if (multiDestination)
        {
            if (i >= toPaths.size())
            {
                errorCode = ERROR_INVALID_PARAMETER;
                return false;
            }
            target = toPaths[i];
        }
        else
        {
            target = JoinProgressPath(toPaths.front(), ProgressBaseName(fromPaths[i]));
        }

        DWORD transferError = ERROR_SUCCESS;
        if (!TransferNativePath(
                move, fromPaths[i], target, progress,
                transferError, aborted, unsupported))
        {
            errorCode = static_cast<int>(transferError);
            return false;
        }
    }

    progress.CompletedBytes = progress.TotalBytes;
    g_ProgressOverallDoneBytes.store(progress.CompletedBytes);
    QueueProgressRedraw(true);
    return true;
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

bool ProgressPathNeedsOverallBar(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ProgressShellItemsNeedOverallBar(IShellItemArray* items)
{
    if (!items)
        return false;

    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || count == 0)
        return false;

    if (count > 1)
        return true;

    IShellItem* item = nullptr;
    if (FAILED(items->GetItemAt(0, &item)) || !item)
        return false;

    SFGAOF attributes = 0;
    const bool isFolder =
        SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)) &&
        (attributes & SFGAO_FOLDER) != 0;
    item->Release();
    return isFolder;
}

bool ProgressPasteItemsNeedOverallBar(const std::vector<BackgroundPasteItem>& items)
{
    if (items.size() > 1)
        return true;

    return items.size() == 1 && ProgressPathNeedsOverallBar(items.front().Source);
}

bool StartBackgroundShellPaste(
    ShellClipboardData& clip,
    const std::wstring& destination,
    std::vector<BackgroundShellPastePlanItem> plan,
    bool usePlan,
    bool allowSystemConflictUi)
{
    bool expected = false;
    if (!g_BackgroundOperationRunning.compare_exchange_strong(expected, true))
    {
        ShowMessage(Msg(MOperationAlreadyRunning));
        return true;
    }

    IStream* marshaled = nullptr;
    const HRESULT marshalResult = CoMarshalInterThreadInterfaceInStream(
        IID_IDataObject, clip.DataObject, &marshaled);
    if (FAILED(marshalResult) || !marshaled)
    {
        g_BackgroundOperationRunning.store(false);
        return false;
    }

    auto* state = new (std::nothrow) BackgroundShellPasteJob();
    if (!state)
    {
        marshaled->Release();
        g_BackgroundOperationRunning.store(false);
        return false;
    }

    state->MarshaledDataObject = marshaled;
    state->DestinationDirectory = destination;
    state->Plan = std::move(plan);
    state->UsePlan = usePlan;
    state->Move = clip.Move;
    state->AllowSystemConflictUi = allowSystemConflictUi;
    state->ShowSystemProgressUi = g_SystemProgressUi;
    state->ClipboardSequenceAtStart = GetClipboardSequenceNumber();

    const bool showOverallProgress = state->UsePlan
        ? (state->Plan.size() > 1 || ProgressShellItemsNeedOverallBar(clip.Items))
        : ProgressShellItemsNeedOverallBar(clip.Items);
    BeginNativeProgress(state->Move, state->ShowSystemProgressUi, showOverallProgress);

    try
    {
        std::thread([state]()
        {
            const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool coInitialized = SUCCEEDED(coResult);
            HRESULT hr = coInitialized ? S_OK : coResult;
            BOOL aborted = FALSE;

            IDataObject* dataObject = nullptr;
            IShellItemArray* items = nullptr;
            IShellItem* destinationItem = nullptr;
            IFileOperation* operation = nullptr;
            FarOperationsProgressDialog* operationProgress = nullptr;
            FileOperationProgressSink* progressSink = nullptr;
            DWORD progressCookie = 0;
            bool progressAdvised = false;

            if (SUCCEEDED(hr))
            {
                IStream* stream = state->MarshaledDataObject;
                state->MarshaledDataObject = nullptr; // CoGet... consumes stream
                hr = CoGetInterfaceAndReleaseStream(
                    stream, IID_IDataObject, reinterpret_cast<void**>(&dataObject));
            }

            if (SUCCEEDED(hr) && state->UsePlan)
            {
                hr = SHCreateShellItemArrayFromDataObject(
                    dataObject, IID_PPV_ARGS(&items));
            }

            if (SUCCEEDED(hr))
            {
                hr = SHCreateItemFromParsingName(
                    state->DestinationDirectory.c_str(),
                    nullptr,
                    IID_PPV_ARGS(&destinationItem));
            }

            if (SUCCEEDED(hr))
            {
                hr = CoCreateInstance(
                    CLSID_FileOperation,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&operation));
            }

            if (SUCCEEDED(hr))
            {
                progressSink = new (std::nothrow) FileOperationProgressSink();
                if (progressSink)
                    progressAdvised = SUCCEEDED(operation->Advise(progressSink, &progressCookie));

                FILEOP_FLAGS flags = FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS;
                if (!state->AllowSystemConflictUi)
                    flags |= FOF_NOCONFIRMATION | FOF_NOERRORUI;

                if (!state->ShowSystemProgressUi)
                {
                    operationProgress = new (std::nothrow) FarOperationsProgressDialog();
                    if (operationProgress)
                    {
                        const HRESULT progressResult = operation->SetProgressDialog(operationProgress);
                        if (FAILED(progressResult))
                        {
                            operationProgress->Release();
                            operationProgress = nullptr;
                        }
                    }

                    // FOF_SILENT also suppresses progress-dialog callbacks on
                    // some Shell versions.  When our private progress object
                    // is installed, leave progress enabled so the fallback can
                    // still report useful progress / cancellation state between
                    // Shell work items.  Mid-file cancellation for ordinary
                    // filesystem paths is handled by CopyFileEx below.  If the
                    // object is unavailable, keep Explorer's own window hidden.
                    if (!operationProgress)
                        flags |= FOF_SILENT;
                }

                hr = operation->SetOperationFlags(flags);

                if (SUCCEEDED(hr) && !state->UsePlan)
                {
                    hr = state->Move
                        ? operation->MoveItems(dataObject, destinationItem)
                        : operation->CopyItems(dataObject, destinationItem);
                }
                else if (SUCCEEDED(hr))
                {
                    for (const auto& planItem : state->Plan)
                    {
                        IShellItem* item = nullptr;
                        hr = items->GetItemAt(planItem.Index, &item);
                        if (FAILED(hr) || !item)
                            break;

                        const wchar_t* requestedName =
                            planItem.NewName.empty() ? nullptr : planItem.NewName.c_str();
                        hr = state->Move
                            ? operation->MoveItem(item, destinationItem, requestedName, nullptr)
                            : operation->CopyItem(item, destinationItem, requestedName, nullptr);
                        item->Release();
                        if (FAILED(hr))
                            break;
                    }
                }
            }

            if (SUCCEEDED(hr) && g_ProgressCancelRequested.load())
                hr = E_ABORT;

            if (SUCCEEDED(hr))
                hr = operation->PerformOperations();
            if (operation)
                operation->GetAnyOperationsAborted(&aborted);

            if (operation && progressAdvised)
                operation->Unadvise(progressCookie);
            if (progressSink)
                progressSink->Release();
            if (operation)
                operation->Release();
            if (operationProgress)
                operationProgress->Release();
            if (destinationItem)
                destinationItem->Release();
            if (items)
                items->Release();
            if (dataObject)
                dataObject->Release();
            if (coInitialized)
                CoUninitialize();

            state->Result = hr;
            state->Aborted = aborted != FALSE || hr == E_ABORT || g_ProgressCancelRequested.load();

            if (g_Info.AdvControl)
            {
                g_Info.AdvControl(&PluginGuid, ACTL_SYNCHRO, 0, state);
            }
            else
            {
                g_BackgroundOperationRunning.store(false);
                delete state;
            }
        }).detach();

        // The file operation itself is on the worker thread.  Keep Far's
        // main thread inside the modal progress dialog so Enter, Esc, F10 and
        // mouse clicks belong to that dialog instead of leaking to the panel.
        RunNativeProgressDialog();
    }
    catch (...)
    {
        EndNativeProgress();
        g_BackgroundOperationRunning.store(false);
        delete state;
        return false;
    }

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

FILEOP_FLAGS ApplySystemProgressPreference(FILEOP_FLAGS flags)
{
    // Plugin-managed paths report failures through Far, not Explorer popups.
    flags |= FOF_NOERRORUI;
    if (!g_SystemProgressUi)
        flags |= FOF_SILENT;
    return flags;
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

bool RunBackgroundShellOperation(
    UINT function,
    const std::vector<std::wstring>& fromPaths,
    const std::vector<std::wstring>& toPaths,
    FILEOP_FLAGS flags,
    int& errorCode,
    bool& aborted)
{
    errorCode = 0;
    aborted = false;

    if (fromPaths.empty() || toPaths.empty())
        return false;

    IFileOperation* operation = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOperation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));

    FarOperationsProgressDialog* operationProgress = nullptr;
    FileOperationProgressSink* sink = nullptr;
    DWORD adviseCookie = 0;
    bool advised = false;

    if (SUCCEEDED(hr))
    {
        sink = new (std::nothrow) FileOperationProgressSink();
        if (sink)
        {
            const HRESULT adviseResult = operation->Advise(sink, &adviseCookie);
            advised = SUCCEEDED(adviseResult);
        }

        if ((flags & FOF_SILENT) != 0)
        {
            operationProgress = new (std::nothrow) FarOperationsProgressDialog();
            if (operationProgress)
            {
                const HRESULT progressResult = operation->SetProgressDialog(operationProgress);
                if (FAILED(progressResult))
                {
                    operationProgress->Release();
                    operationProgress = nullptr;
                }
            }
        }

        FILEOP_FLAGS operationFlags =
            static_cast<FILEOP_FLAGS>(flags & ~FOF_MULTIDESTFILES);
        if (operationProgress)
            operationFlags = static_cast<FILEOP_FLAGS>(operationFlags & ~FOF_SILENT);
        hr = operation->SetOperationFlags(operationFlags);
    }

    const bool multiDestination = (flags & FOF_MULTIDESTFILES) != 0;

    for (size_t i = 0; SUCCEEDED(hr) && i < fromPaths.size(); ++i)
    {
        if (g_ProgressCancelRequested.load())
        {
            hr = E_ABORT;
            break;
        }

        std::wstring target;
        if (multiDestination)
        {
            if (i >= toPaths.size())
            {
                hr = E_INVALIDARG;
                break;
            }
            target = toPaths[i];
        }
        else
        {
            target = toPaths.front();
        }
        if (target.empty())
        {
            hr = E_INVALIDARG;
            break;
        }

        const std::wstring destinationDirectory = multiDestination
            ? ParentDirectory(target)
            : target;
        const std::wstring newName = multiDestination ? BaseName(target) : std::wstring();
        if (destinationDirectory.empty())
        {
            hr = E_INVALIDARG;
            break;
        }

        IShellItem* sourceItem = nullptr;
        IShellItem* destinationItem = nullptr;
        hr = SHCreateItemFromParsingName(
            fromPaths[i].c_str(),
            nullptr,
            IID_PPV_ARGS(&sourceItem));
        if (SUCCEEDED(hr))
        {
            hr = SHCreateItemFromParsingName(
                destinationDirectory.c_str(),
                nullptr,
                IID_PPV_ARGS(&destinationItem));
        }

        if (SUCCEEDED(hr))
        {
            const wchar_t* requestedName = multiDestination ? newName.c_str() : nullptr;
            hr = function == FO_MOVE
                ? operation->MoveItem(sourceItem, destinationItem, requestedName, nullptr)
                : operation->CopyItem(sourceItem, destinationItem, requestedName, nullptr);
        }

        if (destinationItem)
            destinationItem->Release();
        if (sourceItem)
            sourceItem->Release();
    }

    if (SUCCEEDED(hr) && g_ProgressCancelRequested.load())
        hr = E_ABORT;

    if (SUCCEEDED(hr))
        hr = operation->PerformOperations();

    BOOL anyAborted = FALSE;
    if (operation)
        operation->GetAnyOperationsAborted(&anyAborted);

    if (operation && advised)
        operation->Unadvise(adviseCookie);
    if (sink)
        sink->Release();
    if (operation)
        operation->Release();
    if (operationProgress)
        operationProgress->Release();

    aborted = anyAborted != FALSE || hr == E_ABORT || g_ProgressCancelRequested.load();
    errorCode = static_cast<int>(hr);
    return SUCCEEDED(hr) && !aborted;
}

void CompleteBackgroundPaste(BackgroundPasteJob* job)
{
    if (!job)
        return;

    DebugLog(
        L"CompleteBackgroundPaste failed=%d aborted=%d anyPerformed=%d errorCode=%d",
        job->Failed ? 1 : 0,
        job->Aborted ? 1 : 0,
        job->AnyPerformed ? 1 : 0,
        job->ErrorCode);

    EndNativeProgress();
    RefreshPanel();

    if (job->AnyPerformed)
    {
        ClearRecord(g_UndoRecord);
        ClearRecord(g_RedoRecord);

        if (!job->Failed && !job->Aborted && job->CandidateSafe &&
            FinalizeCompletedAction(job->Candidate))
        {
            g_UndoRecord = std::move(job->Candidate);
        }
    }

    if (job->Move && (job->AnyPerformed || job->Failed || job->Aborted))
    {
        // Do not destroy a newer clipboard created while the copy/move was
        // running in the background.
        if (GetClipboardSequenceNumber() == job->ClipboardSequenceAtStart)
        {
            if (!job->Failed && !job->Aborted)
                ClearClipboardAfterMove();
            else
                UpdateClipboardAfterPartialMove(job->OriginalSources);
        }
    }

    if (job->Failed && job->ErrorCode)
    {
        ShowMessage(
            std::wstring(Msg(MOperationFailedPrefix)) + L" " +
            std::to_wstring(job->ErrorCode),
            true);
    }

    g_BackgroundOperationRunning.store(false);
    delete job;
}

void CompleteBackgroundShellPaste(BackgroundShellPasteJob* job)
{
    if (!job)
        return;

    EndNativeProgress();
    RefreshPanel();

    // Virtual Shell objects do not have a stable filesystem source path, so
    // they are intentionally not entered into FarFileClipboard Undo/Redo.
    ClearRecord(g_UndoRecord);
    ClearRecord(g_RedoRecord);

    if (job->Move && SUCCEEDED(job->Result) && !job->Aborted &&
        GetClipboardSequenceNumber() == job->ClipboardSequenceAtStart)
    {
        ClearClipboardAfterMove();
    }

    if (FAILED(job->Result) && !job->Aborted)
    {
        ShowMessage(
            std::wstring(Msg(MOperationFailedPrefix)) + L" HRESULT=" +
            std::to_wstring(static_cast<unsigned long>(job->Result)),
            true);
    }

    g_BackgroundOperationRunning.store(false);
    delete job;
}


bool StartBackgroundPaste(BackgroundPasteJob job)
{
    DebugLog(
        L"StartBackgroundPaste requested move=%d items=%llu systemUi=%d conflictUi=%d kind=%d",
        job.Move ? 1 : 0,
        static_cast<unsigned long long>(job.Items.size()),
        job.ShowSystemProgressUi ? 1 : 0,
        job.AllowSystemConflictUi ? 1 : 0,
        static_cast<int>(job.Kind));

    bool expected = false;
    if (!g_BackgroundOperationRunning.compare_exchange_strong(expected, true))
    {
        ShowMessage(Msg(MOperationAlreadyRunning));
        return true;
    }

    job.ClipboardSequenceAtStart = GetClipboardSequenceNumber();
    auto* state = new (std::nothrow) BackgroundPasteJob(std::move(job));
    if (!state)
    {
        g_BackgroundOperationRunning.store(false);
        ShowMessage(Msg(MBackgroundStartFailed), true);
        return true;
    }

    const bool showOverallProgress = ProgressPasteItemsNeedOverallBar(state->Items);
    DebugLog(L"StartBackgroundPaste showOverallProgress=%d", showOverallProgress ? 1 : 0);
    BeginNativeProgress(state->Move, state->ShowSystemProgressUi, showOverallProgress);

    try
    {
        std::thread([state]()
        {
            DebugLog(
                L"background worker ENTER move=%d items=%llu",
                state->Move ? 1 : 0,
                static_cast<unsigned long long>(state->Items.size()));
            const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool coInitialized = SUCCEEDED(coResult);
            DebugLog(
                L"background worker CoInitializeEx hr=0x%08lX initialized=%d",
                static_cast<unsigned long>(coResult),
                coInitialized ? 1 : 0);

            FILEOP_FLAGS flags = FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS;
            if (!state->ShowSystemProgressUi)
                flags |= FOF_SILENT;
            if (!state->AllowSystemConflictUi)
                flags |= FOF_NOCONFIRMATION | FOF_NOERRORUI;

            const UINT function = state->Move ? FO_MOVE : FO_COPY;

            auto runOperation = [&](const std::vector<std::wstring>& sources,
                                    const std::vector<std::wstring>& targets,
                                    FILEOP_FLAGS operationFlags,
                                    int& rc,
                                    bool& aborted)
            {
                // The Far-native progress UI must have a real, immediate
                // cancellation primitive.  Use CopyFileEx /
                // MoveFileWithProgress for ordinary filesystem paths; these
                // APIs explicitly cancel the current file.  Keep
                // IFileOperation only when the user deliberately asked for
                // Windows UI/conflict handling or when a tree contains a
                // reparse point that needs Shell semantics.
                if (!state->ShowSystemProgressUi && !state->AllowSystemConflictUi)
                {
                    bool unsupported = false;
                    DebugLog(
                        L"runOperation native engine START sources=%llu targets=%llu flags=0x%08X",
                        static_cast<unsigned long long>(sources.size()),
                        static_cast<unsigned long long>(targets.size()),
                        static_cast<unsigned>(operationFlags));
                    const bool ok = RunCancellableFileSystemOperation(
                        function, sources, targets, operationFlags,
                        rc, aborted, unsupported);
                    DebugLog(
                        L"runOperation native engine END ok=%d rc=%d aborted=%d unsupported=%d cancelRequested=%d",
                        ok ? 1 : 0,
                        rc,
                        aborted ? 1 : 0,
                        unsupported ? 1 : 0,
                        g_ProgressCancelRequested.load() ? 1 : 0);
                    if (!unsupported || aborted || g_ProgressCancelRequested.load())
                        return ok;

                    g_ProgressOverallTotalBytes.store(0);
                    g_ProgressOverallDoneBytes.store(0);
                    QueueProgressRedraw();
                }

                DebugLog(L"runOperation SHELL fallback START");
                const bool shellOk = RunBackgroundShellOperation(
                    function, sources, targets, operationFlags, rc, aborted);
                DebugLog(
                    L"runOperation SHELL fallback END ok=%d rc=%d aborted=%d cancelRequested=%d",
                    shellOk ? 1 : 0,
                    rc,
                    aborted ? 1 : 0,
                    g_ProgressCancelRequested.load() ? 1 : 0);
                return shellOk;
            };

            if (state->Kind == BackgroundPasteKind::ToDirectory)
            {
                std::vector<std::wstring> sources;
                sources.reserve(state->Items.size());
                for (const auto& item : state->Items)
                    sources.push_back(item.Source);

                int rc = 0;
                bool aborted = false;
                const bool ok = runOperation(
                    sources,
                    { state->DestinationDirectory },
                    flags,
                    rc,
                    aborted);

                // IFileOperation can still stop after partially changing disk.
                // Once it was invoked, completion must conservatively replace
                // the previous one-level history.
                state->AnyPerformed = true;
                state->Failed = !ok && !aborted;
                state->Aborted = aborted;
                state->ErrorCode = rc;
            }
            else
            {
                flags |= FOF_MULTIDESTFILES;

                std::vector<std::wstring> sources;
                std::vector<std::wstring> targets;
                sources.reserve(state->Items.size());
                targets.reserve(state->Items.size());
                for (const auto& item : state->Items)
                {
                    sources.push_back(item.Source);
                    targets.push_back(item.Target);
                }

                int rc = 0;
                bool aborted = false;
                const bool ok = runOperation(
                    sources,
                    targets,
                    flags,
                    rc,
                    aborted);

                state->AnyPerformed = true;
                state->Failed = !ok && !aborted;
                state->Aborted = aborted;
                state->ErrorCode = rc;
            }

            if (coInitialized)
                CoUninitialize();

            DebugLog(
                L"background worker COMPLETE failed=%d aborted=%d errorCode=%d cancelRequested=%d",
                state->Failed ? 1 : 0,
                state->Aborted ? 1 : 0,
                state->ErrorCode,
                g_ProgressCancelRequested.load() ? 1 : 0);

            if (g_Info.AdvControl)
            {
                const intptr_t syncResult = g_Info.AdvControl(&PluginGuid, ACTL_SYNCHRO, 0, state);
                DebugLog(L"background worker ACTL_SYNCHRO completion result=%lld", static_cast<long long>(syncResult));
            }
            else
            {
                g_BackgroundOperationRunning.store(false);
                delete state;
            }
        }).detach();

        DebugLog(L"StartBackgroundPaste worker started; entering modal progress dialog");
        RunNativeProgressDialog();
        DebugLog(L"StartBackgroundPaste returned from modal progress dialog");
    }
    catch (...)
    {
        EndNativeProgress();
        g_BackgroundOperationRunning.store(false);
        delete state;
        ShowMessage(Msg(MBackgroundStartFailed), true);
    }

    return true;
}

bool PasteClipboardFilesUsingShell(
    const ClipboardFiles& clip,
    const std::wstring& destination)
{
    BackgroundPasteJob job{};
    job.Kind = BackgroundPasteKind::ToDirectory;
    job.Move = clip.Move;
    // Windows conflict UI is only needed when there is actually a top-level
    // collision.  Without a collision, even "Windows behavior" can use the
    // cancellable native transfer engine and still produce the same result.
    job.AllowSystemConflictUi =
        g_ConflictMode == ConflictMode::System &&
        HasTopLevelCollision(clip.Paths, destination);
    job.ShowSystemProgressUi = g_SystemProgressUi;
    job.DestinationDirectory = destination;
    job.OriginalSources = clip.Paths;
    job.Items.reserve(clip.Paths.size());

    for (const auto& source : clip.Paths)
    {
        BackgroundPasteItem item{};
        item.Source = source;
        item.Target = JoinPath(destination, BaseName(source));
        job.Items.push_back(std::move(item));
    }

    job.CandidateSafe = BuildSafeActionRecord(
        clip.Paths,
        destination,
        clip.Move ? FileAction::Move : FileAction::Copy,
        job.Candidate);

    return StartBackgroundPaste(std::move(job));
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
    BackgroundPasteJob job{};
    job.Kind = BackgroundPasteKind::ExactTargets;
    job.Move = clip.Move;
    job.AllowSystemConflictUi = false;
    job.ShowSystemProgressUi = g_SystemProgressUi;
    job.OriginalSources = clip.Paths;
    job.Candidate.Action = clip.Move ? FileAction::Move : FileAction::Copy;

    bool historySafe = true;
    job.Items.reserve(clip.Paths.size());
    job.Candidate.Items.reserve(clip.Paths.size());

    for (const auto& source : clip.Paths)
    {
        if (!PathExists(source))
        {
            ShowMessage(std::wstring(Msg(MObjectNotFound)) + L"\n" + source, true);
            return true;
        }

        std::wstring target;
        if (!FindAutoRenameTarget(source, destination, target))
        {
            ShowMessage(Msg(MAutoRenameFailed), true);
            return true;
        }

        BackgroundPasteItem operation{};
        operation.Source = source;
        operation.Target = target;
        job.Items.push_back(std::move(operation));

        if (historySafe)
        {
            FileActionItem historyItem{};
            historyItem.Source = source;
            historyItem.Destination = target;
            if (GetFileIdentity(source, historyItem.SourceIdentity))
                job.Candidate.Items.push_back(std::move(historyItem));
            else
                historySafe = false;
        }
    }

    job.CandidateSafe = historySafe &&
        job.Candidate.Items.size() == job.Items.size() &&
        !job.Items.empty();

    if (job.Items.empty())
        return true;

    return StartBackgroundPaste(std::move(job));
}

bool PasteClipboardFilesToDirectory(const std::wstring& destination)
{
    if (g_BackgroundOperationRunning.load())
    {
        ShowMessage(Msg(MOperationAlreadyRunning));
        return true;
    }

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

    if (g_BackgroundOperationRunning.load())
    {
        ShowMessage(Msg(MOperationAlreadyRunning));
        return true;
    }

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
            ApplySystemProgressPreference(FOF_ALLOWUNDO),
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
            ApplySystemProgressPreference(
                FOF_MULTIDESTFILES | FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS),
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

    if (g_BackgroundOperationRunning.load())
    {
        ShowMessage(Msg(MOperationAlreadyRunning));
        return true;
    }

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
        ApplySystemProgressPreference(
            FOF_MULTIDESTFILES | FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS),
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
    if (ReadNumberSetting(settings.Handle, SystemProgressUiSetting, number))
        g_SystemProgressUi = number != 0;

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
    bool systemProgressUi,
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
    const bool progressUiOk = WriteNumberSetting(settings.Handle, SystemProgressUiSetting, systemProgressUi ? 1ULL : 0ULL);
    const bool conflictOk = WriteNumberSetting(settings.Handle, ConflictModeSetting, static_cast<unsigned long long>(conflictMode));
    const bool templateOk = WriteStringSetting(settings.Handle, AutoRenameTemplateSetting, autoRenameTemplate);
    g_Info.SettingsControl(settings.Handle, SCTL_FREE, 0, nullptr);
    return copyOk && cutOk && pasteOk && pasteIntoOk && undoOk && redoOk && precheckOk && progressUiOk && conflictOk && templateOk;
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
        ApplySystemProgressPreference(
            FOF_MULTIDESTFILES | FOF_NOCONFIRMATION |
            FOF_NOCONFIRMMKDIR | FOF_NOCOPYSECURITYATTRIBS),
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

bool PasteShellClipboardWithPluginConflicts(
    ShellClipboardData& clip,
    const std::wstring& destination)
{
    DWORD count = 0;
    if (FAILED(clip.Items->GetCount(&count)))
        return false;

    bool stickyDecisionActive = false;
    ConflictChoice stickyChoice = ConflictChoice::Cancel;
    std::vector<BackgroundShellPastePlanItem> plan;
    plan.reserve(count);

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
        item->Release();

        std::wstring newName;
        const std::wstring originalTarget = JoinPath(destination, originalName);
        const bool collision = PathExists(originalTarget);
        bool skipItem = false;

        if (g_ConflictMode == ConflictMode::AutoRename)
        {
            if (!FindAutoRenameTargetForObject(
                    originalName, directory, destination, newName))
            {
                ShowMessage(Msg(MAutoRenameFailed), true);
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
                    return true;
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

        if (!skipItem)
            plan.push_back({ i, std::move(newName) });
    }

    if (plan.empty())
        return true;

    if (!StartBackgroundShellPaste(clip, destination, std::move(plan), true, false))
        ShowMessage(Msg(MBackgroundStartFailed), true);
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
    {
        if (!StartBackgroundShellPaste(clip, destination, {}, false, true))
            ShowMessage(Msg(MBackgroundStartFailed), true);
        return true;
    }

    return PasteShellClipboardWithPluginConflicts(clip, destination);
}

bool PasteClipboardFilesWithFarConflicts(const ClipboardFiles& clip, const std::wstring& destination)
{
    bool stickyDecisionActive = false;
    ConflictChoice stickyChoice = ConflictChoice::Cancel;
    bool historySafe = true;

    BackgroundPasteJob job{};
    job.Kind = BackgroundPasteKind::ExactTargets;
    job.Move = clip.Move;
    job.AllowSystemConflictUi = false;
    job.ShowSystemProgressUi = g_SystemProgressUi;
    job.OriginalSources = clip.Paths;
    job.Candidate.Action = clip.Move ? FileAction::Move : FileAction::Copy;
    job.Items.reserve(clip.Paths.size());
    job.Candidate.Items.reserve(clip.Paths.size());

    for (const auto& source : clip.Paths)
    {
        if (!PathExists(source))
        {
            ShowMessage(std::wstring(Msg(MObjectNotFound)) + L"\n" + source, true);
            return true;
        }

        const std::wstring originalName = BaseName(source);
        if (originalName.empty())
            return true;

        std::wstring target = JoinPath(destination, originalName);
        const bool hadCollision = PathExists(target);
        bool replacingExisting = false;
        bool skipItem = false;

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
                {
                    skipItem = true;
                    break;
                }

                if (decision.Choice == ConflictChoice::Cancel)
                {
                    // Planning happens before the worker starts, so Cancel
                    // really means no file has been changed yet.
                    return true;
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
        }

        if (skipItem)
            continue;

        BackgroundPasteItem operation{};
        operation.Source = source;
        operation.Target = target;
        job.Items.push_back(std::move(operation));

        if (!replacingExisting && historySafe)
        {
            FileActionItem historyItem{};
            historyItem.Source = source;
            historyItem.Destination = target;
            if (GetFileIdentity(source, historyItem.SourceIdentity))
                job.Candidate.Items.push_back(std::move(historyItem));
            else
                historySafe = false;
        }
    }

    job.CandidateSafe = historySafe &&
        job.Candidate.Items.size() == job.Items.size() &&
        !job.Items.empty();

    if (job.Items.empty())
        return true;

    return StartBackgroundPaste(std::move(job));
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
    CfgSystemProgressUi,
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
    bool& systemProgressUi,
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
        constexpr intptr_t dialogWidth = 86;
        constexpr intptr_t dialogHeight = 20;
        constexpr intptr_t boxRight = dialogWidth - 4;
        constexpr intptr_t separatorRight = boxRight - 2;

        FarDialogItem items[CfgCount]{};
        // Leave real horizontal breathing room for localized checkbox text.
        // The old 70-column layout let the Russian progress-UI option write
        // straight through the right border of the double box.
        items[CfgBox] = MakeDialogItem(
            DI_DOUBLEBOX, 3, 1, boxRight, 18, DIF_NONE, Msg(MPluginName));

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
            DI_TEXT, 5, 8, separatorRight, 8, DIF_SEPARATOR, L"");
        items[CfgInvalidPrecheck] = MakeDialogItem(
            DI_CHECKBOX, 7, 9, 0, 9, DIF_NONE, Msg(MConfigPrecheckInvalid));
        items[CfgInvalidPrecheck].Selected = precheckInvalidOperations ? 1 : 0;
        items[CfgSystemProgressUi] = MakeDialogItem(
            DI_CHECKBOX, 7, 10, 0, 10, DIF_NONE, Msg(MConfigSystemProgressUi));
        items[CfgSystemProgressUi].Selected = systemProgressUi ? 1 : 0;

        items[CfgConflictsSeparator] = MakeDialogItem(
            DI_TEXT, 5, 11, separatorRight, 11, DIF_SEPARATOR, L"");
        items[CfgConflictAsk] = MakeDialogItem(
            DI_RADIOBUTTON, 7, 12, 0, 12, DIF_GROUP, Msg(MConfigConflictAsk));
        items[CfgConflictAsk].Selected = conflictMode == ConflictMode::Ask ? 1 : 0;
        items[CfgConflictSystem] = MakeDialogItem(
            DI_RADIOBUTTON, 7, 13, 0, 13, DIF_NONE, Msg(MConfigConflictSystem));
        items[CfgConflictSystem].Selected = conflictMode == ConflictMode::System ? 1 : 0;
        items[CfgConflictAutoRename] = MakeDialogItem(
            DI_RADIOBUTTON, 7, 14, 0, 14, DIF_NONE, Msg(MConfigConflictAutoRename));
        items[CfgConflictAutoRename].Selected = conflictMode == ConflictMode::AutoRename ? 1 : 0;
        const FARDIALOGITEMFLAGS templateDisabled =
            conflictMode == ConflictMode::AutoRename ? DIF_NONE : DIF_DISABLE;
        items[CfgAutoRenameTemplateLabel] = MakeDialogItem(
            DI_TEXT, 10, 15, 0, 15, templateDisabled, Msg(MConfigAutoRenameTemplate));
        items[CfgAutoRenameTemplateEdit] = MakeDialogItem(
            DI_EDIT, 24, 15, boxRight - 5, 15,
            DIF_SELECTONENTRY | DIF_NOAUTOCOMPLETE | templateDisabled,
            autoRenameTemplate.c_str(), 128);

        items[CfgBottomSeparator] = MakeDialogItem(
            DI_TEXT, 5, 16, separatorRight, 16, DIF_SEPARATOR, L"");
        items[CfgDefaults] = MakeDialogItem(DI_BUTTON, 10, 17, 0, 17, DIF_NONE, Msg(MConfigDefaults));
        items[CfgOk] = MakeDialogItem(DI_BUTTON, 44, 17, 0, 17, DIF_DEFAULTBUTTON, Msg(MConfigOk));
        items[CfgCancel] = MakeDialogItem(DI_BUTTON, 56, 17, 0, 17, DIF_NONE, Msg(MConfigCancel));

        HANDLE dialog = g_Info.DialogInit(
            &PluginGuid,
            &ConfigDialogGuid,
            -1,
            -1,
            dialogWidth,
            dialogHeight,
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
            systemProgressUi =
                g_Info.SendDlgMessage(dialog, DM_GETCHECK, CfgSystemProgressUi, nullptr) != 0;

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
            systemProgressUi = false;
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
    info->Version = { 1, 0, 2, 0, VS_RELEASE };
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

    ResetDebugLog();
    DebugLog(L"=== FarFileClipboard cancellation diagnostic build START ===");
    DebugLog(L"debugLogPath='%ls'", DebugLogPath().c_str());
    DebugLog(
        L"SetStartupInfoW info=%p StructSize=%llu ModuleName='%ls'",
        info,
        static_cast<unsigned long long>(info->StructSize),
        info->ModuleName ? info->ModuleName : L"(null)");

    g_Info = *info;
    LoadSettings();
    DebugLog(
        L"settings loaded: systemProgressUi=%d conflictMode=%llu precheck=%d",
        g_SystemProgressUi ? 1 : 0,
        static_cast<unsigned long long>(g_ConflictMode),
        g_PrecheckInvalidOperations ? 1 : 0);
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
    bool systemProgressUi = g_SystemProgressUi;
    ConflictMode conflictMode = g_ConflictMode;
    std::wstring autoRenameTemplate = g_AutoRenameTemplate;
    if (!ShowConfigurationDialog(letters, precheckInvalidOperations, systemProgressUi, conflictMode, autoRenameTemplate))
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
            precheckInvalidOperations, systemProgressUi, conflictMode, autoRenameTemplate))
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
    g_SystemProgressUi = systemProgressUi;
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

extern "C" __declspec(dllexport) intptr_t WINAPI ProcessSynchroEventW(const ProcessSynchroEventInfo* info)
{
    if (info && info->Event == SE_COMMONSYNCHRO && info->Param)
    {
        if (info->Param == &g_ProgressSyncToken)
        {
            UpdateNativeProgress();
            return 0;
        }

        auto* job = static_cast<BackgroundJobBase*>(info->Param);
        DebugLog(
            L"ProcessSynchroEventW completion param=%p jobType=%d",
            info->Param,
            static_cast<int>(job->Type));
        if (job->Type == BackgroundJobType::FileSystemPaste)
            CompleteBackgroundPaste(static_cast<BackgroundPasteJob*>(info->Param));
        else if (job->Type == BackgroundJobType::ShellPaste)
            CompleteBackgroundShellPaste(static_cast<BackgroundShellPasteJob*>(info->Param));
    }

    return 0;
}

extern "C" __declspec(dllexport) intptr_t WINAPI ProcessConsoleInputW(ProcessConsoleInputInfo* info)
{
    if (!info)
        return 0;

#ifdef FFC_CANCEL_DIAGNOSTICS
    if (g_BackgroundOperationRunning.load() && info->Rec.EventType != 0)
        DebugLogInputRecord(L"ProcessConsoleInputW while background operation active", info->Rec);
#endif

    if (info->Rec.EventType != KEY_EVENT)
        return 0;

    const KEY_EVENT_RECORD& key = info->Rec.Event.KeyEvent;
    if (!key.bKeyDown)
        return 0;

    const bool panelsWindow = IsPanelsWindow();
#ifdef FFC_CANCEL_DIAGNOSTICS
    if (g_BackgroundOperationRunning.load())
    {
        DebugLog(
            L"ProcessConsoleInputW keyDown vk=0x%04X panelsWindow=%d",
            static_cast<unsigned>(key.wVirtualKeyCode),
            panelsWindow ? 1 : 0);
    }
#endif
    if (!panelsWindow)
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
