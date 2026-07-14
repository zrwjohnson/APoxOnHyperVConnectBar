@echo off
setlocal
cd /d "%~dp0"

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -find VC\Auxiliary\Build\vcvars64.bat 2^>nul`) do set "VCVARS=%%i"
)
if not defined VCVARS (
  for %%v in (18 17) do for %%e in (Community Professional Enterprise BuildTools) do (
    if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS ( echo [ERROR] Visual Studio C++ tools not found. & exit /b 1 )
call "%VCVARS%" >nul

echo Building VmMinimize.exe (goes INSIDE the guest VM)...
cl /nologo /std:c++17 /O2 /MT /EHsc /DNDEBUG /DUNICODE /D_UNICODE guest.cpp /Fe:VmMinimize.exe /link ws2_32.lib
if errorlevel 1 exit /b 1

echo Building VmMinimizeHost.exe (runs on the Hyper-V HOST)...
cl /nologo /std:c++17 /O2 /MT /EHsc /DNDEBUG /DUNICODE /D_UNICODE host.cpp /Fe:VmMinimizeHost.exe /link ws2_32.lib user32.lib psapi.lib
if errorlevel 1 exit /b 1

del /q *.obj >nul 2>&1
echo.
echo Done:
echo   VmMinimizeHost.exe  - keep on the host, install with install-host.cmd
echo   VmMinimize.exe      - copy INTO the VM, make a desktop shortcut to it
endlocal
