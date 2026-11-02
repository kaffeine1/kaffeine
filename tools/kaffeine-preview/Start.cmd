@echo off
rem Copyright (C) 2026, The AROS Development Team. All rights reserved.
rem Author: Michele Dipace (@kaffeine1)
py -3 "%~dp0preview.py" start
set "PREVIEW_EXIT=%ERRORLEVEL%"
echo.
pause
exit /b %PREVIEW_EXIT%
