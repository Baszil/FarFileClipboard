@echo off
setlocal
pushd "%~dp0"

call build.cmd
if errorlevel 1 goto :fail

set "RELEASE_DIR=%CD%\release"
set "STAGE=%RELEASE_DIR%\FarFileClipboard-1.0.0-x64"
set "ZIP=%RELEASE_DIR%\FarFileClipboard-1.0.0-x64.zip"

if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
if exist "%STAGE%" rmdir /S /Q "%STAGE%"
if exist "%ZIP%" del /Q "%ZIP%"

mkdir "%STAGE%"
xcopy /E /I /Y "dist\FarFileClipboard" "%STAGE%\FarFileClipboard" >nul
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
echo PACKAGE FAILED.
popd
exit /b 1
