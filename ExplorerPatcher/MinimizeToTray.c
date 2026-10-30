// Minimize windows to the notification area, in the spirit of RBTray
// (https://github.com/benbuck/rbtray, Copyright (C) 1998-2010 Nikolay Redko, J.D. Purcell,
// (C) 2015 Benbuck Nason, GPL-2.0-or-later). This is a reimplementation that runs inside the
// shell explorer.exe instead of injecting a hook DLL into every process:
//
//  * A low-level mouse hook (WH_MOUSE_LL) on a dedicated thread detects a right-click on a
//    window's minimize button (or Shift + right-click on its title bar) by asking the window
//    under the cursor what is there (WM_NCHITTEST). This works for 32-bit and 64-bit apps alike
//    and loads nothing into other processes. The hook thread does nothing else, so mouse input
//    is never held up by slow work.
//  * A worker thread owns the tray icons, the Ctrl+Alt+Down hotkey and out-of-context WinEvent
//    hooks that notice when a hidden window is shown by someone else, renamed or destroyed.
//  * The windows we hid are recorded in a volatile registry key, so that if Explorer restarts
//    they get their tray icons back (or are shown again when the feature has been turned off),
//    instead of being stranded invisible.

#include "MinimizeToTray.h"
#include <shellapi.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "def.h"

#define MTT_REGPATH         TEXT(REGPATH) L"\\MinimizeToTray"
#define MTT_CLASSNAME       L"ExplorerPatcher_MinimizeToTray"
#define MTT_MAX_ITEMS       64

#define WM_MTT_MINIMIZE     (WM_APP + 1) // lParam = HWND to minimize to tray
#define WM_MTT_TRAYCMD      (WM_APP + 2) // tray icon callback (NOTIFYICON_VERSION_4)
#define WM_MTT_STOP         (WM_APP + 3) // restore everything and quit

#define MTT_HOTKEY_ID       1
#define MTT_TIMER_RETRY     1
#define MTT_RETRY_INTERVAL  2000
#define MTT_RETRY_MAX       30
#define MTT_HOOK_REARM_MS   (5 * 60 * 1000)
#define MTT_HITTEST_TIMEOUT 60

#define IDM_MTT_RESTORE     1
#define IDM_MTT_CLOSE       2
#define IDM_MTT_RESTOREALL  3

typedef struct
{
    HWND hWnd;
    BOOL bIconShown;
} MTT_ITEM;

// Worker thread state
static MTT_ITEM g_items[MTT_MAX_ITEMS];
static HWND g_hWndWorker;
static UINT g_uMsgTaskbarCreated;
static UINT g_uRetryCount;
static HWINEVENTHOOK g_hWinEventShowDestroy;
static HWINEVENTHOOK g_hWinEventNameChange;

// Hook thread state
static HANDLE g_hHookThread;
static DWORD g_dwHookThreadId;
static HWND g_hWndPending; // window whose minimize button got the right button down

// Owned by MinimizeToTray_SetEnabled
static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE g_hWorkerThread;

static HMODULE g_hModuleThis;

typedef BOOL (WINAPI *PhysicalToLogicalPointForPerMonitorDPI_t)(HWND, LPPOINT);
typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
static PhysicalToLogicalPointForPerMonitorDPI_t pfnPhysicalToLogicalPointForPerMonitorDPI;

