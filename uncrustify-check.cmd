@echo off
setlocal EnableExtensions DisableDelayedExpansion
if /i "%~1"=="--help" (
    echo Usage: uncrustify-check.cmd
    echo Check tracked C/C++ files without changing them.
    echo Exit codes: 0 = clean, 1 = formatting differences or file errors, 2 = setup error.
    exit /b 0
)
if not "%~1"=="" (
    echo ERROR: Unknown argument. Use --help.
    exit /b 2
)
call "%~dp0uncrustify-apply.cmd" --check
exit /b %errorlevel%
