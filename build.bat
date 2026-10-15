@echo off
setlocal
cd /d "%~dp0"
if not "%~1"=="" (
    echo Usage: build.bat
    exit /b 2
)
if not defined APPLEPIE_SDK_DIR set "APPLEPIE_SDK_DIR=%~dp0deps\applepie_sdk"
for %%h in (applepie.h applepie_client.h applepie_native.h applepie_seh.h applepie_pointer_scope.h) do (
    if not exist "%APPLEPIE_SDK_DIR%\%%h" (
        echo [ERROR] Missing SDK header: %APPLEPIE_SDK_DIR%\%%h
        echo Use the bundled deps\applepie_sdk or set APPLEPIE_SDK_DIR to a complete compatible SDK.
        exit /b 1
    )
)
echo [SDK] %APPLEPIE_SDK_DIR%
where cl >nul 2>nul
if errorlevel 1 (
    if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
        echo [ERROR] Use an x64 Developer Command Prompt or install MSVC.
        exit /b 1
    )
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
rc /nologo /fo bin\version.res src\version.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /O2 /MD /LD /EHsc /std:c++17 /W4 /wd4505 /I"%APPLEPIE_SDK_DIR%" ^
    src\anti_afk.cpp bin\version.res user32.lib ^
    /Fo:bin\anti_afk.obj /Fe:bin\anti_afk.dll ^
    /link /INCREMENTAL:NO /OPT:NOICF /MAP:bin\anti_afk.map
if errorlevel 1 exit /b 1
echo [OK] %CD%\bin\anti_afk.dll
exit /b 0
