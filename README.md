# APoxOnHypervConnectBar

A tiny app that lets you control the floating **connect bar** at the top of a
Hyper-V virtual machine window — show it, minimise it, or hide it completely.

Hyper-V's VM window (`vmconnect.exe`) has that connect bar pinned across the top,
and unlike the Remote Desktop client it gives you no way to get rid of it. This
app fixes that.

It's a **single file** — no installer, no extra files, nothing to set up. Just run
`APoxOnHypervConnectBar.exe`.

---

## What the three modes do

| Mode | What happens |
| --- | --- |
| **Show** | The connect bar is always visible (pinned). |
| **Minimise** | The connect bar auto-hides (unpinned). Move the mouse to the top-centre of the screen to bring it back for a moment. |
| **Hidden** | The connect bar is completely hidden until you switch back to Show or Minimise. |

Whatever you pick is applied to **every** open VM window.

---

## Where to run it

**Run it on your physical PC — the Hyper-V host.** Not inside the VM.

The connect bar is drawn by `vmconnect.exe`, which runs on the host (it's part of
Hyper-V Manager). Inside the guest OS there's no connect bar and nothing for the
app to attach to.

- ✅ **Host (your physical PC)** — run the app here.
- ❌ **Guest (inside the VM)** — running it here does nothing ("No VM window found").

> This works with Hyper-V's own **VMConnect** window. It is not for plain Remote
> Desktop (`mstsc`) sessions — that's a different connect bar.

---

## How to use it

1. On your PC, open / enter a VM in **Hyper-V Manager** (so a VM window is on screen).
2. Run **`APoxOnHypervConnectBar.exe`**.
3. Choose a mode — **Show**, **Minimise**, or **Hidden**.
4. Click **Apply**.
5. Done. The status line confirms it (e.g. *"Applied: Hidden to 1 VM window."*).

**To leave a full-screen VM**, press **Ctrl + Alt + Left Arrow**.

**Administrator prompt:** if the VM window is running with admin rights, you'll get
a UAC prompt — just accept it and the app finishes the job. (You can also
right-click the app → **Run as administrator** to avoid the prompt mid-way.)

**Changing your mind:** open the app again any time and Apply a different mode.
Your last choice is remembered.

---

## Good to know

- A mode stays in effect for that VM session. If you **close and reopen** the VM,
  just run the app and click Apply again (it remembers your last choice).
- Nothing is changed permanently on your PC — the app only affects the running
  `vmconnect.exe`. Close the VM and everything is back to normal.
- Because it works by injecting a small helper into `vmconnect.exe`, some
  **antivirus** tools may flag it. That's normal for this kind of tool; allow it
  or add an exclusion if needed.

---

## Troubleshooting

The app has a built-in **Help / Troubleshooting** button with the full guide. The
common ones:

- **"No VM window found"** — open/connect a VM in Hyper-V Manager first, then Apply
  again. And make sure you're running it on the host, not inside the VM.
- **A UAC prompt appears** — accept it; the VM window is elevated and the app needs
  the same rights to attach.
- **"Antivirus may have blocked it"** — allow the app in your antivirus, then retry.
- **Hidden works but Minimise doesn't auto-hide** — Minimise relies on clicking the
  bar's pin button; if it doesn't take on your build of Windows, just click the
  pin (thumb-tack) on the bar yourself. Show and Hidden always work.

---

## For developers

The full source is in the **`src/`** folder. To build the single-file exe:

- **Command line:** run `src\build.bat` (needs Visual Studio 2022/2026 with the
  *Desktop development with C++* workload). Output: `src\dist\APoxOnHypervConnectBar.exe`.
- **Visual Studio:** open `src\APoxOnHypervConnectBar.sln`, choose **Release / x64**,
  and Build.

See [`src/README.md`](src/README.md) for the technical details of how it works.
