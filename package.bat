@echo off
rem Builds the plugin and assembles dist\KenshiMP\ = the folder to copy into [Kenshi]\mods\
setlocal
cd /d "%~dp0"
call plugin\build.bat || exit /b 1
set "D=dist\KenshiMP"
rem Start from an empty folder: it is uploaded as is to the Steam Workshop (no stale files).
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
copy /y plugin\out\KenshiMP.dll "%D%\" >nul
copy /y kenshimp.cfg "%D%\" >nul
copy /y GUIDE.md "%D%\" >nul
copy /y RISKS.md "%D%\" >nul
copy /y LICENSE "%D%\" >nul
copy /y NOTICE.md "%D%\" >nul
copy /y README.md "%D%\" >nul
copy /y ..\deps\KenshiLib_Examples\HelloWorld\HelloWorld\HelloWorld.mod "%D%\KenshiMP.mod" >nul
> "%D%\RE_Kenshi.json" echo { "Plugins" : [ "KenshiMP.dll" ] }
rem Without RE_Kenshi: loader + KenshiLib (RE_Kenshi 0.3.5's build, GPLv3) + address table of the
rem stock Steam build, named as KenshiLib expects it (see tools\README.md) + enable/disable scripts.
copy /y plugin\out\KenshiMP_Loader.dll "%D%\" >nul
copy /y ..\deps\re_release\install\KenshiLib.dll "%D%\" >nul || exit /b 1
mkdir "%D%\rva\RE_Kenshi\RVAs"
copy /y tools\rva\Steam_1.0.68.br "%D%\rva\RE_Kenshi\RVAs\Steam_1.0.65.br" >nul
copy /y "tools\Enable KenshiMP.bat" "%D%\" >nul
copy /y "tools\Disable KenshiMP.bat" "%D%\" >nul
copy /y tools\kenshimp_enable.ps1 "%D%\" >nul
copy /y tools\kenshimp_disable.ps1 "%D%\" >nul
echo Package ready: %~dp0%D%
