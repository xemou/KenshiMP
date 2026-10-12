# Verifies one in-game scenario against its logs and prints PASS / FAIL / SKIP per check.
#   verify.ps1 -Scenario host -Game out\host_game.txt -Bot out\host_bot.txt
# A check is a regular expression that must match (at least -min times) in the game's log (RE_Kenshi_log.txt,
# "KenshiMP:" lines) or in the bot's output. A "never" check must NOT match (crash, exception...).
# Exit code = number of FAILs.
param([Parameter(Mandatory)][string]$Scenario, [string]$Game = '', [string]$Bot = '', [string]$Report = '')
$ErrorActionPreference = 'Stop'
$checks = @{}

# ---------------------------------------------------------------------------------------------- host
# The game hosts (new game, test world); the bot joins as a second player and runs its feature script.
$checks['host'] = @(
  @{ id='H01'; what='handshake: the bot joined as player 1';                      on='bot';  re='connected as player 1' },
  @{ id='H02'; what='world clock synced to the client';                           on='bot';  re='world sync: host time' },
  @{ id='H03'; what='weather of the host sent to the client';                     on='bot';  re='weather from host: \d+ region' },
  @{ id='H04'; what='world states (unique NPCs) sent';                            on='bot';  re='world states: \d+ unique' },
  @{ id='H05'; what='host NPC inventories mirrored to the client';                on='bot';  re='inventories=[1-9]' },
  @{ id='H06'; what='worn backpack content mirrored';                             on='bot';  re='backpack of 01000001: 1 stack' },
  @{ id='H07'; what='load sharing: shared world announced';                       on='bot';  re='zone mode from the host: shared world' },
  @{ id='H08'; what='load sharing: client far away goes to its own world';        on='bot';  re='zone mode from the host: our own world' },
  @{ id='H09'; what='load sharing: back to the shared world';                     on='bot';  re='zone mode from the host: our own world(?s).*zone mode from the host: shared world' },
  @{ id='H10'; what='player trade offer routed to the other player';              on='bot';  re='trade offer from player 0' },
  @{ id='H11'; what='loot validated by the owner (take applied)';                 on='bot';  re='LOOTED from player 0' },
  @{ id='H12'; what='refused transfer: the requester is told to undo';            on='bot';  re='REFUSED the take' },
  @{ id='H13'; what='chat sent by a client';                                      on='bot';  re='script: chat sent' },
  @{ id='H14'; what='town door hits by a client reach the host (door broken)';    on='bot';  re='town building 5000618-Genesis\.mod .* 1/1 door\(s\) broken' },
  @{ id='H15'; what='bounty for a crime seen by the host NPCs';                   on='bot';  re='bounty crime from the host: .* \+500' },
  @{ id='H16'; what='ground item picked up by the host, owner told';              on='bot';  re='ground item \S+ picked up by player 0' },
  @{ id='H17'; what='host item dropped on the ground announced to the client';    on='bot';  re='host dropped 1 x \S+ on the ground' },
  @{ id='H18'; what='melee damage routed to the victim''s owner';                 on='bot';  re='DAMAGE for player 1 on 01000001' },
  @{ id='H19'; what='ranged shots routed (client side sends them)';               on='bot';  re='shot sent' },
  @{ id='H20'; what='war declared and combat task mirrored';                      on='bot';  re='script: war declared(?s).*host task -> 3' },
  @{ id='H21'; what='peace made';                                                 on='bot';  re='script: peace made' },
  @{ id='H22'; what='merchant money transfer refused and undone';                 on='bot';  re='UNDO from player 0: remove 50 x \$money' },
  @{ id='H23'; what='client''s own-world town change reported to the host';       on='bot';  re='own world: 5000618-Genesis\.mod reported' },
  @{ id='H24'; what='client''s own-world unique death reported to the host';      on='bot';  re='own world: unique \S+ reported dead' },
  @{ id='H25'; what='host table: the unique is dead for everybody';               on='bot';  re='test unique \S+ is 0 \(0 dead\)' },
  @{ id='H26'; what='gear given to a host character: accepted or refused-and-undone'; on='any'; re='UNDO from player 0: give back 2 x|gave 2 x \S+ to \S+ \(\d+ item\(s\) created\)' },
  @{ id='H27'; what='task of a client character mirrored on the host';            on='bot';  re='host task -> 27' },
  @{ id='H28'; what='client floor sent';                                          on='bot';  re='floor test:' },
  @{ id='G01'; what='trade window: loot moved';                                   on='game'; re='autotest: trade loot -> moved' },
  @{ id='G02'; what='worn backpack: item taken out';                              on='game'; re='autotest: backpack take -> moved' },
  @{ id='G03'; what='crime on a ghost: bounty added';                             on='game'; re='autotest: crime on ghost -> \+500' },
  @{ id='G04'; what='ground item picked up';                                      on='game'; re='autotest: ground pickup -> picked up' },
  @{ id='G05'; what='bag given and worn';                                         on='game'; re='autotest: bag given -> .* worn=1' },
  @{ id='G06'; what='bag dropped';                                                on='game'; re='autotest: bag dropped -> dropped' },
  @{ id='G07'; what='factions screen opens';                                      on='game'; re='autotest: factions screen opened -> 1' },
  @{ id='G08'; what='ALLY button works';                                          on='game'; re='autotest: ALLY pressed in the factions screen -> 1' },
  @{ id='G09'; what='relation changed to allies';                                 on='game'; re='autotest: relation with \S+ now 100' },
  @{ id='G10'; what='multiplayer window opens';                                   on='game'; re='autotest: lobby window opened -> 1' },
  @{ id='G11'; what='advanced settings toggle';                                   on='game'; re='autotest: Advanced settings pressed -> 1' },
  @{ id='G12'; what='multiplayer window closes';                                  on='game'; re='autotest: lobby open after close -> 0' },
  @{ id='G13'; what='a client spoofing the host faction is refused';              on='game'; re='faction .* ignored \(players. characters go to their own faction\)' },
  @{ id='G14'; what='host zone watch / load sharing decisions logged';            on='game'; re='load sharing: player 1 is' },
  @{ id='N01'; what='no crash';                                                   on='game'; never=$true; re='Crash detected|has crashed' },
  @{ id='N02'; what='no engine exception swallowed in a hook';                    on='game'; never=$true; re='hook .* failed' }
)

