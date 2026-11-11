@echo off
rem Double-click launcher for the live cricket scoreboard. Add options after
rem the script name, for example:  cricket_score.py --team IND
cd /d "%~dp0"
python cricket_score.py %*
pause
