---
name: release
description: Test a mod build in the real game through the ModLoader, then publish a release (PR merge, RELEASE_NOTES.md, v* tag, verify the assets). Use when asked to install a test build, release, tag, or publish a new version of the 100% TMX mod.
---

# Test in the game, then release

## 1. Test build in the ModLoader

1. Build: `cmake --build build --config Release`.
2. Install as a new test version folder (never overwrite a loaded one):
   `%LOCALAPPDATA%\TMLoader\database\TmForever\products\100% TMX + Bingo\<x.y.z>\`
   with `100TMX.dll` and the `description.yaml` of the previous version.
   Compare SHA-256 of the copied DLL with the build.
3. `profiles\default.yaml` must not pin an older `version:` under the mod.
4. The user closes the ModLoader **completely** and reopens it, then plays.
5. Read `Documents\100TMX\log.txt` from the last `mod started` line:
   `update:` line, `via local player`, `FINISH ...`, `worker: tile N ... HTTP 200, captured`.
   Do not release before the user says the build worked in the game.

## 2. Release

1. `RELEASE_NOTES.md`: first line `## 100% TMX + Bingo X.Y.Z - <summary>` (the
   in-game notice shows that line), then *New in X.Y.Z*. Player language, short.
2. README up to date for anything a player sees. Commit on the branch, push, PR.
3. Merge the PR once CI is green (`gh pr ready` first if it is a draft).
4. Tag the merge commit and push the tag:
   `git tag -a vX.Y.Z <merge-sha> -m "100% TMX + Bingo X.Y.Z" && git push origin vX.Y.Z`
5. Wait for the `build` run on the tag, then check:
   - release assets: `100TMX.zip`, `.dll`, `.asi`, `SHA256SUMS.txt`, `tmx_version.h`;
   - `tmx_version.h` says `X.Y.Z`, the zip has `100% TMX + Bingo/X.Y.Z/`;
   - `https://api.github.com/repos/cheatoskar/100-TMX-Bingo-Plugin/releases/latest` is the new tag.
6. Users on 1.0.0 have no update notice - remind the user to announce it.

Replacing a published release (delete release + tag, re-tag) only when nobody
has downloaded it yet (`gh release view --json assets` download counts);
otherwise bump the patch version.
