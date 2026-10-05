#pragma once

// Minimal Far Manager 3 plugin ABI subset used by FarFileClipboard.
// Layout and numeric values are taken from the public Far 3 plugin API.

#include <windows.h>
#include <cstddef>
#include <cstdint>

using UUID = GUID;

using PLUGIN_FLAGS = unsigned long long;
constexpr PLUGIN_FLAGS PF_PRELOAD = 0x0000000000000001ULL;
constexpr PLUGIN_FLAGS PF_NONE    = 0;

using FARMESSAGEFLAGS = unsigned long long;
constexpr FARMESSAGEFLAGS FMSG_WARNING  = 0x0000000000000001ULL;
constexpr FARMESSAGEFLAGS FMSG_ALLINONE = 0x0000000000000010ULL;
constexpr FARMESSAGEFLAGS FMSG_MB_OK     = 0x0000000000010000ULL;
constexpr FARMESSAGEFLAGS FMSG_MB_YESNO  = 0x0000000000040000ULL;

using INPUTBOXFLAGS = unsigned long long;
constexpr INPUTBOXFLAGS FIB_NOUSELASTHISTORY = 0x0000000000000008ULL;
constexpr INPUTBOXFLAGS FIB_BUTTONS           = 0x0000000000000010ULL;

#define PANEL_ACTIVE ((HANDLE)(intptr_t)-1)
#define PANEL_STOP   ((HANDLE)(intptr_t)-1)

// FILE_CONTROL_COMMANDS subset
constexpr int FCTL_GETPANELINFO         = 1;
constexpr int FCTL_UPDATEPANEL          = 2;
constexpr int FCTL_REDRAWPANEL          = 3;
constexpr int FCTL_GETPANELITEM         = 21;
constexpr int FCTL_GETSELECTEDPANELITEM = 22;
constexpr int FCTL_GETCURRENTPANELITEM  = 23;
constexpr int FCTL_GETPANELDIRECTORY    = 24;

// PANELINFOFLAGS subset
using PANELINFOFLAGS = unsigned long long;
constexpr PANELINFOFLAGS PFLAGS_PLUGIN = 0x0000000000000800ULL;

// PANELINFOTYPE subset
constexpr int PTYPE_FILEPANEL = 0;

// ADVANCED_CONTROL_COMMANDS subset
constexpr int ACTL_SYNCHRO       = 20;
constexpr int ACTL_GETWINDOWTYPE = 28;

// SYNCHRO_EVENTS subset
constexpr intptr_t SE_COMMONSYNCHRO = 0;

// WINDOWINFO_TYPE subset
constexpr int WTYPE_UNKNOWN = -1;
constexpr int WTYPE_PANELS  = 1;

// FAR_SETTINGS_CONTROL_COMMANDS subset
constexpr int SCTL_CREATE = 0;
constexpr int SCTL_FREE   = 1;
constexpr int SCTL_SET    = 2;
constexpr int SCTL_GET    = 3;

// FARSETTINGSTYPES subset
constexpr int FST_UNKNOWN = 0;
constexpr int FST_QWORD   = 2;
constexpr int FST_STRING  = 3;

// FAR_PLUGIN_SETTINGS_LOCATION subset
constexpr intptr_t PSL_ROAMING = 0;

// VERSION_STAGE
constexpr int VS_RELEASE = 0;

struct VersionInfo
{
    DWORD Major;
    DWORD Minor;
    DWORD Revision;
    DWORD Build;
    int Stage;
};

struct GlobalInfo
{
    size_t StructSize;
    VersionInfo MinFarVersion;
    VersionInfo Version;
    UUID Guid;
    const wchar_t* Title;
    const wchar_t* Description;
    const wchar_t* Author;
    void* Instance;
};

struct PluginMenuItem
{
    const UUID* Guids;
    const wchar_t* const* Strings;
    size_t Count;
};

struct PluginInfo
{
    size_t StructSize;
    PLUGIN_FLAGS Flags;
    PluginMenuItem DiskMenu;
    PluginMenuItem PluginMenu;
    PluginMenuItem PluginConfig;
    const wchar_t* CommandPrefix;
    void* Instance;
};

