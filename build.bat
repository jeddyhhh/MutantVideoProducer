@echo off
rem Builds Mutant Video Producer on Windows with the MSVC toolchain.
rem Usage: build.bat [Release|Debug|RelWithDebInfo] [CMake targets...] (default: all)
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set BUILD_TARGETS=
shift
:read_targets
if "%~1"=="" goto configure
set BUILD_TARGETS=%BUILD_TARGETS% %~1
shift
goto read_targets
:configure

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL (
    echo Could not find a Visual Studio installation with the C++ x64 toolset.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set BUILD_DIR=%~dp0build\%CONFIG%
set AI_BUNDLE_ARG=
if defined MVP_BUNDLE_AI set AI_BUNDLE_ARG=-DMVP_BUNDLE_AI=%MVP_BUNDLE_AI%
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% %AI_BUNDLE_ARG% || exit /b 1
if defined BUILD_TARGETS (
    cmake --build "%BUILD_DIR%" --target %BUILD_TARGETS% || exit /b 1
) else (
    cmake --build "%BUILD_DIR%" || exit /b 1
)

echo.
echo Built: %BUILD_DIR%\bin\MutantVideoProducer.exe
