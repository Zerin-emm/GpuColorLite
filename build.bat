@echo off
REM ============================================================================
REM  build.bat -- Build GpuColorLite.exe (single-file executable)
REM
REM  Requires: Visual Studio 2022 Build Tools (MSVC v143 + Windows SDK)
REM  Output:   output\GpuColorLite.exe
REM ============================================================================
setlocal

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

set "OUT=%SCRIPT_DIR%output"
if not exist "%OUT%" mkdir "%OUT%"

REM ---- Locate Visual Studio ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install Visual Studio 2022 Build Tools.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [ERROR] No Visual Studio installation with C++ tools found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    exit /b 1
)

REM ---- Locate C++/WinRT headers from Windows SDK ----
set "SDK_INC=%ProgramFiles(x86)%\Windows Kits\10\Include"
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDK_INC%\10.*" 2^>nul') do (
    if not defined CXXWINRT if exist "%SDK_INC%\%%d\cppwinrt\winrt\base.h" set "CXXWINRT=%SDK_INC%\%%d\cppwinrt"
)
if not defined CXXWINRT (
    echo [ERROR] C++/WinRT headers not found.
    exit /b 1
)

REM ---- Step 1: resources (icon + version info) ----
echo [1/3] Compiling resources...
rc /nologo /fo "%OUT%\app.res" app.rc
if errorlevel 1 (
    echo [ERROR] Resource compilation failed.
    exit /b 1
)

REM ---- Step 2: compile and link ----
echo [2/3] Compiling GpuColorLite.cpp...
cl /nologo /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /MT /O2 /W3 /I"%CXXWINRT%" /Fo"%OUT%\\" GpuColorLite.cpp "%OUT%\app.res" /Fe:"%OUT%\GpuColorLite.exe" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib psapi.lib windowsapp.lib
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

REM ---- Step 3: cleanup ----
echo [3/3] Cleaning intermediate files...
del /q "%OUT%\*.obj" "%OUT%\app.res" 2>nul

echo.
echo [OK] Built: %OUT%\GpuColorLite.exe
endlocal
