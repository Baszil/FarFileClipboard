@echo off
setlocal EnableExtensions
pushd "%~dp0"

set "ARCH=%~1"
if not defined ARCH set "ARCH=x64"

if /I "%ARCH%"=="x86" (
    set "ARCH_NAME=x86"
) else if /I "%ARCH%"=="Win32" (
    set "ARCH_NAME=x86"
) else if /I "%ARCH%"=="x64" (
    set "ARCH_NAME=x64"
) else if /I "%ARCH%"=="ARM64" (
    set "ARCH_NAME=ARM64"
) else (
    echo ERROR: unsupported architecture "%ARCH%".
    echo Usage: package.cmd [x86^|x64^|ARM64]
    popd
    exit /b 2
)

call build.cmd %ARCH_NAME%
if errorlevel 1 goto :fail

set "RELEASE_DIR=%CD%\release"
set "STAGE=%RELEASE_DIR%\FarFileClipboard-1.0.0-%ARCH_NAME%"
set "ZIP=%RELEASE_DIR%\FarFileClipboard-1.0.0-%ARCH_NAME%.zip"

if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
if exist "%STAGE%" rmdir /S /Q "%STAGE%"
if exist "%ZIP%" del /Q "%ZIP%"

mkdir "%STAGE%"
xcopy /E /I /Y "dist\FarFileClipboard-%ARCH_NAME%" "%STAGE%\FarFileClipboard" >nul
if errorlevel 1 goto :fail
copy /Y "install.cmd" "%STAGE%\install.cmd" >nul
copy /Y "install.ps1" "%STAGE%\install.ps1" >nul
copy /Y "README.md" "%STAGE%\README.md" >nul
copy /Y "README.ru.md" "%STAGE%\README.ru.md" >nul

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -CompressionLevel Optimal"
if errorlevel 1 goto :fail

rmdir /S /Q "%STAGE%"

echo.
echo OK: %ZIP%
popd
exit /b 0

:fail
echo PACKAGE FAILED for %ARCH_NAME%.
popd
exit /b 1
