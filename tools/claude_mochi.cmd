@echo off
py -3 "%~dp0agent_mochi.py" claude -- %*
exit /b %ERRORLEVEL%
