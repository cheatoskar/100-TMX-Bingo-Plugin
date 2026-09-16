#include "overlay.h"

#include <d3d9.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "state.h"
#include "textures.h"
#include "worker.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmx {
namespace overlay {
namespace {

bool g_ready = false;
bool g_uiOpen = false;
bool g_toggleHeld = false;
HWND g_window = nullptr;
// Kept from the frame we are drawing in: textures belong to this device and may
// only be made on this thread.
IDirect3DDevice9* g_device = nullptr;
// Whether a map is loaded, copied out of the last frame's state. It is what
// decides whether the panel may take the mouse.
bool g_inRace = false;
WNDPROC g_originalWndProc = nullptr;
int g_selectedTile = -1;

const ImVec4 kOpen(0.36f, 0.78f, 0.44f, 1.0f);
// An unclaimed tile is neutral, not good news - green on twenty-five of them
// drowned out the one colour that means something: a player's.
const ImVec4 kFree(0.55f, 0.57f, 0.62f, 1.0f);
// Red for a tile somebody else has taken, green for one still going: the two
// states you scan the grid for. Gold stays for your own, because "taken" and
// "taken by me" are not the same news.
const ImVec4 kTaken(0.91f, 0.35f, 0.35f, 1.0f);
const ImVec4 kDone(0.72f, 0.72f, 0.75f, 1.0f);
const ImVec4 kMine(1.00f, 0.78f, 0.24f, 1.0f);
const ImVec4 kWarn(0.95f, 0.55f, 0.35f, 1.0f);
const ImVec4 kMuted(0.62f, 0.64f, 0.70f, 1.0f);

// The same clock the worker ages toasts against, so a message set here is
// cleared there on schedule instead of sticking to the panel forever.
double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string timeString(int ms) {
  if (ms <= 0) return "-";
  int minutes = ms / 60000;
  int seconds = (ms / 1000) % 60;
  int hundredths = (ms % 1000) / 10;
  char buf[32];
  if (minutes > 0) {
    sprintf_s(buf, sizeof(buf), "%d:%02d.%02d", minutes, seconds, hundredths);
  } else {
    sprintf_s(buf, sizeof(buf), "%d.%02d", seconds, hundredths);
  }
  return buf;
}

void setup(IDirect3DDevice9* device) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the mod keeps its own settings; ImGui keeps none
  ImGui::StyleColorsDark();

  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.WindowBorderSize = 1.0f;

  D3DDEVICE_CREATION_PARAMETERS params{};
  if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow) {
    g_window = params.hFocusWindow;
    ImGui_ImplWin32_Init(g_window);
    // Subclassing rather than a global hook: it is the game's own window, and
    // the original procedure is called for everything the panel does not eat.
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndProc)));
  }

  ImGui_ImplDX9_Init(device);
  g_ready = true;
  log::line("overlay ready (window %p, subclassed %s)", (void*)g_window, g_originalWndProc ? "yes" : "NO");
}

// Whether the panel is taking the mouse this frame.
//
// The default is automatic: while the game shows a cursor - which is to say in
// the menus and the pause screen - the panel is clickable, and the moment the
// cursor goes away for a race it is not. That is what people expect without
// being told, and it still cannot eat a click mid-run, which is the reason it
// was click-through in the first place.
bool interactive() {
  switch (config().panelInput) {
    case 1: return g_uiOpen;
    case 2: return true;
    default: break;
  }
  // Not the Windows cursor: TrackMania draws its own in the menus and keeps the
  // system one hidden, so CURSOR_SHOWING is false even where a pointer is
  // plainly on screen. The game's own state is the honest signal - no map
  // loaded means menus, and menus are where clicking the panel is safe.
  return g_uiOpen || !g_inRace;
}

void pushCommand(Command::Kind kind, const std::string& text = "", int number = 0) {
  Command command;
  command.kind = kind;
  command.text = text;
  command.number = number;
  shared().push(command);
}

// ------------------------------------------------------------------ the panel

