@echo off
setlocal EnableExtensions
pushd "%~dp0"

set "ARCH=%~1"
if not defined ARCH set "ARCH=x64"

if /I "%ARCH%"=="x86" (
    set "CMAKE_ARCH=Win32"
    set "ARCH_NAME=x86"
) else if /I "%ARCH%"=="Win32" (
    set "CMAKE_ARCH=Win32"
    set "ARCH_NAME=x86"
) else if /I "%ARCH%"=="x64" (
    set "CMAKE_ARCH=x64"
    set "ARCH_NAME=x64"
) else if /I "%ARCH%"=="ARM64" (
    set "CMAKE_ARCH=ARM64"
    set "ARCH_NAME=ARM64"
) else (
    echo ERROR: unsupported architecture "%ARCH%".
    echo Usage: build.cmd [x86^|x64^|ARM64]
    popd
    exit /b 2
)

set "BUILD_DIR=build-%ARCH_NAME%"

echo [1/3] Configuring FarFileClipboard %ARCH_NAME% with Visual Studio 2022...
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A %CMAKE_ARCH%
if errorlevel 1 goto :fail

echo [2/3] Building Release...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :fail

echo [3/3] Verifying PE architecture...
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\verify-pe.ps1" -Path "dist\FarFileClipboard-%ARCH_NAME%\FarFileClipboard.dll" -Expected "%ARCH_NAME%"
if errorlevel 1 goto :fail

echo.
echo OK: dist\FarFileClipboard-%ARCH_NAME%\FarFileClipboard.dll
echo.
popd
exit /b 0

:fail
echo.
echo BUILD FAILED for %ARCH_NAME%.
popd
exit /b 1
