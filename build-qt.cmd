@echo off
setlocal
set VSLANG=1033
cd /d "%~dp0"
set "QT_PREFIX=%~1"
if not defined QT_PREFIX set "QT_PREFIX=%QT_ROOT_DIR%"
if not defined QT_PREFIX (
  echo Usage: build-qt.cmd path-to-Qt-msvc2022_64 [path-to-released-audio_mixer.dll]
  exit /b 2
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "QT_APP_VS=%%i"
if not defined QT_APP_VS exit /b 2
call "%QT_APP_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set VSLANG=1033
set "QT_APP_CMAKE=%QT_APP_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%QT_APP_CMAKE%" set "QT_APP_CMAKE=cmake"
"%QT_APP_CMAKE%" -S . -B build\qt-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%QT_PREFIX%" -DCMAKE_INSTALL_PREFIX="%CD%\dist\qt" -DMIXER_DLL:FILEPATH="%~2"
if errorlevel 1 exit /b 1
"%QT_APP_CMAKE%" --build build\qt-msvc --config Release --parallel
if errorlevel 1 exit /b 1
"%QT_APP_CMAKE%" --install build\qt-msvc --config Release
exit /b %errorlevel%
