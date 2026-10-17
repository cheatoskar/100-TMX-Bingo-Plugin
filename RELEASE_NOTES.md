## 100% TMX + Bingo 1.1.0 - a new look, your own time online

Bingo boards and the [100% TMX](https://100tmx.com) project, inside TrackMania
Nations and United Forever. Race your friends on a bingo board without leaving
the game, and see what any map is worth to the project the moment it loads.

### Download

**`100TMX.zip`** is all you need. Unzip it, then either run
**`100TMX-Installer.exe`**, or copy the **`100% TMX + Bingo`** folder into
`%LOCALAPPDATA%\TMLoader\database\TmForever\products` by hand - `README.txt` in
the zip explains both, in English and German. You need the
[TrackMania ModLoader](https://tomashu.dev/software/tmloader/) first.

Then press **F9 → Connection → Connect** in game and approve the code on the
website. The full guide is in the [README](https://github.com/cheatoskar/100-TMX-Bingo-Plugin#readme).

### What it does

- **Bingo in the game** - the board, who holds each tile, the time to beat, and
  *Play this map* to load any tile straight into the running game.
- **Tiles taken the moment you cross the line** on self-reported boards, read
  from the game's own "race finished" state - a pause is never a finish. Or type
  a time on a *No check* board with **Enter my time...**
- **The 100% TMX window** - is this map still open, what it is worth, and if
  somebody finishes it while you are driving.
- **Replays uploaded for you** - every replay TrackMania autosaves goes to TMX
  through the browser extension, signed in as you. The mod never sees your TMX
  login.
- **Two windows**, moved and resized separately, that stay out of the way while
  you drive and are clickable whenever you are not.

### New in 1.1.0

- **A new look**, in the style of the TM Vibrant Shaders menu, with the mod's
  logo in the ModLoader. Both mods can run together: whichever menu is drawn on
  top gets the mouse, and Ctrl+F9 stays with the shaders.
- **Your own time online.** On a server the mod could read whoever the server
  listed first - a stranger's 4:38 while you drove 0:24. It now finds the
  player sitting at your PC.
- **The tile button goes away once the tile is yours.** A tile taken
  automatically still offered *Take the tile* until the next refresh.
- **Panels fold and close themselves**: **-** folds a panel to its title row,
  **x** hides it. They used to open the menu instead. Drag the bottom-right
  corner to make a panel wider or narrower; the height follows the content.
- **The TMX top 10** for the map you are on, with where your run would place.
- **Told when a new version is out**, with a download link - in the 100% TMX
  panel and under **About**. Switch it off there if you would rather not.
- **Map pictures no longer smear and the text no longer vanishes** on some maps
  and camera angles; the game left a texture setting behind that the overlay
  did not reset.
- **Works on Windows 7** (with update KB3140245): the mod asks for TLS 1.2,
  without which the site, TMX and GitHub refused every request.
- **Your machine link is encrypted** in `config.ini`, tied to your Windows
  account. A copied file is useless elsewhere; the mod asks to connect again.
- Long sessions no longer lose the race when the game's memory grows past 2 GB.
- Uninstalling no longer leaves the ModLoader profile pointing at a deleted
  folder, which stopped the game from starting.

**Updating from 1.0.0:** install as usual, then close the ModLoader completely
(also from the tray) and open it again - it only sees a new version on start.
1.0.0 cannot tell you about updates itself; from 1.1.0 on it will.

Credit in the 100% project still comes only from replays on TMX - the mod shows
what the site knows and can never award a finish.
