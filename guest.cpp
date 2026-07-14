// VmMinimize.exe (runs INSIDE the guest VM)
//
// A windowless, no-pop-up program: it connects to the Hyper-V host over a
// Hyper-V Socket (AF_HYPERV) and sends a "minimise" signal, then exits. Put a
// desktop shortcut to it inside the VM; clicking it minimises the VM window on
// the host. Needs no administrator rights in the guest.
//
// The host must be running VmMinimizeHost.exe and have registered the service
// GUID (see install-host.cmd). Guest and host must use the same GUID + message.

#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <hvsocket.h>

#pragma comment(lib, "ws2_32.lib")

// Shared service id + message - MUST match host.cpp.
// {9b6a7c2e-1234-4a5b-8c7d-abcdef012345}
DEFINE_GUID(APOX_VMMIN_SERVICE,
    0x9b6a7c2e, 0x1234, 0x4a5b, 0x8c, 0x7d, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45);
static const char kMessage[] = "APOXMIN1";

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) { WSACleanup(); return 1; }

    SOCKADDR_HV addr = {};
    addr.Family    = AF_HYPERV;
    addr.VmId      = HV_GUID_PARENT;      // the Hyper-V host / parent partition
    addr.ServiceId = APOX_VMMIN_SERVICE;

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
        send(s, kMessage, static_cast<int>(sizeof(kMessage) - 1), 0);

    closesocket(s);
    WSACleanup();
    return 0;
}
