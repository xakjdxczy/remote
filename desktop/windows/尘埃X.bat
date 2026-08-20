@echo off
set ROOT=%~dp0..\..
if exist "%ROOT%\desktop\cpp\build\Release\DustX.exe" (
  start "" "%ROOT%\desktop\cpp\build\Release\DustX.exe"
  exit /b 0
)
if exist "%~dp0DustX\DustX.exe" (
  start "" "%~dp0DustX\DustX.exe"
  exit /b 0
)
if exist "%ROOT%\dist\DustX\DustX.exe" (
  start "" "%ROOT%\dist\DustX\DustX.exe"
  exit /b 0
)
echo 还没有打好桌面程序。请在 Windows 上编译，或从 GitHub Actions 下载 DustX-windows：
echo   cmake -B desktop\cpp\build -G "Visual Studio 17 2022" -A x64
echo   cmake --build desktop\cpp\build --config Release
pause
exit /b 1
