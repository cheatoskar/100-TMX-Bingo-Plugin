// The overlay without the game: draws the real panel and menu over a plain
// background, with made-up but realistic state, and saves a screenshot.
//
//   tmx_preview <scene> <out.png> [WxH] [click X Y]...
//
// Scenes: panel, panel-compact, panel-pairing, panel-menus, playing, bingo,
// connection, settings, status, about. `click X Y` (repeatable) clicks there
// first, `1920x1080` sets the size. Map pictures are fetched from TMX like in the game,
// so it needs the network; it waits for them before taking the picture.
//
// It exists because the overlay can otherwise only be looked at inside
// TrackMania, which makes every layout change a game restart.
#include <windows.h>
#include <d3d9.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "config.h"
#include "overlay.h"
#include "state.h"
#include "textures.h"

#pragma comment(lib, "d3d9.lib")

namespace tmx::bridge {
extern bool g_previewPairing;
}

namespace {

using namespace tmx;

struct Sample {
  int id;
  const char* name;
};
// Real maps, so the pictures are real: the most awarded on TMNF-X.
const Sample kMaps[] = {
    {924307, "[PF] Ph/\\ntom Fake"}, {67906, "[PF] Planing Fury"},   {11817, "Somewhere I belong"},
    {2233, "A01-Race"},               {426786, "No Sense !"},         {414041, "ESL-Hockolicious"},
    {599660, "End of Time"},          {257802, "[PF] Pure Feelings"}, {1807326, "[PL] Paradise Lost"},
    {1956720, "[PF] Rupture"},        {5269, "Leave it behind"},      {93481, "Smooth Life"},
    {2814, "Little Blue Mailbox"},    {2302895, "Tears of the past"}, {1617251, "[PR] Revolution"},
    {5282133, "[PF] Principium finis"}, {1134826, "[PN] ParaNormal"}, {3369586, "[PF] Paced Fever"},
    {4894563, "Chicken Invaders mtc"}, {175621, "Light Force"},       {3131260, "[Px] World (BMP)"},
    {2167465, "Avatar"},              {237282, "[PF] Tensai Racer 3"}, {64160, "Standortfaktor"},
    {6634013, "[PL] Physical Laws"},
};

std::string trackUrl(int id) { return "https://tmnf.exchange/trackshow/" + std::to_string(id); }

unsigned int rgb(unsigned r, unsigned g, unsigned b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

void fillState(const std::string& scene) {
  shared().write([&](State& s) {
    s.attached = true;
    s.sawMap = true;
    s.buildKey = "4A6B1C2D-00A3F000";
    s.profile = "tmnf-2.11.26";
    s.variant = "tmnf";
    s.linked = true;
    s.inRace = scene != "panel-menus";
    s.uid = "x6WkQxUOAUlnF3lmS2Hd8wXt7Wi";
    s.mapName = kMaps[8].name;
    s.raceState = 2;
    s.raceStateTrusted = true;
    s.raceTimeMs = 43217;
    s.finishProved = true;
    s.calib.haveChain = true;
    s.calib.chain = "0x1F4/0x28/0x2BC";
    s.calib.reading = 43217;

    MapStatus& m = s.map;
    m.known = true;
    m.onExchange = true;
    m.uid = s.uid;
    m.site = "tmnf";
    m.exchange = "TMNF-X";
    m.trackId = kMaps[8].id;
    m.name = kMaps[8].name;
    m.url = trackUrl(m.trackId);
    m.imageUrl = m.url + "/image/0";
    m.uploadUrl = "https://tmnf.exchange/replayupload/" + std::to_string(m.trackId);
    m.open = 1;
    m.excluded = 0;
    m.score = 412;
    m.claimed = true;

    s.alsoHere = {{"Kestrel", ""}};

    BoardHit hit;
    hit.boardId = "b1";
    hit.title = "Friday night 5x5";
    hit.idx = 8;
    hit.held = true;
    hit.holderName = "Brumm";
    hit.holderTime = 45020;
    hit.holderColor = rgb(96, 165, 250);
    hit.selfReported = true;
    s.hits = {hit};

    s.boards = {{"b1", "private", "Friday night 5x5", 5, "2026-10-20T20:00:00Z", 0, 0},
                {"b2", "weekly", "Weekly card", 10, "2026-10-19T00:00:00Z", 0, 0},
                {"b3", "private", "Red vs Blue", 4, "2026-10-25T18:00:00Z", 2, 1}};
    s.selectedBoard = "b1";

    BoardView& b = s.board;
    b.id = "b1";
    b.title = "Friday night 5x5";
    b.kind = "private";
    b.size = 5;
    b.verify = "trust";
    b.loaded = true;
    const unsigned colours[] = {rgb(250, 204, 21), rgb(96, 165, 250), rgb(244, 114, 182), rgb(74, 222, 128)};
    const char* holders[] = {"you", "Brumm", "Kestrel", "Ayla"};
    for (int i = 0; i < 25; i++) {
      Tile t;
      t.idx = i;
      t.site = "tmnf";
      t.exchange = "TMNF-X";
      t.trackId = kMaps[i].id;
      t.name = kMaps[i].name;
      t.url = trackUrl(t.trackId);
      t.imageUrl = t.url + "/image/0";
      t.playUrl = "https://tmnf.exchange/trackplay/" + std::to_string(t.trackId);
      const int who = (i * 7) % 9;
      if (who < 4 || i / 5 == 1) {
        const int h = i / 5 == 1 ? 0 : who;
        t.held = true;
        t.holderName = holders[h];
        t.mine = h == 0;
        t.holderColor = colours[h];
        t.holderTime = 30000 + i * 1371;
      }
      b.tiles.push_back(t);
    }
    // The map you are on is tile 9; and row 2 is all yours.
    b.tiles[8].held = true;
    b.tiles[8].mine = false;
    b.tiles[8].holderName = "Brumm";
    b.tiles[8].holderColor = colours[1];
    b.tiles[8].holderTime = 45020;
    b.lines = {{"row", 1, colours[0], true}};
    b.ladder = {{"you", colours[0], 7, 1, 12, true, 0},
                {"Brumm", colours[1], 5, 0, 5, false, 0},
                {"Kestrel", colours[2], 4, 0, 4, false, 0},
                {"Ayla", colours[3], 2, 0, 2, false, 0}};

    s.toast = "Replay handed to the browser.";
    s.toastUntil = 1e12;
    s.bingoToast = "Tile 3 taken by Kestrel";
    s.bingoToastUntil = 1e12;
    if (scene == "connection") {
      s.linked = false;
      s.linking = true;
      s.userCode = "KQ7-4XM";
      s.verifyUrl = "https://100tmx.com/link";
    }
  });
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scene = argc > 1 ? argv[1] : "panel";
  const std::string out = argc > 2 ? argv[2] : "preview.png";
  int width = 1600;
  int height = 900;
  std::vector<POINT> clicks;
  for (int i = 3; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "click" && i + 2 < argc) {
      clicks.push_back({atoi(argv[i + 1]), atoi(argv[i + 2])});
      i += 2;
    } else if (arg.find('x') != std::string::npos) {
      sscanf_s(arg.c_str(), "%dx%d", &width, &height);
    }
  }

  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"tmx-preview";
  RegisterClassW(&wc);
  RECT rect{0, 0, width, height};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowW(wc.lpszClassName, L"100TMX preview", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left,
                              rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = width;
  pp.BackBufferHeight = height;
  pp.hDeviceWindow = window;
  IDirect3DDevice9* device = nullptr;
  const HRESULT hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                             D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)
                         : E_FAIL;
  if (FAILED(hr)) {
    // Most often a game holding the screen in exclusive fullscreen - TrackMania
    // itself, say. D3D9 hands out no second device until it lets go.
    printf("no D3D9 device (0x%08lX) - is a fullscreen game running?\n", static_cast<unsigned long>(hr));
    return 1;
  }

  fillState(scene);
  bridge::g_previewPairing = scene == "panel-pairing";
  config().overlay = true;
  config().panelImages = scene != "panel-compact";
  static const char* const kPages[] = {"playing", "bingo", "connection", "settings", "status", "about"};
  bool menu = false;
  for (int i = 0; i < 6; i++) {
    if (scene == kPages[i]) {
      config().menuPage = i;
      menu = true;
    }
  }
  overlay::setMenuOpen(menu);

  // Frames until every picture on screen has arrived, or ten seconds.
  const auto started = std::chrono::steady_clock::now();
  for (int frame = 0;; frame++) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(58, 74, 92), 1.0f, 0);
    device->BeginScene();
    overlay::draw(device);
    device->EndScene();
    device->Present(nullptr, nullptr, nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    bool pending = false;
    const State view = shared().read();
    for (const Tile& t : view.board.tiles) pending |= textures::status(t.imageUrl) == textures::Status::Loading;
    pending |= textures::status(view.map.imageUrl) == textures::Status::Loading;
    if ((frame > 30 && !pending && seconds > 1.5) || seconds > 10.0) break;
  }

