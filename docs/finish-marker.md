# Finding the marker that says "this run is over"

> **Resolved in 0.9.2 (2026-09-23), without Cheat Engine.** The marker is
> `race_state` at `+0x314` of the local CTrackManiaPlayerInfo - 0 before the
> start, 1 running, 2 finished - with `race_finished` at `+0x33C` and the race
> clock `race_time` at `+0x2B0` of the same object. TMInterface's
> `PlayerInfoStruct` publishes the layout; Twinkie's GrindingStats counts
> finishes off the same field. Tested live: a pause at a checkpoint leaves it at
> 1, the finish line flips it to 2 in the same tick, on record and non-record
> runs alike.
>
> Every "it reads 0 while driving" below was the **wrong object**:
> `resolvePlayerSub` accepted any memory whose fields happened to be small,
> zeros included. The fix is identity - an object is only believed when its
> `+0x2B0` is the very address the calibrated clock was read from.
>
> Two more things it took to make it reliable. The mod reads the game on a
> thread of its own and queues each finish, and the best time moving is a
> second proof for a finish that was not seen as it happened. And the log is
> written from a thread of its own: appending one line to
> `Documents\100TMX\log.txt` measured **10-22 seconds** right after a record
> finish on this machine, and with the write inline that stalled the finish
> watcher through the whole results screen.
>
> The rest of this document is the plan as it stood before, kept for the
> reasoning.

Written 2026-09-20, after four attempts at inferring it. **A plan, not a
description of anything that works yet.**

## Why this matters more than it looks

Bingo boards set to *no check* or *plugin and replay only* are decided by the
time the game measured. That is the whole arrangement: no replay is uploaded,
no exchange is asked, the overlay reports what the clock said. So the mod has
to know, exactly, when a run ended.

Everything else has a second source of truth and can limp along without this:

| Feature | What corroborates it today |
|---|---|
| Uploading a replay | the autosave file - the game only writes one for a completed run |
| Project credit | the first replay on TMX, which the site reads for itself |
| **A bingo tile on a self-reported board** | **nothing. This is the gap.** |

And the autosave cannot stand in for it: TrackMania does not write one for a
run that fails to beat your own record on that map, while such a run is
perfectly valid for a tile. Gating tiles on the file means a board silently
stops working on maps its players have already driven - which is the exact
situation those board modes exist for.

## What has been tried, and what the log said

| Attempt | Assumption | What actually happened |
|---|---|---|
| 0.8.0 and earlier | clock stops = finished | Escape stops it too: a paused run reported a finish, with a partial time |
| 0.8.1 | `player_state` is 1 while driving, 2 at the finish | it reads **0** while driving - so "not 2, therefore running" was always true and no finish was ever detected |
| 0.8.3 | a finish has passed every checkpoint | the counter reads **1 at the start line** (`cp 1/18` at 80 ms) - so "all passed" was never true, and finishes were missed on exactly the maps where the counter could be read |
| 0.8.4 | the autosave proves it | true, and it is what guards auto-submit now - but it is absent for a non-record run, which is why this document exists |

Three of those shipped. The common mistake was reasoning about a memory
location instead of measuring it; each time the answer was in the log within
a minute of finally looking. **Measure first this time.**

## What we already have to work with

`scan.h` was built for exactly this shape of problem when the race clock had
it, and none of it is clock-specific:

- `scan::findValue(int)` - every readable address holding a value
- `scan::keepHolding(addresses, value)` - narrow across two observations
- `scan::findChains(root, target)` - turn an address into
  `app + 0x1F4 -> +0x28 -> +0x2BC`, which is what survives a restart
- `scan::parse` / `scan::resolve` - read one back

And `config.cpp` already parses an `[offsets]` section including
`player_state`, `player_sub`, `player_time`, `race_player_info`. So **once the
right offset is known it goes in `Documents\100TMX\config.ini` and works
immediately - no build, no release.** That is worth knowing before anybody
spends an evening in Cheat Engine: the finding is a line of text, not a patch.

Known build, for the record: `4d431494-00a62000`, profile `tmf-modloader`,
player sub resolved at `race+0x44 -> +0x28 -> +0x24` (and once at
`race+0xB8 -> +0x24 -> +0x20`, so the walk is not stable across maps either).

## Route A - Cheat Engine, half an hour, exact