# ---------------------------------------------------------------------------------------------- client
# The bot hosts and replays a recorded world; the game joins as a client with the zone script 30 s / 60 s.
$checks['client'] = @(
  @{ id='C01'; what='the game connected as a client';                             on='game'; re='connected, I am player 1' },
  @{ id='C02'; what='host NPCs spawned as ghosts';                                on='game'; re='NPC ghost\(s\) spawned' },
  @{ id='C03'; what='client shared world (host simulates around us)';             on='game'; re='load sharing: the host simulates the world around us' },
  @{ id='C04'; what='client far from the host: own world';                        on='game'; re='load sharing: we simulate our own surroundings' },
  @{ id='C05'; what='back in the shared world: local NPCs kept until the host''s arrive'; on='game'; re='keeping the \d+ local NPC' },
  @{ id='C06'; what='the host''s NPCs arrived, then local ones removed';          on='game'; re='the host.s NPCs arrived after \d+ ms' },
  @{ id='C07'; what='switching to the own world handles the remembered NPCs';     on='game'; re='load sharing: \d+ remembered NPC' },
  @{ id='C08'; what='town door broken by the host applied';                       on='game'; re='autotest: town door' },
  @{ id='C09'; what='bounty from the host applied';                               on='game'; re='bount' },
  @{ id='N01'; what='no crash';                                                   on='game'; never=$true; re='Crash detected|has crashed' }
)

# ---------------------------------------------------------------------------------------------- title join
# The bot hosts first; the game joins on the TITLE SCREEN, then creates its world while connected.
$checks['title'] = @(
  @{ id='T01'; what='connected on the title screen';                              on='game'; re='connected, I am player 1.*autotest: pressed ConfirmButton' },
  @{ id='T02'; what='world created while connected: buildings are furnished';     on='game'; re='building census r=250: .* (?:[1-9]\d*) furniture' },
  @{ id='T03'; what='at least one building furnished';                            on='game'; re='building census r=250: .* (?:[1-9]\d*) structure\(s\) furnished' },
  @{ id='T04'; what='shared world entered';                                       on='game'; re='load sharing: the host simulates the world around us' },
  @{ id='N01'; what='no crash';                                                   on='game'; never=$true; re='Crash detected|has crashed' }
)

