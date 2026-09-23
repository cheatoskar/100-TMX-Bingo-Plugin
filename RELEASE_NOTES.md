## 100% TMX + Bingo 1.0.0 - the first official release

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

### New in 1.0.0

- **No more trojan warning from the installer, we hope.** The old installer
  carried the mod inside itself and unpacked it - the pattern Windows Defender
  flags as `Wacatac!ml`. The new one only copies the folder that sits next to it
  in the zip, and the folder can be copied by hand just as well.
- Finishes are detected on runs longer than 30 minutes.
- The replay uploader sends every autosaved replay, not only project maps.

Credit in the 100% project still comes only from replays on TMX - the mod shows
what the site knows and can never award a finish.
