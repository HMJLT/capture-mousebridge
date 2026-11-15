@echo off
setlocal
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] Visual Studio Installer / vswhere.exe not found
  exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo [ERROR] MSVC C++ tools were not found
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%OUT%"
cl /nologo /std:c11 /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 ^
  /I "%SRC%" ^
  "%SRC%\main.c" ^
  "%SRC%\common.c" ^
  "%SRC%\serial_link.c" ^
  "%SRC%\capture.c" ^
  "%SRC%\input_bridge.c" ^
  "%SRC%\udp_link.c" ^
  "%SRC%\mcp.c" ^
  "%SRC%\link.c" ^
  "%SRC%\pipe_mcp.c" ^
  "%SRC%\lan_mcp.c" ^
  "%SRC%\gui.c" ^
  /Fe:CaptureMouseBridge.exe ^
  /link /SUBSYSTEM:WINDOWS ^
  mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib mfcore.lib ole32.lib ^
  user32.lib gdi32.lib advapi32.lib ws2_32.lib shell32.lib
if errorlevel 1 (echo [ERROR] build failed & exit /b 1)
echo [OK] %OUT%\CaptureMouseBridge.exe
endlocal