struct ConfigureInfo
{
    size_t StructSize;
    const UUID* Guid;
    void* Instance;
};

struct OpenInfo
{
    size_t StructSize;
    int OpenFrom;
    const UUID* Guid;
    intptr_t Data;
    void* Instance;
};

using PROCESSCONSOLEINPUT_FLAGS = unsigned long long;
struct ProcessConsoleInputInfo
{
    size_t StructSize;
    PROCESSCONSOLEINPUT_FLAGS Flags;
    INPUT_RECORD Rec;
    void* Instance;
};

struct ProcessSynchroEventInfo
{
    size_t StructSize;
    intptr_t Event;
    void* Param;
    void* Instance;
};

struct WindowType
{
    size_t StructSize;
    int Type;
};

struct FarPanelDirectory
{
    size_t StructSize;
    const wchar_t* Name;
    const wchar_t* Param;
    UUID PluginId;
    const wchar_t* File;
};

using PLUGINPANELITEMFLAGS = unsigned long long;

struct FarPanelItemFreeInfo
{
    size_t StructSize;
    HANDLE hPlugin;
};

using FARPANELITEMFREECALLBACK = void (WINAPI *)(void* UserData, const FarPanelItemFreeInfo* Info);

struct UserDataItem
{
    void* Data;
    FARPANELITEMFREECALLBACK FreeData;
};

struct PluginPanelItem
{
    FILETIME CreationTime;
    FILETIME LastAccessTime;
    FILETIME LastWriteTime;
    FILETIME ChangeTime;
    unsigned long long FileSize;
    unsigned long long AllocationSize;
    const wchar_t* FileName;
    const wchar_t* AlternateFileName;
    const wchar_t* Description;
    const wchar_t* Owner;
    const wchar_t* const* CustomColumnData;
    size_t CustomColumnNumber;
    PLUGINPANELITEMFLAGS Flags;
    UserDataItem UserData;
    uintptr_t FileAttributes;
    uintptr_t NumberOfLinks;
    uintptr_t CRC32;
    intptr_t Reserved[2];
};

struct FarGetPluginPanelItem
{
    size_t StructSize;
    size_t Size;
    PluginPanelItem* Item;
};

struct PanelInfo
{
    size_t StructSize;
    HANDLE PluginHandle;
    UUID OwnerGuid;
    PANELINFOFLAGS Flags;
    size_t ItemsNumber;
    size_t SelectedItemsNumber;
    RECT PanelRect;
    size_t CurrentItem;
    size_t TopPanelItem;
    intptr_t ViewMode;
    int PanelType;
    int SortMode;
};

struct FarSettingsCreate
{
    size_t StructSize;
    UUID Guid;
    HANDLE Handle;
};

struct FarSettingsItem
{
    size_t StructSize;
    size_t Root;
    const wchar_t* Name;
    int Type;
    int ReservedPadding;
    union
    {
        unsigned long long Number;
        const wchar_t* String;
        struct
        {
            size_t Size;
            const void* Data;
        } Data;
    };
};

using FARAPIMESSAGE = intptr_t (WINAPI *)(
    const UUID* PluginId,
    const UUID* Id,
    FARMESSAGEFLAGS Flags,
    const wchar_t* HelpTopic,
    const wchar_t* const* Items,
    size_t ItemsNumber,
    intptr_t ButtonsNumber);

using FARAPIPANELCONTROL = intptr_t (WINAPI *)(
    HANDLE hPanel,
    int Command,
    intptr_t Param1,
    void* Param2);

using FARAPIADVCONTROL = intptr_t (WINAPI *)(
    const UUID* PluginId,
    int Command,
    intptr_t Param1,
    void* Param2);

using FARAPIINPUTBOX = intptr_t (WINAPI *)(
    const UUID* PluginId,
    const UUID* Id,
    const wchar_t* Title,
    const wchar_t* SubTitle,
    const wchar_t* HistoryName,
    const wchar_t* SrcText,
    wchar_t* DestText,
    size_t DestSize,
    const wchar_t* HelpTopic,
    INPUTBOXFLAGS Flags);

