@echo off
setlocal enabledelayedexpansion
rem ---------------------------------------------------------------------------
rem Build APoxOnHyperVConnectBar as a single self-contained x64 executable.
rem
rem   1. compile ApiHooker.dll  (Detours hook payload)
rem   2. embed that DLL into the launcher as an RCDATA resource
rem   3. compile + link the launcher -> ..\dist\APoxOnHyperVConnectBar.exe (project-root dist)
rem
rem Requires Visual Studio 2022/2026 with the "Desktop development with C++"
rem workload. Just double-click this file or run it from any command prompt.
rem ---------------------------------------------------------------------------
cd /d "%~dp0"

echo === Locating Visual Studio build tools ===
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -find VC\Auxiliary\Build\vcvars64.bat 2^>nul`) do set "VCVARS=%%i"
)
if not defined VCVARS (
  for %%v in (18 17) do (
    for %%e in (Community Professional Enterprise BuildTools) do (
      if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)
if not defined VCVARS (
  echo [ERROR] Could not locate vcvars64.bat. Install Visual Studio with the
  echo         "Desktop development with C++" workload and try again.
  exit /b 1
)
call "%VCVARS%" >nul
echo Using: %VCVARS%
echo.

if not exist build     mkdir build
if not exist ..\dist   mkdir ..\dist

echo === [1/3] Building ApiHooker.dll ===
rem /MT = static CRT so the tool has no vcruntime140.dll / VC++ redist dependency.
cl /nologo /std:c++17 /O2 /MT /EHsc /DNDEBUG /DUNICODE /D_UNICODE /D_WINDOWS /D_USRDLL /DAPIHOOKER_EXPORTS ^
   /I ApiHooker /I detours\include ^
   /Fo:build\ /LD ApiHooker\dllmain.cpp /Fe:build\ApiHooker.dll ^
   /link detours\lib\detours.lib user32.lib advapi32.lib
if errorlevel 1 (echo [ERROR] DLL build failed. & exit /b 1)
echo.

echo === [2/3] Embedding ApiHooker.dll into the launcher resource ===
copy /Y build\ApiHooker.dll Launcher\ApiHooker.dll >nul
pushd Launcher
rc /nologo /fo "..\build\Launcher.res" Launcher.rc
if errorlevel 1 (popd & echo [ERROR] Resource compile failed. & exit /b 1)
popd
echo.

echo === [3/3] Building APoxOnHyperVConnectBar.exe (single file) ===
cl /nologo /std:c++17 /O2 /MT /EHsc /DNDEBUG /DUNICODE /D_UNICODE ^
   /I Launcher ^
   /Fo:build\ Launcher\launcher.cpp build\Launcher.res ^
   /Fe:..\dist\APoxOnHyperVConnectBar.exe
if errorlevel 1 (echo [ERROR] Launcher build failed. & exit /b 1)
echo.

del /Q Launcher\ApiHooker.dll >nul 2>&1
echo === Done. Single-file executable: ===
echo     %~dp0..\dist\APoxOnHyperVConnectBar.exe
endlocal
