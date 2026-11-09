@echo off
rem Builds GoogleDrive.dll (x64) with the MSYS2 UCRT64 GCC toolchain.
rem
rem   build.cmd            build into out\GoogleDrive
rem   build.cmd install    build, then copy into %FARHOME%\Plugins\GoogleDrive
rem   build.cmd package    build, then zip out\GoogleDrive into out\GoogleDrive-<version>-x64.zip
rem
rem Optional environment variables:
rem   MSYS2_UCRT64        path to the ucrt64\bin folder (default C:\msys64\ucrt64\bin)
rem   FARHOME             Far Manager folder for "install" (default: %ProgramFiles%\Far Manager)
rem   BUILTIN_CLIENT_ID / BUILTIN_CLIENT_SECRET
rem                       OAuth client compiled into the plugin (see src\builtin_client.h)
setlocal
set ROOT=%~dp0
if "%MSYS2_UCRT64%"=="" set MSYS2_UCRT64=C:\msys64\ucrt64\bin
set PATH=%MSYS2_UCRT64%;%PATH%
set OUT=%ROOT%out\GoogleDrive
if not exist "%OUT%" mkdir "%OUT%"

set DEFS=
if not "%BUILTIN_CLIENT_ID%"=="" set DEFS=-DBUILTIN_CLIENT_ID="\"%BUILTIN_CLIENT_ID%\"" -DBUILTIN_CLIENT_SECRET="\"%BUILTIN_CLIENT_SECRET%\""

g++ -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 %DEFS% ^
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    -shared -static -static-libgcc -static-libstdc++ ^
    -I"%ROOT%third_party" ^
    -o "%OUT%\GoogleDrive.dll" "%ROOT%src\GoogleDrive.cpp" "%ROOT%src\GoogleDrive.def" ^
    -lwinhttp -lws2_32 -lshell32 -lbcrypt -lole32 -s
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
copy /y "%ROOT%GoogleDriveEng.lng" "%OUT%\" >nul
copy /y "%ROOT%GoogleDriveEng.hlf" "%OUT%\" >nul
copy /y "%ROOT%LICENSE" "%OUT%\" >nul
copy /y "%ROOT%CHANGELOG.md" "%OUT%\changelog.md" >nul
echo Built %OUT%\GoogleDrive.dll

if /i "%1"=="install" goto :install
if /i "%1"=="package" goto :package
goto :eof

:install
set FARDIR=%FARHOME%
if "%FARDIR%"=="" set FARDIR=%ProgramFiles%\Far Manager
if not exist "%FARDIR%\Far.exe" (
    echo Far.exe not found in "%FARDIR%" - set FARHOME to your Far Manager folder.
    exit /b 1
)
if not exist "%FARDIR%\Plugins\GoogleDrive" mkdir "%FARDIR%\Plugins\GoogleDrive"
copy /y "%OUT%\*" "%FARDIR%\Plugins\GoogleDrive\" >nul
echo Installed to %FARDIR%\Plugins\GoogleDrive
goto :eof

:package
for /f "tokens=3" %%v in ('findstr /c:"#define PLUGIN_VERSION_STR" "%ROOT%src\version.h"') do set VER=%%~v
set ZIP=%ROOT%out\GoogleDrive-%VER%-x64.zip
if exist "%ZIP%" del "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%' -DestinationPath '%ZIP%'"
echo Packaged %ZIP%
goto :eof
