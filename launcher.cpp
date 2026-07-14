// APoxOnHyperVConnectBar - a tiny GUI micro-app to control the Hyper-V VM
// connect bar. The ApiHooker.dll payload is embedded in this exe as a resource;
// picking a connect-bar mode extracts + injects it into every running
// vmconnect.exe (once each) and then talks to each through a hidden control
// window so later changes don't re-inject.
//
//   Show     - connect bar always visible (pinned)
//   Minimise - connect bar auto-hides (unpinned)
//   Hidden   - connect bar permanently hidden
//   Tray     - no connect-bar hook; the app lives in the system tray and its
//              icon minimises / restores the whole VM window on click
//
// Run it on the Hyper-V HOST (that is where vmconnect.exe / the VM window live).

#include <Windows.h>
#include <Psapi.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Psapi.lib")

// ----- shared with the DLL (connect-bar modes; keep identical in dllmain.cpp) --
#define MODE_SHOW      0
#define MODE_MINIMISE  1
#define MODE_HIDDEN    2
// Launcher-only mode (the DLL never sees it):
#define MODE_TRAY      3
static const wchar_t* kCtlClass   = L"APoxOnHyperVConnectBar_Ctl";
static const wchar_t* kSetModeMsg = L"APoxOnHyperVConnectBar_SetMode";
static const wchar_t* kRegPath    = L"Software\\APoxOnHyperVConnectBar";
static const wchar_t* kRegValue   = L"Mode";
// -----------------------------------------------------------------------------

static const wchar_t* kTargetProcess = L"vmconnect.exe";
static const wchar_t* kElevatedFlag  = L"--elevated";
static const wchar_t* kApplyFlag     = L"--apply";

// control ids / messages
#define IDC_RADIO_SHOW   1001
#define IDC_RADIO_MIN    1002
#define IDC_RADIO_HIDDEN 1003
#define IDC_RADIO_TRAY   1004
#define IDC_APPLY        1010
#define IDC_HELPBTN      1011
#define IDM_TRAY_MINIMISE 2001
#define IDM_TRAY_RESTORE  2002
#define IDM_TRAY_SETTINGS 2003
#define IDM_TRAY_ELEVATE  2004
#define IDM_TRAY_EXIT     2005
#define WM_APP_AUTOAPPLY (WM_APP + 1)
#define WM_APP_TRAY      (WM_APP + 2)
#define TRAY_UID         1

static HINSTANCE g_hInst    = NULL;
static HWND      g_mainWnd  = NULL;
static HWND      g_status   = NULL;
static HFONT     g_font     = NULL;
static HWND      g_helpWnd  = NULL;
static bool      g_elevated = false;
static bool      g_trayActive = false;
static HICON     g_iconBig   = NULL;   // application icon (large, for taskbar / alt-tab)
static HICON     g_iconSmall = NULL;   // application icon (small, for title bar / tray)
static UINT      g_taskbarCreated = 0; // "TaskbarCreated" broadcast - re-add tray on Explorer restart

static const wchar_t* ModeName(int m)
{
    switch (m)
    {
    case MODE_HIDDEN:   return L"Hidden";
    case MODE_MINIMISE: return L"Minimise";
    case MODE_TRAY:     return L"Tray";
    default:            return L"Show";
    }
}
static int RadioIdForMode(int m)
{
    switch (m)
    {
    case MODE_HIDDEN:   return IDC_RADIO_HIDDEN;
    case MODE_MINIMISE: return IDC_RADIO_MIN;
    case MODE_TRAY:     return IDC_RADIO_TRAY;
    default:            return IDC_RADIO_SHOW;
    }
}

// ===========================================================================
// Win32 helpers (process discovery, elevation, embedded DLL, injection)
// ===========================================================================
static bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix)
{
    if (suffix.size() > s.size()) return false;
    return _wcsicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str()) == 0;
}

static void EnableDebugPrivilege()
{
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;
    LUID luid;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid))
    {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(hToken);
}

static bool IsProcessElevated()
{
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    TOKEN_ELEVATION elev = {};
    DWORD sz = 0;
    bool result = false;
    if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &sz))
        result = elev.TokenIsElevated != 0;
    CloseHandle(hToken);
    return result;
}

