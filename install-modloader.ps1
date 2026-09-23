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
  [string]$Version = "0.9.5",
  # Write the product folder here instead of into the ModLoader. CI uses it to
  # build the ready-to-copy zip from the very same code that installs, so the
  # two can never disagree about a file name or a line of YAML.
  [string]$Products = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dll)) {
  Write-Error "100TMX.dll not found at $Dll - download it from the releases page and put it next to this script, or pass -Dll <path>."
}

$loader = "$env:LOCALAPPDATA\TMLoader"
$packaging = $Products -ne ""
if (-not $packaging -and -not (Test-Path $loader)) {
  Write-Error "The TrackMania ModLoader does not seem to be installed ($loader is missing). Get it from https://tomashu.dev/software/tmloader/"
}

# The ModLoader's list shows the product *folder*, not the name inside the yaml -
# that one only appears in the details panel. So the folder is the name.
$products = if ($packaging) { $Products } else { "$loader\database\TmForever\products" }
$product  = "$products\100% TMX + Bingo"
$target   = "$product\$Version"
New-Item -ItemType Directory -Force -Path $target | Out-Null

# An older install used a different folder, and a profile ticks a mod by that
# same string - so both come across, or the mod quietly switches itself off.
if (-not $packaging) {
  $old = "$products\100TMX"
  if (Test-Path $old) { Remove-Item $old -Recurse -Force }
  Get-ChildItem "$loader\database\TmForever\profiles" -Filter *.yaml -ErrorAction SilentlyContinue | ForEach-Object {
    $text = Get-Content $_.FullName -Raw
    if ($text -match "id: 100TMX") {
      ($text -replace "id: 100TMX(\r?\n)", "id: '100% TMX + Bingo'`$1") | Set-Content $_.FullName -Encoding utf8 -NoNewline
    }
  }
}

# UTF-8 without a BOM: the loader's YAML parser reads these as plain text, and a
# BOM on the first line is exactly the kind of thing that makes `name:` vanish.
$utf8 = New-Object System.Text.UTF8Encoding $false

[System.IO.File]::WriteAllText("$product\description.yaml", @"
name: 100% TMX + Bingo
author: cheatoskar
type: modification
homepage: 'https://100tmx.com/'
description: 'Bingo boards on screen while you drive - your tiles, the time to beat, and a button that starts any of their maps. Plus: is this map still open for the 100% TMX project, what is it worth, who finished it.'
"@, $utf8)

# CoreMod is what actually loads mod DLLs into the game, so it is a real
# dependency even though nothing here calls into it.
[System.IO.File]::WriteAllText("$target\description.yaml", @"
executable: 100TMX.dll
dependencies:
  - id: CoreMod
    version: ^1.0.1
changelog: '- The bingo panel, map status, and map marks.'
"@, $utf8)

Copy-Item $Dll "$target\100TMX.dll" -Force

Write-Host "Installed 100% TMX + Bingo $Version to:" -ForegroundColor Green
Write-Host "  $target"
Write-Host ""
Write-Host "Now open the ModLoader, tick \"100% TMX + Bingo\" in the list, and start the game."
Write-Host "In game: F9 -> Connection -> Connect."
