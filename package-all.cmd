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
echo ALL PACKAGES OK.
echo   release\FarFileClipboard-1.0.0-x86.zip
echo   release\FarFileClipboard-1.0.0-x64.zip
echo   release\FarFileClipboard-1.0.0-ARM64.zip
popd
exit /b 0

:fail
echo.
echo PACKAGE-ALL FAILED.
popd
exit /b 1
