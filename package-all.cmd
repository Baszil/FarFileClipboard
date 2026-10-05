@echo off
setlocal EnableExtensions
pushd "%~dp0"

for %%A in (x86 x64 ARM64) do (
    echo.
    echo ============================================================
    echo Packaging %%A
    echo ============================================================
    call package.cmd %%A
    if errorlevel 1 goto :fail
)

echo.
echo ============================================================
echo Packaging universal installer
echo ============================================================

set "RELEASE_DIR=%CD%\release"
set "STAGE=%RELEASE_DIR%\FarFileClipboard-1.0.2-all"
set "ZIP=%RELEASE_DIR%\FarFileClipboard-1.0.2-all.zip"

if exist "%STAGE%" rmdir /S /Q "%STAGE%"
if exist "%ZIP%" del /Q "%ZIP%"
mkdir "%STAGE%"

for %%A in (x86 x64 ARM64) do (
    xcopy /E /I /Y "dist\FarFileClipboard-%%A" "%STAGE%\FarFileClipboard-%%A" >nul
    if errorlevel 1 goto :fail
)

copy /Y "install.cmd" "%STAGE%\install.cmd" >nul
copy /Y "install.ps1" "%STAGE%\install.ps1" >nul
copy /Y "README.md" "%STAGE%\README.md" >nul
copy /Y "README.ru.md" "%STAGE%\README.ru.md" >nul

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -CompressionLevel Optimal"
if errorlevel 1 goto :fail

rmdir /S /Q "%STAGE%"

echo.
echo ALL PACKAGES OK.
echo   release\FarFileClipboard-1.0.2-x86.zip
echo   release\FarFileClipboard-1.0.2-x64.zip
echo   release\FarFileClipboard-1.0.2-ARM64.zip
echo   release\FarFileClipboard-1.0.2-all.zip
popd
exit /b 0

:fail
echo.
echo PACKAGE-ALL FAILED.
popd
exit /b 1