// Is another process (by pid) elevated? Used to detect the UIPI boundary before
// posting to a VM window - a blocked WM_SYSCOMMAND doesn't report failure.
static bool IsProcessElevatedByPid(DWORD pid)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    HANDLE hToken = NULL;
    bool result = false;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION elev = {};
        DWORD sz = 0;
        if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &sz))
            result = elev.TokenIsElevated != 0;
        CloseHandle(hToken);
    }
    CloseHandle(hProc);
    return result;
}

static bool ProcessIsVmConnect(DWORD pid)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    wchar_t name[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const bool isVm = QueryFullProcessImageNameW(hProc, 0, name, &len) && EndsWithNoCase(name, kTargetProcess);
    CloseHandle(hProc);
    return isVm;
}

// Every running vmconnect.exe (one per connected VM window).
static std::vector<DWORD> FindTargetPids()
{
    std::vector<DWORD> out;
    DWORD pids[4096] = {};
    DWORD needed = 0;
    if (!EnumProcesses(pids, sizeof(pids), &needed))
        return out;
    const DWORD count = needed / sizeof(DWORD);
    for (DWORD i = 0; i < count; ++i)
        if (pids[i] != 0 && ProcessIsVmConnect(pids[i]))
            out.push_back(pids[i]);
    return out;
}

// The control window (if any) belonging to a specific vmconnect.exe pid.
struct CtlSearch { DWORD pid; HWND found; };
static BOOL CALLBACK ctlEnum(HWND hWnd, LPARAM lp)
{
    CtlSearch* s = reinterpret_cast<CtlSearch*>(lp);
    wchar_t cls[64] = {};
    GetClassNameW(hWnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, kCtlClass) == 0)
    {
        DWORD p = 0;
        GetWindowThreadProcessId(hWnd, &p);
        if (p == s->pid) { s->found = hWnd; return FALSE; }
    }
    return TRUE;
}
static HWND FindCtlForPid(DWORD pid)
{
    CtlSearch s = { pid, NULL };
    EnumWindows(ctlEnum, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

// The top-level VM window(s) of vmconnect.exe (what a user would minimise).
static BOOL CALLBACK vmWinEnum(HWND hWnd, LPARAM lp)
{
    std::vector<HWND>* v = reinterpret_cast<std::vector<HWND>*>(lp);
    if (!IsWindowVisible(hWnd)) return TRUE;
    if (GetWindow(hWnd, GW_OWNER) != NULL) return TRUE;                 // skip owned dialogs
    if (GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindowTextLengthW(hWnd) == 0) return TRUE;                   // main window has the VM name
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (ProcessIsVmConnect(pid)) v->push_back(hWnd);
    return TRUE;
}
static std::vector<HWND> FindVmWindows()
{
    std::vector<HWND> v;
    EnumWindows(vmWinEnum, reinterpret_cast<LPARAM>(&v));
    return v;
}

static std::wstring ExtractEmbeddedDll()
{
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_APIHOOKER_DLL), RT_RCDATA);
    if (!hRes) return L"";
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return L"";
    const DWORD size = SizeofResource(NULL, hRes);
    const void* bytes = LockResource(hData);
    if (!bytes || size == 0) return L"";

    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return L"";

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%sApiHooker.%lu.%llu.dll", tempDir, GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok = WriteFile(hFile, bytes, size, &written, NULL);
    CloseHandle(hFile);
    if (!ok || written != size) { DeleteFileW(path); return L""; }
    return path;
}

