// What the overlay draws and the worker fills in.
//
// One struct behind one mutex, copied wholesale for each frame. The render
// thread must never wait on the network and must never hold a lock while it
// draws, so it takes a copy and lets go - a frame drawn from state that is
// 200 ms old is invisible; a frame that waits for an HTTP request is a stutter
// everybody notices.
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tmx {

// What the site says about the map currently loaded.
struct MapStatus {
  bool known = false;        // our catalogue has it
  bool onExchange = false;   // some exchange has it at all
  std::string uid;
  std::string name;
  std::string site;          // "tmnf"
  std::string exchange;      // "TMNF-X"
  int trackId = 0;
  std::string url;
  std::string playUrl;
  int open = -1;             // 1 open, 0 finished, -1 not in the catalogue
  int excluded = -1;
  int score = 0;
  std::string finishedBy;
  std::string finishedAt;
  bool claimed = false;      // a mark is standing for this map
  std::string refused;       // why no mark was taken, when that happened
};

struct BoardSummary {
  std::string id;
  std::string kind;
  std::string title;
  int size = 5;
  std::string endsAt;
};

struct Tile {
  int idx = 0;
  std::string site;
  std::string exchange;
  int trackId = 0;
  std::string name;
  std::string url;
  std::string playUrl;
  bool hasRecord = false;
  bool held = false;
  bool mine = false;
  std::string holderName;
  int holderTime = 0;   // milliseconds; the time to beat
};

struct BoardView {
  std::string id;
  std::string title;
  std::string kind;
  int size = 5;
  std::string endsAt;
  std::vector<Tile> tiles;
  bool loaded = false;
};

// The current map's place on a board the player is in.
/** Somebody else with this map marked as being played, right now. */
struct AlsoHere {
  std::string name;
  std::string since;
};

struct BoardHit {
  std::string boardId;
  std::string title;
  int idx = 0;
  bool held = false;
  bool mine = false;
  std::string holderName;
  int holderTime = 0;
};

struct State {
  // ------------------------------------------------------------- the game
  bool attached = false;
  /** A map has been loaded at least once, so the build check has had a chance. */
  bool sawMap = false;
  std::string buildKey;
  std::string profile;
  std::string variant;
  bool inRace = false;
  std::string uid;
  std::string mapName;
  int raceState = -1;   // game::RaceState
  int raceTimeMs = -1;

  // ---------------------------------------------------------- the account
  bool linked = false;
  bool linking = false;
  std::string userCode;
  std::string verifyUrl;
  std::string linkError;
  /** Set right after a machine is linked, so the sharing question is asked once. */
  bool askSharing = false;

  // ------------------------------------------------------------- the site
  MapStatus map;
  std::vector<BoardHit> hits;
  std::vector<AlsoHere> alsoHere;
  std::vector<BoardSummary> boards;
  BoardView board;
  std::string lastCall;     // "reported the map", "checked tile 7", …
  std::string lastError;

  // A line that fades: the answer to the last thing the player pressed.
  std::string toast;
  double toastUntil = 0;
};

// Commands travel the other way: the overlay pushes, the worker performs.
struct Command {
  enum class Kind { Connect, CancelLink, Disconnect, RefreshBoards, SelectBoard, Play, Check, ReportNow, ReleaseAll };
  Kind kind = Kind::RefreshBoards;
  std::string text;   // board id, or a play URL
  int number = 0;     // tile index
};

class Shared {
 public:
  template <typename Fn>
  void write(Fn fn) {
    std::lock_guard<std::mutex> guard(lock_);
    fn(state_);
  }

  State read() {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
  }

  void push(const Command& command) {
    std::lock_guard<std::mutex> guard(lock_);
    // A queue that grows without limit would mean a held-down button becoming a
    // minute of requests after the fact.
    if (commands_.size() < 32) commands_.push_back(command);
  }

  bool pop(Command* out) {
    std::lock_guard<std::mutex> guard(lock_);
    if (commands_.empty()) return false;
    *out = commands_.front();
    commands_.pop_front();
    return true;
  }

 private:
  std::mutex lock_;
  State state_;
  std::deque<Command> commands_;
};

Shared& shared();

}  // namespace tmx