using FARAPISETTINGSCONTROL = intptr_t (WINAPI *)(
    HANDLE hHandle,
    int Command,
    intptr_t Param1,
    void* Param2);

using FARAPIGETMSG = const wchar_t* (WINAPI *)(
    const UUID* PluginId,
    intptr_t MsgId);

// Dialog API subset.
enum FARDIALOGITEMTYPES
{
    DI_TEXT = 0,
    DI_VTEXT = 1,
    DI_SINGLEBOX = 2,
    DI_DOUBLEBOX = 3,
    DI_EDIT = 4,
    DI_PSWEDIT = 5,
    DI_FIXEDIT = 6,
    DI_BUTTON = 7,
    DI_CHECKBOX = 8,
    DI_RADIOBUTTON = 9,
    DI_COMBOBOX = 10,
    DI_LISTBOX = 11,
    DI_USERCONTROL = 255,
};

using FARDIALOGITEMFLAGS = unsigned long long;
constexpr FARDIALOGITEMFLAGS DIF_NONE             = 0;
constexpr FARDIALOGITEMFLAGS DIF_GROUP            = 0x0000000000000400ULL;
constexpr FARDIALOGITEMFLAGS DIF_SHOWAMPERSAND    = 0x0000000000002000ULL;
constexpr FARDIALOGITEMFLAGS DIF_CENTERGROUP      = 0x0000000000004000ULL;
constexpr FARDIALOGITEMFLAGS DIF_NOBRACKETS       = 0x0000000000008000ULL;
constexpr FARDIALOGITEMFLAGS DIF_SEPARATOR        = 0x0000000000010000ULL;
constexpr FARDIALOGITEMFLAGS DIF_CENTERTEXT       = 0x0000000000040000ULL;
constexpr FARDIALOGITEMFLAGS DIF_BTNNOCLOSE       = 0x0000000000040000ULL;
constexpr FARDIALOGITEMFLAGS DIF_SELECTONENTRY    = 0x0000000000800000ULL;
constexpr FARDIALOGITEMFLAGS DIF_NOAUTOCOMPLETE   = 0x0000000002000000ULL;
constexpr FARDIALOGITEMFLAGS DIF_HIDDEN           = 0x0000000010000000ULL;
constexpr FARDIALOGITEMFLAGS DIF_NOFOCUS          = 0x0000000040000000ULL;
constexpr FARDIALOGITEMFLAGS DIF_DISABLE          = 0x0000000080000000ULL;
constexpr FARDIALOGITEMFLAGS DIF_DEFAULTBUTTON    = 0x0000000100000000ULL;
constexpr FARDIALOGITEMFLAGS DIF_FOCUS            = 0x0000000200000000ULL;

struct FarDialogItem
{
    FARDIALOGITEMTYPES Type;
    intptr_t X1, Y1, X2, Y2;
    union
    {
        intptr_t Selected;
        void* ListItems;
        void* VBuf;
        intptr_t Reserved0;
    };
    const wchar_t* History;
    const wchar_t* Mask;
    FARDIALOGITEMFLAGS Flags;
    const wchar_t* Data;
    size_t MaxLength;
    intptr_t UserData;
    intptr_t Reserved[2];
};

using FARDIALOGFLAGS = unsigned long long;
constexpr FARDIALOGFLAGS FDLG_NONE = 0;
constexpr FARDIALOGFLAGS FDLG_WARNING = 0x0000000000000001ULL;
constexpr FARDIALOGFLAGS FDLG_SMALLDIALOG = 0x0000000000000002ULL;
constexpr FARDIALOGFLAGS FDLG_KEEPCONSOLETITLE = 0x0000000000000010ULL;
constexpr FARDIALOGFLAGS FDLG_NONMODAL = 0x0000000000000020ULL;

