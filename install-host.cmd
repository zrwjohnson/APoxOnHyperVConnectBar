@echo off
rem One-time HOST setup. Registers the Hyper-V Socket service and auto-starts the
rem listener (VmMinimizeHost.exe) elevated at each logon, so there are no pop-ups
rem when you use it. Run this ONCE on the host (it will ask for admin rights).

net session >nul 2>&1 || (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

cd /d "%~dp0"
set "GUID=9b6a7c2e-1234-4a5b-8c7d-abcdef012345"

if not exist "%~dp0VmMinimizeHost.exe" (
  echo [ERROR] VmMinimizeHost.exe not found next to this script. Run build.cmd first.
  pause & exit /b 1
)

echo [1/3] Registering the Hyper-V Socket service...
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\%GUID%" /v ElementName /t REG_SZ /d "APox VM Minimize" /f

echo [2/3] Creating a logon task (runs the listener elevated at sign-in)...
schtasks /create /tn "APoxVmMinimizeHost" /tr "\"%~dp0VmMinimizeHost.exe\"" /sc onlogon /rl highest /f

echo [3/3] Starting the listener now...
schtasks /run /tn "APoxVmMinimizeHost"

echo.
echo Done. The host listener is running and will start automatically at logon.
echo Now copy VmMinimize.exe into your VM and make a desktop shortcut to it.
pause
