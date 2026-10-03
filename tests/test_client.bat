@echo off
rem Client-side test on ONE PC: Kenshi plays the CLIENT, the bot plays the HOST.
rem (The client side has the least in-game testing: run this before playing with a friend.)
rem
rem   tests\test_client.bat record   step 1, once: Kenshi hosts, the bot joins and records what the
rem                                   host sends (NPCs, world...) into tests\out\stream.rec (170 s)
rem   tests\test_client.bat          step 2: the bot hosts on port 47000 and replays that recording;
rem                                   Kenshi joins it as a client
rem
rem The mod list must be the game's exact one (KenshiMP refuses another list). The default below is
rem the test setup's; another one: set KMP_MODS=game 1.0.65;base;... before running this script
rem (copy it from the "mods must match" message or from session.txt of a /report).
setlocal
cd /d "%~dp0"
if not exist out\bot.exe call build_bot.bat || exit /b 1
if "%KMP_MODS%"=="" set "KMP_MODS=game 1.0.65;base;Newwworld;Dialogue;rebirth;KenshiMP"

if /i "%1"=="record" goto record

if not exist out\stream.rec (
  echo No recording yet: run "tests\test_client.bat record" first ^(see the top of this file^).
  exit /b 1
)
echo.
echo === Client test: the bot is the host (port 47000) ===
echo In Kenshi: F4 ^> address 127.0.0.1, port 47000 ^> JOIN, then start a NEW game.
echo Check, and watch the lines below:
echo   1. "Connected" message; the host's NPCs appear around you after a few seconds
echo   2. about 20 s after loading: reminder "far from ReplayHost ... Go to"
echo   3. F4 ^> GO TO on ReplayHost's line: your squad jumps next to the recorded host  -^> REGROUP line here
echo   4. open a chest or storage box of the town: TOWN CONTAINER line here, and the box shows the
echo      items listed here instead of its own; take one: "client took" line here
echo   5. /report in the chat: a KenshiMP_report_... folder on your desktop
echo The bot stops after 10 minutes.
echo.
out\bot.exe --host-replay out\stream.rec 47000 600 "%KMP_MODS%"
exit /b 0

:record
echo.
echo === Recording a host stream ===
echo In Kenshi: F4 ^> HOST, load a game in a town (NPCs and chests around), then press a key here.
pause >nul
out\bot.exe 127.0.0.1 47000 170 4 out\stream.rec "%KMP_MODS%"
echo.
echo Recorded: tests\out\stream.rec. Now in Kenshi: F4 ^> LEAVE (stop hosting), then run
echo   tests\test_client.bat
exit /b 0