constexpr intptr_t DM_CLOSE = 1;
constexpr intptr_t DM_ENABLE = 2;
constexpr intptr_t DM_GETDLGRECT = 6;
constexpr intptr_t DM_SETFOCUS = 13;
constexpr intptr_t DM_REDRAW = 14;
constexpr intptr_t DM_GETFOCUS = 18;
constexpr intptr_t DM_SETTEXTPTR = 22;
constexpr intptr_t DM_GETCHECK = 25;
constexpr intptr_t DM_SETCHECK = 26;
constexpr intptr_t DM_GETITEMPOSITION = 48;
constexpr intptr_t DM_SETINPUTNOTIFY = 49;
constexpr intptr_t DM_GETCONSTTEXTPTR = 63;
constexpr intptr_t DN_BTNCLICK = 4097;
constexpr intptr_t DN_INITDIALOG = 4108;
constexpr intptr_t DN_INPUT = 4115;
constexpr intptr_t DN_CONTROLINPUT = 4116;
constexpr intptr_t DN_CLOSE = 4117;

using FARWINDOWPROC = intptr_t (WINAPI *)(
    HANDLE hDlg,
    intptr_t Msg,
    intptr_t Param1,
    void* Param2);

using FARAPIDIALOGINIT = HANDLE (WINAPI *)(
    const UUID* PluginId,
    const UUID* Id,
    intptr_t X1,
    intptr_t Y1,
    intptr_t X2,
    intptr_t Y2,
    const wchar_t* HelpTopic,
    const FarDialogItem* Item,
    size_t ItemsNumber,
    intptr_t Reserved,
    FARDIALOGFLAGS Flags,
    FARWINDOWPROC DlgProc,
    void* Param);

using FARAPIDIALOGRUN = intptr_t (WINAPI *)(HANDLE hDlg);
using FARAPIDIALOGFREE = void (WINAPI *)(HANDLE hDlg);
using FARAPISENDDLGMESSAGE = intptr_t (WINAPI *)(
    HANDLE hDlg,
    intptr_t Msg,
    intptr_t Param1,
    void* Param2);
using FARAPIDEFDLGPROC = intptr_t (WINAPI *)(
    HANDLE hDlg,
    intptr_t Msg,
    intptr_t Param1,
    void* Param2);

using MENUITEMFLAGS = unsigned long long;
constexpr MENUITEMFLAGS MIF_SELECTED  = 0x0000000000010000ULL;
constexpr MENUITEMFLAGS MIF_SEPARATOR = 0x0000000000040000ULL;
constexpr MENUITEMFLAGS MIF_DISABLE   = 0x0000000000080000ULL;
constexpr MENUITEMFLAGS MIF_NONE      = 0;

struct FarKey
{
    WORD VirtualKeyCode;
    DWORD ControlKeyState;
};

struct FarMenuItem
{
    MENUITEMFLAGS Flags;
    const wchar_t* Text;
    FarKey AccelKey;
    intptr_t UserData;
    intptr_t Reserved[2];
};

using FARMENUFLAGS = unsigned long long;
constexpr FARMENUFLAGS FMENU_WRAPMODE      = 0x0000000000000002ULL;
constexpr FARMENUFLAGS FMENU_AUTOHIGHLIGHT = 0x0000000000000004ULL;
constexpr FARMENUFLAGS FMENU_NONE          = 0;

using FARAPIMENU = intptr_t (WINAPI *)(
    const UUID* PluginId,
    const UUID* Id,
    intptr_t X,
    intptr_t Y,
    intptr_t MaxHeight,
    FARMENUFLAGS Flags,
    const wchar_t* Title,
    const wchar_t* Bottom,
    const wchar_t* HelpTopic,
    const FarKey* BreakKeys,
    intptr_t* BreakCode,
    const FarMenuItem* Item,
    size_t ItemsNumber);

using FARHELPFLAGS = unsigned long long;
constexpr FARHELPFLAGS FHELP_SELFHELP    = 0x0000000000000000ULL;
constexpr FARHELPFLAGS FHELP_USECONTENTS = 0x0000000040000000ULL;

using FARAPISHOWHELP = BOOL (WINAPI *)(
    const wchar_t* ModuleName,
    const wchar_t* Topic,
    FARHELPFLAGS Flags);

