@echo off
rem Builds the plugin and assembles dist\KenshiMP\ = the folder to copy into [Kenshi]\mods\
setlocal
cd /d "%~dp0"
call plugin\build.bat || exit /b 1
set "D=dist\KenshiMP"
if not exist "%D%" mkdir "%D%"
copy /y plugin\out\KenshiMP.dll "%D%\" >nul
copy /y kenshimp.cfg "%D%\" >nul
copy /y GUIDE.md "%D%\" >nul
copy /y RISKS.md "%D%\" >nul
copy /y ..\deps\KenshiLib_Examples\HelloWorld\HelloWorld\HelloWorld.mod "%D%\KenshiMP.mod" >nul
> "%D%\RE_Kenshi.json" echo { "Plugins" : [ "KenshiMP.dll" ] }
echo Package ready: %~dp0%D%
