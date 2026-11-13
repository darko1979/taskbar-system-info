@echo off
rem Builds bin\TaskbarSysInfo.exe with MSVC (Visual Studio 2019+ with "Desktop development with C++").
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul
if errorlevel 1 goto :novs

:build
if not exist build mkdir build
if not exist bin mkdir bin

rc /nologo /i res /fo build\app.res res\app.rc || exit /b 1

cl /nologo /std:c++17 /O1 /GL /GS /MT /W4 /EHsc /GR- /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
   /Fobuild\ src\*.cpp build\app.res ^
   /Fe:bin\TaskbarSysInfo.exe ^
   /link /LTCG /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib gdiplus.lib shell32.lib advapi32.lib pdh.lib iphlpapi.lib dxgi.lib ole32.lib oleaut32.lib dwmapi.lib winhttp.lib bcrypt.lib version.lib || exit /b 1

echo.
echo Built bin\TaskbarSysInfo.exe
exit /b 0

:novs
echo ERROR: Visual Studio with C++ build tools not found. Run from a "x64 Native Tools Command Prompt".
exit /b 1
