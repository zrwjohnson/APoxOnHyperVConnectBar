// APoxOnHypervConnectBar - a tiny GUI micro-app to control the Hyper-V VM
// connect bar. The ApiHooker.dll payload is embedded in this exe as a resource;
// picking a mode extracts + injects it into every running vmconnect.exe (once
// each) and then talks to each through a hidden control window so later changes
// don't re-inject.
//
//   Show     - connect bar always visible (pinned)
//   Minimise - connect bar auto-hides (unpinned)
//   Hidden   - connect bar permanently hidden
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

// ----- shared with the DLL (keep identical in dllmain.cpp) --------------------
#define MODE_SHOW      0
#define MODE_MINIMISE  1
#define MODE_HIDDEN    2
static const wchar_t* kCtlClass   = L"APoxOnHypervConnectBar_Ctl";
static const wchar_t* kSetModeMsg = L"APoxOnHypervConnectBar_SetMode";
static const wchar_t* kRegPath    = L"Software\\APoxOnHypervConnectBar";
static const wchar_t* kRegValue   = L"Mode";
// -----------------------------------------------------------------------------

static const wchar_t* kTargetProcess = L"vmconnect.exe";
static const wchar_t* kElevatedFlag  = L"--elevated";
static const wchar_t* kApplyFlag     = L"--apply";

// control ids / messages
#define IDC_RADIO_SHOW   1001
#define IDC_RADIO_MIN    1002
#define IDC_RADIO_HIDDEN 1003
#define IDC_APPLY        1010
#define IDC_HELPBTN      1011
#define WM_APP_AUTOAPPLY (WM_APP + 1)

static HINSTANCE g_hInst   = NULL;
static HWND      g_mainWnd = NULL;
static HWND      g_status  = NULL;
static HFONT     g_font    = NULL;
static HWND      g_helpWnd = NULL;
static bool      g_elevated = false;

static const wchar_t* ModeName(int m)
{
    switch (m) { case MODE_HIDDEN: return L"Hidden"; case MODE_MINIMISE: return L"Minimise"; default: return L"Show"; }
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
    {
        if (pids[i] == 0) continue;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!hProc) continue;
        wchar_t name[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, name, &len) && EndsWithNoCase(name, kTargetProcess))
            out.push_back(pids[i]);
        CloseHandle(hProc);
    }
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
    return (v > MODE_HIDDEN) ? MODE_SHOW : static_cast<int>(v);
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
// GUI
// ===========================================================================
static void SetStatus(const wchar_t* text)
{
    if (g_status) { SetWindowTextW(g_status, text); UpdateWindow(g_status); }
}

static int GetSelectedMode()
{
    if (IsDlgButtonChecked(g_mainWnd, IDC_RADIO_HIDDEN) == BST_CHECKED) return MODE_HIDDEN;
    if (IsDlgButtonChecked(g_mainWnd, IDC_RADIO_MIN)    == BST_CHECKED) return MODE_MINIMISE;
    return MODE_SHOW;
}

// Ensure the DLL is present in every vmconnect.exe and apply the chosen mode.
static void DoApply(int mode)
{
    SaveRegMode(mode);
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
                continue;   // move on to the other VM windows
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

    // If any VM window needs elevation to attach, relaunch elevated to handle them.
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
        swprintf_s(buf, L"Applied to %d of %d VM windows. Some could not be reached - see Help.",
                   handled, total);
    else if (avBlocked)
        swprintf_s(buf, L"The hook could not load inside vmconnect.exe.\r\nAntivirus may have blocked it - see Help.");
    else
        swprintf_s(buf, L"Could not attach to vmconnect.exe.\r\nSee Help / Troubleshooting.");
    SetStatus(buf);
}

