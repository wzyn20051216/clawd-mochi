@echo off
py -3 "%~dp0agent_mochi.py" codex -- %*
exit /b %ERRORLEVEL%
