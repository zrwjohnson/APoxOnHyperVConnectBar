// VmMinimizeHost.exe (runs on the Hyper-V HOST, in the background)
//
// Listens on a Hyper-V Socket (AF_HYPERV) for the guest's "minimise" signal and
// minimises the VM window (vmconnect.exe) it belongs to. Windowless - it runs
// hidden. Install it to auto-start elevated at logon with install-host.cmd so it
// can minimise an elevated vmconnect and needs no runtime pop-ups.

#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <hvsocket.h>
#include <psapi.h>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

// Shared service id + message - MUST match guest.cpp.
// {9b6a7c2e-1234-4a5b-8c7d-abcdef012345}
DEFINE_GUID(APOX_VMMIN_SERVICE,
    0x9b6a7c2e, 0x1234, 0x4a5b, 0x8c, 0x7d, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45);
static const char kMessage[] = "APOXMIN1";

static bool EndsWithNoCase(const wchar_t* s, const wchar_t* suffix)
{
    const size_t ls = wcslen(s), lf = wcslen(suffix);
    return lf <= ls && _wcsicmp(s + ls - lf, suffix) == 0;
}

static bool ProcessIsVmConnect(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t name[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, name, &len) && EndsWithNoCase(name, L"vmconnect.exe");
    CloseHandle(h);
    return ok;
}

static BOOL CALLBACK enumProc(HWND h, LPARAM lp)
{
    auto* v = reinterpret_cast<std::vector<HWND>*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != NULL) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindowTextLengthW(h) == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (ProcessIsVmConnect(pid)) v->push_back(h);
    return TRUE;
}

// Minimise the VM window the signal came from. The VM the user just clicked in
// is the foreground window on the host, so prefer that; otherwise minimise every
// vmconnect window as a fallback.
static void MinimiseVmWindow()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && ProcessIsVmConnect(pid))
    {
        ShowWindow(fg, SW_MINIMIZE);
        return;
    }
    std::vector<HWND> wins;
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&wins));
    for (HWND h : wins)
        ShowWindow(h, SW_MINIMIZE);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    SOCKET srv = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (srv == INVALID_SOCKET) return 1;

    SOCKADDR_HV addr = {};
    addr.Family    = AF_HYPERV;
    addr.VmId      = HV_GUID_WILDCARD;    // accept from any VM
    addr.ServiceId = APOX_VMMIN_SERVICE;
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return 1;
    if (listen(srv, SOMAXCONN) != 0) return 1;

    for (;;)
    {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(250); continue; }
        char buf[16] = {};
        const int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n >= static_cast<int>(sizeof(kMessage) - 1) &&
            memcmp(buf, kMessage, sizeof(kMessage) - 1) == 0)
        {
            MinimiseVmWindow();
        }
        closesocket(c);
    }
}
