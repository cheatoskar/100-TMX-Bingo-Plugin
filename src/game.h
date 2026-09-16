// Reading TrackMania Forever's own state.
//
// Everything here is a read. The mod never writes a byte of the game's memory,
// never patches code, and never calls anything that changes state - the only
// engine function it calls is the one that turns an id index into the string
// behind it, because a map UID is stored as an index and there is no other way
// to get at it.
//
// The offsets are per executable. Twinkie (MIT, Copyright (c) 2025 Ahmad Saleh)
// is where the published ones come from, and it ships a separate build per TMF
// variant for exactly this reason. So nothing here is trusted on faith:
// `validate()` walks the whole chain while a map is loaded and only accepts a
// profile if what comes out the far end actually looks like a map UID. An
// unrecognised build leaves the mod switched on and silent, which is the only
// acceptable failure mode inside somebody else's game.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tmx {
namespace game {

struct Offsets {
  std::string name;

  // From the executable's base address.
  uintptr_t app = 0;         // -> CGameApp*
  uintptr_t getIdName = 0;   // void __thiscall (unsigned* id, CFastString* out)

  // From CGameApp.
  uintptr_t challenge = 0;   // -> CGameCtnChallenge*, null in the menus
  uintptr_t race = 0;        // -> the race, null outside one

  // From the challenge.
  uintptr_t challengeUid = 0;   // an id index, not a string
  uintptr_t challengeName = 0;  // CFastStringInt

  // The walk to the local player, for the race state. Every step is optional:
  // losing it costs the finish detection and nothing else.
  uintptr_t racePlayerInfo = 0;  // race -> player info nod
  uintptr_t playerInfoPlayer = 0;
  uintptr_t playerSub = 0;
  uintptr_t playerState = 0;
  uintptr_t playerTime = 0;
};

enum class RaceState { Unknown = -1, BeforeStart = 0, Running = 1, Finished = 2 };

struct Snapshot {
  bool attached = false;      // a profile is confirmed
  bool inRace = false;        // a challenge is loaded
  std::string uid;            // the map UID, empty in the menus
  std::string mapName;
  RaceState state = RaceState::Unknown;
  int raceTimeMs = -1;        // -1 when it could not be read
};

// The built-in profiles, plus whatever the ini added.
const std::vector<Offsets>& profiles();
void addProfile(const Offsets& profile);

// A short, stable name for this executable (its PE timestamp and image size),
// shown in the status line so a bug report names the build it came from.
std::string buildKey();

// Try every profile against the running game. Returns true once one produces a
// plausible UID; until a map is loaded there is nothing to check against, so
// this is called again on every poll while unattached.
bool attach();
bool attached();
std::string attachedProfile();

// One reading. Never throws; every unreadable pointer degrades to "not in a
// race" rather than to a crash.
Snapshot read();

// Which executable this is: "tmnf", "tmuf", or empty when it cannot be told.
// Narrows five exchanges to three on the server side.
std::string variant();

}  // namespace game
}  // namespace tmx