static void SweepOldTempDlls()
{
    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return;
    const std::wstring pattern = std::wstring(tempDir) + L"ApiHooker.*.dll";
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do { DeleteFileW((std::wstring(tempDir) + fd.cFileName).c_str()); } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

static bool InjectDll(DWORD pid, const std::wstring& dllPath, DWORD* outError)
{
    if (outError) *outError = 0;
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) { if (outError) *outError = GetLastError(); return false; }

    bool success = false;
    LPVOID remote = NULL;
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    do
    {
        remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) { if (outError) *outError = GetLastError(); break; }
        if (!WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, NULL)) { if (outError) *outError = GetLastError(); break; }
        FARPROC pLoad = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (!pLoad) { if (outError) *outError = GetLastError(); break; }
        HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoad), remote, 0, NULL);
        if (!hThread) { if (outError) *outError = GetLastError(); break; }
        const DWORD waited = WaitForSingleObject(hThread, 7000);
        DWORD remoteResult = 0;
        if (waited == WAIT_OBJECT_0 && GetExitCodeThread(hThread, &remoteResult) && remoteResult != 0)
            success = true;
        else if (outError)
            *outError = (waited == WAIT_OBJECT_0) ? ERROR_MOD_NOT_FOUND : WAIT_TIMEOUT;
        CloseHandle(hThread);
    } while (false);

    if (remote) VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return success;
}

static void SaveRegMode(int mode)
{
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD v = static_cast<DWORD>(mode);
        RegSetValueExW(hKey, kRegValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

static int ReadRegMode()
{
    DWORD v = MODE_SHOW, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegValue, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return MODE_SHOW;
    return (v > MODE_TRAY) ? MODE_SHOW : static_cast<int>(v);
}

static bool RelaunchElevated(int mode)
{
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) return false;
    wchar_t args[64] = {};
    swprintf_s(args, L"%s %s %d", kElevatedFlag, kApplyFlag, mode);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";
    sei.lpFile       = exe;
    sei.lpParameters = args;
    sei.nShow        = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

// ===========================================================================
// System-tray mode - minimise / restore the VM window
// ===========================================================================
static void TrayBalloon(const wchar_t* title, const wchar_t* text)
{
    if (!g_trayActive) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void AddTray()
{
    if (g_trayActive) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall ? g_iconSmall : LoadIconW(NULL, IDI_APPLICATION);
    // szTip is what shows on hover - lead with the program name.
    wcscpy_s(nid.szTip, L"APoxOnHyperVConnectBar - click to minimise/restore the VM window");
    if (Shell_NotifyIconW(NIM_ADD, &nid))
        g_trayActive = true;
}

static void RemoveTray()
{
    if (!g_trayActive) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayActive = false;
}

// Send SC_MINIMIZE / SC_RESTORE to the VM window(s). Returns true if any target
// existed; sets *denied if a send was blocked (elevated VM vs non-elevated app).
static bool SendToVmWindows(UINT sysCommand, bool* denied)
{
    if (denied) *denied = false;
    std::vector<HWND> wins = FindVmWindows();
    if (wins.empty()) return false;
    const bool onlyIconic = (sysCommand == SC_RESTORE);   // don't un-maximize on "Restore"
    for (HWND h : wins)
    {
        if (onlyIconic && !IsIconic(h)) continue;
        // A blocked WM_SYSCOMMAND to a higher-integrity window is silently dropped
        // and PostMessage still returns TRUE, so detect the boundary up front.
        if (denied && !g_elevated)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (IsProcessElevatedByPid(pid)) *denied = true;
        }
        PostMessageW(h, WM_SYSCOMMAND, sysCommand, 0);
    }
    return true;
}

static void NotifyIfDenied(bool denied)
{
    if (denied && !g_elevated)
        TrayBalloon(L"Administrator rights needed",
                    L"The VM window is elevated. Right-click the tray icon and choose \"Restart as administrator\".");
}

static void MinimiseVmWindows()
{
    bool denied = false;
    if (!SendToVmWindows(SC_MINIMIZE, &denied))
        TrayBalloon(L"APoxOnHyperVConnectBar", L"No VM window found. Open a VM in Hyper-V Manager.");
    else
        NotifyIfDenied(denied);
}
static void RestoreVmWindows()
{
    bool denied = false;
    if (!SendToVmWindows(SC_RESTORE, &denied))
        TrayBalloon(L"APoxOnHyperVConnectBar", L"No VM window found.");
    else
        NotifyIfDenied(denied);
}
// Left-click: minimise if any VM window is up, otherwise restore them.
static void ToggleVmWindows()
{
    std::vector<HWND> wins = FindVmWindows();
    if (wins.empty()) { TrayBalloon(L"APoxOnHyperVConnectBar", L"No VM window found. Open a VM in Hyper-V Manager."); return; }
    bool anyRestored = false;
    for (HWND h : wins) if (!IsIconic(h)) anyRestored = true;
    bool denied = false;
    SendToVmWindows(anyRestored ? SC_MINIMIZE : SC_RESTORE, &denied);
    NotifyIfDenied(denied);
}

static void ShowTrayMenu()
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_MINIMISE, L"Minimise VM window");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_RESTORE,  L"Restore VM window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (!g_elevated)
        AppendMenuW(menu, MF_STRING, IDM_TRAY_ELEVATE, L"Restart as administrator");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    SetForegroundWindow(g_mainWnd);   // so the menu dismisses on click-away
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_mainWnd, NULL);
    DestroyMenu(menu);
}