void drawMapBlock(const State& state) {
  if (!state.attached) {
    // Two different situations, and telling them apart is the whole point: in
    // the menus there is no map to check the addresses against, so calling that
    // "not recognised" accuses the build of something it has not done yet.
    if (!state.sawMap) {
      ImGui::TextColored(kMuted, "Waiting for a map.");
      ImGui::TextWrapped("Load any map once and the mod confirms it can read this build.");
    } else {
      ImGui::TextColored(kWarn, "This TrackMania build is not recognised.");
      ImGui::TextColored(kMuted, "build %s", state.buildKey.c_str());
      ImGui::TextWrapped("Nothing is being read or sent. Report that build id and it can be added.");
    }
    return;
  }

  if (!state.inRace) {
    ImGui::TextColored(kMuted, "In the menus.");
    return;
  }

  ImGui::TextUnformatted(state.mapName.empty() ? "(unnamed map)" : state.mapName.c_str());

  const MapStatus& map = state.map;
  if (!map.onExchange) {
    ImGui::TextColored(kMuted, "Not on an exchange.");
    return;
  }

  if (map.open == 1 && map.excluded != 1) {
    ImGui::TextColored(kOpen, "Still open - worth %d", map.score);
  } else if (map.excluded == 1) {
    ImGui::TextColored(kWarn, "On the exclusion list");
  } else if (map.justFinished) {
    ImGui::TextColored(kTaken, "Just finished%s%s", map.justFinishedBy.empty() ? "" : " by ",
                       map.justFinishedBy.c_str());
    ImGui::TextWrapped("TMX has a replay on it now - it is no longer worth the run.");
  } else if (map.open == 0) {
    if (map.finishedBy.empty()) {
      ImGui::TextColored(kDone, "Already finished");
    } else {
      ImGui::TextColored(kDone, "Finished by %s", map.finishedBy.c_str());
    }
  } else {
    ImGui::TextColored(kMuted, "Not in the catalogue yet");
  }

  ImGui::TextColored(kMuted, "%s #%d", map.exchange.c_str(), map.trackId);

  if (map.claimed) {
    ImGui::TextColored(kMine, "Marked as yours on the site");
  } else if (!map.refused.empty()) {
    ImGui::TextColored(kMuted, "%s", map.refused.c_str());
  } else if (!config().shareWhatIAmPlaying) {
    ImGui::TextColored(kMuted, "Sharing is off");
  }

  // Somebody else has this map marked. Not a warning - two people on one map is
  // allowed and always was - but it is the thing you would want to know before
  // spending the evening on it.
  // Straight after a finish the one thing worth doing is uploading the replay:
  // it is what credits the map and what a bingo tile is checked against, and
  // hunting the map down on the website afterwards is where people give up.
  if (state.raceState == 2 && !map.uploadUrl.empty()) {
    ImGui::Separator();
    if (state.raceTimeMs > 0) {
      ImGui::TextColored(kOpen, "Finished in %s", timeString(state.raceTimeMs).c_str());
    }
    if (interactive() && ImGui::Button("Upload the replay to TMX")) {
      pushCommand(Command::Kind::OpenUrl, map.uploadUrl);
    }
    ImGui::TextColored(kMuted, "Opens this map's upload page in your browser.");
    ImGui::Separator();
  }

  for (const AlsoHere& other : state.alsoHere) {
    ImGui::TextColored(kWarn, "also here: %s", other.name.empty() ? "another player" : other.name.c_str());
  }

  for (const BoardHit& hit : state.hits) {
    ImGui::Separator();
    ImGui::TextColored(kMine, "Tile %d on %s", hit.idx + 1,
                       hit.title.empty() ? hit.boardId.c_str() : hit.title.c_str());
    if (hit.held) {
      ImGui::TextColored(kMuted, "%s holds it at %s", hit.mine ? "you" : hit.holderName.c_str(),
                         timeString(hit.holderTime).c_str());
    } else {
      ImGui::TextColored(kMuted, "nobody holds it yet");
    }
    if (state.raceState == 2 && state.raceTimeMs > 0) {
      ImGui::TextColored(kOpen, "Your run: %s", timeString(state.raceTimeMs).c_str());
      ImGui::TextWrapped("Upload the replay to TMX, then press \"I uploaded it\".");
    }
    if (interactive() && ImGui::Button(("I uploaded it##" + hit.boardId).c_str())) {
      pushCommand(Command::Kind::Check, hit.boardId, hit.idx);
    }
  }
}

