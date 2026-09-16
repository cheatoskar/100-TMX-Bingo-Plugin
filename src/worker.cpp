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
#include "http.h"
#include "json.h"
#include "state.h"

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
      });
      toast("This machine is connected.");
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

  std::vector<BoardHit> hits;
  const Json* boards = data.child("boards");
  if (boards && boards->type == Json::Type::Array) {
    for (const Json& b : boards->array) {
      BoardHit hit;
      hit.boardId = b.str("boardId");
      hit.title = b.str("title");
      hit.idx = b.integer("idx");
      const Json* holder = b.child("holder");
      if (holder && !holder->isNull()) {
        hit.held = true;
        hit.mine = holder->flag("mine");
        hit.holderName = holder->str("name");
        hit.holderTime = holder->integer("replayTime");
      }
      hits.push_back(hit);
    }
  }

  shared().write([&](State& s) {
    s.map = map;
    s.hits = hits;
  });
}

void report(const std::string& uid, const std::string& state) {
  if (config().token.empty()) return;

  std::string body = "{\"uid\":" + Json::quote(uid) + ",\"game\":" + Json::quote(game::variant()) +
                     ",\"state\":" + Json::quote(state) +
                     ",\"track\":" + (config().shareWhatIAmPlaying ? "true" : "false") + "}";

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
      boards.push_back(summary);
    }
  }

  shared().write([&](State& s) { s.boards = boards; });
  note("boards up to date");
}

void loadBoard(const std::string& id) {
  if (config().token.empty() || id.empty()) return;

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
      const Json* holder = t.child("holder");
      if (holder && !holder->isNull()) {
        tile.held = true;
        tile.mine = holder->flag("mine");
        tile.holderName = holder->str("name");
        tile.holderTime = holder->integer("replayTime");
      }
      view.tiles.push_back(tile);
    }
  }

  view.loaded = !view.id.empty();
  shared().write([&](State& s) { s.board = view; });
  note("board up to date");
}

void check(const std::string& board, int idx) {
  if (config().token.empty()) return;

  std::string body = "{\"action\":\"check\",\"board\":" + Json::quote(board) +
                     ",\"idx\":" + std::to_string(idx) + "}";
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
    toast("Tile captured - " + std::to_string(ms / 1000) + "." + std::to_string((ms % 1000) / 10) + "s");
  } else {
    std::string reason = data.str("reason");
    if (reason == "no-replay") toast("No replay on TMX for that map yet - upload it first.");
    else if (reason == "too-slow") toast("Uploaded, but slower than the tile's holder.");
    else if (reason == "before-board") toast("That replay was driven before the board started.");
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
  std::string lastUid;
  double lastReport = 0;
  double lastBoardRefresh = 0;
  double lastBoardsRefresh = 0;
  double menuSince = 0;

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
        case Command::Kind::Check:
          check(command.text, command.number);
          break;
        case Command::Kind::ReportNow:
          lastUid.clear();  // force the next pass to report
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
    if (!game::attached()) game::attach();

    game::Snapshot snap = game::read();
    shared().write([&](State& s) {
      s.attached = game::attached();
      s.profile = game::attachedProfile();
      s.inRace = snap.inRace;
      s.uid = snap.uid;
      s.mapName = snap.mapName;
      s.raceState = static_cast<int>(snap.state);
      s.raceTimeMs = snap.raceTimeMs;
      if (s.toastUntil > 0 && nowSeconds() > s.toastUntil) {
        s.toast.clear();
        s.toastUntil = 0;
      }
    });

    const double now = nowSeconds();
    const bool linked = !config().token.empty();

    if (linked && snap.inRace && !snap.uid.empty()) {
      menuSince = 0;
      // A new map, or the mark is old enough to be worth refreshing. The claim
      // lasts two hours on the site, so five minutes is unhurried.
      if (snap.uid != lastUid || now - lastReport > 300) {
        lastUid = snap.uid;
        lastReport = now;
        report(snap.uid, "driving");
      }
    } else if (linked && !snap.inRace && !lastUid.empty()) {
      // Ten seconds of menus before releasing: loading the next map passes
      // through the menus, and releasing a mark to take it straight back would
      // make the remaining list flicker for everybody watching it.
      if (menuSince == 0) menuSince = now;
      if (now - menuSince > 10) {
        lastUid.clear();
        menuSince = 0;
        report("", "menu");
        clearMap();
      }
    }

    if (linked && now - lastBoardsRefresh > 300) {
      lastBoardsRefresh = now;
      loadBoards();
    }
    if (linked && !config().board.empty() && now - lastBoardRefresh > 45) {
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

void signalStop() {
  g_running = false;
  // Detached rather than joined: the caller is the process shutting down, and
  // the loop may be inside an HTTP request with seconds left on its timeout.
  if (g_thread.joinable()) g_thread.detach();
}

}  // namespace worker
}  // namespace tmx