static void MTT_SetThreadPerMonitorAware()
{
    SetThreadDpiAwarenessContext_t pfn = (SetThreadDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (pfn)
    {
        pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

#pragma region "Window helpers"
static BOOL MTT_IsExcludedWindow(HWND hWnd)
{
    if (!hWnd || hWnd == g_hWndWorker || hWnd == GetShellWindow() || hWnd == GetDesktopWindow())
    {
        return TRUE;
    }

    WCHAR wszClassName[64];
    if (!GetClassNameW(hWnd, wszClassName, ARRAYSIZE(wszClassName)))
    {
        return TRUE;
    }

    static const WCHAR* const rgpszExcluded[] = {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Progman",
        L"WorkerW",
        L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland",
        L"Windows.UI.Core.CoreWindow",
        L"ApplicationFrameWindow", // Store apps: hiding the frame breaks them (RBTray has the same limitation)
        MTT_CLASSNAME,
    };
    for (int i = 0; i < ARRAYSIZE(rgpszExcluded); ++i)
    {
        if (!_wcsicmp(wszClassName, rgpszExcluded[i]))
        {
            return TRUE;
        }
    }
    return FALSE;
}

// ShowWindow() on another thread's window waits for that thread; never wait on a hung one
static void MTT_ShowWindow(HWND hWnd, int nCmdShow)
{
    if (IsHungAppWindow(hWnd))
    {
        ShowWindowAsync(hWnd, nCmdShow);
    }
    else
    {
        ShowWindow(hWnd, nCmdShow);
    }
}

static HICON MTT_GetWindowIcon(HWND hWnd, BOOL* pbOwned)
{
    *pbOwned = FALSE;

    static const WPARAM rgType[] = { ICON_SMALL2, ICON_SMALL, ICON_BIG };
    for (int i = 0; i < ARRAYSIZE(rgType); ++i)
    {
        DWORD_PTR dwResult = 0;
        if (SendMessageTimeoutW(hWnd, WM_GETICON, rgType[i], 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 200, &dwResult) && dwResult)
        {
            return (HICON)dwResult;
        }
    }

    HICON hIcon = (HICON)GetClassLongPtrW(hWnd, GCLP_HICONSM);
    if (!hIcon)
    {
        hIcon = (HICON)GetClassLongPtrW(hWnd, GCLP_HICON);
    }
    if (hIcon)
    {
        return hIcon;
    }

    // Many apps (Electron, Java, ...) only have their icon in their executable
    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hWnd, &dwProcessId);
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
    if (hProcess)
    {
        WCHAR wszPath[MAX_PATH];
        DWORD cchPath = ARRAYSIZE(wszPath);
        if (QueryFullProcessImageNameW(hProcess, 0, wszPath, &cchPath))
        {
            SHFILEINFOW sfi;
            ZeroMemory(&sfi, sizeof(sfi));
            if (SHGetFileInfoW(wszPath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon)
            {
                hIcon = sfi.hIcon;
                *pbOwned = TRUE;
            }
        }
        CloseHandle(hProcess);
    }
    if (hIcon)
    {
        return hIcon;
    }

    return LoadIconW(NULL, IDI_APPLICATION);
}

static void MTT_GetWindowTip(HWND hWnd, WCHAR* pszTip, int cchTip)
{
    pszTip[0] = 0;
    // Unlike GetWindowText, this never sends a message, so a hung window can't block us
    InternalGetWindowText(hWnd, pszTip, cchTip);
    if (!pszTip[0])
    {
        wcsncpy_s(pszTip, cchTip, L"Hidden window", _TRUNCATE);
    }
}
#pragma endregion

#pragma region "Persistence (volatile, survives Explorer restarts but not logoff)"
static void MTT_ValueNameForWindow(HWND hWnd, WCHAR* pszName, size_t cchName)
{
    swprintf_s(pszName, cchName, L"%llX", (unsigned long long)(ULONG_PTR)hWnd);
}

static void MTT_PersistAdd(HWND hWnd)
{
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MTT_REGPATH, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        WCHAR wszName[32];
        MTT_ValueNameForWindow(hWnd, wszName, ARRAYSIZE(wszName));
        // Store the owning process id so a recycled window handle is never mistaken for ours
        DWORD dwProcessId = 0;
        GetWindowThreadProcessId(hWnd, &dwProcessId);
        RegSetValueExW(hKey, wszName, 0, REG_DWORD, (const BYTE*)&dwProcessId, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

static void MTT_PersistRemove(HWND hWnd)
{
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MTT_REGPATH, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        WCHAR wszName[32];
        MTT_ValueNameForWindow(hWnd, wszName, ARRAYSIZE(wszName));
        RegDeleteValueW(hKey, wszName);
        RegCloseKey(hKey);
    }
}

typedef void (*MTT_PersistedWindowCallback)(HWND hWnd);

// Calls pfnCallback for every recorded window that still exists, belongs to the same process
// and is still hidden; then clears the record (callers re-add what they keep).
static void MTT_ConsumePersistedWindows(MTT_PersistedWindowCallback pfnCallback)
{
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MTT_REGPATH, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
    {
        return;
    }

    HWND rghWnd[MTT_MAX_ITEMS];
    int cWnd = 0;
    for (DWORD dwIndex = 0; cWnd < MTT_MAX_ITEMS; ++dwIndex)
    {
        WCHAR wszName[32];
        DWORD cchName = ARRAYSIZE(wszName);
        DWORD dwType = 0, dwData = 0, cbData = sizeof(DWORD);
        LSTATUS ls = RegEnumValueW(hKey, dwIndex, wszName, &cchName, NULL, &dwType, (BYTE*)&dwData, &cbData);
        if (ls == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if (ls != ERROR_SUCCESS || dwType != REG_DWORD)
        {
            continue;
        }

        HWND hWnd = (HWND)(ULONG_PTR)wcstoull(wszName, NULL, 16);
        DWORD dwProcessId = 0;
        if (hWnd && IsWindow(hWnd) && GetWindowThreadProcessId(hWnd, &dwProcessId) && dwProcessId == dwData && !IsWindowVisible(hWnd))
        {
            rghWnd[cWnd++] = hWnd;
        }
    }
    RegCloseKey(hKey);
    RegDeleteKeyExW(HKEY_CURRENT_USER, MTT_REGPATH, KEY_WOW64_64KEY, 0);

    for (int i = 0; i < cWnd; ++i)
    {
        pfnCallback(rghWnd[i]);
    }
}

static void MTT_ShowOrphan(HWND hWnd)
{
    MTT_ShowWindow(hWnd, SW_SHOWMINNOACTIVE);
}
#pragma endregion

#pragma region "Tray icons (worker thread)"
static int MTT_FindItem(HWND hWnd)
{
    for (int i = 0; i < MTT_MAX_ITEMS; ++i)
    {
        if (g_items[i].hWnd == hWnd)
        {
            return i;
        }
    }
    return -1;
}

static BOOL MTT_ShowIcon(int i)
{
    HWND hWnd = g_items[i].hWnd;

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWndWorker;
    nid.uID = (UINT)i + 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_MTT_TRAYCMD;
    BOOL bOwnedIcon = FALSE;
    nid.hIcon = MTT_GetWindowIcon(hWnd, &bOwnedIcon);
    MTT_GetWindowTip(hWnd, nid.szTip, ARRAYSIZE(nid.szTip));

    BOOL bOk = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!bOk)
    {
        // Already there (e.g. we raced with TaskbarCreated)
        bOk = Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (bOk)
    {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    if (bOwnedIcon)
    {
        DestroyIcon(nid.hIcon); // the notification area keeps its own copy
    }

    g_items[i].bIconShown = bOk;
    return bOk;
}

static void MTT_HideIcon(int i)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWndWorker;
    nid.uID = (UINT)i + 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_items[i].bIconShown = FALSE;
}

static void MTT_UpdateTip(int i)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWndWorker;
    nid.uID = (UINT)i + 1;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    MTT_GetWindowTip(g_items[i].hWnd, nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Stop tracking a window without touching it
static void MTT_Forget(int i)
{
    HWND hWnd = g_items[i].hWnd;
    if (g_items[i].bIconShown)
    {
        MTT_HideIcon(i);
    }
    g_items[i].hWnd = NULL;
    g_items[i].bIconShown = FALSE;
    MTT_PersistRemove(hWnd);
}

static void MTT_EnsureRetryTimer()
{
    g_uRetryCount = 0;
    SetTimer(g_hWndWorker, MTT_TIMER_RETRY, MTT_RETRY_INTERVAL, NULL);
}

// Track a window that is already hidden (adopted from a previous Explorer instance)
static void MTT_Adopt(HWND hWnd)
{
    if (MTT_FindItem(hWnd) != -1)
    {
        return;
    }
    int i = MTT_FindItem(NULL);
    if (i == -1)
    {
        MTT_ShowOrphan(hWnd);
        return;
    }
    g_items[i].hWnd = hWnd;
    g_items[i].bIconShown = FALSE;
    MTT_PersistAdd(hWnd);
    if (!MTT_ShowIcon(i))
    {
        // The taskbar probably isn't up yet; retry until it is
        MTT_EnsureRetryTimer();
    }
}

static void MTT_MinimizeToTray(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd))
    {
        return;
    }
    // Don't minimize MDI child windows; for other child windows (e.g. a ribbon hosting the
    // caption buttons), act on the top-level window
    if (GetWindowLongPtrW(hWnd, GWL_EXSTYLE) & WS_EX_MDICHILD)
    {
        return;
    }
    if (GetWindowLongPtrW(hWnd, GWL_STYLE) & WS_CHILD)
    {
        hWnd = GetAncestor(hWnd, GA_ROOT);
    }
    if (!hWnd || MTT_IsExcludedWindow(hWnd) || IsHungAppWindow(hWnd))
    {
        return;
    }

    if (MTT_FindItem(hWnd) == -1 && MTT_FindItem(NULL) == -1)
    {
        MessageBeep(MB_ICONWARNING); // too many windows in the tray already
        return;
    }

    // Hide first, then claim a slot: WinEvents can be dispatched while ShowWindow waits on the
    // other app, and a stale "shown" event must not find the window tracked while still visible
    MTT_ShowWindow(hWnd, SW_MINIMIZE);
    MTT_ShowWindow(hWnd, SW_HIDE);
    if (IsWindowVisible(hWnd))
    {
        return; // could not hide it (e.g. a window of an elevated app)
    }

    int i = MTT_FindItem(hWnd);
    if (i == -1)
    {
        i = MTT_FindItem(NULL);
        if (i != -1)
        {
            g_items[i].hWnd = hWnd;
            g_items[i].bIconShown = FALSE;
        }
    }
    if (i != -1 && (g_items[i].bIconShown || MTT_ShowIcon(i)))
    {
        MTT_PersistAdd(hWnd);
        return;
    }

    // No icon means no way back: put the window back on the taskbar
    if (i != -1)
    {
        g_items[i].hWnd = NULL;
    }
    MTT_ShowWindow(hWnd, SW_RESTORE);
    MTT_ShowWindow(hWnd, SW_SHOW);
}

static void MTT_Restore(int i, BOOL bActivate)
{
    HWND hWnd = g_items[i].hWnd;
    MTT_Forget(i); // before showing, so EVENT_OBJECT_SHOW finds nothing to do
    if (!IsWindow(hWnd))
    {
        return;
    }
    if (bActivate)
    {
        MTT_ShowWindow(hWnd, SW_RESTORE);
        MTT_ShowWindow(hWnd, SW_SHOW);
        SetForegroundWindow(hWnd);
    }
    else
    {
        MTT_ShowWindow(hWnd, SW_SHOWMINNOACTIVE);
    }
}

static void MTT_RestoreAll(BOOL bActivate)
{
    for (int i = 0; i < MTT_MAX_ITEMS; ++i)
    {
        if (g_items[i].hWnd)
        {
            MTT_Restore(i, bActivate);
        }
    }
}

static void MTT_ShowContextMenu(int i)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
    {
        return;
    }
    AppendMenuW(hMenu, MF_STRING, IDM_MTT_RESTORE, L"&Restore window");
    AppendMenuW(hMenu, MF_STRING, IDM_MTT_CLOSE, L"&Close window");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_MTT_RESTOREALL, L"Restore &all windows");
    SetMenuDefaultItem(hMenu, IDM_MTT_RESTORE, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hWndWorker); // required so the menu closes when clicking elsewhere
    UINT uFlags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN
        | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    UINT uCmd = (UINT)TrackPopupMenuEx(hMenu, uFlags, pt.x, pt.y, g_hWndWorker, NULL);
    PostMessageW(g_hWndWorker, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    // The window may have gone away while the menu was open
    if (i < 0 || i >= MTT_MAX_ITEMS || !g_items[i].hWnd)
    {
        return;
    }
    switch (uCmd)
    {
        case IDM_MTT_RESTORE:
            MTT_Restore(i, TRUE);
            break;
        case IDM_MTT_CLOSE:
            // Post (not send) so an "are you sure?" prompt can't block us; the WinEvent hook
            // removes the icon once the window is actually destroyed
            PostMessageW(g_items[i].hWnd, WM_CLOSE, 0, 0);
            break;
        case IDM_MTT_RESTOREALL:
            MTT_RestoreAll(TRUE);
            break;
    }
}

static void CALLBACK MTT_WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hWnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    if (!hWnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
    {
        return;
    }
    int i = MTT_FindItem(hWnd);
    if (i == -1)
    {
        return;
    }
    switch (event)
    {
        case EVENT_OBJECT_DESTROY:
            MTT_Forget(i);
            break;
        case EVENT_OBJECT_SHOW:
            // The app showed itself again (e.g. launched a second time): drop our icon.
            // Events arrive asynchronously, so check the current state.
            if (IsWindowVisible(hWnd))
            {
                MTT_Forget(i);
            }
            break;
        case EVENT_OBJECT_NAMECHANGE:
            if (g_items[i].bIconShown)
            {
                MTT_UpdateTip(i);
            }
            break;
    }
}
#pragma endregion

#pragma region "Mouse hook thread"
static LRESULT MTT_NcHitTest(HWND hWnd, POINT ptPhysical)
{
    POINT pt = ptPhysical;
    if (pfnPhysicalToLogicalPointForPerMonitorDPI)
    {
        // The target may be DPI unaware/system aware and expect its own logical coordinates
        pfnPhysicalToLogicalPointForPerMonitorDPI(hWnd, &pt);
    }
    DWORD_PTR dwResult = HTNOWHERE;
    // Short timeout: the whole system's mouse input waits for this low-level hook
    if (!SendMessageTimeoutW(hWnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, MTT_HITTEST_TIMEOUT, &dwResult))
    {
        return HTNOWHERE;
    }
    return (LRESULT)dwResult;
}

// Returns the top-level window to minimize if the point is on its minimize button
// (or on its title bar while Shift is held), otherwise NULL
static HWND MTT_HitTest(POINT pt)
{
    HWND hWndAtPoint = WindowFromPoint(pt);
    if (!hWndAtPoint)
    {
        return NULL;
    }
    HWND hWndRoot = GetAncestor(hWndAtPoint, GA_ROOT);
    if (!hWndRoot || MTT_IsExcludedWindow(hWndRoot))
    {
        return NULL;
    }

    BOOL bShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    LRESULT ht = MTT_NcHitTest(hWndAtPoint, pt);
    if (ht == HTMINBUTTON || (bShift && ht == HTCAPTION))
    {
        return hWndRoot;
    }
    if (hWndAtPoint != hWndRoot && (ht == HTCLIENT || ht == HTTRANSPARENT || ht == HTNOWHERE))
    {
        ht = MTT_NcHitTest(hWndRoot, pt);
        if (ht == HTMINBUTTON || (bShift && ht == HTCAPTION))
        {
            return hWndRoot;
        }
    }
    return NULL;
}

static LRESULT CALLBACK MTT_LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const MSLLHOOKSTRUCT* pInfo = (const MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_RBUTTONDOWN)
        {
            g_hWndPending = MTT_HitTest(pInfo->pt);
            if (g_hWndPending)
            {
                return 1; // swallow, so the app doesn't open its window menu
            }
        }
        else if (wParam == WM_RBUTTONUP && g_hWndPending)
        {
            HWND hWnd = g_hWndPending;
            g_hWndPending = NULL;
            // Like a normal button: only act if released over the same window
            HWND hWndAtPoint = WindowFromPoint(pInfo->pt);
            if (hWndAtPoint && GetAncestor(hWndAtPoint, GA_ROOT) == hWnd)
            {
                PostMessageW(g_hWndWorker, WM_MTT_MINIMIZE, 0, (LPARAM)hWnd);
            }
            return 1;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI MTT_HookThreadProc(LPVOID lpParameter)
{
    HANDLE hReady = (HANDLE)lpParameter;
    // Low-level hook coordinates are physical; make WindowFromPoint interpret them that way
    MTT_SetThreadPerMonitorAware();

    MSG msg;
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE); // create the message queue
    SetEvent(hReady);

    HHOOK hHook = SetWindowsHookExW(WH_MOUSE_LL, MTT_LowLevelMouseProc, g_hModuleThis, 0);
    // Windows silently removes a low-level hook that once took too long; re-arm periodically
    UINT_PTR uTimer = SetTimer(NULL, 0, MTT_HOOK_REARM_MS, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (msg.message == WM_TIMER && msg.hwnd == NULL)
        {
            if (hHook)
            {
                UnhookWindowsHookEx(hHook);
            }
            g_hWndPending = NULL;
            hHook = SetWindowsHookExW(WH_MOUSE_LL, MTT_LowLevelMouseProc, g_hModuleThis, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (uTimer)
    {
        KillTimer(NULL, uTimer);
    }
    if (hHook)
    {
        UnhookWindowsHookEx(hHook);
    }
    return 0;
}
#pragma endregion

#pragma region "Worker thread"
static void MTT_StopHookThread()
{
    if (g_hHookThread)
    {
        PostThreadMessageW(g_dwHookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hHookThread, 2000);
        CloseHandle(g_hHookThread);
        g_hHookThread = NULL;
        g_dwHookThreadId = 0;
    }
}

static LRESULT CALLBACK MTT_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_MTT_MINIMIZE:
            MTT_MinimizeToTray((HWND)lParam);
            return 0;

        case WM_HOTKEY:
            if (wParam == MTT_HOTKEY_ID)
            {
                HWND hWndForeground = GetForegroundWindow();
                if (hWndForeground && (GetWindowLongPtrW(hWndForeground, GWL_STYLE) & WS_MINIMIZEBOX))
                {
                    MTT_MinimizeToTray(hWndForeground);
                }
            }
            return 0;

        case WM_MTT_TRAYCMD:
        {
            int i = (int)HIWORD(lParam) - 1;
            if (i < 0 || i >= MTT_MAX_ITEMS || !g_items[i].hWnd)
            {
                return 0;
            }
            switch (LOWORD(lParam))
            {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    MTT_Restore(i, TRUE);
                    break;
                case WM_CONTEXTMENU:
                    MTT_ShowContextMenu(i);
                    break;
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == MTT_TIMER_RETRY)
            {
                BOOL bPending = FALSE;
                for (int i = 0; i < MTT_MAX_ITEMS; ++i)
                {
                    if (!g_items[i].hWnd || g_items[i].bIconShown)
                    {
                        continue;
                    }
                    if (!IsWindow(g_items[i].hWnd))
                    {
                        MTT_Forget(i);
                    }
                    else if (!MTT_ShowIcon(i))
                    {
                        bPending = TRUE;
                    }
                }
                if (bPending && ++g_uRetryCount >= MTT_RETRY_MAX)
                {
                    // Give up and bring back anything that still has no icon, so nothing stays lost
                    for (int i = 0; i < MTT_MAX_ITEMS; ++i)
                    {
                        if (g_items[i].hWnd && !g_items[i].bIconShown)
                        {
                            MTT_Restore(i, FALSE);
                        }
                    }
                    bPending = FALSE;
                }
                if (!bPending)
                {
                    KillTimer(hWnd, MTT_TIMER_RETRY);
                }
            }
            return 0;

        case WM_MTT_STOP:
            MTT_StopHookThread();
            UnregisterHotKey(hWnd, MTT_HOTKEY_ID);
            KillTimer(hWnd, MTT_TIMER_RETRY);
            MTT_RestoreAll(FALSE);
            RegDeleteKeyExW(HKEY_CURRENT_USER, MTT_REGPATH, KEY_WOW64_64KEY, 0);
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    if (uMsg == g_uMsgTaskbarCreated && g_uMsgTaskbarCreated)
    {
        // The notification area was (re)created: our icons are gone, add them again
        BOOL bPending = FALSE;
        for (int i = 0; i < MTT_MAX_ITEMS; ++i)
        {
            if (g_items[i].hWnd)
            {
                g_items[i].bIconShown = FALSE;
                if (!MTT_ShowIcon(i))
                {
                    bPending = TRUE;
                }
            }
        }
        if (bPending)
        {
            MTT_EnsureRetryTimer();
        }
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static DWORD WINAPI MTT_WorkerThreadProc(LPVOID lpParameter)
{
    HANDLE hReady = (HANDLE)lpParameter;
    MTT_SetThreadPerMonitorAware();
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); // for SHGetFileInfo

    ZeroMemory(g_items, sizeof(g_items));
    g_uMsgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MTT_WndProc;
    wc.hInstance = g_hModuleThis;
    wc.lpszClassName = MTT_CLASSNAME;
    RegisterClassExW(&wc); // may already exist if the feature was turned off and on again

    // A real (hidden) top-level window, not message-only, so it receives the TaskbarCreated broadcast
    g_hWndWorker = CreateWindowExW(WS_EX_TOOLWINDOW, MTT_CLASSNAME, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_hModuleThis, NULL);
    if (!g_hWndWorker)
    {
        SetEvent(hReady);
        if (SUCCEEDED(hrCom))
        {
            CoUninitialize();
        }
        return 0;
    }

    g_hWinEventShowDestroy = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW, NULL, MTT_WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hWinEventNameChange = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL, MTT_WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    RegisterHotKey(g_hWndWorker, MTT_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DOWN); // best effort; may be taken

    // Windows a previous Explorer instance hid
    MTT_ConsumePersistedWindows(MTT_Adopt);

    HANDLE hHookReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (hHookReady)
    {
        g_hHookThread = CreateThread(NULL, 0, MTT_HookThreadProc, hHookReady, 0, &g_dwHookThreadId);
        if (g_hHookThread)
        {
            WaitForSingleObject(hHookReady, 5000); // so WM_QUIT can be posted to it reliably
        }
        CloseHandle(hHookReady);
    }

    SetEvent(hReady);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    MTT_StopHookThread();
    if (g_hWinEventShowDestroy)
    {
        UnhookWinEvent(g_hWinEventShowDestroy);
        g_hWinEventShowDestroy = NULL;
    }
    if (g_hWinEventNameChange)
    {
        UnhookWinEvent(g_hWinEventNameChange);
        g_hWinEventNameChange = NULL;
    }
    g_hWndWorker = NULL;
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
    return 0;
}
#pragma endregion

void MinimizeToTray_SetEnabled(BOOL bEnabled)
{
    AcquireSRWLockExclusive(&g_lock);

    if (!g_hModuleThis)
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)MinimizeToTray_SetEnabled, &g_hModuleThis);
        pfnPhysicalToLogicalPointForPerMonitorDPI = (PhysicalToLogicalPointForPerMonitorDPI_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PhysicalToLogicalPointForPerMonitorDPI");
    }

    if (bEnabled && !g_hWorkerThread)
    {
        HANDLE hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (hReady)
        {
            g_hWorkerThread = CreateThread(NULL, 0, MTT_WorkerThreadProc, hReady, 0, NULL);
            if (g_hWorkerThread)
            {
                WaitForSingleObject(hReady, 10000);
                if (!g_hWndWorker)
                {
                    // Window creation failed and the thread already ended
                    WaitForSingleObject(g_hWorkerThread, 1000);
                    CloseHandle(g_hWorkerThread);
                    g_hWorkerThread = NULL;
                }
            }
            CloseHandle(hReady);
        }
    }
    else if (!bEnabled && g_hWorkerThread)
    {
        if (g_hWndWorker)
        {
            PostMessageW(g_hWndWorker, WM_MTT_STOP, 0, 0);
        }
        WaitForSingleObject(g_hWorkerThread, 10000);
        CloseHandle(g_hWorkerThread);
        g_hWorkerThread = NULL;
    }
    else if (!bEnabled)
    {
        // Feature is off: bring back anything a previous Explorer instance left hidden
        MTT_ConsumePersistedWindows(MTT_ShowOrphan);
    }

    ReleaseSRWLockExclusive(&g_lock);
}
