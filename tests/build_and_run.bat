@echo off
rem Builds and runs the loopback network test with the installed MSVC (VS2022 Build Tools), x64 target.
setlocal
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /EHsc /W3 /Fo:out\ /Fe:out\net_test.exe net_test.cpp ..\core\Session.cpp ..\core\Tunnel.cpp || exit /b 1
out\net_test.exe
