@echo off
setlocal
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"
set "INCLUDE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\winrt"
set "LIB=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64;%PATH%"
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