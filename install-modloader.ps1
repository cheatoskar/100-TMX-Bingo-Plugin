# Installs the mod into the TrackMania ModLoader.
#
# The ModLoader has no "mods folder" to drop a DLL into: it keeps a product
# database under %LOCALAPPDATA%\TMLoader, one folder per mod and one folder per
# version inside it, each with a small description.yaml. This writes that
# layout, which is all "installing" means here - the loader picks the mod up on
# its next start.
#
#   powershell -ExecutionPolicy Bypass -File install-modloader.ps1
#
# Pass -Dll to install a DLL from somewhere else, and -Version to install
# alongside an existing one rather than over it.

param(
  [string]$Dll = "$PSScriptRoot\100TMX.dll",
  [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dll)) {
  Write-Error "100TMX.dll not found at $Dll - download it from the releases page and put it next to this script, or pass -Dll <path>."
}

$loader = "$env:LOCALAPPDATA\TMLoader"
if (-not (Test-Path $loader)) {
  Write-Error "The TrackMania ModLoader does not seem to be installed ($loader is missing). Get it from https://tomashu.dev/software/tmloader/"
}

$product = "$loader\database\TmForever\products\100TMX"
$target  = "$product\$Version"
New-Item -ItemType Directory -Force -Path $target | Out-Null

# UTF-8 without a BOM: the loader's YAML parser reads these as plain text, and a
# BOM on the first line is exactly the kind of thing that makes `name:` vanish.
$utf8 = New-Object System.Text.UTF8Encoding $false

[System.IO.File]::WriteAllText("$product\description.yaml", @"
name: 100TMX
author: cheatoskar
type: modification
homepage: 'https://100tmx.com/'
description: 'Shows whether the map you are on is still open for the 100% TMX project, what it is worth, and the bingo boards you are in - with a button that starts any of their maps.'
"@, $utf8)

# CoreMod is what actually loads mod DLLs into the game, so it is a real
# dependency even though nothing here calls into it.
[System.IO.File]::WriteAllText("$target\description.yaml", @"
executable: 100TMX.dll
dependencies:
  - id: CoreMod
    version: ^1.0.1
changelog: '- Map status, map marks, and the bingo panel.'
"@, $utf8)

Copy-Item $Dll "$target\100TMX.dll" -Force

Write-Host "Installed 100TMX $Version to:" -ForegroundColor Green
Write-Host "  $target"
Write-Host ""
Write-Host "Now open the ModLoader, tick 100TMX in the list, and start the game."
Write-Host "In game: F9 -> Connection -> Connect."
