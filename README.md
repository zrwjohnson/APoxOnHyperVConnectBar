# APoxOnHyperVConnectBar

> Control the floating "connect bar" at the top of a Hyper-V VM window — show it,
> minimise it, or hide it for good.

![](assets/image.png)

The `Remote Desktop Connection` app lets you hide the connect-bar forever, but
Hyper-V's `vmconnect.exe` does not! This horrible, ugly, unclosable floating bar
is annoying!

**Let's put a pox on it.**

## What it is

A tiny **single-file GUI app** (no install, no separate DLL) that lets you pick how
the Hyper-V connect bar behaves:

| Mode | Behaviour |
| --- | --- |
| **Show** | Connect bar always visible (pinned). |
| **Minimise** | Connect bar auto-hides (unpinned) — hover the top-centre of the screen to bring it back. |
| **Hidden** | Connect bar permanently hidden until you choose Show or Minimise again. |
| **Tray** | Launcher-only mode: the app lives in the system tray and its icon minimises / restores the whole VM window on click (no injection). |

## Usage

1. Run **`APoxOnHyperVConnectBar.exe`** on the Hyper-V **host** (the machine
   running Hyper-V — that is where the VM window lives).
2. Open / enter a VM in *Hyper-V Manager* so a VM window exists.
3. Pick a mode and click **Apply**.
4. To leave a full-screen VM, press **Ctrl+Alt+Left Arrow**.

Your last choice is remembered and re-applied automatically whenever the app
re-attaches to the VM. There's a built-in **Help / Troubleshooting** button for
when something doesn't behave.

> **Run it on the host, not inside the guest.** The connect-bar is drawn by
> `vmconnect.exe`, which runs on the Hyper-V host, so there is nothing to control
> from inside the guest OS.

### Administrator rights

If `vmconnect.exe` is running elevated, the app automatically re-launches itself
elevated (you'll get a UAC prompt) and applies your choice from there. You can
also just right-click → **Run as administrator**.

### Notes & limits

- A mode applies to the current `vmconnect.exe` session. If you close and reopen
  the VM, run the app and click Apply again (it remembers your last choice).
- **Show** and **Hidden** are enforced by hooking the bar's window and are the
  most reliable. **Minimise** additionally toggles the bar's real *pin* button so
  Hyper-V auto-hides it; if that doesn't take on your build of Windows, just click
  the pin (thumb-tack) on the bar yourself. The Help window explains this.

## Building

Requires **Visual Studio 2022 or 2026** with the *Desktop development with C++*
workload.

* **From a command prompt:** run [`build.bat`](build.bat). It builds
  `ApiHooker.dll`, embeds it into the launcher, and links the single-file
  `dist\APoxOnHyperVConnectBar.exe`.
* **From the IDE:** open `APoxOnHyperVConnectBar.sln`, pick **Release / x64**, and
  Build. The output is `x64\Release\APoxOnHyperVConnectBar.exe`.

The exe is statically linked (`/MT`) and uses `$(DefaultPlatformToolset)`, so it's
a genuinely self-contained single file that runs on any recent Windows/VS without
a Visual C++ redistributable or a "retarget" prompt.

## How it works

- `ApiHooker.dll` is **embedded inside the app** as a resource. When you Apply a
  mode, the app extracts it to `%TEMP%` and injects it into `vmconnect.exe`
  (`CreateRemoteThread` + `LoadLibraryW`) — once. The DLL creates a hidden control
  window, so later mode changes are sent to it without re-injecting.
- The DLL hooks the WinAPI `ShowWindow` (via Microsoft Detours) on the connect
  bar (window class `BBarWindowClass`, title `BBar`):
  - **Hidden** → suppress every show, so it never appears.
  - **Show** → suppress every hide, so it stays visible.
  - **Minimise** → let Hyper-V manage it, and click the bar's real pin toolbar
    button so it auto-hides.
- **Tray** mode uses no DLL at all — the launcher hosts a notification-area icon
  and minimises / restores the `vmconnect.exe` main window via `WM_SYSCOMMAND`
  (`SC_MINIMIZE` / `SC_RESTORE`).

> Note: because this works by injecting a DLL, some antivirus products may flag
> it. That is inherent to the technique; the full source is here so you can build
> it yourself.

## Tips

![](assets/tips.jpg)
