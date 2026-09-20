#include "worker.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "config.h"
#include "game.h"
#include "hook.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "state.h"
#include "textures.h"

namespace tmx {

// The one instance, shared by the render thread and the worker.
Shared& shared() {
  static Shared instance;
  return instance;
}

namespace worker {
namespace {

std::atomic<bool> g_running{false};
std::thread g_thread;

using Clock = std::chrono::steady_clock;

double nowSeconds() {
  return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void toast(const std::string& text) {
  shared().write([&](State& s) {
    s.toast = text;
    s.toastUntil = nowSeconds() + 6.0;
  });
}

void note(const std::string& what, const std::string& error = "") {
  log::line("%s%s", what.c_str(), error.empty() ? "" : (" - " + error).c_str());
  shared().write([&](State& s) {
    s.lastCall = what;
    s.lastError = error;
  });
}

std::string url(const std::string& path) { return config().baseUrl + path; }

// ---------------------------------------------------------------- the device flow

void startLink() {
  char host[64]{};
  DWORD size = sizeof(host);
  if (!GetComputerNameA(host, &size)) strcpy_s(host, sizeof(host), "TrackMania PC");

  std::string body = "{\"label\":" + Json::quote(host) + "}";
  Response res = post(url("/api/game/device"), body);
  if (!res.ok || res.status != 200) {
    shared().write([&](State& s) {
      s.linking = false;
      s.linkError = res.error.empty() ? "the site refused the request" : res.error;
    });
    return;
  }

  Json data = Json::parse(res.body);
  std::string deviceCode = data.str("deviceCode");
  std::string userCode = data.str("userCode");
  std::string verify = data.str("verifyUrl", config().baseUrl + "/link");
  int interval = data.integer("interval", 5);
  int expiresIn = data.integer("expiresIn", 600);
  if (deviceCode.empty() || userCode.empty()) {
    shared().write([&](State& s) {
      s.linking = false;
      s.linkError = "the site sent no code";
    });
    return;
  }

  shared().write([&](State& s) {
    s.linking = true;
    s.userCode = userCode;
    s.verifyUrl = verify;
    s.linkError.clear();
  });

  // The browser is opened for them: the code is useless without the page, and
  // typing a URL with a fullscreen game in the way is a chore.
  ShellExecuteA(nullptr, "open", (verify + "?code=" + userCode).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

  const double deadline = nowSeconds() + expiresIn;
  std::string pollBody = "{\"deviceCode\":" + Json::quote(deviceCode) + "}";

  while (g_running && nowSeconds() < deadline) {
    // A cancel from the settings window has to be noticed between polls, which
    // is why this loop watches the flag rather than sleeping the whole interval.
    for (int i = 0; i < interval * 4 && g_running; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      State snapshot = shared().read();
      if (!snapshot.linking) return;
    }

    Response poll = post(url("/api/game/device/poll"), pollBody);
    if (!poll.ok) continue;

    Json answer = Json::parse(poll.body);
    std::string status = answer.str("status");
    if (status == "approved") {
      std::string token = answer.str("token");
      if (token.empty()) continue;
      config().token = token;
      config().save();
      shared().write([&](State& s) {
        s.linking = false;
        s.linked = true;
        s.userCode.clear();
        s.linkError.clear();
        // Asked once, here, rather than left as a setting nobody finds: the
        // player just deliberately connected, which is the moment the question
        // makes sense. Still a question - connecting is not consent to publish.
        // Shown once after linking whichever way the switch is set. It used
        // to fire only when sharing was off, which made sense while off was
        // the default - now that it is on, never mentioning it would mean the
        // one thing that sends anything off the machine turning itself on
        // quietly. So it became a notice with an off switch rather than a
        // question with an on switch.
        s.askSharing = true;
      });
      toast("Connected. Check what is being shared.");
      return;
    }
    if (status == "expired") break;
  }

  shared().write([&](State& s) {
    if (s.linking) {
      s.linking = false;
      s.linkError = "the code expired - press Connect again";
    }
  });
}

// ------------------------------------------------------------------- the site

// "#4ade80" as the number ImGui wants. The site sends its palette as CSS hex,
// which is the one format both ends can read without a lookup table.
unsigned int parseColor(const std::string& hex) {
  if (hex.size() < 7 || hex[0] != '#') return 0;
  const unsigned long value = strtoul(hex.c_str() + 1, nullptr, 16);
  const unsigned int r = (value >> 16) & 0xFF;
  const unsigned int g = (value >> 8) & 0xFF;
  const unsigned int b = value & 0xFF;
  return 0xFF000000u | (b << 16) | (g << 8) | r;  // ImGui packs ABGR
}

void applyMapAnswer(const Json& data, const std::string& uid) {
  MapStatus map;
  map.uid = uid;

  const Json* m = data.child("map");
  if (m && !m->isNull()) {
    map.onExchange = true;
    map.site = m->str("site");
    map.exchange = m->str("exchange");
    map.trackId = m->integer("trackId");
    map.name = m->str("name");
    map.url = m->str("url");
    map.playUrl = m->str("playUrl");
    map.uploadUrl = m->str("uploadUrl");
    map.open = m->tribool("open");
    map.excluded = m->tribool("excluded");
    map.score = m->integer("score");
    map.finishedBy = m->str("finishedBy");
    map.finishedAt = m->str("finishedAt");
    map.known = m->flag("known");
  }

  const Json* claim = data.child("claim");
  map.claimed = claim && !claim->isNull();
  map.refused = data.str("refused");

  std::vector<AlsoHere> alsoHere;
  if (const Json* others = data.child("alsoHere"); others && others->type == Json::Type::Array) {
    for (const Json& o : others->array) alsoHere.push_back({o.str("name"), o.str("since")});
  }

  std::vector<BoardHit> hits;
  const Json* boards = data.child("boards");
  if (boards && boards->type == Json::Type::Array) {
    for (const Json& b : boards->array) {
      BoardHit hit;
      hit.boardId = b.str("boardId");
      hit.teams = b.integer("teams");
      hit.myTeam = b.integer("myTeam");
      hit.title = b.str("title");
      hit.idx = b.integer("idx");
      const std::string how = b.str("verify", "board");
      hit.selfReported = how == "trust" || how == "game";
      const Json* holder = b.child("holder");
      if (holder && !holder->isNull()) {
        hit.held = true;
        hit.mine = holder->flag("mine");
        hit.holderName = holder->str("name");
        hit.holderTime = holder->integer("replayTime");
        hit.holderTeam = holder->integer("team");
        hit.holderTeamName = holder->str("teamName");
        hit.holderColor = parseColor(holder->str("color"));
      }
      hits.push_back(hit);
    }
  }

  shared().write([&](State& s) {
    s.map = map;
    s.hits = hits;
    s.alsoHere = alsoHere;
  });
}

void report(const std::string& uid, const std::string& state, int checkpoint = -1, int checkpoints = -1) {
  if (config().token.empty()) return;

  // The checkpoint count rides along with the report it was already making.
  // It is what turns "somebody is on this map" into "somebody is three
  // checkpoints in", and it costs nothing extra to send.
  std::string body = "{\"uid\":" + Json::quote(uid) + ",\"game\":" + Json::quote(game::variant()) +
                     ",\"state\":" + Json::quote(state) +
                     ",\"track\":" + (config().shareWhatIAmPlaying ? "true" : "false");
  if (checkpoint >= 0) body += ",\"checkpoint\":" + std::to_string(checkpoint);
  if (checkpoints >= 0) body += ",\"checkpoints\":" + std::to_string(checkpoints);
  body += "}";

  Response res = post(url("/api/game/now-playing"), body, config().token);
  if (!res.ok) {
    note("could not reach the site", res.error);
    return;
  }
  if (res.status == 401) {
    // The token was revoked from the website. Forget it here rather than
    // retrying forever: the player revoked it on purpose.
    config().token.clear();
    config().save();
    shared().write([](State& s) { s.linked = false; });
    note("this machine was disconnected on the website");
    return;
  }
  if (res.status != 200) {
    note("the site refused the report", "HTTP " + std::to_string(res.status));
    return;
  }

  applyMapAnswer(Json::parse(res.body), uid);
  note(state == "menu" ? "back in the menus" : "reported the map");
}

// "Somebody has just taken this map."
//
// The catalogue only learns that from the hourly sync, which is far too slow
// for a person who is on the map right now - so this asks the site's live
// check, which reads TMX itself. Only ever for a map the catalogue still thinks
// is open, and only every few minutes: it spends somebody else's API budget.
void checkStillOpen(const std::string& site, int trackId) {
  if (site.empty() || trackId <= 0) return;

  Response res = get(url("/api/" + site + "/map/" + std::to_string(trackId) + "/replay-check"), config().token);
  if (!res.ok || res.status != 200) return;

  Json data = Json::parse(res.body);
  if (!data.flag("ok") || !data.flag("finished")) return;

  std::string who;
  if (const Json* first = data.child("first"); first && !first->isNull()) who = first->str("name");

  shared().write([&](State& s) {
    if (s.map.trackId != trackId) return;   // they have moved on since
    s.map.open = 0;
    s.map.justFinished = true;
    s.map.justFinishedBy = who;
    s.map.finishedBy = who;
    s.toast = who.empty() ? "This map has just been finished by somebody else."
                          : "This map has just been finished by " + who + ".";
    s.toastUntil = nowSeconds() + 20.0;
  });
  log::line("TMX says this map has just been finished%s%s", who.empty() ? "" : " by ", who.c_str());
}

void clearMap() {
  shared().write([](State& s) {
    s.map = MapStatus();
    s.hits.clear();
  });
}

void loadBoards() {
  if (config().token.empty()) return;
  Response res = get(url("/api/game/bingo"), config().token);
  if (!res.ok || res.status != 200) {
    note("could not load your boards", res.error);
    return;
  }

  Json data = Json::parse(res.body);
  std::vector<BoardSummary> boards;
  const Json* list = data.child("boards");
  if (list && list->type == Json::Type::Array) {
    for (const Json& b : list->array) {
      BoardSummary summary;
      summary.id = b.str("id");
      summary.kind = b.str("kind");
      summary.title = b.str("title");
      summary.size = b.integer("size", 5);
      summary.endsAt = b.str("endsAt");
      // 0 on an ordinary board, so a picker that ignores this still reads right.
      summary.teams = b.integer("teams");
      summary.myTeam = b.integer("myTeam");
      boards.push_back(summary);
    }
  }

  shared().write([&](State& s) { s.boards = boards; });
  note("boards up to date");
}

void loadBoard(const std::string& id) {
  if (id.empty()) {
    shared().write([](State& s) {
      s.board = BoardView();
    });
    return;
  }
  if (config().token.empty()) return;

  Response res = get(url("/api/game/bingo?board=" + id), config().token);
  if (!res.ok || res.status != 200) {
    note("could not load that board", res.error);
    return;
  }

  Json data = Json::parse(res.body);
  BoardView view;
  const Json* b = data.child("board");
  if (b && !b->isNull()) {
    view.id = b->str("id");
    view.title = b->str("title");
    view.kind = b->str("kind");
    view.size = b->integer("size", 5);
    view.endsAt = b->str("endsAt");
    view.teamCount = b->integer("teamCount");
    view.teamSize = b->integer("teamSize");
    view.myTeam = b->integer("myTeam");
    view.teamAssign = b->str("teamAssign");
    // Missing on a site older than this mod, and "board" is what every board
    // was before the setting existed - so the strict reading is the default and
    // no time is ever offered by accident.
    view.verify = b->str("verify", "board");
  }

  const Json* tiles = data.child("tiles");
  if (tiles && tiles->type == Json::Type::Array) {
    for (const Json& t : tiles->array) {
      Tile tile;
      tile.idx = t.integer("idx");
      tile.site = t.str("site");
      tile.exchange = t.str("exchange");
      tile.trackId = t.integer("trackId");
      tile.name = t.str("name");
      tile.url = t.str("url");
      tile.playUrl = t.str("playUrl");
      tile.hasRecord = t.flag("hasRecord");
      tile.imageUrl = t.str("imageUrl");
      tile.uploadUrl = t.str("uploadUrl");
      const Json* holder = t.child("holder");
      if (holder && !holder->isNull()) {
        tile.held = true;
        tile.mine = holder->flag("mine");
        tile.holderName = holder->str("name");
        tile.holderTime = holder->integer("replayTime");
        tile.holderColor = parseColor(holder->str("color"));
        tile.holderTeam = holder->integer("team");
        tile.ourTeam = holder->flag("myTeam");
      }
      // Asked for here, on the worker, so the render thread only ever picks up
      // pixels that are already decoded and waiting.
      textures::request(tile.trackId, tile.imageUrl);
      view.tiles.push_back(tile);
    }
  }

  // Struck-through lines, handed down rather than worked out - see BoardLine.
  // Absent from a site older than this mod, in which case nothing is drawn,
  // which is exactly how it behaved before.
  if (const Json* rows = data.child("lines"); rows && rows->type == Json::Type::Array) {
    for (const Json& r : rows->array) {
      BoardLine line;
      line.kind = r.str("kind");
      line.n = r.integer("n");
      line.color = parseColor(r.str("color"));
      line.mine = r.flag("mine");
      view.lines.push_back(line);
    }
  }

  if (const Json* rows = data.child("ladder"); rows && rows->type == Json::Type::Array) {
    for (const Json& r : rows->array) {
      LadderRow row;
      row.name = r.str("name");
      row.color = parseColor(r.str("color"));
      row.tiles = r.integer("tiles");
      row.lines = r.integer("lines");
      row.points = r.integer("points");
      row.mine = r.flag("mine");
      row.team = r.integer("team");
      view.ladder.push_back(row);
    }
  }

  // The team standing, empty on a board played individually - which is the one
  // check the panel makes, rather than asking the board how it was configured.
  if (const Json* rows = data.child("teams"); rows && rows->type == Json::Type::Array) {
    for (const Json& r : rows->array) {
      TeamRow row;
      row.team = r.integer("team");
      row.name = r.str("name");
      row.color = parseColor(r.str("color"));
      row.tiles = r.integer("tiles");
      row.lines = r.integer("lines");
      row.points = r.integer("points");
      row.players = r.integer("players");
      row.full = r.flag("full");
      row.mine = r.flag("mine");
      view.teams.push_back(row);
    }
  }

  view.loaded = !view.id.empty();

  // What changed since the last look. The board is a shared thing - somebody
  // taking a tile you were about to drive is the single most useful thing the
  // panel can tell you, and it is invisible unless the difference is noticed
  // here.
  shared().write([&](State& s) {
    if (s.board.loaded && s.board.id == view.id) {
      for (const Tile& now : view.tiles) {
        const Tile* before = nullptr;
        for (const Tile& old : s.board.tiles) {
          if (old.idx == now.idx) {
            before = &old;
            break;
          }
        }
        if (!before) continue;
        const bool taken = now.held && (!before->held || before->holderName != now.holderName);
        // A teammate taking a tile is your side scoring, not a loss, and saying
        // it the same way as an opponent taking one would read as bad news.
        if (taken && !now.mine) {
          const std::string who = now.holderName.empty() ? std::string("somebody") : now.holderName;
          s.toast = "Tile " + std::to_string(now.idx + 1) + (now.ourTeam ? " taken for us by " : " taken by ") + who;
          s.toastUntil = nowSeconds() + 12.0;
          log::line("%s", s.toast.c_str());
        }
      }
    }
    s.board = view;
  });
  note("board up to date");
}

/**
 * Take a tile.
 *
 * `timeMs` is only ever sent on a self-reported board, and the site is what
 * enforces that - it ignores the field everywhere else. A time measured by the
 * game is self-reported however it was measured, exactly like a map claim, so
 * this is not a shortcut around the strict rule: it is the same arrangement
 * that board already advertises, one keypress instead of an alt-tab.
 */
void check(const std::string& board, int idx, int timeMs = 0) {
  if (config().token.empty()) return;

  std::string body = "{\"action\":\"check\",\"board\":" + Json::quote(board) +
                     ",\"idx\":" + std::to_string(idx);
  if (timeMs > 0) body += ",\"time\":" + std::to_string(timeMs);
  body += "}";
  Response res = post(url("/api/game/bingo"), body, config().token);
  if (!res.ok) {
    toast("Could not reach the site.");
    return;
  }

  Json data = Json::parse(res.body);
  if (res.status != 200) {
    toast(data.str("error", "That did not work."));
    return;
  }

  if (data.flag("captured")) {
    int ms = data.integer("replayTime");
    if (ms > 0) {
      toast("Tile captured - " + std::to_string(ms / 1000) + "." + std::to_string((ms % 1000) / 10) + "s");
    } else {
      // A self-reported tile taken with no time at all: there is nothing to
      // print, and "captured - 0.0s" would read as a bug.
      toast("Tile taken.");
    }
  } else {
    std::string reason = data.str("reason");
    if (reason == "no-replay") toast("No replay on TMX for that map yet - upload it first.");
    else if (reason == "too-slow") {
      // On a self-reported board the same refusal means something else: either
      // somebody holds it and you sent no time, or yours was not faster.
      toast(timeMs > 0 ? "Not faster than the tile's holder." : "Somebody holds that tile - give a time to take it.");
    }
    else if (reason == "before-board") {
      toast("That replay predates the board. TMX will not take a slower one, so this board needs its setting changed on the website.");
    }
    else toast("Nothing captured.");
  }

  loadBoard(board);
}

// TMX answers `/trackplay/<id>` with a 302 to a `tmtp://` ManiaCode, which the
// running game executes. So the mod never downloads a map itself, never writes
// into the Tracks folder, and never has to know how installing one works.
void play(const std::string& playUrl) {
  if (playUrl.empty()) {
    toast("That exchange has no play link.");
    return;
  }

  Response res = request("GET", playUrl, "", "", /*followRedirects=*/false);
  if (!res.ok || res.location.empty()) {
    toast("TMX did not hand back a play link.");
    return;
  }
  if (res.location.rfind("tmtp:", 0) != 0) {
    toast("Unexpected answer from TMX.");
    return;
  }

  ShellExecuteA(nullptr, "open", res.location.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  toast("Sent to the game.");
}

// ------------------------------------------------------------------ the loop

void loop() {
  const double startedAt = nowSeconds();
  std::string lastUid;
  double lastReport = 0;
  double lastOpenCheck = 0;
  // The last finish already sent, as uid + time: the results screen holds the
  // same Race.Time for as long as it is up, so without this one finish would
  // be submitted four times a second.
  std::string lastAutoSubmit;
  double lastBoardRefresh = 0;
  double lastBoardsRefresh = 0;
  double menuSince = 0;

  log::line("worker: started (build %s, variant %s)", game::buildKey().c_str(),
            game::variant().empty() ? "unknown" : game::variant().c_str());
  shared().write([](State& s) {
    s.linked = !config().token.empty();
    s.buildKey = game::buildKey();
    s.variant = game::variant();
  });

  if (!config().token.empty()) {
    loadBoards();
    if (!config().board.empty()) loadBoard(config().board);
  }

  while (g_running) {
    // Commands first: a button press should not wait out a poll interval.
    Command command;
    while (g_running && shared().pop(&command)) {
      switch (command.kind) {
        case Command::Kind::Connect:
          startLink();
          if (!config().token.empty()) loadBoards();
          break;
        case Command::Kind::CancelLink:
          shared().write([](State& s) { s.linking = false; });
          break;
        case Command::Kind::Disconnect:
          if (!config().token.empty()) post(url("/api/game/idle"), "{}", config().token);
          config().token.clear();
          config().save();
          shared().write([](State& s) {
            s.linked = false;
            s.boards.clear();
            s.board = BoardView();
            s.map = MapStatus();
            s.hits.clear();
          });
          toast("Disconnected. Nothing more is sent from this machine.");
          break;
        case Command::Kind::RefreshBoards:
          loadBoards();
          if (!config().board.empty()) loadBoard(config().board);
          break;
        case Command::Kind::SelectBoard:
          config().board = command.text;
          config().save();
          loadBoard(command.text);
          lastBoardRefresh = nowSeconds();
          break;
        case Command::Kind::Play:
          play(command.text);
          break;
        case Command::Kind::OpenUrl:
          // Windows hands the URL to the default browser, which brings its own
          // window forward - so an already-open browser is raised rather than a
          // second one started. There is no way to reuse a specific tab, and
          // pretending otherwise would be a promise this cannot keep.
          if (!command.text.empty()) {
            ShellExecuteA(nullptr, "open", command.text.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            toast("Opened the upload page in your browser.");
          }
          break;
        case Command::Kind::Check:
          check(command.text, command.number, command.time);
          break;
        case Command::Kind::ReportNow:
          lastUid.clear();  // force the next pass to report
          break;
        case Command::Kind::CalibrateAddress: {
          // An address out of Cheat Engine. It is only true for the run of the
          // game it was found in, which is why what gets kept is the chain.
          const uintptr_t address =
              static_cast<uintptr_t>(strtoul(command.text.c_str(), nullptr,
                                             command.text.rfind("0x", 0) == 0 ? 16 : 16));
          if (game::calibrateFromAddress(address)) {
            toast("The clock is calibrated - times come from the game now.");
          } else {
            toast(game::calibration().note);
          }
          break;
        }
        case Command::Kind::CalibrateTime:
          if (game::calibrateByTime(command.number)) {
            toast("The clock is calibrated - times come from the game now.");
          } else {
            toast(game::calibration().note);
          }
          break;
        case Command::Kind::ForgetCalibration:
          game::forgetCalibration();
          toast("Calibration cleared.");
          break;
        case Command::Kind::ReleaseAll:
          if (!config().token.empty()) post(url("/api/game/idle"), "{}", config().token);
          clearMap();
          toast("Marks released.");
          break;
      }
    }

    // Attaching is retried until it takes: the offsets can only be validated
    // while a map is loaded, so a player who starts the game and sits in the
    // menus attaches the moment they drive anything.
    if (!game::attached()) {
      if (game::attach()) {
        log::line("game: attached with profile '%s' (build %s)", game::attachedProfile().c_str(),
                  game::buildKey().c_str());
      } else {
        log::once("attach", "game: no offset profile matches yet (build %s) - load a map and this retries",
                  game::buildKey().c_str());
      }
    }

    game::Snapshot snap = game::read();
    log::once("map", "game: %s%s", snap.inRace ? "on map uid " : "in the menus",
              snap.inRace ? snap.uid.c_str() : "");
    shared().write([&](State& s) {
      s.attached = game::attached();
      s.sawMap = game::sawMap();
      s.profile = game::attachedProfile();
      s.inRace = snap.inRace;
      s.uid = snap.uid;
      s.mapName = snap.mapName;
      s.raceState = static_cast<int>(snap.state);
      s.raceStateTrusted = snap.stateTrusted;
      s.raceTimeMs = snap.raceTimeMs;
      s.raceStep = snap.raceStep;
      if (s.toastUntil > 0 && nowSeconds() > s.toastUntil) {
        s.toast.clear();
        s.toastUntil = 0;
      }
    });

    const double now = nowSeconds();
    const bool linked = !config().token.empty();

    // Twenty seconds in with a patched table and not one frame through it means
    // the game draws through something else entirely - worth saying plainly
    // rather than leaving "nothing happens" as the only symptom.
    if (now - startedAt > 20 && !hook::drewOnce()) {
      log::once("noframe", "the overlay hook is installed but the game has never called through it");
    }

    if (linked && snap.inRace && !snap.uid.empty()) {
      menuSince = 0;
      // A new map, or the mark is old enough to be worth refreshing.
      // Every minute rather than every five: the mark expires in four, so
      // that a game that dies without saying goodbye stops claiming a map
      // within minutes instead of hours - and the checkpoint it carries is only
      // worth showing if it is roughly current.
      if (snap.uid != lastUid || now - lastReport > 60) {
        lastUid = snap.uid;
        lastReport = now;
        report(snap.uid, "driving", snap.checkpoint, snap.checkpoints);
      }
    } else if (linked && !snap.inRace && !lastUid.empty()) {
      // Two seconds, not none: loading the next map passes through a moment
      // where no challenge is loaded, and releasing a mark only to take it
      // straight back would flicker on the remaining list for everybody
      // watching. Past that, leaving a map really is leaving it.
      if (menuSince == 0) menuSince = now;
      if (now - menuSince > 2) {
        lastUid.clear();
        menuSince = 0;
        report("", "menu");
        clearMap();
      }
    }

    // While you are on a map that is still open, ask TMX every three minutes
    // whether it still is. This is the endgame: a map can fall while somebody
    // is mid-run on it, and finding that out an hour later is finding it out
    // too late.
    if (linked && snap.inRace && now - lastOpenCheck > 180) {
      State view = shared().read();
      if (view.map.open == 1 && !view.map.site.empty()) {
        lastOpenCheck = now;
        checkStillOpen(view.map.site, view.map.trackId);
      }
    }

    // ------------------------------------------------- finishing a tile
    //
    // Off unless the player asked for it, and then only on a board that checks
    // nothing against TMX anyway. Everywhere else a tile is taken by a replay
    // the site goes and reads, and a time measured here is not that - it is
    // self-reported however it was measured, exactly like a map claim.
    //
    // Guarded three ways beyond the setting: the run has to have finished on a
    // map that is actually a tile of a board the player is in, it has to be a
    // time that would take the tile (the site refuses a slower one anyway, so
    // sending it would only generate a refusal), and each finish counts once -
    // `Race.Time` keeps reading the same value for as long as the results
    // screen is up, which would otherwise be a request every 250 ms.
    if (linked && config().autoSubmitSelfReported && snap.state == game::RaceState::Finished &&
        snap.raceTimeMs >= 1000) {
      const std::string finishKey = snap.uid + ":" + std::to_string(snap.raceTimeMs);
      if (finishKey != lastAutoSubmit) {
        lastAutoSubmit = finishKey;
        log::line("worker: finish %d ms on map %s - checking board tiles", snap.raceTimeMs, snap.uid.c_str());
        State view = shared().read();
        bool submitted = false;

        // 1. Check hits from now-playing
        for (const BoardHit& hit : view.hits) {
          if (!hit.selfReported) {
            log::line("worker: tile %d on board %s is not self-reported (needs TMX replay)", hit.idx, hit.boardId.c_str());
            continue;
          }
          const bool wouldTake = !hit.held || hit.holderTime <= 100 || snap.raceTimeMs < hit.holderTime;
          if (!wouldTake) {
            log::line("worker: tile %d not taken (already held with %d ms, my time %d ms)", hit.idx, hit.holderTime, snap.raceTimeMs);
            continue;
          }
          log::line("worker: AUTO-SUBMITTING tile %d on board %s with time %d ms!", hit.idx, hit.boardId.c_str(), snap.raceTimeMs);
          check(hit.boardId, hit.idx, snap.raceTimeMs);
          submitted = true;
          break;  // one tile per finish; the same map is rarely on two boards
        }

        // 2. Fallback: check current active board directly
        if (!submitted && view.board.loaded && !view.board.id.empty() &&
            (view.board.verify == "trust" || view.board.verify == "game") && view.map.trackId > 0) {
          for (const Tile& t : view.board.tiles) {
            if (t.trackId == view.map.trackId) {
              const bool wouldTake = !t.held || t.holderTime <= 100 || snap.raceTimeMs < t.holderTime;
              if (wouldTake) {
                log::line("worker: AUTO-SUBMITTING (active board fallback) tile %d on board %s with time %d ms!",
                          t.idx, view.board.id.c_str(), snap.raceTimeMs);
                check(view.board.id, t.idx, snap.raceTimeMs);
                submitted = true;
              } else {
                log::line("worker: active board tile %d not taken (held with %d ms, my time %d ms)",
                          t.idx, t.holderTime, snap.raceTimeMs);
              }
              break;
            }
          }
        }
      }
    } else if (snap.state != game::RaceState::Finished) {
      lastAutoSubmit.clear();
    }

    if (linked && now - lastBoardsRefresh > 300) {
      lastBoardsRefresh = now;
      loadBoards();
    }
    if (linked && !config().board.empty() && now - lastBoardRefresh > 20) {
      lastBoardRefresh = now;
      loadBoard(config().board);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  // Leaving politely, but on a short timeout: the game is closing and a slow
  // request must not hold the process open.
  if (!config().token.empty()) {
    request("POST", url("/api/game/idle"), "{}", config().token, true, 2000);
  }
}

}  // namespace

void start() {
  if (g_running.exchange(true)) return;
  g_thread = std::thread(loop);
}

void stop() {
  if (!g_running.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}

void releaseNow() {
  if (config().token.empty()) return;
  log::line("releasing marks - the game is closing");
  // Deliberately synchronous and deliberately short: this runs on the game's
  // own thread as its window closes, and a mark left standing would sit on the
  // remaining list for its last few minutes telling people a lie.
  request("POST", config().baseUrl + "/api/game/idle", "{}", config().token, true, 1500);
}

void signalStop() {
  g_running = false;
  // Detached rather than joined: the caller is the process shutting down, and
  // the loop may be inside an HTTP request with seconds left on its timeout.
  if (g_thread.joinable()) g_thread.detach();
}

}  // namespace worker
}  // namespace tmx
