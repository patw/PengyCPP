@echo off
REM Build Pengy for Windows
REM Prerequisites:
REM   1. Install Qt6: https://www.qt.io/download-qt-installer
REM      (choose MSVC 64-bit, e.g. Qt 6.10.x -> msvc2022_64)
REM   2. Install Visual Studio Build Tools 2022:
REM      https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
REM      (select "Desktop development with C++")
REM   3. Install CMake: https://cmake.org/download/
REM      (or: winget install Kitware.CMake)
REM
REM Run this script from a Developer Command Prompt for VS 2022.

setlocal enabledelayedexpansion
set ROOT=%~dp0
cd /d "%ROOT%"

REM Set Qt6 path — adjust to your Qt installation
if "%QT6_DIR%"=="" (
    if exist "C:\Qt\6.10.0\msvc2022_64" set QT6_DIR=C:\Qt\6.10.0\msvc2022_64
    if exist "C:\Qt\6.10.1\msvc2022_64" set QT6_DIR=C:\Qt\6.10.1\msvc2022_64
    if exist "C:\Qt\6.9.0\msvc2022_64"  set QT6_DIR=C:\Qt\6.9.0\msvc2022_64
)

if "%QT6_DIR%"=="" (
    echo ERROR: Could not find Qt6. Set QT6_DIR environment variable.
    echo Example: set QT6_DIR=C:\Qt\6.10.0\msvc2022_64
    exit /b 1
)

echo Using Qt6: %QT6_DIR%
set PATH=%QT6_DIR%\bin;%PATH%

echo.
echo ==^> Building Pengy (C++/Qt6) for Windows...
if not exist build_windows mkdir build_windows
cd build_windows

cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT6_DIR%"

cmake --build . --config Release

echo.
echo ==^> Done! Binary: build_windows\Release\pengy.exe

echo ==^> Bundling Qt DLLs...
set DIST=%ROOT%Pengy-Windows
if not exist "%DIST%" mkdir "%DIST%"
copy build_windows\Release\pengy.exe "%DIST%\" >nul
cd "%DIST%"
windeployqt pengy.exe

echo.
echo ==^> Packaged: %DIST%
echo ==^> Distribute by zipping the Pengy-Windows folder
endlocal
