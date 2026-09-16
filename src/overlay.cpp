#include "overlay.h"

#include <d3d9.h>

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmx {
namespace overlay {
namespace {

bool g_ready = false;
bool g_uiOpen = false;
bool g_toggleHeld = false;
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
int g_selectedTile = -1;

const ImVec4 kOpen(0.36f, 0.78f, 0.44f, 1.0f);
const ImVec4 kDone(0.72f, 0.72f, 0.75f, 1.0f);
const ImVec4 kMine(1.00f, 0.78f, 0.24f, 1.0f);
const ImVec4 kWarn(0.95f, 0.55f, 0.35f, 1.0f);
const ImVec4 kMuted(0.62f, 0.64f, 0.70f, 1.0f);

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
      ImGui::TextColored(kOpen, "You finished in %s", timeString(state.raceTimeMs).c_str());
      ImGui::TextWrapped("Upload the replay to TMX, then press \"I uploaded it\".");
    }
    if (g_uiOpen && ImGui::Button(("I uploaded it##" + hit.boardId).c_str())) {
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

  const float cell = 26.0f * config().overlayScale;
  const int size = board.size > 0 ? board.size : 5;

  for (const Tile& tile : board.tiles) {
    if (tile.idx % size != 0) ImGui::SameLine();

    ImVec4 colour = tile.held ? (tile.mine ? kMine : kDone) : kOpen;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colour.x * 0.35f, colour.y * 0.35f, colour.z * 0.35f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, colour);

    char label[32];
    sprintf_s(label, sizeof(label), "%d##tile%d", tile.idx + 1, tile.idx);
    // The tile the player is standing on gets a border rather than a colour of
    // its own: colour already means "who holds this".
    const bool here = !state.map.uid.empty() && tile.trackId == state.map.trackId && tile.site == state.map.site;
    if (here) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    if (ImGui::Button(label, ImVec2(cell, cell)) && g_uiOpen) g_selectedTile = tile.idx;
    if (here) ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
      if (tile.held) {
        ImGui::TextColored(kMuted, "%s - %s", tile.mine ? "you" : tile.holderName.c_str(),
                           timeString(tile.holderTime).c_str());
      } else {
        ImGui::TextColored(kMuted, "unclaimed%s", tile.hasRecord ? ", map already has a replay" : "");
      }
      ImGui::EndTooltip();
    }
  }

  if (g_selectedTile >= 0) {
    for (const Tile& tile : board.tiles) {
      if (tile.idx != g_selectedTile) continue;
      ImGui::Separator();
      ImGui::TextUnformatted(tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
      ImGui::TextColored(kMuted, "%s #%d%s", tile.exchange.c_str(), tile.trackId,
                         tile.hasRecord ? " - already has a replay" : " - never finished");
      if (tile.held) {
        ImGui::TextColored(tile.mine ? kMine : kDone, "%s holds it at %s",
                           tile.mine ? "you" : tile.holderName.c_str(), timeString(tile.holderTime).c_str());
      }
      if (g_uiOpen) {
        if (ImGui::Button("Play this map")) pushCommand(Command::Kind::Play, tile.playUrl);
        ImGui::SameLine();
        if (ImGui::Button("I uploaded it")) pushCommand(Command::Kind::Check, board.id, tile.idx);
      }
      break;
    }
  }
}

void drawPanel(const State& state) {
  ImGuiIO& io = ImGui::GetIO();
  const float margin = 16.0f;
  const float width = 260.0f * config().overlayScale;
  ImVec2 position(config().overlayCorner == 0 ? margin : io.DisplaySize.x - width - margin, margin + 80.0f);

  ImGui::SetNextWindowPos(position, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(config().overlayAlpha);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav;
  // Click-through until the panel is opened: while driving it is a readout, and
  // a window that eats the mouse in a racing game is a bug, not a feature.
  if (!g_uiOpen) flags |= ImGuiWindowFlags_NoInputs;

  if (ImGui::Begin("##tmx-panel", nullptr, flags)) {
    ImGui::TextColored(kMine, "100%% TMX");
    ImGui::Separator();
    drawMapBlock(state);
    ImGui::Separator();
    drawBoard(state);

    if (!state.toast.empty()) {
      ImGui::Separator();
      ImGui::TextWrapped("%s", state.toast.c_str());
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

      int corner = config().overlayCorner;
      if (ImGui::RadioButton("Left", corner == 0)) {
        config().overlayCorner = 0;
        config().save();
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Right", corner == 1)) {
        config().overlayCorner = 1;
        config().save();
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

  // The toggle is read here rather than in the window procedure so it works
  // even when the game has swallowed the key.
  const bool down = (GetAsyncKeyState(config().toggleKey) & 0x8000) != 0;
  if (down && !g_toggleHeld) {
    g_uiOpen = !g_uiOpen;
    log::line("toggle key: window %s", g_uiOpen ? "open" : "closed");
  }
  g_toggleHeld = down;

  State state = shared().read();

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
  if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void shutdown() {
  if (!g_ready) return;
  g_ready = false;
  if (g_window && g_originalWndProc) {
    SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    g_originalWndProc = nullptr;
  }
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
}

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (g_ready && g_uiOpen) {
    ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    ImGuiIO& io = ImGui::GetIO();
    // Only swallow what the panel is actually using. A driving input must reach
    // the game even with the window open, or somebody loses a run to a keypress.
    if ((io.WantCaptureMouse && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (io.WantCaptureKeyboard && message >= WM_KEYFIRST && message <= WM_KEYLAST)) {
      return 1;
    }
  }
  return CallWindowProcW(g_originalWndProc, window, message, wparam, lparam);
}

}  // namespace overlay
}  // namespace tmx
