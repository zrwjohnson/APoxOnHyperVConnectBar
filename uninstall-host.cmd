@echo off
rem Removes the host listener task and the Hyper-V Socket service registration.

net session >nul 2>&1 || (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

set "GUID=9b6a7c2e-1234-4a5b-8c7d-abcdef012345"
schtasks /end /tn "APoxVmMinimizeHost" 2>nul
schtasks /delete /tn "APoxVmMinimizeHost" /f 2>nul
taskkill /im VmMinimizeHost.exe /f 2>nul
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\%GUID%" /f 2>nul
echo Removed the host listener and service registration.
pause