// ===========================================================================
// GUI
// ===========================================================================
static void SetStatus(const wchar_t* text)
{
    if (g_status) { SetWindowTextW(g_status, text); UpdateWindow(g_status); }
}

static int GetSelectedMode()
{
    if (IsDlgButtonChecked(g_mainWnd, IDC_RADIO_TRAY)   == BST_CHECKED) return MODE_TRAY;
    if (IsDlgButtonChecked(g_mainWnd, IDC_RADIO_HIDDEN) == BST_CHECKED) return MODE_HIDDEN;
    if (IsDlgButtonChecked(g_mainWnd, IDC_RADIO_MIN)    == BST_CHECKED) return MODE_MINIMISE;
    return MODE_SHOW;
}

// Ensure the DLL is present in every vmconnect.exe and apply the chosen mode.
static void DoApply(int mode)
{
    SaveRegMode(mode);

    if (mode == MODE_TRAY)
    {
        AddTray();
        if (!g_trayActive)   // shell/tray unavailable - don't strand an invisible window
        {
            ShowWindow(g_mainWnd, SW_SHOW);
            SetForegroundWindow(g_mainWnd);
            SetStatus(L"Could not create the system-tray icon. Please try again.");
            return;
        }
        ShowWindow(g_mainWnd, SW_HIDE);   // live in the tray
        TrayBalloon(L"APoxOnHyperVConnectBar",
                    L"Running in the tray. Click the icon to minimise / restore the VM window.");
        return;
    }

    if (g_trayActive) RemoveTray();       // switching away from Tray mode

    SetStatus(L"Working - attaching to vmconnect.exe...");
    HCURSOR oldCursor = SetCursor(LoadCursorW(NULL, IDC_WAIT));

    std::vector<DWORD> pids = FindTargetPids();
    if (pids.empty())
    {
        SetCursor(oldCursor);
        SetStatus(L"No VM window found.\r\nOpen / enter a VM in Hyper-V Manager, then click Apply again.");
        return;
    }

    const UINT setMode = RegisterWindowMessageW(kSetModeMsg);
    std::wstring dll;   // extracted lazily, reused for every injection
    int total = static_cast<int>(pids.size());
    int handled = 0;
    bool needElevation = false, avBlocked = false;

    for (DWORD pid : pids)
    {
        HWND ctl = FindCtlForPid(pid);
        bool freshInject = false;

        if (!ctl)
        {
            EnableDebugPrivilege();
            if (dll.empty())
            {
                SweepOldTempDlls();
                dll = ExtractEmbeddedDll();
                if (dll.empty()) { SetCursor(oldCursor); SetStatus(L"Error: could not unpack the hook component."); return; }
            }
            DWORD err = 0;
            if (InjectDll(pid, dll, &err))
            {
                freshInject = true;   // the DLL applies the saved mode from the registry on startup
                for (int i = 0; i < 40 && !ctl; ++i) { Sleep(50); ctl = FindCtlForPid(pid); }
            }
            else
            {
                if (err == ERROR_ACCESS_DENIED) needElevation = true;
                else if (err == ERROR_MOD_NOT_FOUND) avBlocked = true;
                continue;
            }
        }

        bool delivered = false;
        if (ctl)
        {
            DWORD_PTR res = 0;
            delivered = SendMessageTimeoutW(ctl, setMode, static_cast<WPARAM>(mode), 0,
                                            SMTO_ABORTIFHUNG, 3000, &res) != 0 && res == 1;
        }
        if (delivered || freshInject)
            ++handled;
    }

    SetCursor(oldCursor);

    if (needElevation && !g_elevated)
    {
        SetStatus(L"Administrator rights are required - relaunching...");
        if (RelaunchElevated(mode)) { DestroyWindow(g_mainWnd); return; }
    }

    wchar_t buf[220];
    if (handled == total)
        swprintf_s(buf, L"Applied: %s to %d VM window%s.\r\nReconnecting a VM resets it - just Apply again.",
                   ModeName(mode), total, total == 1 ? L"" : L"s");
    else if (handled > 0)
        swprintf_s(buf, L"Applied to %d of %d VM windows. Some could not be reached - see Help.", handled, total);
    else if (avBlocked)
        swprintf_s(buf, L"The hook could not load inside vmconnect.exe.\r\nAntivirus may have blocked it - see Help.");
    else
        swprintf_s(buf, L"Could not attach to vmconnect.exe.\r\nSee Help / Troubleshooting.");
    SetStatus(buf);
}

