@echo off
setlocal
cd /d "%~dp0"
python "translate.py" %*
if errorlevel 1 (
  echo.
  echo The script stopped with an error.
)
echo.
pause
