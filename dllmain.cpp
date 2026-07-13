// ApiHooker.dll - injected into vmconnect.exe to control the Hyper-V connect bar.
//
// Three visibility modes, switchable at run time via a control window that the
// launcher GUI talks to:
//
//   Show     (0) - connect bar forced always visible (behaves like "pinned").
//   Minimise (1) - hook released + real pin unpinned, so vmconnect auto-hides it.
//   Hidden   (2) - connect bar forced permanently hidden.
//
// Show/Hidden are enforced by hooking ShowWindow on the bar (BBarWindowClass).
// Minimise additionally clicks the connect bar's real "pin" toolbar button.

#include "pch.h"
#include <Windows.h>
#include <detours.h>
#include <commctrl.h>   // TB_* toolbar messages / TBBUTTON
#include <stdio.h>      // swprintf_s

// ----- shared with the launcher (keep these identical in launcher.cpp) --------
#define MODE_SHOW      0
#define MODE_MINIMISE  1
#define MODE_HIDDEN    2
static const wchar_t* kCtlClass = L"APoxOnHypervConnectBar_Ctl";
static const wchar_t* kSetModeMsg = L"APoxOnHypervConnectBar_SetMode";
static const wchar_t* kRegPath = L"Software\\APoxOnHypervConnectBar";
static const wchar_t* kRegValue = L"Mode";
// -----------------------------------------------------------------------------

static HMODULE g_hModule = NULL;
static HANDLE  g_hookedMutex = NULL;      // marks THIS vmconnect.exe as already handled
static bool    g_hooked = false;          // did we actually install the detour?
static UINT    g_setModeMsg = 0;          // RegisterWindowMessage id (shared with GUI)
static volatile LONG g_mode = MODE_SHOW;  // current mode, read by the hook

static BOOL (WINAPI* Real_ShowWindow)(HWND hWnd, int nCmdShow) = ShowWindow;

// ---------------------------------------------------------------------------
// The ShowWindow hook - enforces Show / Hidden on the connect bar.
// ---------------------------------------------------------------------------
BOOL WINAPI hookedShowWindow(HWND hWnd, int nCmdShow)
{
    // Local buffer: the hook runs on whatever thread calls ShowWindow.
    wchar_t cls[64];
    if (GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"BBarWindowClass") == 0)
    {
        const LONG mode = g_mode;
        if (mode == MODE_HIDDEN)
        {
            // Suppress every attempt to show it -> stays hidden forever.
            if (nCmdShow != SW_HIDE)
                return TRUE;
        }
        else if (mode == MODE_SHOW)
        {
            // Suppress every attempt to hide it -> stays visible (like pinned).
            if (nCmdShow == SW_HIDE)
                return TRUE;
        }
        // MODE_MINIMISE: let vmconnect manage the bar normally.
    }
    return Real_ShowWindow(hWnd, nCmdShow);
}

void Hook()
{
    DetourRestoreAfterWith();
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Real_ShowWindow, hookedShowWindow);
    DetourTransactionCommit();
    g_hooked = true;
}

void UnHook()
{
    if (!g_hooked)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)Real_ShowWindow, hookedShowWindow);
    DetourTransactionCommit();
    g_hooked = false;
}

// ---------------------------------------------------------------------------
// Locating the connect bar and its pin button (this process only)
// ---------------------------------------------------------------------------
static BOOL CALLBACK findBBarChild(HWND hWnd, LPARAM lp)
{
    wchar_t cls[64] = {};
    GetClassNameW(hWnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"BBarWindowClass") == 0) { *reinterpret_cast<HWND*>(lp) = hWnd; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK findBBarTop(HWND hWnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == GetCurrentProcessId())
    {
        wchar_t cls[64] = {};
        GetClassNameW(hWnd, cls, ARRAYSIZE(cls));
        if (wcscmp(cls, L"BBarWindowClass") == 0) { *reinterpret_cast<HWND*>(lp) = hWnd; return FALSE; }
        EnumChildWindows(hWnd, findBBarChild, lp);
        if (*reinterpret_cast<HWND*>(lp)) return FALSE;
    }
    return TRUE;
}
static HWND FindBBar()
{
    HWND result = NULL;
    EnumWindows(findBBarTop, reinterpret_cast<LPARAM>(&result));
    return result;
}