static const wchar_t* kHelpText =
    L"APoxOnHyperVConnectBar - Help & Troubleshooting\r\n"
    L"==============================================\r\n\r\n"
    L"WHAT IT DOES\r\n"
    L"This app controls the floating \"connect bar\" that Hyper-V's vmconnect.exe\r\n"
    L"shows at the top of a VM window, and can also minimise the VM window from the\r\n"
    L"system tray. Pick a mode and click Apply.\r\n\r\n"
    L"  * Show      - the connect bar is always visible (pinned).\r\n"
    L"  * Minimise  - the connect bar auto-hides (unpinned). Move the mouse to the\r\n"
    L"                top-centre of the screen to bring it back temporarily.\r\n"
    L"  * Hidden    - the connect bar is permanently hidden until you choose\r\n"
    L"                Show or Minimise again.\r\n"
    L"  * Tray      - the app moves to the system tray (Show hidden icons area).\r\n"
    L"                Left-click the tray icon to minimise the VM window; click it\r\n"
    L"                again to restore it. Right-click for a menu.\r\n\r\n"
    L"HOW TO USE\r\n"
    L"  1. Run this app on the Hyper-V HOST (the machine running Hyper-V), not\r\n"
    L"     inside the guest OS.\r\n"
    L"  2. Open / enter a VM in Hyper-V Manager so a VM window exists.\r\n"
    L"  3. Choose a mode and click Apply.\r\n"
    L"  4. To leave a full-screen VM, press Ctrl+Alt+Left Arrow.\r\n\r\n"
    L"THE TRAY MODE\r\n"
    L"  - After choosing Tray and clicking Apply, the app's window disappears and a\r\n"
    L"    small icon appears in the notification area (you may need to click the\r\n"
    L"    up-arrow \"Show hidden icons\" to see it).\r\n"
    L"  - Left-click the icon to minimise the VM window; left-click again to bring\r\n"
    L"    it back.\r\n"
    L"  - Right-click the icon for: Minimise / Restore VM window, Settings (re-open\r\n"
    L"    this window), and Exit.\r\n"
    L"  - Closing the window with X while in Tray mode just hides it back to the\r\n"
    L"    tray; use Exit on the tray menu to quit.\r\n"
    L"  - Tray mode does not touch the connect bar - it only minimises the whole VM\r\n"
    L"    window - so it needs no injection.\r\n\r\n"
    L"TROUBLESHOOTING\r\n"
    L"  \"No VM window found\"\r\n"
    L"     - Start and connect to a VM in Hyper-V Manager first, then try again.\r\n"
    L"     - Run it on the host, not inside the VM.\r\n\r\n"
    L"  A UAC prompt appears / \"Administrator rights required\"\r\n"
    L"     - vmconnect.exe is running elevated, so the app must elevate too. Accept\r\n"
    L"       the prompt (or right-click the app > Run as administrator). In Tray\r\n"
    L"       mode, use the tray menu's \"Restart as administrator\".\r\n\r\n"
    L"  \"Antivirus may have blocked it\" / injection blocked\r\n"
    L"     - The connect-bar modes inject a small DLL into vmconnect.exe, which some\r\n"
    L"       antivirus / EDR products block. Allow the app or add an exclusion.\r\n"
    L"       (Tray mode does not inject and is not affected.)\r\n\r\n"
    L"  Hidden works but Minimise doesn't auto-hide\r\n"
    L"     - Minimise clicks the connect bar's real pin button. If that doesn't\r\n"
    L"       take on your build of Windows, click the pin (thumb-tack) yourself.\r\n\r\n"
    L"  The bar / setting comes back after reconnecting\r\n"
    L"     - Settings last for that vmconnect.exe session. Reopen the VM, run the\r\n"
    L"       app and Apply again. Your last choice is remembered.\r\n\r\n"
    L"NOTE\r\n"
    L"  This is an unofficial tool. It changes nothing permanently on your system -\r\n"
    L"  it only affects the running vmconnect.exe / VM window.\r\n";

