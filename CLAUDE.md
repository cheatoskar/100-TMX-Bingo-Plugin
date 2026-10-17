# 100% TMX + Bingo — the game mod

In-game overlay for TrackMania Nations/United Forever: bingo boards and the
100% TMX project status, talking to https://100tmx.com (the website repo is the
parent folder, `cheatoskar/100-tmx-website`). 32-bit DLL loaded by the TMLoader
**ModLoader**, D3D9 `EndScene` hook, Dear ImGui 1.91.5 (fetched by CMake).
File-by-file overview: README, section *For developers*.

## The rules

- **The mod is a client, never evidence.** It never awards a finish; credit is
  the first replay on TMX. It may report what is played and take self-reported
  bingo tiles (`game`/`trust` boards). Do not describe that as anti-cheat.
- **Never block the game thread.** Network is on the worker thread, the game is
  read on its own thread; the render path only takes the latest `State` copy.
- **Every pointer read is guarded** (`plausible()` + SEH reads in `game.cpp`).
  The game is large-address-aware: the top is `GetSystemInfo`, not 2 GB.
- **The local player** is `[race+0x330]`, else network info entry 0
  (`[app+0x12C]` -> `+0x2FC`), accepted only if `info+0x238 -> player+0x1C`
  closes the loop. Entry 0 of the race's player list is a stranger online.
- **Render under our own state block** (`renderDrawData`): the game leaves
  texture-coordinate generation set, which smeared pictures and hid all text.
- **Panels size by `fitHeight()`, never by `SetNextWindowSize(w, 0)`** - an
  auto-fitting window has no resize grip.
- **Two mods share the screen with TM Vibrant Shaders**: window properties
  `TMMods.MouseOwner` / `TMMods.TopOverlay` decide who gets the mouse; bare F9 is
  ours, Ctrl+F9 is theirs.
- The token in `Documents\100TMX\config.ini` is DPAPI-sealed (`dpapi:` prefix);
  plaintext from old versions is resealed on load.

## Build and test

```bash
cmake -S . -B build -A Win32                # VS 2026 BuildTools, Win32 only
cmake --build build --config Release        # 100TMX.dll, installer, tmx_preview.exe
build/Release/tmx_preview.exe panel out.png 1280x800   # render the real overlay to PNG
```

Scenes for the previewer are in `tools/preview/main.cpp`. A preview does not
replace the game: install and drive (see the **release** skill for the
ModLoader test loop). Diagnostics are in `Documents\100TMX\log.txt`; the
first lines say which build is running.

Traps:
- **TMLoader only rescans versions on start.** Close it completely (tray too)
  after installing, or the game keeps the old DLL.
- The `built ...` time in the log is `__TIME__` of `dllmain.cpp` only - it does
  not change when other files do. Check the DLL hash instead.
- Local builds report version `1.0.0`; CI sets the real one from the tag.
- Files are CRLF: edit with the Edit tool or Python `newline=''`, never `sed -i`.

## Shipping

Branch + PR to `main`, CI must be green. A `v*` tag builds and publishes the
release with `RELEASE_NOTES.md` on top; the in-game update notice reads GitHub's
latest release. Steps: `.claude/skills/release/SKILL.md`.
