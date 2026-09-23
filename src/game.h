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
  uintptr_t playerCheckpoints = 0;   // checkpoints passed this run
  uintptr_t challengeBlocks = 0;     // the map's checkpoint buffer; its size is the count
};

enum class RaceState { Unknown = -1, BeforeStart = 0, Running = 1, Finished = 2 };

struct Snapshot {
  bool attached = false;      // a profile is confirmed
  bool inRace = false;        // a challenge is loaded
  std::string uid;            // the map UID, empty in the menus
  std::string mapName;
  RaceState state = RaceState::Unknown;
  /**
   * Whether the state came from the profile's own chain rather than the search.
   *
   * The search infers the state from a clock it found by behaviour, and on a
   * build where that clock turns out to be the wrong one the state is wrong
   * with it. Anything that *locks* something - the panel refusing the mouse
   * mid-race - may only act on the trusted one; a guess is allowed to offer,
   * never to forbid.
   */
  bool stateTrusted = false;
  int raceTimeMs = -1;        // -1 when it could not be read
  /**
   * How far the walk to the local player got, when it did not get there.
   *
   * "Cannot read the race state" is true but useless on its own - the chain is
   * five dereferences deep and any of them can be the wrong offset for a build.
   * Naming the step that failed turns a bug report into an address.
   *
   * 0 fine · 1 no race · 2 no player info · 3 no player · 4 no sub-object ·
   * 5 the state field itself would not read
   */
  int raceStep = 0;
  int checkpoint = -1;        // passed so far this run
  int checkpoints = -1;       // on the map
  /**
   * A freeze that has been *proved* to be a finish, in milliseconds. 0 when
   * there is nothing proved.
   *
   * The clock stopping means either the line was crossed or Escape was
   * pressed, and nothing readable on this build tells the two apart at the
   * time. What tells them apart afterwards is what the clock does next: a
   * paused run carries on from where it stopped, a finished one starts the
   * next run from zero. So a freeze is provisional, and this is the answer
   * once the game has given it - a second or two later, when the player does
   * whatever they do next.
   *
   * Stays set until the next freeze on the same map, so a reader polling
   * every 250 ms cannot miss it.
   */
  int confirmedFinishMs = 0;
  /**
   * The game's own finish: race_state went 1 -> 2 on the player-info object
   * whose race_time is the calibrated clock. Set the moment the line is
   * crossed and held while the results screen is up; 0 otherwise.
   */
  int gameFinishMs = 0;
  /**
   * The race clock advanced within the last 600 ms. Paused (the Escape menu),
   * on the results screen and on the start line it is false - which is when a
   * player reaches for the panel, and race_state alone cannot say so: the game
   * keeps it at "running" through a pause.
   */
  bool clockMoving = false;
  /**
   * Somebody is driving: the clock is ticking *and* the car has moved in the
   * last 1.5 s. What decides whether the panel may take the mouse - see
   * overlay's interactive().
   */
  bool driving = false;
  /** The speedometer, km/h, when the player object is verified. */
  int speed = 0;
  /** Whether that object was found and verified this tick. */
  bool playerInfoVerified = false;
  /** One line of its fields, for the Status tab while this is being tested. */
  std::string finishProbe;
};

/** One finished run, as the game reported it. */
struct FinishEvent {
  std::string uid;
  int ms = 0;
  const char* how = "";   // "race_state" at the line, or "new best" seen late
};

/**
 * The next finish not yet taken, in order. Finishes are latched as read()
 * sees them, so one that happened while nobody was asking is still here.
 */
bool popFinish(FinishEvent* out);

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

/**
 * Whether any profile has ever found a loaded map at all.
 *
 * The difference between "we cannot read this build" and "you have not driven
 * anything yet": the offsets can only be checked against a map that is loaded,
 * so in the menus there is nothing to be wrong about.
 */
bool sawMap();

// One reading. Never throws; every unreadable pointer degrades to "not in a
// race" rather than to a crash.
Snapshot read();

/**
 * The game's root object, for anything that needs to walk from it.
 *
 * Zero until a profile has attached. Exposed for the calibration, which starts
 * its search here: a chain from this pointer is stable across runs, and a raw
 * address is not.
 */
uintptr_t appPointer();

/**
 * Work out how to reach the clock, from an address that is known to hold it.
 *
 * `address` comes either from Cheat Engine or from `calibrateByTime` below.
 * On success the chain is stored in the config and used from the next poll on,
 * and this is the only path by which the mod ever learns a build it was not
 * shipped knowing.
 */
bool calibrateFromAddress(uintptr_t address);

/**
 * The same thing without Cheat Engine: the player types the time the game just
 * showed them, in milliseconds, and the addresses holding it are the candidates.
 *
 * Call it twice with two different runs and the second call only considers what
 * survived the first - one run usually leaves a few hundred coincidences, two
 * leave almost none.
 */
bool calibrateByTime(int milliseconds);

/** What the calibration is currently able to say, for the settings panel. */
struct CalibrationState {
  bool haveChain = false;
  std::string chain;
  /** What the chain reads right now, or -1. */
  int reading = -1;
  /** How many addresses are still in the running during a by-time calibration. */
  size_t candidates = 0;
  /** Ints currently keeping time - the automatic search, mid-flight. */
  int ticking = 0;
  /** Whether any of them has been seen to reset, which is what settles it. */
  bool sawReset = false;
  std::string note;
};
CalibrationState calibration();

/** Throw the calibration away and start again. */
void forgetCalibration();

// Which executable this is: "tmnf", "tmuf", or empty when it cannot be told.
// Narrows five exchanges to three on the server side.
std::string variant();

}  // namespace game
}  // namespace tmx
