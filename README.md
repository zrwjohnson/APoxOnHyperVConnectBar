# VmMinimize — minimise the VM window from *inside* the VM

A desktop icon **inside the guest VM** that minimises the whole VM window on the
host — no pop-ups, no leaving the VM, no antivirus-tripping injection.

## How it works

A program inside the guest can't touch the host's window directly, so:

1. Inside the VM, a tiny program (**`VmMinimize.exe`**) sends a signal to the host
   over a **Hyper-V Socket** (a built-in guest↔host channel — no networking).
2. On the host, a hidden listener (**`VmMinimizeHost.exe`**) receives it and
   minimises the VM's `vmconnect.exe` window (the one you're looking at).

No firewall, no IP addresses, and nothing is exposed to the network — Hyper-V
Sockets only connect a guest to its own host.

## Build

Run **`build.cmd`** (needs Visual Studio with the C++ workload). It produces:

- `VmMinimizeHost.exe` — stays on the host.
- `VmMinimize.exe` — goes into the VM.

## Set up the HOST (once)

1. Right-click **`install-host.cmd`** → **Run as administrator** (or just
   double-click — it will ask for admin).

   It registers the Hyper-V Socket service and creates a logon task so the
   listener starts automatically, elevated, at every sign-in (elevated so it can
   minimise an elevated `vmconnect`, and so there are **no pop-ups** when you use
   it).

To remove it later: run **`uninstall-host.cmd`** as administrator.

## Set up the GUEST (each VM)

1. Copy **`VmMinimize.exe`** into the VM (drag-drop with Enhanced Session, a
   shared folder, or download it inside the VM).
2. In the VM, right-click it → **Send to → Desktop (create shortcut)**, or make a
   shortcut anywhere you like (Start menu, taskbar, a hotkey via the shortcut's
   properties).

That's it — no admin needed inside the VM.

## Use it

Inside the VM, **double-click the `VmMinimize` shortcut**. The VM window minimises
on the host and you're back at your host desktop. Bring it back from the host
taskbar as usual.

Tip: give the shortcut a keyboard shortcut (Shortcut properties → *Shortcut key*)
so you can minimise with a keypress from inside the VM.

## Requirements

- Host: Windows 10/11 or Windows Server with Hyper-V.
- Guest: Windows 10/11 (Hyper-V Sockets need the VMBus integration, which is on by
  default for Windows guests). Gen-2 VMs recommended.

## Troubleshooting

- **Nothing happens:** make sure the host listener is running — Task Manager →
  Details → `VmMinimizeHost.exe`. If it's missing, re-run `install-host.cmd` and
  sign out/in (or `schtasks /run /tn APoxVmMinimizeHost`).
- **Still nothing:** confirm the VM's *Data Exchange / Integration Services* are
  enabled and the guest is a Windows 10/11 build with Hyper-V Socket support.
- **It minimises the wrong VM (multiple VMs open):** it targets the VM window that
  currently has focus (the one you clicked from); if that can't be determined it
  minimises all VM windows.