static const wchar_t* kHelpText =
    L"APoxOnHypervConnectBar - Help & Troubleshooting\r\n"
    L"==============================================\r\n\r\n"
    L"WHAT IT DOES\r\n"
    L"This app controls the floating \"connect bar\" that Hyper-V's vmconnect.exe\r\n"
    L"shows at the top of a VM window. Pick a mode and click Apply. The choice is\r\n"
    L"applied to every open VM window.\r\n\r\n"
    L"  * Show      - the connect bar is always visible (pinned).\r\n"
    L"  * Minimise  - the connect bar auto-hides (unpinned). Move the mouse to the\r\n"
    L"                top-centre of the screen to bring it back temporarily.\r\n"
    L"  * Hidden    - the connect bar is permanently hidden until you choose\r\n"
    L"                Show or Minimise again.\r\n\r\n"
    L"HOW TO USE\r\n"
    L"  1. Run this app on the Hyper-V HOST (the machine running Hyper-V), not\r\n"
    L"     inside the guest OS. The connect bar belongs to vmconnect.exe on the\r\n"
    L"     host, so there is nothing to control from inside the guest.\r\n"
    L"  2. Open / enter a VM in Hyper-V Manager so a VM window exists.\r\n"
    L"  3. Choose a mode and click Apply.\r\n"
    L"  4. To leave a full-screen VM, press Ctrl+Alt+Left Arrow.\r\n\r\n"
    L"TROUBLESHOOTING\r\n"
    L"  \"No VM window found\"\r\n"
    L"     - Start and connect to a VM in Hyper-V Manager first, then Apply again.\r\n"
    L"     - The VM must be actually connected (a vmconnect.exe window open).\r\n\r\n"
    L"  A UAC prompt appears / \"Administrator rights are required\"\r\n"
    L"     - vmconnect.exe is running elevated, so the app must elevate too.\r\n"
    L"       Accept the prompt (or right-click the app > Run as administrator).\r\n\r\n"
    L"  \"Antivirus may have blocked it\" / injection blocked\r\n"
    L"     - This tool works by injecting a small DLL into vmconnect.exe, which\r\n"
    L"       some antivirus / EDR products block. Allow the app in your AV, or add\r\n"
    L"       an exclusion, then try again.\r\n\r\n"
    L"  Hidden works but Minimise doesn't auto-hide\r\n"
    L"     - Minimise clicks the connect bar's real pin button so vmconnect can\r\n"
    L"       auto-hide it. On some Windows builds that automated click may not\r\n"
    L"       take - if so, click the pin (thumb-tack) on the bar yourself to\r\n"
    L"       unpin it. Show and Hidden do not depend on the pin.\r\n\r\n"
    L"  Nothing happens at all\r\n"
    L"     - Make sure you ran it on the host with a connected VM window.\r\n"
    L"     - Try Hidden first (the most reliable mode) to confirm it is attaching.\r\n"
    L"     - Move the mouse to the top edge of the VM to nudge the bar.\r\n\r\n"
    L"  Only some VM windows changed\r\n"
    L"     - The status line reports how many were reached. Ones that need admin\r\n"
    L"       rights are handled after you accept the UAC prompt.\r\n\r\n"
    L"  The bar comes back after reconnecting\r\n"
    L"     - The setting lasts for that vmconnect.exe session. If you close and\r\n"
    L"       reopen the VM, run the app and Apply again. Your last choice is\r\n"
    L"       remembered and re-applied when the app re-attaches.\r\n\r\n"
    L"NOTE\r\n"
    L"  This is an unofficial tool that works by API hooking. It changes nothing\r\n"
    L"  permanently on your system - it only affects the running vmconnect.exe.\r\n";

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
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"APoxHelpWindow";
        RegisterClassW(&wc);
        registered = true;
    }
    g_helpWnd = CreateWindowExW(0, L"APoxHelpWindow", L"Help & Troubleshooting",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 520,
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
              BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 44, 390, 22, IDC_RADIO_SHOW);
    MakeChild(hWnd, L"BUTTON", L"Minimise  -  connect bar auto-hides (unpinned)",
              BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 70, 390, 22, IDC_RADIO_MIN);
    MakeChild(hWnd, L"BUTTON", L"Hidden  -  connect bar permanently hidden",
              BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 96, 390, 22, IDC_RADIO_HIDDEN);

    MakeChild(hWnd, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, 22, 134, 100, 30, IDC_APPLY);
    MakeChild(hWnd, L"BUTTON", L"Help / Troubleshooting", BS_PUSHBUTTON | WS_TABSTOP, 134, 134, 180, 30, IDC_HELPBTN);

    g_status = MakeChild(hWnd, L"STATIC", L"", SS_LEFT, 16, 178, 410, 48, 0);

    MakeChild(hWnd, L"STATIC",
              L"Run on the Hyper-V host with a VM window open. Ctrl+Alt+Left leaves a full-screen VM.",
              SS_LEFT, 16, 232, 410, 32, 0);

    int mode = ReadRegMode();
    int id = (mode == MODE_HIDDEN) ? IDC_RADIO_HIDDEN : (mode == MODE_MINIMISE) ? IDC_RADIO_MIN : IDC_RADIO_SHOW;
    CheckRadioButton(hWnd, IDC_RADIO_SHOW, IDC_RADIO_HIDDEN, id);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        CreateControls(hWnd);
        return 0;
    case WM_APP_AUTOAPPLY:
        DoApply(static_cast<int>(wParam));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_APPLY:   DoApply(GetSelectedMode()); return 0;
        case IDC_HELPBTN: ShowHelp(); return 0;
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
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

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"APoxMainWindow";
    RegisterClassW(&wc);

    RECT rc = { 0, 0, 446, 284 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_mainWnd = CreateWindowExW(0, L"APoxMainWindow", L"APoxOnHypervConnectBar",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);
    if (!g_mainWnd) { if (g_font) DeleteObject(g_font); return 1; }

    ShowWindow(g_mainWnd, nCmdShow);
    UpdateWindow(g_mainWnd);

    if (applyMode >= 0)
    {
        int id = (applyMode == MODE_HIDDEN) ? IDC_RADIO_HIDDEN : (applyMode == MODE_MINIMISE) ? IDC_RADIO_MIN : IDC_RADIO_SHOW;
        CheckRadioButton(g_mainWnd, IDC_RADIO_SHOW, IDC_RADIO_HIDDEN, id);
        // Apply after the window has fully painted (keeps the UI from looking hung).
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
    return 0;
}
