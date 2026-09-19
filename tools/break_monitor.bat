@echo off
rem Starts the PC break monitor for the LED matrix. Pass options through, e.g.
rem   break_monitor.bat --work 45 --break 5
cd /d "%~dp0"
python break_monitor.py %*
pause
