@echo off
rem Builds KenshiMP.dll with the portable VC++ 2010 x64 toolchain extracted in deps\vc2010
rem (no Visual Studio 2010 install needed). Output: plugin\out\KenshiMP.dll
setlocal
set "ROOT=%~dp0..\.."
set "VC=%ROOT%\deps\vc2010\vc_stdamd64\Program Files(64)\Microsoft Visual Studio 10.0\VC"
set "VCX86=%ROOT%\deps\vc2010\vc_stdx86\Program Files\Microsoft Visual Studio 10.0\VC"
set "SDK=%ROOT%\deps\vc2010\WinSDKBuild_amd64\Program Files\Microsoft SDKs\Windows\v7.1"
set "KLIB=%ROOT%\deps\KenshiLib\Include"
set "KLIBS=%ROOT%\deps\KenshiLib_Examples_deps\KenshiLib\Libraries"
set "BOOST=%ROOT%\deps\KenshiLib_Examples_deps\boost_1_60_0"

set "PATH=%VC%\bin\amd64;%PATH%"
set "INCLUDE=%VCX86%\include;%SDK%\Include;%KLIB%;%KLIB%\ogre;%BOOST%"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64;%KLIBS%;%BOOST%\stage\lib"

cd /d "%~dp0"
if not exist out mkdir out
rem /GL + /LTCG (whole program optimisation) are REQUIRED: without them &Class::method yields a
rem thunk inside our DLL and KenshiLib::GetRealAddress() rejects it.
cl /nologo /c /O2 /GL /MD /EHsc /W3 /Zi /DNDEBUG /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE ^
   /Foout\ /Fdout\KenshiMP_cl.pdb KenshiMP.cpp Characters.cpp Buildings.cpp World.cpp Npcs.cpp Items.cpp Weather.cpp Chat.cpp Lobby.cpp Ground.cpp Lang.cpp Trade.cpp Steam.cpp Towns.cpp Bounties.cpp ..\core\Session.cpp ..\core\Tunnel.cpp || exit /b 1
link /nologo /DLL /LTCG /DEBUG /OPT:REF /OPT:ICF /OUT:out\KenshiMP.dll /PDB:out\KenshiMP.pdb ^
   out\KenshiMP.obj out\Characters.obj out\Buildings.obj out\World.obj out\Npcs.obj out\Items.obj out\Weather.obj out\Chat.obj out\Lobby.obj out\Ground.obj out\Lang.obj out\Trade.obj out\Steam.obj out\Towns.obj out\Bounties.obj out\Session.obj out\Tunnel.obj KenshiLib.lib OgreMain_x64.lib MyGUIEngine_x64.lib ws2_32.lib user32.lib advapi32.lib || exit /b 1
echo Built %~dp0out\KenshiMP.dll
rem Loader for games without RE_Kenshi (Ogre plugin, see Loader.cpp). KenshiLib is delay-loaded:
rem the loader picks which KenshiLib.dll to load before the first call.
cl /nologo /c /O2 /MD /EHsc /W3 /DNDEBUG /D_WINDOWS /D_USRDLL /Foout\ Loader.cpp || exit /b 1
link /nologo /DLL /OUT:out\KenshiMP_Loader.dll out\Loader.obj KenshiLib.lib delayimp.lib advapi32.lib /DELAYLOAD:KenshiLib.dll || exit /b 1
echo Built %~dp0out\KenshiMP_Loader.dll
