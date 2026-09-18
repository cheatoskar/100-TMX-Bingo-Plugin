#include "game.h"

#include "log.h"
#include "config.h"
#include "scan.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <vector>

namespace tmx {
namespace game {
namespace {

std::mutex g_lock;
std::vector<Offsets> g_profiles;
int g_attached = -1;  // index into g_profiles
bool g_sawMap = false;
uintptr_t g_base = 0;

// TrackMania's own string types, as published in Twinkie's TrackMania.h.
struct CFastString {
  int size;
  char* cstr;
};
struct CFastStringInt {
  int size;
  wchar_t* cstr;
};

uintptr_t exeBase() {
  if (!g_base) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return g_base;
}

// A pointer that could plausibly be one. Cheap first gate before the guarded
// read, because most wrong offsets produce either zero or something tiny.
bool plausible(uintptr_t address) {
  return address > 0x10000 && address < 0x7FFF0000;
}

// The guarded read. SEH rather than IsBadReadPtr, which races with anything
// that unmaps between the check and the read; here the read itself is what is
// protected. No C++ objects live in this frame, which is what lets __try work.
bool readRaw(uintptr_t address, void* out, size_t size) {
  if (!plausible(address)) return false;
  __try {
    memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

template <typename T>
bool readAt(uintptr_t address, T* out) {
  return readRaw(address, out, sizeof(T));
}

uintptr_t deref(uintptr_t address) {
  uintptr_t value = 0;
  return readAt<uintptr_t>(address, &value) ? value : 0;
}

using GetIdNameFn = void*(__thiscall*)(unsigned int* id, CFastString* out);

// The one call into the game. Guarded the same way as a read: a wrong offset
// here is a jump into arbitrary code, so the profile is only ever trusted after
// this has produced something that looks like a UID.
bool callGetIdName(uintptr_t fn, unsigned int id, CFastString* out) {
  __try {
    reinterpret_cast<GetIdNameFn>(fn)(&id, out);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A map UID is 22-27 characters of base64url-ish text. Anything else means the
// offsets are wrong, and that is exactly what `attach()` needs to know.
bool looksLikeUid(const std::string& uid) {
  if (uid.size() < 10 || uid.size() > 40) return false;
  for (char c : uid) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '+' || c == '.';
    if (!ok) return false;
  }
  return true;
}

std::string readUid(const Offsets& o, uintptr_t challenge) {
  unsigned int id = 0;
  if (!readAt<unsigned int>(challenge + o.challengeUid, &id) || id == 0) return std::string();

  CFastString result{};
  if (!callGetIdName(exeBase() + o.getIdName, id, &result)) return std::string();
  if (result.size <= 0 || result.size > 256 || !plausible(reinterpret_cast<uintptr_t>(result.cstr))) {
    return std::string();
  }

  std::string out(static_cast<size_t>(result.size), '\0');
  if (!readRaw(reinterpret_cast<uintptr_t>(result.cstr), &out[0], out.size())) return std::string();
  out.resize(strnlen(out.c_str(), out.size()));
  return out;
}

std::string readName(const Offsets& o, uintptr_t challenge) {
  CFastStringInt name{};
  if (!readAt<CFastStringInt>(challenge + o.challengeName, &name)) return std::string();
  if (name.size <= 0 || name.size > 256 || !plausible(reinterpret_cast<uintptr_t>(name.cstr))) return std::string();

  std::wstring wide(static_cast<size_t>(name.size), L'\0');
  if (!readRaw(reinterpret_cast<uintptr_t>(name.cstr), &wide[0], wide.size() * sizeof(wchar_t))) {
    return std::string();
  }
  wide.resize(wcsnlen(wide.c_str(), wide.size()));

  int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &out[0], size, nullptr, nullptr);
  return out;
}

// The published offsets, from Twinkie's TwinkTrackmania.cpp.
//
// `getIdName` carries the +0x200000 Twinkie applies with the comment "might be
// modloader related" - kept verbatim because it is what is known to work, and
// the second profile is the same table without it, so whichever is right for a
// given build is found by `attach()` rather than guessed here.
Offsets baseProfile(const char* name, uintptr_t idNameExtra) {
  Offsets o;
  o.name = name;
  o.app = 0x972EB8;
  o.getIdName = 0x3357D0 + idNameExtra;
  o.challenge = 0x198;
  o.race = 0x454;
  o.challengeUid = 220;    // 0xDC
  o.challengeName = 0x108;
  o.racePlayerInfo = 0x330;
  o.playerInfoPlayer = 0x238;
  o.playerSub = 0x1C;
  o.playerState = 0x314;
  o.playerTime = 0x2B0;
  o.playerCheckpoints = 0x330;
  o.challengeBlocks = 0x60;
  return o;
}

void ensureDefaults() {
  if (!g_profiles.empty()) return;
  g_profiles.push_back(baseProfile("tmf-modloader", 0x200000));
  g_profiles.push_back(baseProfile("tmf-plain", 0));
}


/**
 * Find `racePlayerInfo` when the profile's value does not work.
 *
 * Builds differ in one step of the walk far more often than in all of it: the
 * app pointer, the challenge and the UID read fine on the build this was
 * written for, and it stops dead at the hop from the race to the player info.
 * Rather than ship a profile per build - a release per player - the mod looks
 * for the offset itself.
 *
 * **A single pass is not enough, and the first version of this proved it.** Any
 * pointer chain landing on an int that happens to be 0..2 and another that
 * happens to look like a lap time will pass, and on a real build two different
 * offsets did: 0x620 on one run and 0x44 on the next. Both cannot be right.
 *
 * So a candidate has to show a *running clock*. The first pass collects every
 * offset whose chain reads plausibly and remembers its time; a later pass keeps
 * only those whose time has moved. Nothing else in the process advances a
 * millisecond counter in lockstep with a race, which is what makes it a real
 * discriminator rather than a tighter guess. The answer is only adopted when
 * exactly one candidate survives - two survivors mean the test was still too
 * weak, and taking either would be the same mistake again.
 */
/**
 * Finding the race clock by what it does, not by where it should be.
 *
 * Three assumptions have now failed in a row on one real build: that only the
 * first hop differs (one candidate, its value never moved), that the two hops
 * differ but the fields inside the object do not (909 chains, none kept time),
 * and before that, that a single pass at map load would see anything at all.
 * Each was plausible and each was wrong, so this drops the lot.
 *
 * What is left is behaviour. A race clock is an `int` that counts milliseconds
 * in step with the wall clock, and nothing else in the process does that. So
 * the search keeps the two pointer hops - which only bound where to look - and
 * stops assuming anything about *which field* inside the object is the clock.
 * It watches every int in reach and keeps the ones that keep time.
 *
 * The state comes off the clock rather than out of a second guessed field:
 * running when it advances, finished when it has stopped at something above
 * zero, before the start when it is zero. That is the same information with one
 * fewer thing to be wrong about.
 */
struct Candidate {
  uintptr_t hop1 = 0;    // race -> ?
  uintptr_t hop2 = 0;    // ? -> the object
  uintptr_t field = 0;   // the int inside it
  int value = 0;
  unsigned tick = 0;
  int advances = 0;
  unsigned firstMoved = 0;   // when it left zero - the race clock leaves last
  /**
   * Seen to fall from a real time back to zero.
   *
   * This is what actually tells the race clock apart from everything else that
   * keeps time, and it took far too long to find. A map clock, a session clock
   * and a media clock all tick 1:1 with the wall and never look back; the race
   * clock returns to zero every single time the player restarts - which, on a
   * project about grinding one map, is every twenty seconds.
   *
   * The old test was "whichever reads lowest once a few have qualified", which
   * is a proxy for the same idea and wrong whenever a second clock happens to
   * start late. This is the property itself.
   */
  bool reset = false;
};

std::vector<Candidate> g_candidates;
uintptr_t g_clock1 = 0, g_clock2 = 0, g_clockField = 0;
uintptr_t g_raceInfoFor = 0;
unsigned g_lastScan = 0;
unsigned g_firstQualified = 0;
int g_lastClock = -1;
unsigned g_lastClockMove = 0;
uintptr_t g_cachedPlayerSub = 0;
uintptr_t g_cachedTimeOffset = 0x2BC;
uintptr_t g_cachedStateOffset = 0x314;
uintptr_t g_cachedRace = 0;

const int kMaxRaceMs = 30 * 60 * 1000;

// ---------------------------------------------------------------------------
// The calibrated clock.
//
// One chain, derived once from an address somebody proved was the clock, and
// checked on every read. It exists because every other route in this file is a
// guess: a table of offsets from another build, or a hunt for "an int that
// ticks". Both were wrong on this machine for a week.
// ---------------------------------------------------------------------------
scan::Chain g_timeChain;
bool g_timeChainLoaded = false;
uintptr_t g_timeAddress = 0;      // resolved for this run
int g_timeLast = -1;

// A by-time calibration in progress: the addresses that held the first time.
std::vector<uintptr_t> g_calibCandidates;
std::string g_calibNote;

uintptr_t g_appForScan = 0;

void loadTimeChain() {
  if (g_timeChainLoaded) return;
  g_timeChainLoaded = true;
  if (config().timeChain.empty()) return;

  g_timeChain = scan::parse(config().timeChain);
  if (!g_timeChain.hops.empty() || g_timeChain.delta) {
    log::line("game: calibrated clock chain from the ini: app -> %s", g_timeChain.text().c_str());
  }
}

/** The clock through the calibrated chain, or -1 when it is not usable. */
int calibratedTime(uintptr_t app) {
  loadTimeChain();
  if (g_timeChain.hops.empty() && !g_timeChain.delta) return -1;
  if (!app) return -1;

  // Re-resolved every read rather than cached: the objects behind it are
  // replaced on every map load, and a stale pointer that still happens to be
  // mapped reads as a plausible, wrong time - which is the failure this whole
  // exercise is about.
  const uintptr_t at = scan::resolve(app, g_timeChain);
  if (!at) return -1;
  g_timeAddress = at;

  int value = 0;
  if (!readAt<int>(at, &value)) return -1;
  if (value < 0 || value > kMaxRaceMs) return -1;
  return value;
}

// Every int in reach that could be a clock right now. Bounded hard: this runs
// on the worker thread while somebody is driving, and an unbounded list would
// be the cure becoming the disease.
void collectCandidates(uintptr_t race) {
  g_candidates.clear();
  const unsigned now = GetTickCount();
  for (uintptr_t k1 = 0x40; k1 <= 0x800; k1 += 4) {
    const uintptr_t mid = deref(race + k1);
    if (!plausible(mid)) continue;
    for (uintptr_t k2 = 0x00; k2 <= 0x600; k2 += 4) {
      const uintptr_t obj = deref(mid + k2);
      if (!plausible(obj)) continue;
      for (uintptr_t k3 = 0x00; k3 <= 0x400; k3 += 4) {
        int v = 0;
        if (!readAt<int>(obj + k3, &v)) continue;
        // Near zero, not merely "a plausible lap time". This is the difference
        // between *a* clock and the *race* clock: the search starts a couple of
        // seconds after the race object appears, which is the countdown, and a
        // per-run timer is at zero then while a session or media clock is
        // already large. Accepting any small-looking number found 40,000
        // candidates - the cap - and settled on one that kept running happily
        // after the finish line, which is how a finish went undetected.
        if (v < 0 || v > 1500) continue;
        g_candidates.push_back({k1, k2, k3, v, now, 0});
        if (g_candidates.size() >= 40000) return;
      }
    }
  }
}

// Keep the ones moving in step with the wall clock. Everything else is a
// counter, a coordinate, or a coincidence.
bool narrowCandidates(uintptr_t race) {
  const unsigned now = GetTickCount();
  std::vector<Candidate> alive;
  alive.reserve(g_candidates.size() / 2 + 8);

  for (Candidate c : g_candidates) {
    const uintptr_t mid = deref(race + c.hop1);
    if (!plausible(mid)) continue;
    const uintptr_t obj = deref(mid + c.hop2);
    if (!plausible(obj)) continue;
    int v = 0;
    if (!readAt<int>(obj + c.field, &v) || v < 0 || v > kMaxRaceMs) continue;

    const int elapsed = static_cast<int>(now - c.tick);
    const int moved = v - c.value;

    // Back to the start line. Large enough that a stopwatch being reset cannot
    // be confused with a lap counter ticking over, and only counted while the
    // old value was a real time rather than noise near zero.
    if (c.value >= 2000 && v <= 600) {
      c.reset = true;
      c.value = v;
      c.tick = now;
      alive.push_back(c);
      continue;
    }

    if (moved != 0) {
      // Forwards, and by about as long as we waited. A frame counter moves in
      // step too but by ~60 a second, not ~1000, so the lower bound matters as
      // much as the upper one.
      if (moved < elapsed / 2 || moved > elapsed + 60) continue;
      if (c.advances == 0) c.firstMoved = now;
      c.advances++;
      c.value = v;
      c.tick = now;
    } else {
      // Keep tick fresh while the countdown is waiting at zero, so a clock
      // that starts after 3 seconds is not discarded for low average rate.
      c.tick = now;
    }
    alive.push_back(c);
  }
  g_candidates = alive;

  /*
   * Which of the clocks that keep time is the *race* clock.
   *
   * Taking the first one to qualify gave a time of 12.30 where the replay said
   * 9.70 - out by 2.6 seconds, which is the countdown. That clock starts when
   * the map does; the race clock starts when the lights go green. Both keep
   * perfect time, so no amount of watching the tick rate separates them.
   *
   * What separates them is *when they left zero*: the race clock is the last
   * one to start, and so reads lowest at every instant afterwards. So the
   * qualified ones are given a moment to gather and the lowest is taken, rather
   * than whichever crossed the line of four advances first.
   */
  const Candidate* best = nullptr;
  int qualified = 0;
  int ticking = 0;
  for (const Candidate& c : g_candidates) {
    if (c.advances < 4) continue;
    ticking++;
    // Ticking *and* seen to go back to zero. Nothing else in the process does
    // both, so this needs no tie-break, no grace period and no guess about
    // which clock started last.
    if (!c.reset) continue;
    qualified++;
    if (!best || c.value < best->value) best = &c;
  }

  if (!best) {
    log::once("clockwait",
              "game: %d clocks keep time, none has restarted yet - drive a lap and press restart once",
              ticking);
    return false;
  }

  g_clock1 = best->hop1;
  g_clock2 = best->hop2;
  g_clockField = best->field;

  /*
   * Turn the find into something that survives the game closing.
   *
   * Locking on used to be the end of it, so every restart began the hunt again
   * - and a hunt that has to watch somebody drive is a hunt that is wrong for
   * the first minute of every session. The address is converted into a chain
   * from the game's root object and written to the ini, which is the same thing
   * a hand calibration produces and is read back on the next launch.
   */
  const uintptr_t mid = deref(race + best->hop1);
  const uintptr_t obj = plausible(mid) ? deref(mid + best->hop2) : 0;
  if (plausible(obj) && g_appForScan) {
    const std::vector<scan::Chain> chains = scan::findChains(g_appForScan, obj + best->field, 4, 40000);
    if (!chains.empty()) {
      const scan::Chain* shortestChain = &chains.front();
      for (const scan::Chain& c : chains) {
        if (c.hops.size() < shortestChain->hops.size()) shortestChain = &c;
      }
      g_timeChain = *shortestChain;
      g_timeChainLoaded = true;
      config().timeChain = shortestChain->text();
      config().save();
      log::line("game: clock learned and saved - app -> %s (no calibration needed again on this build)",
                shortestChain->text().c_str());
    } else {
      log::line("game: clock found, but no chain from the app root - it will be re-learned next session");
    }
  }
  log::line("game: race clock at race+0x%X -> +0x%X -> +0x%X, reading %d ms (%d clocks kept time; took lowest). "
            "config.ini [offsets]: race_player_info = 0x%X, player_sub = 0x%X, player_time = 0x%X",
            static_cast<unsigned>(best->hop1), static_cast<unsigned>(best->hop2),
            static_cast<unsigned>(best->field), best->value, qualified,
            static_cast<unsigned>(best->hop1), static_cast<unsigned>(best->hop2),
            static_cast<unsigned>(best->field));
  return true;
}

// Does this profile produce a real UID right now? Only answerable while a map
// is loaded, which is why attaching is retried rather than done once at startup.
bool validate(const Offsets& o) {
  uintptr_t app = deref(exeBase() + o.app);
  if (!plausible(app)) return false;

  uintptr_t challenge = deref(app + o.challenge);
  if (!plausible(challenge)) return false;  // in the menus: cannot tell yet

  // A map is loaded, so from here on a failure really is this build being
  // unreadable rather than there being nothing to read.
  g_sawMap = true;
  return looksLikeUid(readUid(o, challenge));
}

}  // namespace

const std::vector<Offsets>& profiles() {
  std::lock_guard<std::mutex> guard(g_lock);
  ensureDefaults();
  return g_profiles;
}

void addProfile(const Offsets& profile) {
  std::lock_guard<std::mutex> guard(g_lock);
  ensureDefaults();
  // Ahead of the built-ins: an ini entry is somebody telling us about their own
  // build, which beats a guess every time.
  g_profiles.insert(g_profiles.begin(), profile);
  g_attached = -1;
}

std::string buildKey() {
  uintptr_t base = exeBase();
  IMAGE_DOS_HEADER dos{};
  if (!readAt<IMAGE_DOS_HEADER>(base, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return "unknown";

  IMAGE_NT_HEADERS32 nt{};
  if (!readAt<IMAGE_NT_HEADERS32>(base + static_cast<uintptr_t>(dos.e_lfanew), &nt)) return "unknown";

  char buf[64];
  sprintf_s(buf, sizeof(buf), "%08x-%08x", nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage);
  return buf;
}

bool attach() {
  std::lock_guard<std::mutex> guard(g_lock);
  ensureDefaults();
  if (g_attached >= 0) return true;

  for (size_t i = 0; i < g_profiles.size(); i++) {
    if (validate(g_profiles[i])) {
      g_attached = static_cast<int>(i);
      return true;
    }
  }
  return false;
}

bool attached() {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_attached >= 0;
}

bool sawMap() {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_sawMap;
}

std::string attachedProfile() {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_attached >= 0 ? g_profiles[static_cast<size_t>(g_attached)].name : std::string();
}

bool isValidSub(uintptr_t sub, uintptr_t* outTimeOff, uintptr_t* outStateOff, int* outTime, int* outState) {
  if (!plausible(sub)) return false;
  uintptr_t vt = deref(sub);
  if (!plausible(vt)) return false;

  for (uintptr_t toff : {0x2BCu, 0x2B0u}) {
    int t = -1;
    if (readAt<int>(sub + toff, &t) && t >= 0 && t <= kMaxRaceMs) {
      for (uintptr_t soff : {0x314u, 0x320u}) {
        int s = -1;
        if (readAt<int>(sub + soff, &s) && s >= 0 && s <= 3) {
          if (outTimeOff) *outTimeOff = toff;
          if (outStateOff) *outStateOff = soff;
          if (outTime) *outTime = t;
          if (outState) *outState = s;
          return true;
        }
      }
    }
  }
  return false;
}

uintptr_t resolvePlayerSub(uintptr_t race, uintptr_t app, uintptr_t* outTimeOff, uintptr_t* outStateOff, int* outTime, int* outState) {
  const uintptr_t base = exeBase();

  // 1. Direct path from app network player (Cheat Engine rows 14 & 30)
  uintptr_t net = deref(app + 0x12C);
  if (plausible(net)) {
    for (uintptr_t off1 : {0x1ACu, 0x23Cu}) {
      uintptr_t p1 = deref(net + off1);
      if (!plausible(p1)) continue;
      for (uintptr_t off2 : {0x78u, 0x20u}) {
        uintptr_t p2 = deref(p1 + off2);
        if (!plausible(p2)) continue;
        for (uintptr_t off3 : {0x2Cu, 0x24u}) {
          uintptr_t sub = deref(p2 + off3);
          if (isValidSub(sub, outTimeOff, outStateOff, outTime, outState)) {
            log::once("sub_resolved_net", "game: resolved true player sub via app net (0x%X -> 0x%X -> 0x%X): sub=%p",
                      static_cast<unsigned>(off1), static_cast<unsigned>(off2), static_cast<unsigned>(off3), (void*)sub);
            return sub;
          }
        }
      }
    }
  }

  // 2. Direct path from base+0x966E1C (Cheat Engine row 6)
  {
    uintptr_t b966 = deref(base + 0x966E1C);
    if (plausible(b966)) {
      uintptr_t p1 = deref(b966 + 0x6C);
      if (plausible(p1)) {
        uintptr_t p2 = deref(p1 + 0x4);
        if (plausible(p2)) {
          uintptr_t sub = deref(p2 + 0x24);
          if (isValidSub(sub, outTimeOff, outStateOff, outTime, outState)) {
            log::once("sub_resolved_966", "game: resolved true player sub via base+0x966E1C: sub=%p", (void*)sub);
            return sub;
          }
        }
      }
    }
  }

  // 3. Direct path from base+0x4DAE74 (Cheat Engine row 10)
  {
    uintptr_t b4DA = deref(base + 0x4DAE74);
    if (plausible(b4DA)) {
      uintptr_t p1 = deref(b4DA + 0x12C);
      if (plausible(p1)) {
        uintptr_t p2 = deref(p1 + 0x18);
        if (plausible(p2)) {
          uintptr_t sub = deref(p2 + 0x24);
          if (isValidSub(sub, outTimeOff, outStateOff, outTime, outState)) {
            log::once("sub_resolved_4DA", "game: resolved true player sub via base+0x4DAE74: sub=%p", (void*)sub);
            return sub;
          }
        }
      }
    }
  }

  // 4. From race (Cheat Engine rows 2, 3, 4, 5, 26)
  if (plausible(race)) {
    for (uintptr_t offInfo : {0x44u, 0x68u, 0x48u, 0x330u, 0x620u, 0xA0u, 0xB8u}) {
      uintptr_t info = deref(race + offInfo);
      if (!plausible(info)) continue;

      for (uintptr_t offPlayer : {0x28u, 0x24u, 0x78u, 0x50u, 0x60u, 0x88u, 0x58u, 0x238u, 0xBCu}) {
        uintptr_t player = deref(info + offPlayer);
        if (!plausible(player)) continue;

        // 3-hop: info -> player -> sub (check 0x24, 0x2C first!)
        for (uintptr_t offSub : {0x24u, 0x2Cu, 0x20u, 0x28u, 0x1Cu}) {
          uintptr_t sub = deref(player + offSub);
          if (isValidSub(sub, outTimeOff, outStateOff, outTime, outState)) {
            log::once("sub_resolved_race", "game: resolved true player sub at race+0x%X -> +0x%X -> +0x%X: sub=%p",
                      static_cast<unsigned>(offInfo), static_cast<unsigned>(offPlayer), static_cast<unsigned>(offSub), (void*)sub);
            return sub;
          }
        }

        // 4-hop: info -> player -> mid -> sub (e.g. 0x44 -> 0x50 -> 0x4 -> 0x24)
        for (uintptr_t offMid : {0x4u, 0x18u, 0x78u}) {
          uintptr_t mid = deref(player + offMid);
          if (!plausible(mid)) continue;
          for (uintptr_t offSub : {0x24u, 0x2Cu}) {
            uintptr_t sub = deref(mid + offSub);
            if (isValidSub(sub, outTimeOff, outStateOff, outTime, outState)) {
              log::once("sub_resolved_race4", "game: resolved true player sub at race+0x%X -> +0x%X -> +0x%X -> +0x%X: sub=%p",
                        static_cast<unsigned>(offInfo), static_cast<unsigned>(offPlayer), static_cast<unsigned>(offMid),
                        static_cast<unsigned>(offSub), (void*)sub);
              return sub;
            }
          }
        }
      }
    }
  }

  return 0;
}

Snapshot read() {
  Snapshot snap;

  Offsets o;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    ensureDefaults();
    if (g_attached < 0) return snap;
    o = g_profiles[static_cast<size_t>(g_attached)];
  }
  snap.attached = true;

  uintptr_t app = deref(exeBase() + o.app);
  if (!plausible(app)) return snap;

  uintptr_t challenge = deref(app + o.challenge);
  if (!plausible(challenge)) return snap;  // menus

  snap.uid = readUid(o, challenge);
  if (!looksLikeUid(snap.uid)) {
    // The pointer was there but the string was not. Report menus rather than a
    // half-truth - the site is never told about a map we cannot name.
    snap.uid.clear();
    return snap;
  }

  snap.inRace = true;
  snap.mapName = readName(o, challenge);

  // Everything below is a bonus. A failed step costs the finish prompt, not the
  // map report, so each one simply stops - but it records *which* step, because
  // "the race state cannot be read" is not something anybody can act on and
  // "it stops at the race pointer" is.
  g_appForScan = app;

  uintptr_t race = deref(app + o.race);
  if (!plausible(race)) {
    snap.raceStep = 1;
    return snap;
  }
  // The map's own checkpoint count, which needs nothing from the player: the
  // buffer is a CFastBuffer, size first, so the total is readable on its own.
  unsigned int total = 0;
  if (readAt<unsigned int>(challenge + o.challengeBlocks, &total) && total < 1000) {
    snap.checkpoints = static_cast<int>(total);
  }

  // The profile's own chain first. On the build this was written for it works,
  const unsigned now = GetTickCount();

  if (g_cachedRace != race) {
    g_cachedRace = race;
    g_cachedPlayerSub = 0;
    g_cachedTimeOffset = 0x2BC;
    g_cachedStateOffset = 0x314;
    g_lastClock = -1;
    g_lastClockMove = 0;
  }

  int resolvedTime = -1, resolvedState = -1;

  // 0. The calibrated chain, if this build has been taught one. It is checked
  //    first and, when it answers, nothing below runs: it is the only reading
  //    here that was ever verified against what the game displayed.
  const int calibrated = calibratedTime(app);
  if (calibrated >= 0) {
    snap.raceTimeMs = calibrated;
    snap.stateTrusted = true;

    if (calibrated != g_lastClock) {
      g_lastClock = calibrated;
      g_lastClockMove = now;
    }

    if (calibrated <= 100) {
      snap.state = RaceState::BeforeStart;
    } else if (g_lastClockMove > 0 && now - g_lastClockMove > 400) {
      // A race clock that has stopped is a finished run. With the real clock
      // this is reliable in a way it never was while the number itself was in
      // doubt - a map clock never stops, so a freeze used to mean nothing.
      snap.state = RaceState::Finished;
    } else {
      snap.state = RaceState::Running;
    }

    static int s_lastCalibState = -1;
    if (static_cast<int>(snap.state) != s_lastCalibState) {
      s_lastCalibState = static_cast<int>(snap.state);
      log::line("game: race state -> %d (calibrated clock, %d ms at %p)", s_lastCalibState, calibrated,
                (void*)g_timeAddress);
    }
    return snap;
  }

  // 1. Try reading from cached player sub
  if (g_cachedPlayerSub) {
    int t = -1, s = -1;
    if (readAt<int>(g_cachedPlayerSub + g_cachedTimeOffset, &t) && t >= 0 && t <= kMaxRaceMs &&
        readAt<int>(g_cachedPlayerSub + g_cachedStateOffset, &s) && s >= 0 && s <= 3) {
      resolvedTime = t;
      resolvedState = s;
    } else {
      g_cachedPlayerSub = 0;
    }
  }

  // 2. Resolve if not cached
  if (!g_cachedPlayerSub) {
    uintptr_t toff = 0x2BC, soff = 0x314;
    uintptr_t sub = resolvePlayerSub(race, app, &toff, &soff, &resolvedTime, &resolvedState);
    if (sub) {
      g_cachedPlayerSub = sub;
      g_cachedTimeOffset = toff;
      g_cachedStateOffset = soff;
    }
  }

  if (g_cachedPlayerSub && resolvedTime >= 0) {
    snap.raceTimeMs = resolvedTime;
    snap.stateTrusted = true;

    if (resolvedTime != g_lastClock) {
      g_lastClock = resolvedTime;
      g_lastClockMove = now;
    }

    // At the start line / countdown, the timer is at 0 or a tiny number (e.g. <= 100 ms).
    if (resolvedTime <= 100) {
      snap.state = RaceState::BeforeStart;
    } else if (resolvedState == 2) {
      // Game officially marked the race as Finished
      snap.state = RaceState::Finished;
    } else if (resolvedTime >= 1000 && g_lastClockMove > 0 && now - g_lastClockMove > 500) {
      // Time frozen after finish line for > 500ms (and car drove at least 1 second)
      snap.state = RaceState::Finished;
    } else {
      snap.state = RaceState::Running;
    }

    static int s_lastLoggedState = -1;
    if (static_cast<int>(snap.state) != s_lastLoggedState) {
      s_lastLoggedState = static_cast<int>(snap.state);
      log::line("game: race state -> %d (time: %d ms, sub: %p, toff: 0x%X, soff: 0x%X)",
                s_lastLoggedState, resolvedTime, (void*)g_cachedPlayerSub,
                static_cast<unsigned>(g_cachedTimeOffset), static_cast<unsigned>(g_cachedStateOffset));
    }

    int passed = 0;
    if ((readAt<int>(g_cachedPlayerSub + o.playerCheckpoints, &passed) ||
         readAt<int>(g_cachedPlayerSub + 0x330u, &passed) ||
         readAt<int>(g_cachedPlayerSub + 0x33Cu, &passed)) &&
        passed >= 0 && passed < 1000) {
      snap.checkpoint = passed;
    }
    return snap;
  }

  // Periodic diagnostic log when sub was not resolved
  const uintptr_t base = exeBase();
  static unsigned lastDiag = 0;
  if (now - lastDiag > 3000) {
    lastDiag = now;
    uintptr_t b966 = deref(base + 0x966E1C);
    uintptr_t b966_p1 = plausible(b966) ? deref(b966 + 0x6C) : 0;
    uintptr_t b966_p2 = plausible(b966_p1) ? deref(b966_p1 + 0x4) : 0;
    uintptr_t b966_s = plausible(b966_p2) ? deref(b966_p2 + 0x24) : 0;

    uintptr_t b4DA = deref(base + 0x4DAE74);
    uintptr_t b4DA_p1 = plausible(b4DA) ? deref(b4DA + 0x12C) : 0;
    uintptr_t b4DA_p2 = plausible(b4DA_p1) ? deref(b4DA_p1 + 0x18) : 0;
    uintptr_t b4DA_s = plausible(b4DA_p2) ? deref(b4DA_p2 + 0x24) : 0;

    uintptr_t net = deref(app + 0x12C);
    uintptr_t net_p1 = plausible(net) ? deref(net + 0x1AC) : 0;
    uintptr_t net_p2 = plausible(net_p1) ? deref(net_p1 + 0x78) : 0;

    uintptr_t r44 = deref(race + 0x44);
    uintptr_t r68 = deref(race + 0x68);

    log::line("game diag: race=%p r44=%p r68=%p app=%p net=%p p1=%p p2=%p",
              (void*)race, (void*)r44, (void*)r68, (void*)app, (void*)net, (void*)net_p1, (void*)net_p2);
    log::line("game diag b966: b=%p p1=%p p2=%p s=%p | b4DA: b=%p p1=%p p2=%p s=%p",
              (void*)b966, (void*)b966_p1, (void*)b966_p2, (void*)b966_s,
              (void*)b4DA, (void*)b4DA_p1, (void*)b4DA_p2, (void*)b4DA_s);
  }

  // Fallback: dynamic clock candidate search
  if (g_raceInfoFor != race) {
    g_raceInfoFor = race;
    g_clock1 = g_clock2 = g_clockField = 0;
    g_candidates.clear();
    g_lastScan = 0;
    g_firstQualified = 0;
    g_lastClock = -1;
    g_lastClockMove = 0;
  }
  // Retried while it has nothing: the race object exists the moment a map loads
  // and what hangs off it does not, so a single pass at load time searches a
  // half-built world and finds none.
  if (!g_clockField && g_candidates.empty() && now - g_lastScan > 6000) {
    g_lastScan = now;
    collectCandidates(race);
    log::line("game: watching %zu ints for one that keeps time", g_candidates.size());
  }
  if (!g_clockField && !g_candidates.empty()) narrowCandidates(race);

  if (g_clockField) {
    const uintptr_t mid = deref(race + g_clock1);
    const uintptr_t obj = plausible(mid) ? deref(mid + g_clock2) : 0;
    int v = 0;
    if (plausible(obj) && readAt<int>(obj + g_clockField, &v) && v >= 0 && v <= kMaxRaceMs) {
      snap.raceTimeMs = v;
      // The state, taken off the clock rather than a second guessed field:
      // moving is running, stopped above zero is a finished lap, zero is the
      // start line. Same information, one fewer thing to be wrong about.
      if (v != g_lastClock) {
        g_lastClock = v;
        g_lastClockMove = now;
      }
      if (v == 0) snap.state = RaceState::BeforeStart;
      else if (now - g_lastClockMove > 600) snap.state = RaceState::Finished;
      else snap.state = RaceState::Running;
      return snap;
    }
  }

  snap.raceStep = 2;
  return snap;
}

uintptr_t appPointer() {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_appForScan;
}

namespace {

/** Store a chain, tell the log, and make the next read use it. */
void adoptChain(const scan::Chain& chain, const char* how) {
  g_timeChain = chain;
  g_timeChainLoaded = true;
  config().timeChain = chain.text();
  config().timeAddress.clear();
  config().save();

  g_calibCandidates.clear();
  g_calibNote = "Calibrated: app -> " + chain.text();
  log::line("game: calibrated %s - app -> %s (saved to config.ini)", how, chain.text().c_str());
}

/**
 * Which of several chains to keep.
 *
 * The shortest wins. A longer chain passes through more objects, and every one
 * of those is another thing that can be replaced on the next map load while the
 * old one stays mapped - which reads as a plausible, frozen time rather than as
 * an error.
 */
const scan::Chain* shortest(const std::vector<scan::Chain>& chains) {
  const scan::Chain* best = nullptr;
  for (const scan::Chain& c : chains) {
    if (!best || c.hops.size() < best->hops.size()) best = &c;
  }
  return best;
}

}  // namespace

bool calibrateFromAddress(uintptr_t address) {
  const uintptr_t app = appPointer();
  if (!app) {
    g_calibNote = "Load a map first - there is nothing to search from in the menus.";
    log::line("game: calibration asked for, but no app pointer yet");
    return false;
  }

  const std::vector<scan::Chain> chains = scan::findChains(app, address);
  if (chains.empty()) {
    g_calibNote = "No route from the game's root object to that address. Is it still the right one?";
    return false;
  }

  for (const scan::Chain& c : chains) log::line("game: candidate chain app -> %s", c.text().c_str());
  adoptChain(*shortest(chains), "from an address");
  return true;
}

bool calibrateByTime(int milliseconds) {
  if (milliseconds <= 0) {
    g_calibNote = "Give the time the game showed, like 13.91.";
    return false;
  }
  const uintptr_t app = appPointer();
  if (!app) {
    g_calibNote = "Load a map first.";
    return false;
  }

  // A second round narrows the first: only addresses that held the first time
  // and now hold this one are still in the running. Two runs is usually enough
  // to go from hundreds of coincidences to a handful.
  if (!g_calibCandidates.empty()) {
    std::vector<uintptr_t> kept = scan::keepHolding(g_calibCandidates, milliseconds);
    if (kept.empty()) {
      g_calibCandidates = scan::findValue(milliseconds);
      g_calibNote = "Nothing survived both runs - started again from this one (" +
                    std::to_string(g_calibCandidates.size()) + " addresses).";
      return false;
    }
    g_calibCandidates = kept;
  } else {
    g_calibCandidates = scan::findValue(milliseconds);
  }

  if (g_calibCandidates.empty()) {
    g_calibNote = "Nothing in memory holds that number. Type it exactly as the game showed it.";
    return false;
  }

  // With one address left the answer is unambiguous. With a handful, take the
  // first that the game's own object graph can reach - a clock the game reads
  // hangs off its objects, and a coincidence in some allocator's bookkeeping
  // does not.
  for (uintptr_t candidate : g_calibCandidates) {
    const std::vector<scan::Chain> chains = scan::findChains(app, candidate, 4, 40000);
    if (chains.empty()) continue;
    adoptChain(*shortest(chains), "from a typed time");
    return true;
  }

  g_calibNote = std::to_string(g_calibCandidates.size()) +
                " addresses hold that time but none hang off the game's objects. Drive another run and type that "
                "time too.";
  return false;
}

CalibrationState calibration() {
  CalibrationState out;
  loadTimeChain();
  out.haveChain = !g_timeChain.hops.empty() || g_timeChain.delta != 0;
  if (out.haveChain) out.chain = g_timeChain.text();
  out.reading = out.haveChain ? calibratedTime(appPointer()) : -1;
  out.candidates = g_calibCandidates.size();
  for (const Candidate& c : g_candidates) {
    if (c.advances >= 4) out.ticking++;
    if (c.reset) out.sawReset = true;
  }
  out.note = g_calibNote;
  return out;
}

void forgetCalibration() {
  g_timeChain = scan::Chain();
  g_timeChainLoaded = true;
  g_calibCandidates.clear();
  g_calibNote.clear();
  config().timeChain.clear();
  config().save();
  log::line("game: calibration cleared");
}

std::string variant() {
  wchar_t path[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return std::string();

  std::wstring lower(path);
  for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));

  // The executable is `TmForever.exe` in both games, so the folder is what
  // tells them apart - and when it does not, saying nothing is correct: the
  // server then asks every exchange instead of three.
  if (lower.find(L"nations") != std::wstring::npos) return "tmnf";
  if (lower.find(L"united") != std::wstring::npos) return "tmuf";
  return std::string();
}

}  // namespace game
}  // namespace tmx