// Exact public PluginStartupInfo prefix through SettingsControl.
// Unused callbacks are pointer-sized placeholders to preserve offsets.
struct PluginStartupInfo
{
    size_t StructSize;
    const wchar_t* ModuleName;
    FARAPIMENU Menu;
    FARAPIMESSAGE Message;
    FARAPIGETMSG GetMsg;
    FARAPIPANELCONTROL PanelControl;
    void* SaveScreen;
    void* RestoreScreen;
    void* GetDirList;
    void* GetPluginDirList;
    void* FreeDirList;
    void* FreePluginDirList;
    void* Viewer;
    void* Editor;
    void* Text;
    void* EditorControl;
    void* FSF;
    FARAPISHOWHELP ShowHelp;
    FARAPIADVCONTROL AdvControl;
    FARAPIINPUTBOX InputBox;
    void* ColorDialog;
    FARAPIDIALOGINIT DialogInit;
    FARAPIDIALOGRUN DialogRun;
    FARAPIDIALOGFREE DialogFree;
    FARAPISENDDLGMESSAGE SendDlgMessage;
    FARAPIDEFDLGPROC DefDlgProc;
    void* ViewerControl;
    void* PluginsControl;
    void* FileFilterControl;
    void* RegExpControl;
    void* MacroControl;
    FARAPISETTINGSCONTROL SettingsControl;
};

static_assert(sizeof(void*) == 4 || sizeof(void*) == 8, "Unsupported Windows ABI pointer size");

#if INTPTR_MAX == INT32_MAX
static_assert(sizeof(GlobalInfo) == 76, "Unexpected x86 GlobalInfo ABI layout");
static_assert(sizeof(PluginInfo) == 64, "Unexpected x86 PluginInfo ABI layout");
static_assert(sizeof(FarPanelDirectory) == 32, "Unexpected x86 FarPanelDirectory ABI layout");
static_assert(sizeof(PluginPanelItem) == 112, "Unexpected x86 PluginPanelItem ABI layout");
static_assert(sizeof(FarGetPluginPanelItem) == 12, "Unexpected x86 FarGetPluginPanelItem ABI layout");
static_assert(sizeof(PanelInfo) == 80, "Unexpected x86 PanelInfo ABI layout");
static_assert(sizeof(FarSettingsCreate) == 24, "Unexpected x86 FarSettingsCreate ABI layout");
static_assert(sizeof(FarSettingsItem) == 32, "Unexpected x86 FarSettingsItem ABI layout");
static_assert(sizeof(FarDialogItem) == 64, "Unexpected x86 FarDialogItem ABI layout");
static_assert(sizeof(PluginStartupInfo) == 128, "Unexpected x86 PluginStartupInfo ABI prefix layout");
static_assert(sizeof(ProcessSynchroEventInfo) == 16, "Unexpected x86 ProcessSynchroEventInfo ABI layout");
#elif INTPTR_MAX == INT64_MAX
static_assert(sizeof(GlobalInfo) == 96, "Unexpected 64-bit GlobalInfo ABI layout");
static_assert(sizeof(PluginInfo) == 104, "Unexpected 64-bit PluginInfo ABI layout");
static_assert(sizeof(FarPanelDirectory) == 48, "Unexpected 64-bit FarPanelDirectory ABI layout");
static_assert(sizeof(PluginPanelItem) == 160, "Unexpected 64-bit PluginPanelItem ABI layout");
static_assert(sizeof(FarGetPluginPanelItem) == 24, "Unexpected 64-bit FarGetPluginPanelItem ABI layout");
static_assert(sizeof(PanelInfo) == 104, "Unexpected 64-bit PanelInfo ABI layout");
static_assert(sizeof(FarSettingsCreate) == 32, "Unexpected 64-bit FarSettingsCreate ABI layout");
static_assert(sizeof(FarSettingsItem) == 48, "Unexpected 64-bit FarSettingsItem ABI layout");
static_assert(sizeof(FarDialogItem) == 112, "Unexpected 64-bit FarDialogItem ABI layout");
static_assert(sizeof(PluginStartupInfo) == 256, "Unexpected 64-bit PluginStartupInfo ABI prefix layout");
static_assert(sizeof(ProcessSynchroEventInfo) == 32, "Unexpected 64-bit ProcessSynchroEventInfo ABI layout");
#endif
