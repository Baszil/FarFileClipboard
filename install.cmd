@echo off
setlocal EnableExtensions
set "INSTALLER_PS1=%~dp0install.ps1"

if not exist "%INSTALLER_PS1%" (
  echo ERROR: install.ps1 was not found next to install.cmd.
  exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%INSTALLER_PS1%" %*
exit /b %ERRORLEVEL%
