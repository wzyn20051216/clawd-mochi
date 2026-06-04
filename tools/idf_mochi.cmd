@echo off
setlocal

set "PROJECT_ROOT=%~dp0.."
if "%IDF_PATH%"=="" (
    echo IDF_PATH is not set. Please run this helper inside an ESP-IDF terminal. 1>&2
    exit /b 1
)
set "CONDA_PREFIX="
set "CONDA_DEFAULT_ENV="
set "CONDA_PROMPT_MODIFIER="
set "PYTHONHOME="
set "PYTHONPATH="

if not exist "%IDF_PATH%\export.bat" (
    echo Missing ESP-IDF export.bat: "%IDF_PATH%\export.bat" 1>&2
    echo Please open an ESP-IDF terminal, or set IDF_PATH to your ESP-IDF install path. 1>&2
    exit /b 1
)

if "%~1"=="" (
    set "MOCHI_NAME=build"
    set "IDF_ARGS=build"
) else (
    set "MOCHI_NAME=%~1"
    set "IDF_ARGS=%*"
)

if "%MOCHI_NAME:~0,1%"=="-" (
    set "MOCHI_NAME=idf"
)

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

cd /d "%PROJECT_ROOT%"
if errorlevel 1 exit /b %ERRORLEVEL%

py -3 "%PROJECT_ROOT%\tools\mochi_task.py" --name "%MOCHI_NAME%" -- idf.py %IDF_ARGS%
exit /b %ERRORLEVEL%