  // Clicks, sent through the window procedure the overlay subclassed, the way
  // the game's own window would deliver them; then the pointer is moved off
  // so no hover is left in the picture.
  // Without pumping messages while clicking: the window is hidden, so Windows
  // answers the first move with WM_MOUSELEAVE, which would take the pointer
  // away again before the click lands.
  auto frame = [&](bool pump) {
    MSG msg;
    while (pump && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    device->BeginScene();
    overlay::draw(device);
    device->EndScene();
    device->Present(nullptr, nullptr, nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  };
  for (const POINT& at : clicks) {
    const LPARAM where = MAKELPARAM(at.x, at.y);
    SendMessageW(window, WM_MOUSEMOVE, 0, where);
    frame(false);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, where);
    frame(false);
    SendMessageW(window, WM_LBUTTONUP, 0, where);
    frame(false);
  }
  if (!clicks.empty()) {
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(width - 2, height - 2));
    for (int i = 0; i < 20; i++) frame(true);
  }

  IDirect3DSurface9* back = nullptr;
  IDirect3DSurface9* copy = nullptr;
  device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
  device->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr);
  // The back buffer after Present is undefined with DISCARD, so draw once more
  // and read it before presenting.
  device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(58, 74, 92), 1.0f, 0);
  device->BeginScene();
  overlay::draw(device);
  device->EndScene();
  device->GetRenderTargetData(back, copy);
  D3DLOCKED_RECT locked{};
  std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
  if (SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
    for (int y = 0; y < height; y++) {
      const unsigned char* row = static_cast<const unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
      for (int x = 0; x < width; x++) {
        unsigned char* px = &rgba[(static_cast<size_t>(y) * width + x) * 4];
        px[0] = row[x * 4 + 2];
        px[1] = row[x * 4 + 1];
        px[2] = row[x * 4 + 0];
        px[3] = 255;
      }
    }
    copy->UnlockRect();
  }
  stbi_write_png(out.c_str(), width, height, 4, rgba.data(), width * 4);
  printf("wrote %s\n", out.c_str());

  overlay::shutdown();
  copy->Release();
  back->Release();
  device->Release();
  d3d->Release();
  DestroyWindow(window);
  // The fetch threads may still be inside a request; the process ending is
  // what stops them.
  ExitProcess(0);
}
