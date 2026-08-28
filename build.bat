@echo off
setlocal enabledelayedexpansion
REM Build MoniKa with VS Build Tools (no IDE required).
REM Prefer running from "x64 Native Tools Command Prompt for VS". Otherwise this
REM tries to locate and call vcvars64.bat via vswhere.

where cl >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" (
    echo [!] Could not find vswhere / VS Build Tools. Open the x64 Native Tools prompt and re-run.
    exit /b 1
  )
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
  if not defined VSPATH (
    echo [!] VC Tools not installed. Install "Desktop development with C++" workload.
    exit /b 1
  )
  call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
)

if not exist x64\Release mkdir x64\Release
cl /nologo /std:c++17 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE ^
   src\main.cpp src\app.cpp src\oracle.cpp src\scanner.cpp ^
   src\dek_cache.cpp src\etw.cpp src\watcher.cpp src\tray.cpp ^
   /Fe:x64\Release\MoniKa.exe /Fo:x64\Release\ ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
   advapi32.lib tdh.lib bcrypt.lib shell32.lib user32.lib ole32.lib

if errorlevel 1 ( echo [!] build failed & exit /b 1 )
echo [+] Built x64\Release\MoniKa.exe
echo     Double-click to run in the tray. Windows will request administrator access.
endlocal