# ---------------------------------------------------------------------------------------------- trader
# Bot host with a shopkeeper (squad template) at a shop; the game stands next to the shop (autotest_goto_shop).
$checks['trader'] = @(
  @{ id='S01'; what='shopkeeper ghost created in a trader squad';                 on='game'; re='trader ghost \S+ .*isATrader=1' },
  @{ id='S02'; what='the squad template of the host was found';                   on='game'; re='trader \S+: squad .* -> template' },
  @{ id='S03'; what='shop shelves mirrored from the host';                        on='game'; re='town container \S+ opened \(shop shelves\)' },
  @{ id='S04'; what='the host answered with its shelf content';                   on='bot';  re='TOWN CONTAINER \S+ opened' },
  @{ id='S05'; what='shelf content applied';                                      on='game'; re='inventory of \S+ updated: \d+ stack' },
  @{ id='S06'; what='a purchase during a trade reaches the host';                 on='game'; re='took \d+ x .* from \S+ \(sent to its owner\)' },
  @{ id='S07'; what='the host validated the purchase';                            on='bot';  re='TOWN CONTAINER \S+: client took .*validated' },
  @{ id='N01'; what='no crash';                                                   on='game'; never=$true; re='Crash detected|has crashed' }
)

# ---------------------------------------------------------------------------------------------- share
# The bot hosts and walks (KMP_TEST_HOSTPATH): own world -> shared -> own -> shared, with a 12 s zone load.
$checks['share'] = @(
  @{ id='W01'; what='shared world: local NPCs kept until the host''s arrive';     on='game'; re='keeping the [1-9]\d* local NPC' },
  @{ id='W02'; what='the host''s NPCs arrived, local ones removed after that';    on='game'; re='the host.s NPCs arrived after \d+ ms: removing the local NPCs' },
  @{ id='W03'; what='own world again: removed NPCs remembered, some to bring back'; on='game'; re='load sharing: \d+ remembered NPC\(s\): [1-9]\d* to bring back' },
  @{ id='W04'; what='remembered NPCs brought back';                               on='game'; re='\d+ NPC\(s\) brought back' },
  @{ id='W05'; what='gear put back on a restored NPC';                            on='game'; re='restored NPC .* worn/held item\(s\) put back' },
  @{ id='W06'; what='local NPCs exist again in the own world (census)';           on='game'; re='\[own world\]: zone loaded=1; local NPCs \d+/[1-9]\d*/' },
  @{ id='W07'; what='bot: client sent to its own world when far';                 on='bot';  re='zone mode for player 1: on its own' },
  @{ id='W08'; what='bot: client back in the shared world, NPCs sent after the load'; on='bot';  re='zone mode for player 1: shared(?s).*zone loaded: sending our NPCs' },
  @{ id='W09'; what='bot: host NPCs despawned for a client that left';            on='bot';  re='NPC\(s\) despawned for the client' },
  @{ id='N01'; what='no crash';                                                   on='game'; never=$true; re='Crash detected|has crashed' }
)
if (-not $checks.ContainsKey($Scenario)) { Write-Host "unknown scenario '$Scenario'"; exit 99 }
$gameText = if ($Game -and (Test-Path $Game)) { [IO.File]::ReadAllText((Resolve-Path $Game), [Text.Encoding]::UTF8) -replace "`r", '' } else { '' }
$botText  = if ($Bot  -and (Test-Path $Bot))  { [IO.File]::ReadAllText((Resolve-Path $Bot),  [Text.Encoding]::UTF8) -replace "`r", '' } else { '' }
$kmpOnly = ($gameText -split "`n" | Where-Object { $_ -match 'KenshiMP' }) -join "`n"

$fail = 0; $rows = @()
foreach ($c in $checks[$Scenario]) {
  $text = if ($c.on -eq 'bot') { $botText } elseif ($c.on -eq 'any') { $kmpOnly + "`n" + $botText } else { $kmpOnly + "`n" + $gameText }
  if ($c.on -eq 'bot' -and -not $botText) { $res = 'SKIP' }
  elseif ($c.on -eq 'game' -and -not $gameText) { $res = 'SKIP' }
  else {
    $hit = [regex]::IsMatch($text, $c.re, 'Singleline')
    if ($c.ContainsKey('never') -and $c.never) { $res = if ($hit) { 'FAIL' } else { 'PASS' } } else { $res = if ($hit) { 'PASS' } else { 'FAIL' } }
  }
  if ($res -eq 'FAIL') { ++$fail }
  $rows += [pscustomobject]@{ Scenario = $Scenario; Id = $c.id; Result = $res; Check = $c.what }
}
$rows | Format-Table Id, Result, Check -AutoSize | Out-String -Width 200 | Write-Host
"{0}: {1} PASS, {2} FAIL, {3} SKIP" -f $Scenario, @($rows | Where-Object Result -eq 'PASS').Count, $fail, @($rows | Where-Object Result -eq 'SKIP').Count | Write-Host
if ($Report) { $rows | ForEach-Object { "| $($_.Scenario) | $($_.Id) | $($_.Result) | $($_.Check) |" } | Add-Content $Report -Encoding UTF8 }
exit $fail



