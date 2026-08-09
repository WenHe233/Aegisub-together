@echo off
setlocal
cd /d "%~dp0"
python "backfill_translations.py" %*
if errorlevel 1 (
  echo.
  echo The script stopped with an error.
)
echo.
pause
