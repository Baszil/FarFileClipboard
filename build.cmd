@echo off
setlocal
pushd "%~dp0"

set "BUILD_DIR=build"

echo [1/2] Configuring FarFileClipboard x64 with Visual Studio 2022...
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail

echo [2/2] Building Release...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :fail

echo.
echo OK: dist\FarFileClipboard\FarFileClipboard.dll
echo.
popd
exit /b 0

:fail
echo.
echo BUILD FAILED.
popd
exit /b 1