static LRESULT CALLBACK HelpWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HWND s_edit = NULL;
    switch (msg)
    {
    case WM_CREATE:
        s_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hWnd, NULL, g_hInst, NULL);
        SendMessageW(s_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SetWindowTextW(s_edit, kHelpText);
        return 0;
    case WM_SIZE:
        MoveWindow(s_edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    case WM_DESTROY:
        g_helpWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowHelp()
{
    if (g_helpWnd) { SetForegroundWindow(g_helpWnd); return; }
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = HelpWndProc;
        wc.hInstance     = g_hInst;
        wc.hIcon         = g_iconBig;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"APoxHelpWindow";
        RegisterClassW(&wc);
        registered = true;
    }
    g_helpWnd = CreateWindowExW(0, L"APoxHelpWindow", L"Help & Troubleshooting",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 560,
        g_mainWnd, NULL, g_hInst, NULL);
    ShowWindow(g_helpWnd, SW_SHOW);
}

static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, NULL);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

static void CreateControls(HWND hWnd)
{
    MakeChild(hWnd, L"STATIC", L"Choose how the Hyper-V connect bar should behave:",
              SS_LEFT, 16, 14, 400, 20, 0);

    MakeChild(hWnd, L"BUTTON", L"Show  -  connect bar always visible (pinned)",
              BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 44, 400, 22, IDC_RADIO_SHOW);
    MakeChild(hWnd, L"BUTTON", L"Minimise  -  connect bar auto-hides (unpinned)",
              BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 70, 400, 22, IDC_RADIO_MIN);
    MakeChild(hWnd, L"BUTTON", L"Hidden  -  connect bar permanently hidden",
              BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 96, 400, 22, IDC_RADIO_HIDDEN);
    MakeChild(hWnd, L"BUTTON", L"Tray  -  minimise the VM window from the system tray",
              BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 122, 400, 22, IDC_RADIO_TRAY);

    MakeChild(hWnd, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, 22, 160, 100, 30, IDC_APPLY);
    MakeChild(hWnd, L"BUTTON", L"Help / Troubleshooting", BS_PUSHBUTTON | WS_TABSTOP, 134, 160, 180, 30, IDC_HELPBTN);

    g_status = MakeChild(hWnd, L"STATIC", L"", SS_LEFT, 16, 204, 410, 48, 0);

    MakeChild(hWnd, L"STATIC",
              L"Run on the Hyper-V host with a VM window open. Ctrl+Alt+Left leaves a full-screen VM.",
              SS_LEFT, 16, 260, 410, 32, 0);

    CheckRadioButton(hWnd, IDC_RADIO_SHOW, IDC_RADIO_TRAY, RadioIdForMode(ReadRegMode()));
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // "TaskbarCreated" is a dynamically-registered id, so it can't be a case label.
    // Explorer broadcasts it after a restart; re-add our tray icon so we don't end
    // up an invisible, unreachable process.
    if (msg == g_taskbarCreated && g_taskbarCreated != 0)
    {
        if (g_trayActive) { g_trayActive = false; AddTray(); }
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        CreateControls(hWnd);
        return 0;
    case WM_APP_AUTOAPPLY:
        DoApply(static_cast<int>(wParam));
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONUP)
        {
            // Coalesce the two WM_LBUTTONUP events of a double-click into one toggle.
            static ULONGLONG s_lastClick = 0;
            const ULONGLONG now = GetTickCount64();
            const bool tooSoon = (now - s_lastClick) < GetDoubleClickTime();
            s_lastClick = now;
            if (!tooSoon) ToggleVmWindows();
        }
        else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_APPLY:         DoApply(GetSelectedMode()); return 0;
        case IDC_HELPBTN:       ShowHelp(); return 0;
        case IDM_TRAY_MINIMISE: MinimiseVmWindows(); return 0;
        case IDM_TRAY_RESTORE:  RestoreVmWindows(); return 0;
        case IDM_TRAY_SETTINGS: ShowWindow(g_mainWnd, SW_SHOW); SetForegroundWindow(g_mainWnd); return 0;
        case IDM_TRAY_ELEVATE:  if (RelaunchElevated(MODE_TRAY)) DestroyWindow(g_mainWnd); return 0;
        case IDM_TRAY_EXIT:     DestroyWindow(g_mainWnd); return 0;
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_CLOSE:
        if (g_trayActive) { ShowWindow(hWnd, SW_HIDE); return 0; }  // hide to tray instead of closing
        break;   // -> DefWindowProc destroys the window
    case WM_DESTROY:
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static int ParseApplyArg()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int mode = -1;
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], kElevatedFlag) == 0) g_elevated = true;
            else if (_wcsicmp(argv[i], kApplyFlag) == 0 && i + 1 < argc) mode = _wtoi(argv[i + 1]);
        }
        LocalFree(argv);
    }
    return mode;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    g_hInst = hInstance;

    const int applyMode = ParseApplyArg();
    g_elevated = g_elevated || IsProcessElevated();

    // Single instance: a plain launch (no --apply) while one is already running
    // just re-shows the existing window and exits, avoiding duplicate tray icons.
    // The elevated relaunch handoff always carries --apply, so it is exempt.
    if (applyMode < 0)
    {
        HANDLE hOnce = CreateMutexW(NULL, FALSE, L"Local\\APoxOnHyperVConnectBar_SingleInstance");
        if (hOnce && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (HWND existing = FindWindowW(L"APoxMainWindow", NULL))
            {
                ShowWindow(existing, SW_SHOW);
                SetForegroundWindow(existing);
            }
            CloseHandle(hOnce);
            return 0;
        }
        // else: keep hOnce open for this process's lifetime (released on exit).
    }

    // Explorer broadcasts this after a restart so we can re-add our tray icon.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    // Load the embedded application icon at the right sizes for the taskbar
    // (large) and the title bar / tray (small).
    g_iconBig   = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    g_iconSmall = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = g_iconBig;     // taskbar / alt-tab
    wc.hIconSm       = g_iconSmall;   // title-bar ("drag bar")
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"APoxMainWindow";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 446, 312 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_mainWnd = CreateWindowExW(0, L"APoxMainWindow", L"APoxOnHyperVConnectBar",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);
    if (!g_mainWnd)
    {
        if (g_font) DeleteObject(g_font);
        if (g_iconBig) DestroyIcon(g_iconBig);
        if (g_iconSmall) DestroyIcon(g_iconSmall);
        return 1;
    }

    // In Tray auto-apply, don't flash the window up first.
    ShowWindow(g_mainWnd, (applyMode == MODE_TRAY) ? SW_HIDE : nCmdShow);
    UpdateWindow(g_mainWnd);

    if (applyMode >= 0)
    {
        CheckRadioButton(g_mainWnd, IDC_RADIO_SHOW, IDC_RADIO_TRAY, RadioIdForMode(applyMode));
        PostMessageW(g_mainWnd, WM_APP_AUTOAPPLY, static_cast<WPARAM>(applyMode), 0);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(g_mainWnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (g_font) DeleteObject(g_font);
    if (g_iconBig) DestroyIcon(g_iconBig);
    if (g_iconSmall) DestroyIcon(g_iconSmall);
    return 0;
}
