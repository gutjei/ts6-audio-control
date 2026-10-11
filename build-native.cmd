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
cl /nologo /O2 /MT /W4 /c native\vendor\minhook\src\buffer.c native\vendor\minhook\src\hook.c native\vendor\minhook\src\trampoline.c native\vendor\minhook\src\hde\hde64.c /Fo"%ROUTER_OUT%/"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /LD /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000A native\router.cpp native\agent.cpp /Fo"%ROUTER_OUT%/" /Fe"%ROUTER_OUT%\audio_mixer.dll" /link "%ROUTER_OUT%\buffer.obj" "%ROUTER_OUT%\hook.obj" "%ROUTER_OUT%\trampoline.obj" "%ROUTER_OUT%\hde64.obj" ole32.lib mmdevapi.lib uuid.lib advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /LD /D_WIN32_WINNT=0x0A00 native\audio_inventory.cpp /Fo"%ROUTER_OUT%\audio_inventory.obj" /Fe"%ROUTER_OUT%\audio_inventory.dll" /link ole32.lib mmdevapi.lib uuid.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /LD /D_WIN32_WINNT=0x0A00 native\transport.cpp /Fo"%ROUTER_OUT%\transport.obj" /Fe"%ROUTER_OUT%\audio_transport.dll"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 native\clock.cpp /Fo"%ROUTER_OUT%\clock.obj" /Fe"%ROUTER_OUT%\audio_clock.exe"
exit /b %errorlevel%
