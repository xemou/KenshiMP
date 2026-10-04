# Writes <package>\gui\core\core_settings.xml: how Kenshi starts KenshiMP without RE_Kenshi and without
# touching the game's files. Kenshi adds the gui\ folders of every ENABLED mod to MyGUI's resources,
# so this file replaces the game's own core_settings.xml (same two settings, copied below) and adds
# a MyGUI "Plugin" list: MyGUI loads KenshiMP_Loader.dll and calls its dllStartPlugin at startup.
# A path that does not exist is skipped by MyGUI ("not found" in MyGUI.log), so the list holds every
# place the mod can be: the local mods folder, and the Steam Workshop folder once the item number is
# known (workshop\item_id.txt, written after the first upload - see workshop\UPLOAD.md).
# Paths are relative to the game folder (the game's working directory).
param([Parameter(Mandatory = $true)][string]$Dir)
$ErrorActionPreference = "Stop"
$paths = @('mods/KenshiMP/KenshiMP_Loader.dll')
$idFile = Join-Path $PSScriptRoot '..\workshop\item_id.txt'
if (Test-Path $idFile) {
    $id = (Get-Content $idFile -Raw).Trim()
    if ($id -notmatch '^\d+$') { throw "workshop\item_id.txt must hold the Workshop item number only (found '$id')" }
    $paths += "../../workshop/content/233860/$id/KenshiMP_Loader.dll"
}
$lines = @(
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<MyGUI>',
    '	<MyGUI type="Font">',
    '		<Property key="Default" value="Kenshi_StandardFont_Medium"/>',
    '	</MyGUI>',
    '	<MyGUI type="Pointer">',
    '		<Property key="Default" value="arrow"/>',
    '		<Property key="Layer" value="Pointer"/>',
    '	</MyGUI>',
    '	<!-- KenshiMP: starts the multiplayer plugin (missing paths are skipped) -->',
    '	<MyGUI type="Plugin">'
)
foreach ($p in $paths) { $lines += "		<path source=`"$p`"/>" }
$lines += '	</MyGUI>', '</MyGUI>'
$out = Join-Path $Dir 'gui\core'
New-Item -ItemType Directory -Force $out | Out-Null
[IO.File]::WriteAllText((Join-Path $out 'core_settings.xml'), ($lines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding $false))
Write-Host ("gui\core\core_settings.xml: " + ($paths -join ', '))