// The pin is a check-style button on one of the bar's child toolbars.
struct PinButton { HWND toolbar; int command; int index; };
static BOOL CALLBACK findPinChild(HWND hWnd, LPARAM lp)
{
    PinButton* pin = reinterpret_cast<PinButton*>(lp);
    wchar_t cls[64] = {};
    GetClassNameW(hWnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"ToolbarWindow32") != 0)
        return TRUE;

    const int count = static_cast<int>(SendMessageW(hWnd, TB_BUTTONCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        TBBUTTON btn = {};
        if (SendMessageW(hWnd, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&btn)) &&
            (btn.fsStyle & BTNS_CHECK) && btn.idCommand != 0)
        {
            pin->toolbar = hWnd;
            pin->command = btn.idCommand;
            pin->index   = i;
            return FALSE;   // found the toggle (pin) button
        }
    }
    return TRUE;
}
static bool FindPin(HWND bbar, PinButton* out)
{
    PinButton pin = { NULL, 0, -1 };
    EnumChildWindows(bbar, findPinChild, reinterpret_cast<LPARAM>(&pin));
    if (pin.toolbar) { *out = pin; return true; }
    return false;
}

// Best-effort: make the connect bar's real pin match `pinned` by synthesizing a
// mouse click on the pin button (which both flips the toolbar's own checked
// state and fires vmconnect's pin handler, exactly like a real click). The bar
// must already be visible - callers show it first.
static void SetPin(HWND bbar, bool pinned)
{
    PinButton pin;
    if (!FindPin(bbar, &pin))
        return;
    const bool current = SendMessageW(pin.toolbar, TB_ISBUTTONCHECKED, pin.command, 0) != 0;
    if (current == pinned)
        return;
    RECT r = {};
    if (!SendMessageW(pin.toolbar, TB_GETITEMRECT, pin.index, reinterpret_cast<LPARAM>(&r)))
        return;
    const LPARAM pt = MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    SendMessageW(pin.toolbar, WM_LBUTTONDOWN, MK_LBUTTON, pt);
    SendMessageW(pin.toolbar, WM_LBUTTONUP, 0, pt);
}

// ---------------------------------------------------------------------------
// Apply a mode
// ---------------------------------------------------------------------------
static void ApplyMode(LONG mode)
{
    if (mode < MODE_SHOW || mode > MODE_HIDDEN)
        mode = MODE_SHOW;
    InterlockedExchange(&g_mode, mode);

    HWND bbar = FindBBar();
    if (!bbar)
        return;   // no bar yet; the hook will enforce the mode once it appears

    switch (mode)
    {
    case MODE_HIDDEN:
        Real_ShowWindow(bbar, SW_HIDE);
        break;
    case MODE_SHOW:
        Real_ShowWindow(bbar, SW_SHOW);   // show, then pin so it stays
        SetPin(bbar, true);
        break;
    case MODE_MINIMISE:
        Real_ShowWindow(bbar, SW_SHOW);   // un-stick (e.g. coming from Hidden)...
        SetPin(bbar, false);              // ...then unpin so vmconnect auto-hides
        break;
    }
}

static LONG ReadRegMode()
{
    DWORD value = MODE_SHOW, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegValue, RRF_RT_REG_DWORD,
                     NULL, &value, &size) != ERROR_SUCCESS)
        return MODE_SHOW;
    if (value > MODE_HIDDEN)
        value = MODE_SHOW;
    return static_cast<LONG>(value);
}

// ---------------------------------------------------------------------------
// Control window - lets the launcher change the mode without re-injecting.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK CtlWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_setModeMsg && g_setModeMsg != 0)
    {
        ApplyMode(static_cast<LONG>(wParam));
        return 1;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static DWORD WINAPI ControlThread(LPVOID)
{
    g_setModeMsg = RegisterWindowMessageW(kSetModeMsg);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = CtlWndProc;
    wc.hInstance     = g_hModule;
    wc.lpszClassName = kCtlClass;
    RegisterClassW(&wc);

    // Hidden, never-shown top-level window (findable cross-process via FindWindow).
    HWND hCtl = CreateWindowExW(0, kCtlClass, L"", 0, 0, 0, 0, 0,
                                NULL, NULL, g_hModule, NULL);

    ApplyMode(ReadRegMode());   // apply the last saved / default mode on startup

    MSG m;
    while (hCtl && GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_hModule = hModule;
        // Bail out if a previous injection already handled THIS process. The name
        // is per-process so a different vmconnect.exe still gets its own hook.
        wchar_t mutexName[96];
        swprintf_s(mutexName, L"Local\\APoxOnHypervConnectBar_Hooked_%lu", GetCurrentProcessId());
        g_hookedMutex = CreateMutexW(NULL, FALSE, mutexName);
        if (g_hookedMutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(g_hookedMutex);
            g_hookedMutex = NULL;
            return TRUE;
        }
        DisableThreadLibraryCalls(hModule);
        Hook();
        // Run the control window on its own thread (off the loader lock).
        if (HANDLE hThread = CreateThread(NULL, 0, ControlThread, NULL, 0, NULL))
            CloseHandle(hThread);
        break;
    }
    case DLL_PROCESS_DETACH:
        UnHook();
        if (g_hookedMutex)
        {
            CloseHandle(g_hookedMutex);
            g_hookedMutex = NULL;
        }
        break;
    }
    return TRUE;
}
