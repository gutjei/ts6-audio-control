@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio Build Tools with Desktop development with C++ is required.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ROUTER_VS=%%i"
if not defined ROUTER_VS (
  echo No Visual Studio installation with C++ tools was found.
  exit /b 1
)
call "%ROUTER_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "ROUTER_OUT=build"
if not "%~1"=="" set "ROUTER_OUT=%~1"
if not exist "%ROUTER_OUT%" mkdir "%ROUTER_OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /LD /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000A native\router.cpp /Fo"%ROUTER_OUT%\router.obj" /Fe"%ROUTER_OUT%\audio_router_volume.dll" /link ole32.lib mmdevapi.lib uuid.lib
exit /b %errorlevel%
