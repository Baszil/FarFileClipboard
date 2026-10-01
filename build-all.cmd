@echo off
setlocal EnableExtensions
pushd "%~dp0"

for %%A in (x86 x64 ARM64) do (
    echo.
    echo ============================================================
    echo Building %%A
    echo ============================================================
    call build.cmd %%A
    if errorlevel 1 goto :fail
)

echo.
echo ALL BUILDS OK.
echo   dist\FarFileClipboard-x86\FarFileClipboard.dll
echo   dist\FarFileClipboard-x64\FarFileClipboard.dll
echo   dist\FarFileClipboard-ARM64\FarFileClipboard.dll
popd
exit /b 0

:fail
echo.
echo BUILD-ALL FAILED.
popd
exit /b 1