void drawBoard(const State& state) {
  const BoardView& board = state.board;
  if (!board.loaded) {
    ImGui::TextColored(kMuted, "No board selected.");
    return;
  }

  ImGui::TextUnformatted(board.title.empty() ? board.id.c_str() : board.title.c_str());

  const int size = board.size > 0 ? board.size : 5;

  // The grid fills whatever width the window has been dragged to, so making the
  // panel bigger makes the board bigger rather than adding empty space.
  const float available = ImGui::GetContentRegionAvail().x;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float cell = std::max(22.0f, (available - spacing * (size - 1)) / static_cast<float>(size));

  // Past this a tile is big enough for the map to be worth more than its
  // number; below it the screenshot would be a smudge.
  const bool showImages = cell >= 46.0f;

  for (const Tile& tile : board.tiles) {
    if (tile.idx % size != 0) ImGui::SameLine();

    // The holder's own colour, straight from the board's palette, so the grid
    // here and the grid in the browser are the same picture. Red is the
    // fallback when a colour did not come through; green still means open.
    ImVec4 colour = kFree;
    if (tile.held) {
      colour = tile.holderColor ? ImGui::ColorConvertU32ToFloat4(tile.holderColor) : kTaken;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colour.x * 0.35f, colour.y * 0.35f, colour.z * 0.35f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, colour);

    char label[32];
    sprintf_s(label, sizeof(label), "%d##tile%d", tile.idx + 1, tile.idx);
    // The tile the player is standing on gets a border rather than a colour of
    // its own: colour already means "who holds this".
    const bool here = !state.map.uid.empty() && tile.trackId == state.map.trackId && tile.site == state.map.site;
    if (here) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);

    void* image = showImages ? textures::get(g_device, tile.trackId) : nullptr;
    bool pressed = false;
    if (image) {
      // The holder's colour becomes the tint, so a board of screenshots still
      // reads as a board of claims at a glance.
      const ImVec4 tint = tile.held ? ImVec4(colour.x, colour.y, colour.z, 1.0f) : ImVec4(1, 1, 1, 1);
      pressed = ImGui::ImageButton(label, reinterpret_cast<ImTextureID>(image), ImVec2(cell - 10, cell - 10),
                                   ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
    } else {
      pressed = ImGui::Button(label, ImVec2(cell, cell));
    }
    if (pressed && interactive()) g_selectedTile = tile.idx;
    if (here) ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
      if (void* image = textures::get(g_device, tile.trackId)) {
        ImGui::Image(reinterpret_cast<ImTextureID>(image), ImVec2(240, 135));
      }
      if (tile.held) {
        ImGui::TextColored(kMuted, "%s - %s", tile.mine ? "you" : tile.holderName.c_str(),
                           timeString(tile.holderTime).c_str());
      } else {
        ImGui::TextColored(kMuted, "unclaimed%s", tile.hasRecord ? ", map already has a replay" : "");
      }
      ImGui::EndTooltip();
    }
  }

  // Who is ahead, in the board's own colours. Three rows: the panel is 260px
  // wide and the question it answers is "am I winning", not "what is the exact
  // order of eight people".
  if (!board.ladder.empty()) {
    ImGui::Separator();
    int shown = 0;
    for (const LadderRow& row : board.ladder) {
      if (shown++ >= 3 && !row.mine) continue;
      ImGui::TextColored(row.color ? ImGui::ColorConvertU32ToFloat4(row.color) : kMuted, "%d.", shown);
      ImGui::SameLine();
      ImGui::TextUnformatted(row.name.empty() ? "somebody" : row.name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(kMuted, "%d tile%s%s", row.tiles, row.tiles == 1 ? "" : "s",
                         row.lines > 0 ? (row.lines == 1 ? " + a line" : " + lines") : "");
    }
  }

  if (g_selectedTile >= 0) {
    for (const Tile& tile : board.tiles) {
      if (tile.idx != g_selectedTile) continue;
      ImGui::Separator();
      if (void* image = textures::get(g_device, tile.trackId)) {
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::Image(reinterpret_cast<ImTextureID>(image), ImVec2(width, width * 9.0f / 16.0f));
      }
      ImGui::TextUnformatted(tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
      ImGui::TextColored(kMuted, "%s #%d%s", tile.exchange.c_str(), tile.trackId,
                         tile.hasRecord ? " - already has a replay" : " - never finished");
      if (tile.held) {
        ImGui::TextColored(tile.mine ? kMine : kTaken, "%s holds it at %s",
                           tile.mine ? "you" : tile.holderName.c_str(), timeString(tile.holderTime).c_str());
      }
      if (interactive()) {
        if (ImGui::Button("Play this map")) pushCommand(Command::Kind::Play, tile.playUrl);
        ImGui::SameLine();
        if (ImGui::Button("I uploaded it")) pushCommand(Command::Kind::Check, board.id, tile.idx);
        if (!tile.uploadUrl.empty()) {
          ImGui::SameLine();
          if (ImGui::Button("Upload")) pushCommand(Command::Kind::OpenUrl, tile.uploadUrl);
        }
      }
      break;
    }
  }
}

void drawPanel(const State& state) {
  ImGuiIO& io = ImGui::GetIO();
  const float margin = 16.0f;
  const float width = 260.0f * config().overlayScale;

  // Placed once per session: after that the window owns its own position, so a
  // drag is not fought by a SetNextWindowPos on the very next frame. The saved
  // position wins over the corner; the corner is only where it starts out.
  static bool placed = false;
  if (!placed) {
    const bool saved = config().overlayX >= 0 && config().overlayY >= 0;
    ImVec2 position = saved ? ImVec2(config().overlayX, config().overlayY)
                            : ImVec2(config().overlayCorner == 0 ? margin : io.DisplaySize.x - width - margin,
                                     margin + 80.0f);
    // Nudged back on screen if the resolution shrank since it was saved -
    // a panel parked off the edge would look exactly like the mod being broken.
    position.x = position.x < 0 ? margin : (position.x > io.DisplaySize.x - 60 ? io.DisplaySize.x - width - margin : position.x);
    position.y = position.y < 0 ? margin : (position.y > io.DisplaySize.y - 40 ? margin : position.y);
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    placed = true;
  }
  // Sized once, then the window owns it: drag the corner, and a board dragged
  // wide enough starts showing the maps instead of their numbers.
  const bool savedSize = config().overlayW > 80 && config().overlayH > 80;
  ImGui::SetNextWindowSize(savedSize ? ImVec2(config().overlayW, config().overlayH) : ImVec2(width, 340),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(220, 180), ImVec2(1600, 1600));
  ImGui::SetNextWindowBgAlpha(config().overlayAlpha);

  // A title bar rather than a bare box: it is what you grab to move it and
  // what you click to fold it away, and it is the same shape as the settings
  // window, so there is one idea to learn instead of two.
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  // Click-through until the panel is opened: while driving it is a readout, and
  // a window that eats the mouse in a racing game is a bug, not a feature. That
  // also means it can only be dragged with the window open, which is the only
  // time somebody means to move it.
  if (!interactive()) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

  if (ImGui::Begin("100% TMX + Bingo###tmx-panel", nullptr, flags)) {
    drawMapBlock(state);
    ImGui::Separator();
    drawBoard(state);

    if (!state.toast.empty()) {
      ImGui::Separator();
      ImGui::TextWrapped("%s", state.toast.c_str());
    }

    // Remembered when the drag ends rather than every frame: this writes a file.
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (interactive() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        (position.x != config().overlayX || position.y != config().overlayY || size.x != config().overlayW ||
         size.y != config().overlayH)) {
      config().overlayX = position.x;
      config().overlayY = position.y;
      config().overlayW = size.x;
      config().overlayH = size.y;
      config().save();
    }
  }
  ImGui::End();
}

// --------------------------------------------------------------- the settings

void drawSettings(const State& state) {
  ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("100% TMX", &g_uiOpen)) {
    ImGui::End();
    return;
  }

  if (ImGui::BeginTabBar("##tabs")) {
    if (ImGui::BeginTabItem("Bingo")) {
      if (!state.linked) {
        ImGui::TextWrapped("Connect this machine first, on the Connection tab.");
      } else if (state.boards.empty()) {
        ImGui::TextWrapped("You are not in any board that is still running. Join one on the website and press Refresh.");
        if (ImGui::Button("Refresh")) pushCommand(Command::Kind::RefreshBoards);
      } else {
        for (const BoardSummary& board : state.boards) {
          const bool selected = board.id == config().board;
          std::string label = (board.title.empty() ? board.id : board.title) + "  (" + board.kind + ", ends " +
                              board.endsAt.substr(0, 10) + ")";
          if (ImGui::RadioButton(label.c_str(), selected) && !selected) {
            g_selectedTile = -1;
            pushCommand(Command::Kind::SelectBoard, board.id);
          }
        }
        ImGui::Separator();
        if (ImGui::Button("Refresh")) pushCommand(Command::Kind::RefreshBoards);
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "The panel shows the board you pick here.");
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Connection")) {
      if (state.linking) {
        ImGui::TextWrapped("Type this code at %s", state.verifyUrl.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, kMine);
        ImGui::TextUnformatted(state.userCode.c_str());
        ImGui::PopStyleColor();
        ImGui::TextColored(kMuted, "The browser should already be open on that page.");
        if (ImGui::Button("Cancel")) pushCommand(Command::Kind::CancelLink);
      } else if (state.linked && state.askSharing) {
        ImGui::TextColored(kOpen, "This machine is connected.");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Show the map you are on as being played, on the remaining list? Only the map's id is sent, and you can "
            "turn it off at any time.");
        if (ImGui::Button("Yes, share what I am playing")) {
          config().shareWhatIAmPlaying = true;
          config().save();
          shared().write([](State& s) { s.askSharing = false; });
          pushCommand(Command::Kind::ReportNow);
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now")) {
          shared().write([](State& s) { s.askSharing = false; });
        }
      } else if (state.linked) {
        ImGui::TextColored(kOpen, "This machine is connected.");
        ImGui::TextWrapped("It speaks for the account you approved it with. Disconnect here or on the website.");
        if (ImGui::Button("Disconnect")) pushCommand(Command::Kind::Disconnect);
      } else {
        ImGui::TextWrapped(
            "Connecting shows a code here and opens the website, where you approve it while signed in with Discord.");
        if (ImGui::Button("Connect")) pushCommand(Command::Kind::Connect);
      }
      if (!state.linkError.empty()) {
        ImGui::Separator();
        ImGui::TextColored(kWarn, "%s", state.linkError.c_str());
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Settings")) {
      bool share = config().shareWhatIAmPlaying;
      if (ImGui::Checkbox("Share what I am playing", &share)) {
        config().shareWhatIAmPlaying = share;
        config().save();
        // Switching it off is not only about future requests: whatever is
        // standing now should go too.
        pushCommand(share ? Command::Kind::ReportNow : Command::Kind::ReleaseAll);
      }
      ImGui::TextColored(kMuted,
                         "Sends the map's UID and nothing else, and marks it as\n"
                         "being played on the remaining list for two hours.");

      ImGui::Separator();
      bool overlayOn = config().overlay;
      if (ImGui::Checkbox("Show the panel while driving", &overlayOn)) {
        config().overlay = overlayOn;
        config().save();
      }

      ImGui::TextColored(kMuted, "The panel takes the mouse:");
      const int input = config().panelInput;
      if (ImGui::RadioButton("in the menus", input == 0)) {
        config().panelInput = 0;
        config().save();
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("only with this window", input == 1)) {
        config().panelInput = 1;
        config().save();
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("always", input == 2)) {
        config().panelInput = 2;
        config().save();
      }
      ImGui::Separator();
      if (ImGui::Button("Reset its position and size")) {
        config().overlayX = -1.0f;
        config().overlayY = -1.0f;
        config().overlayW = 0.0f;
        config().overlayH = 0.0f;
        config().save();
        // Takes effect on the next start, because the window owns its position
        // for the life of the session - said plainly rather than left puzzling.
        shared().write([](State& s) {
          s.toast = "Position cleared - it moves back next time the game starts.";
          s.toastUntil = nowSeconds() + 8.0;
        });
      }

      float scale = config().overlayScale;
      if (ImGui::SliderFloat("Size", &scale, 0.7f, 1.8f, "%.2f")) config().overlayScale = scale;
      float alpha = config().overlayAlpha;
      if (ImGui::SliderFloat("Opacity", &alpha, 0.2f, 1.0f, "%.2f")) config().overlayAlpha = alpha;
      if (ImGui::IsItemDeactivatedAfterEdit()) config().save();

      ImGui::Separator();
      ImGui::TextColored(kMuted, "F9 opens and closes this window.");
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Status")) {
      ImGui::Text("Game build: %s", state.buildKey.c_str());
      ImGui::Text("Offsets: %s", state.attached ? state.profile.c_str() : "not recognised");
      ImGui::Text("Variant: %s", state.variant.empty() ? "unknown" : state.variant.c_str());
      ImGui::Text("Map UID: %s", state.uid.empty() ? "-" : state.uid.c_str());
      ImGui::Separator();
      ImGui::Text("Last: %s", state.lastCall.empty() ? "-" : state.lastCall.c_str());
      if (!state.lastError.empty()) ImGui::TextColored(kWarn, "Error: %s", state.lastError.c_str());
      ImGui::Separator();
      ImGui::TextWrapped(
          "The mod never awards a finish. Credit comes from the first replay uploaded to TMX, the same as it always "
          "has.");
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::End();
}

}  // namespace

bool capturingInput() { return g_ready && g_uiOpen; }

void draw(IDirect3DDevice9* device) {
  if (!g_ready) setup(device);
  if (!g_ready) return;
  g_device = device;

  // The toggle is read here rather than in the window procedure so it works
  // even when the game has swallowed the key.
  const bool down = (GetAsyncKeyState(config().toggleKey) & 0x8000) != 0;
  if (down && !g_toggleHeld) {
    g_uiOpen = !g_uiOpen;
    log::line("toggle key: window %s", g_uiOpen ? "open" : "closed");
  }
  g_toggleHeld = down;

  State state = shared().read();
  g_inRace = state.inRace;

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  if (config().overlay || g_uiOpen) drawPanel(state);
  if (g_uiOpen) drawSettings(state);

  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void invalidate() {
  // The map thumbnails are this device's, too: keeping them across a reset is
  // how an alt-tab turns into a crash.
  textures::releaseAll();
  if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void shutdown() {
  if (!g_ready) return;
  g_ready = false;
  textures::releaseAll();
  if (g_window && g_originalWndProc) {
    SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    g_originalWndProc = nullptr;
  }
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
}

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  // The one reliable notice that the game is going away. DllMain's detach is
  // too late and too dangerous for a network call - it runs under the loader
  // lock - so the mark is dropped here, while there is still a normal thread
  // and a normal message loop.
  if (message == WM_CLOSE || message == WM_DESTROY) {
    static bool released = false;
    if (!released) {
      released = true;
      worker::releaseNow();
    }
  }

  // Input is handed to ImGui on every message, not only while the settings
  // window is open. That gate was the whole bug: the panel was allowed to take
  // the mouse in the menus, but ImGui never heard a click, so the buttons sat
  // there doing nothing until F9 was pressed.
  if (g_ready) {
    ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);

    // Swallowing is the part that stays conditional. Only what the panel is
    // actually using, and only while it may take input at all - a driving input
    // must always reach the game, or somebody loses a run to a keypress.
    if (interactive()) {
      ImGuiIO& io = ImGui::GetIO();
      if ((io.WantCaptureMouse && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
          (io.WantCaptureKeyboard && message >= WM_KEYFIRST && message <= WM_KEYLAST)) {
        return 1;
      }
    }
  }
  return CallWindowProcW(g_originalWndProc, window, message, wparam, lparam);
}

}  // namespace overlay
}  // namespace tmx
