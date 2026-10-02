@echo off
rem Builds the test bot (second fake player) with VS2022 Build Tools.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /EHsc /W3 /Foout\ /Feout\bot.exe bot.cpp ..\core\Session.cpp
