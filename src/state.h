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
  std::string uploadUrl;
  int open = -1;             // 1 open, 0 finished, -1 not in the catalogue
  int excluded = -1;
  int score = 0;
  std::string finishedBy;
  std::string finishedAt;
  bool claimed = false;      // a mark is standing for this map
  std::string refused;       // why no mark was taken, when that happened
  /** Set when TMX itself was asked and said somebody has just finished this map. */
  bool justFinished = false;
  std::string justFinishedBy;
};

struct BoardSummary {
  std::string id;
  std::string kind;
  std::string title;
  int size = 5;
  std::string endsAt;
  /** Sides on this board, or 0 for everybody for themselves. */
  int teams = 0;
  /** Which side the player is on, or 0. */
  int myTeam = 0;
};

struct Tile {
  int idx = 0;
  std::string site;
  std::string exchange;
  int trackId = 0;
  std::string name;
  std::string url;
  std::string playUrl;
  std::string uploadUrl;
  bool hasRecord = false;
  bool held = false;
  bool mine = false;
  std::string holderName;
  int holderTime = 0;   // milliseconds; the time to beat
  /** The holder's colour on this board - the same one the website paints. */
  unsigned int holderColor = 0;
  /**
   * The side that holds it, or 0 without teams.
   *
   * `mine` stays personal - "you hold this" - while `ourTeam` is "my side holds
   * this". Three states, not two: a tile my team already owns is not worth an
   * evening, and without this the grid cannot say so.
   */
  int holderTeam = 0;
  bool ourTeam = false;
  /** TMX's screenshot of the map. */
  std::string imageUrl;
};

/**
 * A side of a team board.
 *
 * The standing that matters there: tiles and lines are scored per team, so the
 * player ladder below it is a roster rather than a ranking.
 */
struct TeamRow {
  int team = 0;
  std::string name;
  unsigned int color = 0;
  int tiles = 0;
  int lines = 0;
  int points = 0;
  int players = 0;
  bool full = false;
  bool mine = false;
};

/** A row of the board's standing, as the website ranks it. */
struct LadderRow {
  std::string name;
  unsigned int color = 0;
  int tiles = 0;
  int lines = 0;
  int points = 0;
  bool mine = false;
  /** The side they play for, or 0. */
  int team = 0;
};

/**
 * A line somebody has completed, as the site works it out.
 *
 * Not recomputed here on purpose. What counts as a line - and, on a team
 * board, whose line it is - lives in one place, `score.ts`, and the website's
 * stroke and the board's points are already drawn from it. A second
 * implementation in C++ would be free to disagree with both, and the first
 * time it did nobody would know which one to believe.
 */
struct BoardLine {
  /** "row", "col", "diag" or "anti". */
  std::string kind;
  /** Which row or column; 0 on either diagonal. */
  int n = 0;
  unsigned int color = 0;
  bool mine = false;
};

struct BoardView {
  std::string id;
  std::string title;
  std::string kind;
  int size = 5;
  std::string endsAt;
  std::vector<Tile> tiles;
  /** Completed lines, struck through on the grid. Usually empty. */
  std::vector<BoardLine> lines;
  std::vector<LadderRow> ladder;
  /** Empty unless this board is played in sides - the one check that decides. */
  std::vector<TeamRow> teams;
  int teamCount = 0;
  int teamSize = 0;       // 0 = no cap
  int myTeam = 0;
  std::string teamAssign; // "choose" | "random"; display only, joining is on the website
  /**
   * How a tile is taken here: "board", "any" or "trust".
   *
   * Only `trust` changes what this mod may do. There the site takes a time from
   * us, because the board checks nothing against TMX at all and says so on its
   * face - it is the same self-reporting the board already advertises, not a
   * hole in a stricter rule. On the other two the button stays "I uploaded it"
   * and the site goes and looks.
   */
  std::string verify;
  bool loaded = false;

  /**
   * Boards this overlay may put a time on.
   *
   * "trust" also takes one typed on the website; "game" takes one from here and
   * nowhere else, so on that kind the overlay is the only way in and the site's
   * own button is closed. Both are self-reported - the second is a house rule
   * about where the number comes from, not a check that it is true.
   */
  bool selfReported() const { return verify == "trust" || verify == "game"; }
  /** The website cannot take a tile here, so the overlay must always offer to. */
  bool gameOnly() const { return verify == "game"; }
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
  /** Teams on that board, and which side is ours. 0 when it has none. */
  int teams = 0;
  int myTeam = 0;
  /** The side holding it, and what it is called - so a line can name it. */
  int holderTeam = 0;
  std::string holderTeamName;
  unsigned int holderColor = 0;
  /**
   * That board takes a self-reported time, so a finish here can go straight on
   * it. Carried per hit because the player may be in several boards at once and
   * only some of them work that way.
   */
  bool selfReported = false;
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
  /** True only when the state came from a known-good offset, not the search. */
  bool raceStateTrusted = false;
  /**
   * Whether the stopped clock has been shown to be a finish.
   *
   * A stopped clock is a finish or the pause menu, and at the moment it stops
   * nothing readable tells the two apart. Two things later do: TrackMania
   * writing a replay, which it does not for a pause, and the clock restarting
   * from zero, which a resumed pause does not do either. Until one of them
   * lands the panel says the clock stopped rather than claiming a finish -
   * somebody standing at a checkpoint should not be congratulated.
   */
  bool finishProved = false;

  int raceTimeMs = -1;
  /** Which dereference the walk to the player died on. See game::Snapshot. */
  int raceStep = 0;

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
  enum class Kind {
    Connect, CancelLink, Disconnect, RefreshBoards, SelectBoard, Play, Check, ReportNow, ReleaseAll, OpenUrl,
    // Teaching this build where its race clock lives. Both walk the game's
    // object graph, which takes long enough to matter - so they are commands
    // for the worker rather than something a button does on the render thread.
    CalibrateAddress, CalibrateTime, ForgetCalibration
  };
  Kind kind = Kind::RefreshBoards;
  std::string text;   // board id, or a play URL
  int number = 0;     // tile index
  /**
   * A time in milliseconds, for a Check on a self-reported board.
   *
   * Zero means "no time" - which takes an unclaimed tile and nothing else,
   * because there is then nothing for a challenger to beat. The site enforces
   * that; this only carries it.
   */
  int time = 0;
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
