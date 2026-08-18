@echo off
set ROOT=%~dp0..\..
set EXE=%~dp0尘埃X\尘埃X.exe
if exist "%EXE%" (
  start "" "%EXE%"
  exit /b 0
)
if exist "%ROOT%\dist\尘埃X\尘埃X.exe" (
  start "" "%ROOT%\dist\尘埃X\尘埃X.exe"
  exit /b 0
)
echo 还没有打好桌面程序。开发机打包一次即可（用户之后只需双击 尘埃X.exe）：
echo   python -m remote pack
pause
exit /b 1
