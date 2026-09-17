#include "game.h"

#include "log.h"

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
struct Candidate {
  uintptr_t offset = 0;
  int time = 0;
  unsigned tick = 0;
  int advances = 0;   // times its clock moved in step with the wall clock
};

std::vector<Candidate> g_candidates;
uintptr_t g_raceInfoFound = 0;
uintptr_t g_raceInfoFor = 0;

// Reads the rest of the chain from a candidate offset. False when any step of
// it does not look like a player.
bool chainReads(const Offsets& o, uintptr_t race, uintptr_t k, int* state, int* time) {
  const uintptr_t info = deref(race + k);
  if (!plausible(info)) return false;
  const uintptr_t player = deref(info + o.playerInfoPlayer);
  if (!plausible(player)) return false;
  const uintptr_t sub = deref(player + o.playerSub);
  if (!plausible(sub)) return false;
  if (!readAt<int>(sub + o.playerState, state) || *state < 0 || *state > 2) return false;
  if (!readAt<int>(sub + o.playerTime, time)) return false;
  // A lap under half an hour, or a clock that has not started.
  return *time >= -1 && *time <= 30 * 60 * 1000;
}

// Everything that could be it. Cheap enough to redo per race object: a few
// hundred guarded reads, each of which costs a failed page access at worst.
void collectCandidates(const Offsets& o, uintptr_t race) {
  g_candidates.clear();
  const unsigned now = GetTickCount();
  for (uintptr_t k = 0x40; k <= 0x800; k += 4) {
    int state = 0, time = 0;
    if (chainReads(o, race, k, &state, &time)) g_candidates.push_back({k, time, now, 0});
  }
  log::line("game: %zu candidate race_player_info offsets - watching for one whose clock keeps time",
            g_candidates.size());
}

/**
 * Narrow the candidates by watching their clocks.
 *
 * The first version of this took the first chain that read plausibly, and on a
 * real build that gave 0x620 one run and 0x44 the next - both cannot be right,
 * and a test that accepts either is not a test. Plenty of integers sit in 0..2
 * and plenty of others look like a lap time.
 *
 * What nothing else in the process does is advance a millisecond counter *in
 * step with the wall clock*. So each pass compares the candidate's movement
 * against how long actually elapsed: a real race timer advances by roughly that
 * much, a coincidence does not. Three such agreements in a row is taken as
 * proof; at 250 ms a pass that is about a second of driving.
 */
uintptr_t narrowCandidates(const Offsets& o, uintptr_t race) {
  const unsigned now = GetTickCount();
  std::vector<Candidate> alive;

  for (Candidate c : g_candidates) {
    int state = 0, time = 0;
    if (!chainReads(o, race, c.offset, &state, &time)) continue;   // stopped making sense

    const int elapsed = static_cast<int>(now - c.tick);
    const int moved = time - c.time;
    if (moved != 0) {
      // Forwards, and by about as long as we waited. A 50 ms allowance either
      // way for the loop not being a metronome.
      const bool keepsTime = moved > 0 && moved <= elapsed + 50;
      if (!keepsTime) continue;   // jumped, or ran backwards: not a race clock
      c.advances++;
      c.time = time;
      c.tick = now;
    }
    alive.push_back(c);
  }

  g_candidates = alive;
  for (const Candidate& c : g_candidates) {
    if (c.advances < 3) continue;
    log::line("game: race_player_info is 0x%X on this build (profile said 0x%X) - pin it with "
              "race_player_info = 0x%X under [offsets] in config.ini",
              static_cast<unsigned>(c.offset), static_cast<unsigned>(o.racePlayerInfo),
              static_cast<unsigned>(c.offset));
    return c.offset;
  }
  return 0;
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
  uintptr_t race = deref(app + o.race);
  if (!plausible(race)) {
    snap.raceStep = 1;
    return snap;
  }
  uintptr_t info = deref(race + o.racePlayerInfo);
  if (!plausible(info)) {
    // The profile's offset does not work on this build. Look for the right one
    // once per race object rather than every quarter second, and remember it.
    if (g_raceInfoFor != race) {
      g_raceInfoFor = race;
      g_raceInfoFound = 0;
      collectCandidates(o, race);
    }
    if (!g_raceInfoFound) g_raceInfoFound = narrowCandidates(o, race);
    if (g_raceInfoFound) info = deref(race + g_raceInfoFound);
    if (!plausible(info)) {
      snap.raceStep = 2;
      return snap;
    }
  }
  uintptr_t player = deref(info + o.playerInfoPlayer);
  if (!plausible(player)) {
    snap.raceStep = 3;
    return snap;
  }
  uintptr_t sub = deref(player + o.playerSub);
  if (!plausible(sub)) {
    snap.raceStep = 4;
    return snap;
  }

  int state = 0;
  if (readAt<int>(sub + o.playerState, &state) && state >= 0 && state <= 2) {
    snap.state = static_cast<RaceState>(state);
  } else {
    snap.raceStep = 5;
  }
  int time = 0;
  if (readAt<int>(sub + o.playerTime, &time)) snap.raceTimeMs = time;

  // How far through the lap. The map's own total is the size of its checkpoint
  // buffer, which is a CFastBuffer - size first, then the pointer - so the
  // count is readable without following anything.
  int passed = 0;
  if (readAt<int>(sub + o.playerCheckpoints, &passed) && passed >= 0 && passed < 1000) {
    snap.checkpoint = passed;
  }
  unsigned int total = 0;
  if (readAt<unsigned int>(challenge + o.challengeBlocks, &total) && total < 1000) {
    snap.checkpoints = static_cast<int>(total);
  }

  return snap;
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