The fast path, and the one that needs no new code at all.

The marker is a field that changes **when you cross the line** and does *not*
change when you press Escape. That difference is the whole search, and Cheat
Engine's "changed / unchanged" scans are built for it.

1. Attach to `TmForever.exe`. Scan type **Exact Value**, value type **4 Bytes**.
2. Load a map, sit at the start line. **First Scan** for `0`.
   (Nearly everything is zero; that is fine, the next steps cut it down.)
3. Drive, cross the line, wait on the results screen. **Next Scan → Changed**.
4. Restart the map, drive to a checkpoint, press **Escape**. **Next Scan →
   Unchanged**. This is the step that throws away every field that merely
   reacts to the clock stopping.
5. Repeat 3 and 4 twice more, alternating. A handful of addresses survive.
6. For each survivor: watch its value in the table while you finish, then
   while you pause. The one you want holds a small constant while driving and
   flips to another small constant only at the finish. Note both values.
7. Right-click → **Pointer scan for this address**, or simpler: give the
   address to the mod. `[debug] time_address` already does this for the clock;
   the same one-shot for the finish flag is ~20 lines
   (`finish_address`), and the mod then prints the chain it derived.
8. Put the chain in `config.ini` and check the Status tab reads it.

What to watch out for, from the clock hunt:

- **A heap address is true for one run of the game.** Only the *chain* is
  worth keeping. Do not paste an address into a profile.
- Scan while a map is loaded. In the menus the player object does not exist
  and every candidate is garbage.
- If nothing survives step 5, widen: the flag may be a byte rather than an
  int, or it may live on the race object rather than the player. Try value
  type **Byte** with the same sequence before assuming it is not there.

## Route B - let the mod find it, for everybody else

Route A tells us the answer for one build. There are at least two TMF builds
in circulation and a Nations/United split on top, so the mod should be able to
learn this the way it already learns the clock.

The design mirrors the clock calibration, with one difference: the clock could
be labelled by the player typing a number, and this has to be labelled by the
player saying **which of two things just happened**.

1. **Snapshot a window, not the process.** The player sub is known; read
   `playerSub - 0x100 .. +0x600` as ints (448 of them) each tick and keep the
   last "driving" snapshot. Cheap, bounded, and no process-wide scan on the
   worker thread.
2. **On a freeze, keep the frozen snapshot** and the diff against driving.
3. **Ask, once, in the panel**: "Did you just finish, or pause?" with two
   buttons. One sample of each is already enough to cut the candidate set
   hard; three of each settles it.
4. **Keep the offsets that changed on every finish and on no pause.** If that
   set is empty, widen the window to the race object and repeat - and if it is
   still empty, say so plainly in the panel rather than guessing, which is the
   failure this whole document is about.
5. **Store it** as `finish_chain` in `[debug]` next to `time_chain`, derived
   through `scan::findChains` so it survives a restart. Ship the confirmed
   ones as built-in profiles keyed on the build id.

Cost: roughly a day. It is the same machinery as `scan.cpp`, and the
calibration UI already exists in the Status tab and can be copied.

## What happens in the meantime

Nothing automatic puts a time on a board. Concretely:

- **The button works and is correct.** Finish a map that is a tile and the
  panel offers "Take the tile with 13.91". A person pressing that cannot be
  fooled by a pause, because they know what they just did. Bingo is playable
  from the game today; it is one click rather than none.
- **Auto-submit stays gated on the replay file**, so it works for a record run
  and quietly declines otherwise. Not good enough to keep, which is why this
  document exists, but it is never *wrong*.
- **An optional interim guard, if auto-submit without a replay is wanted
  sooner:** refuse any automatic time below ~40 % of the map's author time.
  The overlay already has `authorTime` on every tile. A partial time from a
  pause at the second checkpoint is a small fraction of the author time, and
  no real run is within 40 % of it. This is a heuristic and must be labelled
  as one - it narrows the hole, it does not close it, and a tile is still
  self-reported either way.

## The rule that does not move

Whatever is found here, a time measured in the game remains **self-reported**.
The board says so, the site says so, and none of this touches project credit,
which comes from the first replay on TMX and nothing else. Finding the finish
marker makes the overlay honest about *when* a run ended. It does not make it
evidence of *what* the run was.
