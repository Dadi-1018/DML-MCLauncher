@echo off
rem ===========================================================================
rem  Dream Minecraft Launcher (DML) build script
rem  Tries, in order: MSVC (+rc) -> MinGW g++ (+windres) -> Zig (no resources)
rem  Output: build\DML.exe
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build

rem ---------------------------------------------------------------------------
rem  1) MSVC
rem ---------------------------------------------------------------------------
echo [1/3] Looking for MSVC ...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSDIR=%%i"
    )
)
if defined VSDIR (
    if exist "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" (
        echo     Found: !VSDIR!
        call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul
        rc /nologo /fo build\app.res res\app.rc
        if errorlevel 1 goto :fail
        cl /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE ^
           src\main.cpp build\app.res /Fe:build\DML.exe /link /SUBSYSTEM:WINDOWS ^
           gdiplus.lib shell32.lib ole32.lib user32.lib gdi32.lib winhttp.lib crypt32.lib
        if errorlevel 1 goto :fail
        goto :done
    )
)

rem ---------------------------------------------------------------------------
rem  2) MinGW g++ + windres
rem ---------------------------------------------------------------------------
echo [2/3] Looking for g++ ...
set "GXX="
for /f "delims=" %%i in ('where g++ 2^>nul') do if not defined GXX set "GXX=%%i"
if not defined GXX if exist "%ProgramFiles%\RedPanda-Cpp\mingw64\bin\g++.exe" set "GXX=%ProgramFiles%\RedPanda-Cpp\mingw64\bin\g++.exe"

if defined GXX (
    echo     Found: !GXX!
    rem Derive the mingw root from g++.exe
    for %%i in ("!GXX!") do set "MINGWBIN=%%~dpi"
    for %%i in ("!MINGWBIN!..") do set "MINGWROOT=%%~fi"

    rem cc1.exe needs libwinpthread etc. on PATH
    set "PATH=!MINGWROOT!\bin;!PATH!"

    rem windres spawns gcc with an unquoted path, so a toolchain installed under
    rem a directory containing spaces must be reached through a space-free alias.
    set "WRES=!MINGWROOT!\bin\windres.exe"
    echo !MINGWROOT! | find " " >nul
    if not errorlevel 1 (
        if not exist build\tc mklink /J build\tc "!MINGWROOT!" >nul 2>nul
        if exist build\tc\bin\windres.exe set "WRES=%~dp0build\tc\bin\windres.exe"
    )

    echo     Compiling resources ...
    "!WRES!" res\app.rc -O coff -o build\app.res.o
    if errorlevel 1 goto :fail

    echo     Compiling and linking ...
    "!GXX!" -std=c++17 -O2 -mwindows -static -s ^
        src\main.cpp build\app.res.o -o build\DML.exe ^
        -lgdiplus -lshell32 -lole32 -lgdi32 -luser32 -lcomdlg32 -luuid -lwinhttp -lcrypt32
    if errorlevel 1 goto :fail
    del /q build\DML.pdb 2>nul
    goto :done
)

rem ---------------------------------------------------------------------------
rem  3) Zig (bundled clang; cannot compile .rc, so no icon / header logo)
rem ---------------------------------------------------------------------------
echo [3/3] Looking for zig ...
set "ZIGEXE="
where zig >nul 2>nul && set "ZIGEXE=zig"
if not defined ZIGEXE if exist "%USERPROFILE%\.workbuddy\binaries\zig\ziglang\zig.exe" set "ZIGEXE=%USERPROFILE%\.workbuddy\binaries\zig\ziglang\zig.exe"
if defined ZIGEXE (
    echo     [WARN] zig cannot compile .rc - output will have no icon / logo
    "%ZIGEXE%" c++ -target x86_64-windows-gnu -std=c++17 -O2 ^
        src\main.cpp -o build\DML.exe ^
        -Wl,--subsystem,windows -Wl,-s -lgdiplus -lshell32 -lole32 -lgdi32 -luser32 -lwinhttp -lcrypt32
    if errorlevel 1 goto :fail
    del /q build\DML.pdb 2>nul
    goto :done
)

rem ---------------------------------------------------------------------------
echo.
echo [ERROR] No C++ compiler found.
echo         Install "Visual Studio Build Tools" with the C++ workload, or MinGW-w64.
exit /b 1

:fail
echo.
echo [ERROR] Compilation failed.
exit /b 1

:done
echo.
echo [OK] build\DML.exe
endlocal
