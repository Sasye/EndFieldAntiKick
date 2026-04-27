@echo off
echo ==========================================
echo   Endfield AntiKick Build Script
echo ==========================================
echo.

:: Check if Visual Studio environment is set up
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] cl.exe not found!
    echo Please run this from "x64 Native Tools Command Prompt for VS"
    echo Or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    pause
    exit /b 1
)

echo [OK] MSVC compiler found
echo.

:: Build anti_afk.dll
echo [1/3] Compiling version resource ...
rc /nologo /fo bin\version.res src\version.rc

echo [2/3] Building anti_afk.dll ...
cl /nologo /O2 /MD /LD /EHsc /std:c++17 ^
    /I"deps\minhook_lib\include" ^
    src\anti_afk.cpp ^
    "deps\minhook_lib\lib\libMinHook.x64.lib" ^
    bin\version.res ^
    /Fe"bin\anti_afk.dll" ^
    /link /DLL

if %errorlevel% neq 0 (
    echo [ERROR] anti_afk.dll build failed!
    pause
    exit /b 1
)
echo [OK] anti_afk.dll built successfully
echo.

:: Build d3dcompiler_47.dll (proxy loader)
echo [3/3] Building d3dcompiler_47.dll (proxy loader) ...
cl /nologo /O2 /MD /LD /EHsc /std:c++17 ^
    src\proxy_d3dcompiler.cpp ^
    /Fe"bin\d3dcompiler_47.dll" ^
    /link /DLL

if %errorlevel% neq 0 (
    echo [ERROR] d3dcompiler_47.dll build failed!
    pause
    exit /b 1
)
echo [OK] d3dcompiler_47.dll built successfully
echo.

:: Clean up intermediate files
del /q anti_afk.obj 2>nul
del /q anti_afk.exp 2>nul
del /q anti_afk.lib 2>nul
del /q proxy_d3dcompiler.obj 2>nul
del /q proxy_d3dcompiler.exp 2>nul
del /q proxy_d3dcompiler.lib 2>nul

echo ==========================================
echo   Build Complete!
echo ==========================================
echo.
echo Output files in bin\:
echo   - anti_afk.dll          (anti-kick payload)
echo   - d3dcompiler_47.dll    (proxy loader)
echo.
echo To install, copy to game folder:
echo   bin\anti_afk.dll         -> game folder
echo   bin\d3dcompiler_47.dll   -> game folder
echo   anti_afk_config.txt      -> game folder
echo.
pause
