@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
call :main %*
set "rs_result=%errorlevel%"
popd
exit /b %rs_result%

:main
set "rs_config=debug"
if /i "%~1"=="release" set "rs_config=release"
if not "%~2"=="" goto :usage
if not "%~1"=="" if /i not "%~1"=="debug" if /i not "%~1"=="release" if /i not "%~1"=="test" if /i not "%~1"=="run" goto :usage

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :configure
set "rs_vs="
for /f "usebackq tokens=*" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "rs_vs=%%V"
if not defined rs_vs (
    echo Visual Studio C++ x64 build tools are required. 1>&2
    exit /b 1
)
call "%rs_vs%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

:configure
cmake --preset %rs_config%
if errorlevel 1 exit /b 1
cmake --build --preset %rs_config%
if errorlevel 1 exit /b 1
if /i "%~1"=="test" goto :test
if /i "%~1"=="run" goto :run
exit /b 0

:test
ctest --preset %rs_config%
exit /b %errorlevel%

:run
"build\%rs_config%\bin\rs-vm.exe" --firmware "build\%rs_config%\images\firmware.bin" --disk "build\%rs_config%\images\disk.img"
exit /b %errorlevel%

:usage
echo Usage: build.bat [debug^|release^|run^|test] 1>&2
exit /b 2
