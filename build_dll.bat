@echo off
REM ============================================================================
REM  build_dll.bat -- Build GpuColorLite.dll (for integration into other apps)
REM
REM  Requires: Visual Studio 2022 Build Tools (MSVC v143 + Windows SDK)
REM  Output:   output\GpuColorLite.dll, .lib, .h
REM
REM  Exported C API (stdcall, return 0 on success):
REM    int  GclSetColor(u8 r, u8 g, u8 b)        set GPU lighting color
REM    int  GclSaveHsv(u32 h, u32 s, u32 v)      save HSV (h 0..359, s/v 0..1000)
REM    int  GclLoadHsv(u32* h, u32* s, u32* v)   load HSV
REM    int  GclSaveRgb(u8 r, u8 g, u8 b)         save RGB
REM    int  GclLoadRgb(u8* r, u8* g, u8* b)      load RGB
REM    const char* GclLastError()                last error message (UTF-8)
REM    const char* GclVersion()                  version string
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

REM ---- Step 1: resources ----
echo [1/4] Compiling resources...
rc /nologo /fo "%OUT%\app.res" app.rc
if errorlevel 1 (
    echo [ERROR] Resource compilation failed.
    exit /b 1
)

REM ---- Step 2: build DLL ----
echo [2/4] Compiling GpuColorLite.dll...
cl /nologo /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DGPUCOLORLITE_BUILD_DLL /LD /MT /O2 /W3 /I"%CXXWINRT%" /Fo"%OUT%\\" GpuColorLite.cpp "%OUT%\app.res" /Fe:"%OUT%\GpuColorLite.dll" /link /DLL /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib psapi.lib windowsapp.lib
if errorlevel 1 (
    echo [ERROR] DLL build failed.
    exit /b 1
)

REM ---- Step 3: emit public header ----
echo [3/4] Writing GpuColorLite.h...
> "%OUT%\GpuColorLite.h" echo #pragma once
>> "%OUT%\GpuColorLite.h" echo /* GpuColorLite.h -- C ABI for GpuColorLite.dll
>> "%OUT%\GpuColorLite.h" echo  *  Return value: 0 = success, non-zero = failure.
>> "%OUT%\GpuColorLite.h" echo  *  Strings: UTF-8, owned by the DLL, do not free.
>> "%OUT%\GpuColorLite.h" echo  */
>> "%OUT%\GpuColorLite.h" echo #ifdef __cplusplus
>> "%OUT%\GpuColorLite.h" echo extern "C" {
>> "%OUT%\GpuColorLite.h" echo #endif
>> "%OUT%\GpuColorLite.h" echo.
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) int __stdcall GclSetColor(unsigned char r, unsigned char g, unsigned char b);
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) int __stdcall GclSaveHsv(unsigned hue, unsigned saturationPermille, unsigned valuePermille);
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) int __stdcall GclLoadHsv(unsigned *hue, unsigned *saturationPermille, unsigned *valuePermille);
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) int __stdcall GclSaveRgb(unsigned char r, unsigned char g, unsigned char b);
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) int __stdcall GclLoadRgb(unsigned char *r, unsigned char *g, unsigned char *b);
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) const char *__stdcall GclLastError(void);
>> "%OUT%\GpuColorLite.h" echo __declspec(dllimport) const char *__stdcall GclVersion(void);
>> "%OUT%\GpuColorLite.h" echo.
>> "%OUT%\GpuColorLite.h" echo #ifdef __cplusplus
>> "%OUT%\GpuColorLite.h" echo }
>> "%OUT%\GpuColorLite.h" echo #endif

REM ---- Step 4: cleanup ----
echo [4/4] Cleaning intermediate files...
del /q "%OUT%\*.obj" "%OUT%\app.res" "%OUT%\*.exp" 2>nul

echo.
echo [OK] Built:
echo      %OUT%\GpuColorLite.dll
echo      %OUT%\GpuColorLite.lib
echo      %OUT%\GpuColorLite.h
endlocal
