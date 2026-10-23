@echo off
rem ===========================================================================
rem  MCLite build script
rem  Tries, in order: MSVC (Visual Studio) -> MinGW g++ -> Zig (bundled clang)
rem  Output: build\MCLite.exe
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build

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
        cl /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE ^
           src\main.cpp /Fe:build\MCLite.exe /link /SUBSYSTEM:WINDOWS ^
           gdiplus.lib shell32.lib ole32.lib user32.lib gdi32.lib
        if errorlevel 1 goto :fail
        goto :done
    )
)

echo [2/3] Looking for g++ ...
where g++ >nul 2>nul
if not errorlevel 1 (
    g++ -std=c++17 -O2 -mwindows -s ^
        src\main.cpp -o build\MCLite.exe ^
        -lgdiplus -lshell32 -lole32 -lgdi32 -luser32 -static-libgcc -static-libstdc++
    if errorlevel 1 goto :fail
    del /q build\MCLite.pdb 2>nul
    goto :done
)

echo [3/3] Looking for zig ...
set "ZIGEXE="
where zig >nul 2>nul && set "ZIGEXE=zig"
if not defined ZIGEXE if exist "%USERPROFILE%\.workbuddy\binaries\zig\ziglang\zig.exe" set "ZIGEXE=%USERPROFILE%\.workbuddy\binaries\zig\ziglang\zig.exe"
if defined ZIGEXE (
    "%ZIGEXE%" c++ -target x86_64-windows-gnu -std=c++17 -O2 ^
        src\main.cpp -o build\MCLite.exe ^
        -Wl,--subsystem,windows -Wl,-s -lgdiplus -lshell32 -lole32 -lgdi32 -luser32
    if errorlevel 1 goto :fail
    del /q build\MCLite.pdb 2>nul
    goto :done
)

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
echo [OK] build\MCLite.exe
endlocal
