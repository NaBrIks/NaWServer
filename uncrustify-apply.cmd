@echo off
setlocal EnableExtensions DisableDelayedExpansion
if /i "%~1"=="--help" goto help
set "formatMode=apply"
if /i "%~1"=="--check" (
    set "formatMode=check"
) else if not "%~1"=="" (
    echo ERROR: Unknown argument. Use --help.
    exit /b 2
)
if not "%~2"=="" exit /b 2
pushd "%~dp0" || exit /b 2
where git.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: git.exe is missing from PATH.
    goto setupError
)
rem Use .exe explicitly: this script must never call itself.
where uncrustify.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: Add the directory containing uncrustify.exe to PATH, then reopen your terminal.
    goto setupError
)
for /f "delims=" %%V in ('uncrustify.exe --version') do set "formatVersion=%%V"
if not "%formatVersion%"=="Uncrustify-0.83.0_f" (
    echo ERROR: This profile requires Uncrustify-0.83.0_f.
    goto setupError
)
git.exe rev-parse --show-toplevel >nul 2>&1
if errorlevel 1 goto setupError
if not exist "uncrustify.cfg" goto setupError
uncrustify.exe -q -c uncrustify.cfg --update-config >nul
if errorlevel 1 goto setupError
set /a formatCount=0, formatFailures=0 >nul
rem Git lists tracked files only. Skip generated assets and vendored sources.
set "formatFileList=%TEMP%\nawserver-uncrustify-%RANDOM%-%RANDOM%.txt"
git.exe -c "core.quotepath=false" ls-files -- "*.c" "*.cc" "*.cpp" "*.cxx" "*.h" "*.hh" "*.hpp" "*.hxx" ":(glob,exclude)**/assets.hpp" ":(glob,exclude)**/vendor/**" ":(glob,exclude)**/third_party/**" ":(glob,exclude)**/generated/**" ":(glob,exclude)**/fonts/**" > "%formatFileList%"
if errorlevel 1 (
    del /q "%formatFileList%" >nul 2>&1
    goto setupError
)
for /f "usebackq delims=" %%F in ("%formatFileList%") do call :processFile "%%F"
del /q "%formatFileList%" >nul 2>&1
echo %formatMode%: %formatCount% files, %formatFailures% failures.
popd
if not "%formatFailures%"=="0" exit /b 1
exit /b 0

:processFile
rem A tracked file may have been deleted in the working tree.
if not exist "%~1" exit /b 0
set /a formatCount+=1 >nul
echo [%formatMode%] %~1
if "%formatMode%"=="check" (
    uncrustify.exe -q -c uncrustify.cfg --check "%~1"
) else (
    uncrustify.exe -q -c uncrustify.cfg --replace --no-backup "%~1"
)
if errorlevel 1 set /a formatFailures+=1 >nul
exit /b 0

:setupError
echo ERROR: Formatting setup failed. No source files were processed.
popd
exit /b 2

:help
echo Usage: uncrustify-apply.cmd [--check ^| --help]
echo Default: format tracked C/C++ files in this repository in place.
echo --check: report formatting differences without changing files.
echo Skips deleted files, generated assets.hpp, vendor and third_party directories.
echo Requires git.exe and uncrustify.exe in PATH. Uses the repository uncrustify.cfg.
exit /b 0
