## 100% TMX + Bingo 1.1.0

Bingo boards from [100tmx.com](https://100tmx.com) and the 100% TMX project status, right inside **TrackMania Nations & United Forever**.

![The 100% TMX and Bingo panels next to the menu, in game](https://raw.githubusercontent.com/cheatoskar/100-TMX-Bingo-Plugin/{{TAG}}/images/cover.jpg)

---

### What's new

**A new look.** The menu and the panels now look like the TM Vibrant Shaders menu, and both mods run side by side: whichever menu is on top gets the mouse, and `Ctrl+F9` stays with the shaders.

**Panels you can arrange.** Fold them (**-**), close them (**x**) and resize them by dragging the bottom-right corner.

**The TMX top 10** for the current map, and where your run would place in it.

**Update notice.** The mod tells you when a new version is out. You can turn that off under **About**.

**Your own time, always.** On online servers the mod sometimes showed another player's time. It now always reads yours.

Also fixed: smeared map pictures and disappearing text on some maps, losing track of the race in long sessions, uninstalling breaking the ModLoader profile, and the *Take the tile* button staying after the tile is yours. The mod runs on Windows 7 with update KB3140245, and the connection token in `config.ini` is now encrypted for your Windows account.

---

### Install

**With the ModLoader, updates itself:** close the ModLoader (also the tray icon), open `%LOCALAPPDATA%\TMLoader\config.yaml` with Notepad and add this line under `servers:`:

```yaml
  - 'https://cheatoskar.github.io/100-TMX-Bingo-Plugin/modloader/'
```

Then tick *100% TMX + Bingo* in your profile. New versions arrive on their own from then on.

**With the installer:** download `100TMX.zip`, extract it and run `100TMX-Installer.exe` (or copy the folder by hand, see `README.txt`).

**Coming from 1.0.0:** install as usual, then close the ModLoader completely (including the tray icon) and start it again. 1.0.0 can't tell you about updates, 1.1.0 and later can.

In the game `F9` opens the menu, and *Connection* → *Connect* links it to your 100tmx.com account. Step by step: [README](https://github.com/cheatoskar/100-TMX-Bingo-Plugin#readme).
