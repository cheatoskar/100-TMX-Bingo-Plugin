// What you see while you drive, and the F9 menu.
//
// Two windows with one look, the look of the TM Vibrant Shaders menu: Segoe UI
// scaled with the screen, rounded panels with a coloured bar on their title,
// switches instead of checkboxes, and every map shown as its picture. The
// panel is the readout you drive with; the menu is everything else, with a
// navigation bar down its side. The menu opens on the side of the screen the
// panel is not on, so the two never sit on top of each other.
#include "overlay.h"

#include <d3d9.h>
#include <shellapi.h>

#include <shlobj.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bridge.h"
#include "config.h"
#include "tmx_version.h"
#include "game.h"
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
// Whether a map is loaded, copied out of the last frame's state.
bool g_inRace = false;
// -1 unknown, 0 before the start, 1 running, 2 finished. Unknown means this
// TrackMania build's offsets did not resolve - see `interactive()`.
int g_raceState = -1;
bool g_raceStateTrusted = false;
bool g_raceClockMoving = false;
bool g_raceDriving = false;
double g_readAt = 0;
WNDPROC g_originalWndProc = nullptr;

// The tile picked on the panel and the one picked in the menu are kept apart:
// clicking a tile in one window must not open something in the other.
int g_panelTile = -1;
int g_menuTile = -1;
// Which board those picks belong to, so a pick never survives a board switch.
std::string g_tileBoard;

// The panel is placed by us once at start, after a reset, and when the screen
// size changes; in between it is wherever it was dragged.
bool g_placePanel = true;
// Where the panel is, so the menu can open on the other side of the screen.
float g_panelCentreX = 0.0f;

// Rebinding the menu key: waiting for the next key press.
bool g_capturingKey = false;
bool g_keyHeldAtStart[256] = {};

// What a press of the mouse or a key swallowed on the way down, so its release
// is swallowed too - and only then. A release the game never saw the press of
// is harmless; a press it saw whose release is eaten is a key stuck down.
bool g_swallowedButton[5] = {};
bool g_swallowedKey[256] = {};

float g_scale = 1.0f;          // 1 at 1080p, more on bigger screens
ImFont* g_fontTitle = nullptr;  // headings, map names
ImFont* g_fontBig = nullptr;    // the link code, a finish time

enum Page { kPlaying, kBingo, kConnection, kSettings, kStatus, kAbout, kPageCount };

// ------------------------------------------------------------------ colours
//
// The site's red is the menu's accent - it is what 100tmx.com looks like, and
// it keeps this menu apart from the shaders' orange one at a glance. Switches
// that are on are green, not red: a red switch reads as a warning.
const ImVec4 kAccent(0.85f, 0.16f, 0.16f, 1.0f);   // #d92828
const ImVec4 kAccentHi(1.00f, 0.27f, 0.27f, 1.0f);  // #ff4545
const ImVec4 kOpen(0.36f, 0.78f, 0.44f, 1.0f);
// An unclaimed tile is neutral, not good news - green on twenty-five of them
// drowned out the one colour that means something: a player's.
const ImVec4 kFree(0.55f, 0.57f, 0.62f, 1.0f);
// Red for a tile somebody else has taken, gold for your own: "taken" and
// "taken by me" are not the same news.
const ImVec4 kTaken(0.91f, 0.35f, 0.35f, 1.0f);
const ImVec4 kDone(0.72f, 0.72f, 0.75f, 1.0f);
const ImVec4 kMine(1.00f, 0.78f, 0.24f, 1.0f);
const ImVec4 kWarn(0.95f, 0.55f, 0.35f, 1.0f);
const ImVec4 kMuted(0.62f, 0.64f, 0.70f, 1.0f);
const ImVec4 kInfo(0.38f, 0.58f, 0.90f, 1.0f);

// Each panel has its own muted colour on its title bar, so the menu can be
// found your way around by colour as well as by name.
ImVec4 sectionColour(const char* title) {
  struct Entry {
    const char* title;
    ImVec4 colour;
  };
  static const Entry kColours[] = {
      {"This map", ImVec4(0.86f, 0.30f, 0.30f, 1.0f)},     {"Your run", ImVec4(0.36f, 0.78f, 0.44f, 1.0f)},
      {"Boards", ImVec4(0.88f, 0.76f, 0.36f, 1.0f)},       {"Board", ImVec4(0.88f, 0.76f, 0.36f, 1.0f)},
      {"Tile", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},         {"Standing", ImVec4(0.64f, 0.50f, 0.88f, 1.0f)},
      {"This machine", ImVec4(0.86f, 0.30f, 0.30f, 1.0f)}, {"Sharing", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},
      {"Bingo", ImVec4(0.88f, 0.76f, 0.36f, 1.0f)},        {"Privacy", ImVec4(0.30f, 0.72f, 0.68f, 1.0f)},
      {"Replay upload", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)}, {"Panel", ImVec4(0.64f, 0.50f, 0.88f, 1.0f)},
      {"Keys", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},         {"Versions", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},
      {"Race clock", ImVec4(0.36f, 0.78f, 0.44f, 1.0f)},   {"Last request", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},
      {"Files", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},        {"What this does", ImVec4(0.86f, 0.30f, 0.30f, 1.0f)},
      {"Links", ImVec4(0.38f, 0.58f, 0.90f, 1.0f)},        {"Credits", ImVec4(0.58f, 0.62f, 0.70f, 1.0f)},
  };
  for (const Entry& e : kColours) {
    if (!strcmp(e.title, title)) return e.colour;
  }
  return kAccent;
}

// ------------------------------------------------------------------ helpers

// The same clock the worker ages toasts against, so a message set here is
// cleared there on schedule instead of sticking to the panel forever.
double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * A time as a person types it, in milliseconds - or -1 when it is not one.
 *
 * "1:23.45", "83.45", "83,45" and "83" (whole seconds) all work, because the
 * same board is played by people who write times three different ways and a
 * form that rejects two of them reads as broken.
 */
int parseTimeMs(const char* text) {
  std::string t;
  for (const char* c = text; *c; ++c) {
    if (*c == ' ') continue;
    t += (*c == ',') ? '.' : *c;
  }
  if (t.empty()) return -1;
  int minutes = 0;
  const size_t colon = t.find(':');
  if (colon != std::string::npos) {
    const std::string m = t.substr(0, colon);
    if (m.empty() || m.find_first_not_of("0123456789") != std::string::npos) return -1;
    minutes = atoi(m.c_str());
    t = t.substr(colon + 1);
  }
  if (t.empty() || t.find_first_not_of("0123456789.") != std::string::npos) return -1;
  if (t.find('.') != t.rfind('.')) return -1;
  const double seconds = atof(t.c_str());
  if (colon != std::string::npos && seconds >= 60.0) return -1;
  const double ms = (minutes * 60.0 + seconds) * 1000.0;
  if (ms <= 0 || ms > 24.0 * 3600.0 * 1000.0) return -1;
  return static_cast<int>(ms + 0.5);
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

ImU32 col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

ImVec4 fromU32(unsigned int packed, const ImVec4& fallback) {
  return packed ? ImGui::ColorConvertU32ToFloat4(packed) : fallback;
}

ImTextureID texId(void* texture) { return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)); }

void pushCommand(Command::Kind kind, const std::string& text = "", int number = 0, int time = 0) {
  Command command;
  command.kind = kind;
  command.text = text;
  command.number = number;
  command.time = time;
  shared().push(command);
}

void toastHere(const std::string& text, double seconds = 8.0) {
  shared().write([&](State& s) {
    s.toast = text;
    s.toastUntil = nowSeconds() + seconds;
  });
}

/**
 * Which dereference the walk to the local player died on.
 *
 * Reported in plain words because the person reading it is the person who can
 * do something about it: a build where it stops at the race pointer needs a
 * different `race` offset, one that stops at the sub-object needs a different
 * `player_sub`, and both go in `[offsets]` in config.ini without waiting for a
 * release.
 */
const char* raceStepName(int step) {
  switch (step) {
    case 1: return "no race object";
    case 2: return "no player info";
    case 3: return "no player";
    case 4: return "no player sub-object";
    case 5: return "state field unreadable";
    default: return "unknown";
  }
}

// Whether the panel is taking the mouse this frame.
//
// The default is automatic: the panel is clickable unless a run is actually
// under way. It still cannot eat a click mid-race, which is the reason it was
// click-through in the first place - but everywhere else it answers the mouse.
//
// It used to test `inRace`, which is not "a run is under way" at all: it means
// *a map is loaded*, and a map is still loaded on the results screen, at the
// start line and in the pause menu. So the panel went dead at the exact moment
// its buttons matter - you cross the line, reach for "take the tile", and
// nothing responds.
//
// Not the Windows cursor either: TrackMania draws its own in the menus and
// keeps the system one hidden, so CURSOR_SHOWING is false where a pointer is
// plainly on screen.
bool interactive() {
  switch (config().panelInput) {
    case 1: return g_uiOpen;
    case 2: return true;
    default: break;
  }
  if (g_uiOpen) return true;
  // 0 BeforeStart, 1 Running, 2 Finished. Only the middle one is a run - and
  // only a state we actually trust may lock the panel. Where the state is
  // inferred from a clock the mod went looking for, being wrong means the
  // panel refuses the mouse forever, which is how "I have to press F9 after
  // every finish" happens.
  //
  // And only while somebody is actually driving: the clock ticking *and* the
  // car moving. The game keeps race_state at "running" through a pause, and
  // its clock runs from the end of the countdown whether the car moves or
  // not - so a pause, or alt-tabbing away from a loaded map, used to lock the
  // panel until F9. See Snapshot::driving.
  //
  // And only from a fresh reading. The game reader publishes twenty times a
  // second; a reading more than a second old says nothing about now.
  const bool fresh = g_readAt > 0 && nowSeconds() - g_readAt < 1.0;
  if (fresh && g_raceState == 1 && g_raceStateTrusted && g_raceDriving) return false;
  // Unknown too. A panel nobody can press is useless, and the cost of the
  // other choice is a stray click landing on a small box in a corner.
  return true;
}

// ---------------------------------------------------------------- keys

std::string keyName(int vk) {
  UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  // The keys that share a scan code with the number pad need the extended bit
  // to be named for what they are rather than for the number pad key.
  switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
      scan |= 0x100;
      break;
    default:
      break;
  }
  wchar_t name[64] = {};
  if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) {
    char out[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
  }
  char fallback[32];
  sprintf_s(fallback, sizeof(fallback), "key 0x%02X", vk);
  return fallback;
}

/**
 * Whether TM Vibrant Shaders is loaded alongside.
 *
 * It installs either as a ModLoader mod (TMVibrantShaders.dll) or standalone
 * as the game folder's d3d9.dll - so a d3d9.dll from anywhere but Windows is
 * taken to be it.
 */
bool shadersPresent() {
  static int s_known = -1;
  if (s_known >= 0) return s_known == 1;
  bool present = GetModuleHandleW(L"TMVibrantShaders.dll") != nullptr;
  if (!present) {
    if (HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll")) {
      wchar_t path[MAX_PATH] = {};
      wchar_t windows[MAX_PATH] = {};
      if (GetModuleFileNameW(d3d9, path, MAX_PATH) && GetWindowsDirectoryW(windows, MAX_PATH)) {
        present = _wcsnicmp(path, windows, wcslen(windows)) != 0;
      }
    }
  }
  s_known = present ? 1 : 0;
  return present;
}

// What TM Vibrant Shaders does with a key, or null. Its F9 reloads every
// shader - a hitch each time this menu was opened with it.
const char* shadersUse(int vk) {
  switch (vk) {
    case VK_F6: return "photo mode";
    case VK_F7: return "effects on/off";
    case VK_F8: return "its menu";
    case VK_F9: return "reloading its shaders";
    case VK_F11: return "its frame tracer";
    case VK_F12: return "frame capture";
    default: return nullptr;
  }
}

void startKeyCapture() {
  g_capturingKey = true;
  for (int vk = 0; vk < 256; vk++) g_keyHeldAtStart[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Read while capturing: the first key that goes down becomes the menu key.
// Escape gives up. Mouse buttons, modifiers and the Windows keys are skipped -
// a menu bound to the left button would open on every click in the game.
void pollKeyCapture() {
  if (!g_capturingKey) return;
  for (int vk = 0x08; vk <= 0xFE; vk++) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!down) {
      g_keyHeldAtStart[vk] = false;
      continue;
    }
    if (g_keyHeldAtStart[vk]) continue;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN ||
        vk == VK_RWIN || vk == VK_CAPITAL) {
      continue;
    }
    g_capturingKey = false;
    if (vk == VK_ESCAPE) return;
    config().toggleKey = vk;
    config().save();
    // Held right now: without this the very press that chose it would close
    // the menu on the next frame.
    g_toggleHeld = true;
    log::line("menu key is now %s (0x%02X)", keyName(vk).c_str(), vk);
    return;
  }
}

// ------------------------------------------------- the mouse, shared with others
//
// Two overlays in one game window - this one and TM Vibrant Shaders - each
// subclass it, and whichever subclassed last sees a click first. When their
// windows overlap, a click on the one drawn on top could land on a button of
// the one underneath. So an overlay that wants the pointer says so in a
// property on the game window, holding its module handle, and the other one
// keeps its hands off while it is set. The shaders' menu is drawn last, on
// top, so it may take the property over at any time; this one only takes it
// when it is free.
const wchar_t* kMouseOwner = L"TMMods.MouseOwner";

HANDLE selfHandle() {
  static HANDLE s_self = nullptr;
  if (!s_self) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&selfHandle), &module);
    s_self = module;
  }
  return s_self;
}

bool foreignOwnsMouse(HWND window) {
  HANDLE owner = GetPropW(window, kMouseOwner);
  if (!owner || owner == selfHandle()) return false;
  // Still loaded? A mod that went away with the property set must not leave
  // this one deaf for the rest of the session.
  HMODULE module = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(owner), &module) != 0;
}

void publishMouseOwner() {
  if (!g_window) return;
  const bool want = ImGui::GetIO().WantCaptureMouse && interactive();
  HANDLE owner = GetPropW(g_window, kMouseOwner);
  if (want && !owner) {
    SetPropW(g_window, kMouseOwner, selfHandle());
  } else if (!want && owner == selfHandle()) {
    RemovePropW(g_window, kMouseOwner);
  }
}

// ------------------------------------------------------------------ widgets

// An on/off switch: a pill with a knob that slides over.
bool toggle(const char* id, bool* value) {
  const float h = ImGui::GetFrameHeight(), w = h * 1.8f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  if (clicked) *value = !*value;
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID key = ImGui::GetItemID();
  float t = storage->GetFloat(key, *value ? 1.0f : 0.0f);
  t += ((*value ? 1.0f : 0.0f) - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
  storage->SetFloat(key, t);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec4 off(0.25f, 0.26f, 0.31f, 1.0f);
  const ImVec4 track(off.x + (kOpen.x - off.x) * t, off.y + (kOpen.y - off.y) * t, off.z + (kOpen.z - off.z) * t, 1.0f);
  draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(track, ImGui::IsItemHovered() ? 1.0f : 0.85f), h * 0.5f);
  draw->AddCircleFilled(ImVec2(p.x + h * 0.5f + t * (w - h), p.y + h * 0.5f), h * 0.5f - 3.0f,
                        IM_COL32(255, 255, 255, 240));
  return clicked;
}

// A switch at the right edge with its label in front, wrapped to the room it
// leaves - a long label used to run underneath the switch - and a tooltip on
// both. True when flipped.
bool switchRow(const char* label, bool* value, const char* tooltip = nullptr) {
  ImGui::PushID(label);
  const ImGuiStyle& style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float switchX = ImGui::GetContentRegionMax().x - h * 1.8f;
  const ImVec2 start = ImGui::GetCursorPos();

  ImGui::SetCursorPos(ImVec2(switchX, start.y));
  const bool flipped = toggle("switch", value);
  if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);

  ImGui::SetCursorPos(ImVec2(start.x, start.y + style.FramePadding.y));
  ImGui::PushTextWrapPos(switchX - style.ItemSpacing.x);
  ImGui::Text("%s", label);
  ImGui::PopTextWrapPos();
  if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);

  const float bottom = std::max(ImGui::GetCursorPosY(), start.y + h + style.ItemSpacing.y);
  ImGui::SetCursorPos(ImVec2(start.x, bottom));
  ImGui::Dummy(ImVec2(0.0f, 0.0f));
  ImGui::PopID();
  return flipped;
}

// A rounded chip that is either on or off.
bool chip(const char* label, bool on, const ImVec4& tint = kAccent) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(tint.x, tint.y, tint.z, 0.85f) : ImVec4(0.16f, 0.17f, 0.21f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? tint : ImVec4(0.22f, 0.23f, 0.28f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? tint : ImVec4(0.26f, 0.27f, 0.33f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : ImGui::GetStyle().Colors[ImGuiCol_Text]);
  const bool clicked = ImGui::Button(label);
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar();
  return clicked;
}

// What a button is for, by its colour: the one thing to do here, a good thing
// to do, something that undoes something, or anything else.
enum class Tone { Normal, Primary, Good, Danger };

bool button(const char* label, Tone tone = Tone::Normal, const ImVec2& size = ImVec2(0, 0)) {
  ImVec4 base(0.20f, 0.21f, 0.26f, 1.0f);
  switch (tone) {
    case Tone::Primary: base = kAccent; break;
    case Tone::Good: base = ImVec4(0.20f, 0.55f, 0.30f, 1.0f); break;
    case Tone::Danger: base = ImVec4(0.55f, 0.16f, 0.16f, 1.0f); break;
    default: break;
  }
  const ImVec4 hover(std::min(base.x * 1.25f + 0.04f, 1.0f), std::min(base.y * 1.25f + 0.04f, 1.0f),
                     std::min(base.z * 1.25f + 0.04f, 1.0f), 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, base);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 0.85f, base.y * 0.85f, base.z * 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return clicked;
}

// Keeps the next button on this line when it fits, else starts a new line.
void sameLineIfRoom(const char* nextLabel) {
  const float need = ImGui::CalcTextSize(nextLabel, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f +
                     ImGui::GetStyle().ItemSpacing.x;
  ImGui::SameLine();
  if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
}

// A page title with a line of explanation under it.
void pageTitle(const char* title, const char* hint) {
  ImGui::PushFont(g_fontTitle);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("%s", hint);
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

void muted(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrappedV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}

// A yes/no question in the middle of the screen, opened with ImGui::OpenPopup(title).
// True when answered with `yes`. Everything that cannot be taken back asks first.
bool confirm(const char* title, const char* question, const char* yes) {
  bool answered = false;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * g_scale, 14.0f * g_scale));
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::PushTextWrapPos(380.0f * g_scale);
    ImGui::TextUnformatted(question);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (button(yes, Tone::Primary, ImVec2(130.0f * g_scale, 0.0f))) {
      answered = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Cancel", Tone::Normal, ImVec2(130.0f * g_scale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
  return answered;
}

// A pill with text, drawn at `at`. Returns its size.
ImVec2 tag(ImDrawList* draw, ImVec2 at, const char* text, ImU32 background, ImU32 foreground, float fontSize) {
  const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
  const float padX = fontSize * 0.45f, padY = fontSize * 0.14f;
  const ImVec2 end(at.x + size.x + padX * 2.0f, at.y + size.y + padY * 2.0f);
  draw->AddRectFilled(at, end, background, (end.y - at.y) * 0.5f);
  draw->AddText(ImGui::GetFont(), fontSize, ImVec2(at.x + padX, at.y + padY), foreground, text);
  return ImVec2(end.x - at.x, end.y - at.y);
}

/**
 * A group of things in a rounded box, its title in the group's colour.
 *
 * A scope rather than a callback: open it, draw, and it closes itself, so a
 * panel cannot be left half-open by an early return.
 */
class Panel {
 public:
  explicit Panel(const char* title) {
    const ImVec4 c = sectionColour(title);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.165f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, c);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(c.x, c.y, c.z, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, c);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * g_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * g_scale, 10.0f * g_scale));
    // The wheel scrolls the page, not the (never scrolling) panel.
    ImGui::BeginChild(title, ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = g_fontTitle->FontSize * 0.8f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.1f), ImVec2(p.x + 3.0f * g_scale, p.y + h * 0.95f), col(c), 2.0f);
    draw->AddText(g_fontTitle, h, ImVec2(p.x + 10.0f * g_scale, p.y), col(c), title);
    ImGui::Dummy(ImVec2(0.0f, h + 4.0f * g_scale));
  }
  ~Panel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::Dummy(ImVec2(0.0f, 6.0f * g_scale));
  }
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;
};

/** Two columns when there is room for both at `minWidth` each, one otherwise. */
class Columns {
 public:
  explicit Columns(float minWidth) {
    const float spacing = 12.0f * g_scale;
    two_ = ImGui::GetContentRegionAvail().x >= minWidth * g_scale * 2.0f + spacing;
    if (two_) {
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(spacing * 0.5f, 0.0f));
      two_ = ImGui::BeginTable("columns", 2, ImGuiTableFlags_SizingStretchSame);
      if (two_) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
      } else {
        ImGui::PopStyleVar();
      }
    }
  }
  void next() {
    if (two_) ImGui::TableSetColumnIndex(1);
  }
  ~Columns() {
    if (two_) {
      ImGui::EndTable();
      ImGui::PopStyleVar();
    }
  }
  Columns(const Columns&) = delete;
  Columns& operator=(const Columns&) = delete;

 private:
  bool two_ = false;
};

// ------------------------------------------------------------------ pictures

/**
 * A map's picture filling p..q, or a placeholder that says why there is none.
 *
 * Asked for here as well as by the worker: a picture that was evicted, or lost
 * with a device the game made anew, comes back by itself the next time it is
 * on screen instead of waiting for the next board refresh.
 */
void picture(ImDrawList* draw, const std::string& url, ImVec2 p, ImVec2 q, float rounding,
             ImDrawFlags corners = ImDrawFlags_RoundCornersAll, ImU32 tint = IM_COL32_WHITE) {
  if (void* texture = url.empty() ? nullptr : textures::get(url)) {
    draw->AddImageRounded(texId(texture), p, q, ImVec2(0, 0), ImVec2(1, 1), tint, rounding, corners);
    return;
  }
  textures::request(url);
  draw->AddRectFilled(p, q, IM_COL32(32, 34, 44, 255), rounding, corners);
  const textures::Status status = textures::status(url);
  const char* why = status == textures::Status::Failed ? "No picture"
                    : status == textures::Status::Loading ? "Loading picture..."
                                                            : nullptr;
  if (!why) return;
  const ImVec2 size = ImGui::CalcTextSize(why);
  if (size.x + 8.0f > q.x - p.x || size.y + 4.0f > q.y - p.y) return;
  draw->AddText(ImVec2(p.x + (q.x - p.x - size.x) * 0.5f, p.y + (q.y - p.y - size.y) * 0.5f),
                IM_COL32(255, 255, 255, 110), why);
}

struct Verdict {
  const char* tag;
  ImVec4 colour;
  std::string line;
};

// What the site says about a map, as a word for its corner and a line under it.
Verdict verdictOf(const MapStatus& map) {
  if (!map.onExchange) return {"NOT ON TMX", kMuted, "Not on an exchange."};
  if (map.excluded == 1) return {"EXCLUDED", kWarn, "On the exclusion list"};
  if (map.open == 1) return {"OPEN", kOpen, "Still open - worth " + std::to_string(map.score)};
  if (map.justFinished) {
    return {"JUST TAKEN", kTaken,
            "Just finished" + (map.justFinishedBy.empty() ? std::string() : " by " + map.justFinishedBy)};
  }
  if (map.open == 0) {
    return {"FINISHED", kDone, map.finishedBy.empty() ? "Already finished" : "Finished by " + map.finishedBy};
  }
  return {"NEW", kMuted, "Not in the catalogue yet"};
}

/**
 * The map you are on as a card: its picture, its name over a dark fade, what
 * the site says about it in the corner and which exchange in the other.
 */
void mapCard(const MapStatus& map, const std::string& name, float width) {
  const float fs = ImGui::GetFontSize();
  const float h = std::floor(width * 9.0f / 16.0f);
  const float rounding = 8.0f * g_scale;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 q(p.x + width, p.y + h);
  ImGui::Dummy(ImVec2(width, h));
  ImDrawList* draw = ImGui::GetWindowDrawList();

  picture(draw, map.imageUrl, p, q, rounding);
  // The fade, in two parts so its bottom keeps the picture's round corners: a
  // gradient down to a rounded strip of the same darkness.
  const float strip = rounding + 2.0f;
  const float fadeTop = p.y + h * 0.45f;
  draw->AddRectFilledMultiColor(ImVec2(p.x, fadeTop), ImVec2(q.x, q.y - strip), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200));
  draw->AddRectFilled(ImVec2(p.x, q.y - strip), q, IM_COL32(0, 0, 0, 200), rounding, ImDrawFlags_RoundCornersBottom);

  const float titleSize = fs * 1.3f;
  const float pad = fs * 0.55f;
  const ImVec4 clip(p.x + pad, p.y, q.x - pad, q.y);
  draw->AddText(g_fontTitle, titleSize, ImVec2(p.x + pad, q.y - titleSize - pad), IM_COL32_WHITE,
                name.empty() ? "(unnamed map)" : name.c_str(), nullptr, 0.0f, &clip);

  if (map.onExchange) {
    const Verdict verdict = verdictOf(map);
    tag(draw, ImVec2(p.x + pad, p.y + pad), verdict.tag, col(verdict.colour, 0.95f), IM_COL32(15, 15, 18, 255),
        fs * 0.78f);
    char where[48];
    sprintf_s(where, sizeof(where), "%s #%d", map.exchange.c_str(), map.trackId);
    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fs * 0.78f, FLT_MAX, 0.0f, where);
    tag(draw, ImVec2(q.x - pad - size.x - fs * 0.9f, p.y + pad), where, IM_COL32(0, 0, 0, 170),
        IM_COL32(235, 235, 235, 255), fs * 0.78f);
  }
}

// ------------------------------------------------------------------ bingo

/**
 * The board as a grid of map pictures. Returns the tile clicked, or -1.
 *
 * Drawn by hand rather than as ImGui buttons: an image button and a plain one
 * are not the same size once frame padding is on them, which put the stroke
 * through a completed line a few pixels off. Here every tile is exactly the
 * same square, and the centres the strokes run between are its own.
 */
int tileGrid(const State& state, const BoardView& board, float width, bool clickable, int selected) {
  const int size = std::clamp(board.size > 0 ? board.size : 5, 1, 12);
  const float gap = (width >= 300.0f * g_scale ? 5.0f : 3.0f) * g_scale;
  const float cell = std::floor((width - gap * (size - 1)) / static_cast<float>(size));
  if (cell < 8.0f) return -1;
  const float rounding = std::min(6.0f * g_scale, cell * 0.12f);
  // Past this a tile is big enough for its map to be worth more than its
  // number; below it the screenshot would be a smudge.
  const bool images = cell >= 34.0f * g_scale;
  const float fs = ImGui::GetFontSize();

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  std::vector<ImVec2> centres(static_cast<size_t>(size) * size, ImVec2(-1, -1));
  int clicked = -1;

  for (const Tile& tile : board.tiles) {
    if (tile.idx < 0 || tile.idx >= size * size) continue;
    const int row = tile.idx / size;
    const int column = tile.idx % size;
    const ImVec2 p(origin.x + column * (cell + gap), origin.y + row * (cell + gap));
    const ImVec2 q(p.x + cell, p.y + cell);
    centres[static_cast<size_t>(tile.idx)] = ImVec2((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);

    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(tile.idx);
    const bool pressed = ImGui::InvisibleButton("tile", ImVec2(cell, cell));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (pressed && clickable) clicked = tile.idx;

    // The holder's own colour, straight from the board's palette, so the grid
    // here and the grid in the browser are the same picture.
    const ImVec4 colour = tile.held ? fromU32(tile.holderColor, kTaken) : kFree;
    draw->AddRectFilled(p, q, IM_COL32(28, 30, 38, 255), rounding);
    if (images) {
      // A square crop out of the middle of the 16:9 picture.
      if (void* texture = textures::get(tile.imageUrl)) {
        draw->AddImageRounded(texId(texture), p, q, ImVec2(0.21875f, 0.0f), ImVec2(0.78125f, 1.0f), IM_COL32_WHITE,
                              rounding);
      } else {
        textures::request(tile.imageUrl);
      }
    }
    // A held tile is washed in its holder's colour, so a board of screenshots
    // still reads as a board of claims at a glance.
    if (tile.held) draw->AddRectFilled(p, q, col(colour, images ? 0.34f : 0.55f), rounding);

    // The number, small in the corner over a picture, large without one.
    char number[8];
    sprintf_s(number, sizeof(number), "%d", tile.idx + 1);
    if (images) {
      const float size2 = fs * 0.8f;
      const ImVec2 at(p.x + 3.0f * g_scale, p.y + 2.0f * g_scale);
      draw->AddText(ImGui::GetFont(), size2, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), number);
      draw->AddText(ImGui::GetFont(), size2, at, IM_COL32(255, 255, 255, 230), number);
    } else {
      const float size2 = std::min(fs, cell * 0.45f);
      const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(size2, FLT_MAX, 0.0f, number);
      draw->AddText(ImGui::GetFont(), size2, ImVec2(p.x + (cell - text.x) * 0.5f, p.y + (cell - text.y) * 0.5f),
                    tile.held ? IM_COL32_WHITE : col(kFree), number);
    }

    // The outlines: yours in gold, the tile you are standing on in white, the
    // one picked in this window in the accent. Colour already means "who holds
    // this", so "where am I" and "what did I click" are rings, not fills.
    const bool here = !state.map.uid.empty() && tile.trackId == state.map.trackId && tile.site == state.map.site;
    if (tile.held) draw->AddRect(p, q, col(tile.mine ? kMine : colour), rounding, 0, tile.mine ? 2.5f : 1.5f);
    if (here) {
      draw->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(q.x + 2, q.y + 2), IM_COL32(255, 255, 255, 240), rounding + 2.0f,
                    0, 2.5f);
    }
    if (selected == tile.idx) draw->AddRect(p, q, col(kAccentHi), rounding, 0, 3.0f);
    if (hovered) {
      draw->AddRectFilled(p, q, IM_COL32(255, 255, 255, 26), rounding);
      ImGui::BeginTooltip();
      ImGui::PushFont(g_fontTitle);
      ImGui::TextUnformatted(tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
      ImGui::PopFont();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float w = 260.0f * g_scale;
      ImGui::Dummy(ImVec2(w, w * 9.0f / 16.0f));
      picture(ImGui::GetWindowDrawList(), tile.imageUrl, at, ImVec2(at.x + w, at.y + w * 9.0f / 16.0f), 6.0f * g_scale);
      if (tile.held) {
        // "you", "your side" and "them" are three different answers, and on a
        // team board only the middle one tells you not to bother.
        const char* who = tile.mine ? "you" : tile.ourTeam ? "your team" : tile.holderName.c_str();
        ImGui::TextColored(colour, "%s - %s", who, timeString(tile.holderTime).c_str());
        if (tile.ourTeam && !tile.mine && !tile.holderName.empty()) {
          ImGui::TextColored(kMuted, "(%s)", tile.holderName.c_str());
        }
      } else {
        ImGui::TextColored(kMuted, "unclaimed");
      }
      if (here) ImGui::TextColored(kOpen, "You are on this map.");
      if (clickable) ImGui::TextColored(kMuted, "Click for its map and buttons.");
      ImGui::EndTooltip();
    }
  }

  // A completed line, struck through the way the website strikes it. Which
  // lines exist is the site's answer, not ours - see BoardLine.
  const int last = size - 1;
  for (const BoardLine& line : board.lines) {
    int from = -1;
    int to = -1;
    if (line.kind == "row" && line.n >= 0 && line.n < size) {
      from = line.n * size;
      to = line.n * size + last;
    } else if (line.kind == "col" && line.n >= 0 && line.n < size) {
      from = line.n;
      to = last * size + line.n;
    } else if (line.kind == "diag") {
      from = 0;
      to = size * size - 1;
    } else if (line.kind == "anti") {
      from = last;
      to = last * size;
    }
    if (from < 0 || to < 0 || static_cast<size_t>(to) >= centres.size()) continue;
    const ImVec2 a = centres[static_cast<size_t>(from)];
    const ImVec2 b = centres[static_cast<size_t>(to)];
    // A tile that was never drawn has no centre, and a stroke from the corner
    // of the screen is worse than none.
    if (a.x < 0 || b.x < 0) continue;
    // A dark line underneath, because the board it crosses is a grid of
    // screenshots and a thin bright stroke can vanish into a pale one.
    draw->AddLine(a, b, IM_COL32(0, 0, 0, 150), 7.0f * g_scale);
    draw->AddLine(a, b, line.color ? line.color : IM_COL32(255, 255, 255, 235), 3.5f * g_scale);
  }

  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, size * cell + (size - 1) * gap));
  return clicked;
}

// Who is ahead, in the board's own colours. On a team board the sides are the
// standing and the players a roster, so only the sides are shown there.
void drawStanding(const BoardView& board, int maxRows) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float dot = ImGui::GetFontSize() * 0.32f;
  auto swatch = [&](unsigned int colour) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(dot * 2.0f + 4.0f, h));
    draw->AddCircleFilled(ImVec2(p.x + dot, p.y + h * 0.5f), dot, col(fromU32(colour, kMuted)));
    ImGui::SameLine();
  };

  if (!board.teams.empty()) {
    for (const TeamRow& team : board.teams) {
      // An empty side is real - somebody can still join it - but not worth a row.
      if (team.players == 0 && team.tiles == 0) continue;
      swatch(team.color);
      ImGui::TextColored(fromU32(team.color, kMuted), "%s%s", team.name.c_str(), team.mine ? " (you)" : "");
      ImGui::SameLine();
      ImGui::TextColored(kMuted, "%d pt%s - %d tile%s%s", team.points, team.points == 1 ? "" : "s", team.tiles,
                         team.tiles == 1 ? "" : "s", team.lines > 0 ? (team.lines == 1 ? " + a line" : " + lines") : "");
    }
    return;
  }
  if (board.ladder.empty()) {
    ImGui::TextColored(kMuted, "Nobody has taken a tile yet.");
    return;
  }
  int place = 0;
  for (const LadderRow& row : board.ladder) {
    place++;
    if (place > maxRows && !row.mine) continue;
    swatch(row.color);
    ImGui::TextColored(row.mine ? kMine : ImGui::GetStyle().Colors[ImGuiCol_Text], "%d. %s", place,
                       row.name.empty() ? "somebody" : row.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%d tile%s%s", row.tiles, row.tiles == 1 ? "" : "s",
                       row.lines > 0 ? (row.lines == 1 ? " + a line" : " + lines") : "");
  }
}

/**
 * What can be done with one tile: play its map, and take it the way its board
 * takes tiles. `id` keeps two copies of this - one on the panel, one in the
 * menu - from ever being the same button.
 */
void drawTileActions(const State& state, const BoardView& board, const Tile& tile, const char* id) {
  ImGui::PushID(id);
  ImGui::PushID(tile.idx);
  if (button("Play this map", Tone::Primary)) pushCommand(Command::Kind::Play, tile.playUrl);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("TrackMania downloads it from TMX and starts it.");

  if (board.selfReported()) {
    // Nothing to upload and nothing to check: the board takes a time, and the
    // only one we have is the run just finished on this very map.
    const bool onIt = !state.map.uid.empty() && tile.trackId == state.map.trackId && tile.site == state.map.site;
    const bool haveRun = onIt && state.raceState == 2 && state.raceTimeMs > 0;
    const bool wouldTake = haveRun && (!tile.held || tile.holderTime <= 0 || state.raceTimeMs < tile.holderTime);
    if (wouldTake) {
      const std::string label = "Take with " + timeString(state.raceTimeMs);
      sameLineIfRoom(label.c_str());
      if (button(label.c_str(), Tone::Good)) pushCommand(Command::Kind::Check, board.id, tile.idx, state.raceTimeMs);
    } else if (board.gameOnly()) {
      // "Plugin and replay only" means the time comes from a run the game
      // measured, or from the replay file on the website - never from a
      // button. A mark with no time on it is exactly the typed-in claim that
      // setting exists to rule out, so it is not offered.
      ImGui::TextColored(kMuted, "Finish this map to take it - the time comes from the game.");
    } else {
      // A "no check" board takes a time typed on the website, so it takes one
      // typed here too - the same self-reporting the board already
      // advertises, one step closer. No finish in hand is the usual reason to
      // be here: a run driven before the overlay was open, or on another
      // machine. Marking with no time at all is the second, quieter choice in
      // the same box, and only on a tile nobody holds.
      static char s_time[16] = "";
      static std::string s_error;
      sameLineIfRoom("Enter my time...");
      if (button("Enter my time...")) {
        s_time[0] = '\0';
        s_error.clear();
        ImGui::OpenPopup("##entertime");
      }
      ImGui::SetNextWindowSize(ImVec2(320.0f * g_scale, 0.0f));
      if (ImGui::BeginPopup("##entertime")) {
        ImGui::PushFont(g_fontTitle);
        ImGui::TextWrapped("%s", tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
        ImGui::PopFont();
        if (tile.held && tile.holderTime > 0) {
          ImGui::TextColored(kMuted, "To beat: %s", timeString(tile.holderTime).c_str());
        }
        ImGui::TextColored(kMuted, "Your time, like 1:23.45 or 83.45");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(140.0f * g_scale);
        const bool entered = ImGui::InputTextWithHint("##time", "0:00.00", s_time, sizeof(s_time),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (button("Take the tile", Tone::Good) || entered) {
          const int ms = parseTimeMs(s_time);
          if (ms < 0) {
            s_error = "That is not a time.";
          } else if (tile.held && tile.holderTime > 0 && ms >= tile.holderTime) {
            // The site would refuse it; saying so here saves the round trip.
            s_error = "Not faster than " + timeString(tile.holderTime) + ".";
          } else {
            pushCommand(Command::Kind::Check, board.id, tile.idx, ms);
            ImGui::CloseCurrentPopup();
          }
        }
        if (!s_error.empty()) ImGui::TextColored(kWarn, "%s", s_error.c_str());
        ImGui::Separator();
        if (!tile.held) {
          if (button("Mark it without a time")) {
            pushCommand(Command::Kind::Check, board.id, tile.idx, 0);
            ImGui::CloseCurrentPopup();
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Nobody holds it, so it can be held at no time - until anybody finishes the map.");
          }
          ImGui::SameLine();
        }
        if (button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }
  } else {
    sameLineIfRoom("I uploaded it");
    if (button("I uploaded it", Tone::Good)) pushCommand(Command::Kind::Check, board.id, tile.idx);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The site reads your replay on TMX and takes the tile if it is faster.");
    if (!tile.uploadUrl.empty()) {
      sameLineIfRoom("Upload page");
      if (button("Upload page")) pushCommand(Command::Kind::OpenUrl, tile.uploadUrl);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens the map's replay upload on TMX in your browser.");
    }
  }
  ImGui::PopID();
  ImGui::PopID();
}

// One tile in full: big picture, name, who holds it, and its buttons.
void drawTileDetail(const State& state, const BoardView& board, const Tile& tile, const char* id, bool canClick) {
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = std::floor(width * 9.0f / 16.0f);
  ImGui::Dummy(ImVec2(width, h));
  picture(ImGui::GetWindowDrawList(), tile.imageUrl, p, ImVec2(p.x + width, p.y + h), 8.0f * g_scale);

  ImGui::PushFont(g_fontTitle);
  ImGui::TextWrapped("%s", tile.name.empty() ? "(unnamed map)" : tile.name.c_str());
  ImGui::PopFont();
  ImGui::TextColored(kMuted, "Tile %d - %s #%d", tile.idx + 1, tile.exchange.c_str(), tile.trackId);
  if (tile.held) {
    // A tile your own side holds is not a target: taking it moves it to you
    // and wins the team nothing, which is worth saying before somebody spends
    // an evening on it.
    const ImVec4 tone = tile.mine || tile.ourTeam ? kMine : fromU32(tile.holderColor, kTaken);
    ImGui::TextColored(tone, "%s holds it at %s", tile.mine ? "You" : tile.ourTeam ? "Your team" : tile.holderName.c_str(),
                       timeString(tile.holderTime).c_str());
    if (tile.ourTeam && !tile.mine) ImGui::TextColored(kMuted, "Beating it wins your side nothing.");
  } else {
    ImGui::TextColored(kMuted, "Nobody holds it yet.");
  }
  if (canClick) {
    ImGui::Spacing();
    drawTileActions(state, board, tile, id);
  }
}

/**
 * The actions a finish on the map you are on offers, for one board it is a
 * tile of. `id` keeps the panel's copy and the menu's copy apart.
 */
void drawHit(const State& state, const BoardHit& hit, const char* id, bool canClick) {
  ImGui::PushID(id);
  ImGui::PushID(hit.boardId.c_str());
  ImGui::PushID(hit.idx);

  ImGui::TextColored(kMine, "Tile %d on %s", hit.idx + 1, hit.title.empty() ? hit.boardId.c_str() : hit.title.c_str());
  if (hit.held) {
    // On a team board the side is the thing worth naming: "Red holds it" is
    // what decides whether the next twenty minutes are worth spending.
    const ImVec4 colour = fromU32(hit.holderColor, kMuted);
    if (hit.teams > 0 && !hit.holderTeamName.empty()) {
      ImGui::TextColored(colour, "%s holds it at %s%s", hit.holderTeamName.c_str(), timeString(hit.holderTime).c_str(),
                         hit.holderTeam == hit.myTeam ? " (your side)" : "");
      if (!hit.mine && !hit.holderName.empty()) ImGui::TextColored(kMuted, "driven by %s", hit.holderName.c_str());
    } else {
      ImGui::TextColored(colour, "%s holds it at %s", hit.mine ? "you" : hit.holderName.c_str(),
                         timeString(hit.holderTime).c_str());
    }
  } else {
    ImGui::TextColored(kMuted, "nobody holds it yet");
  }

  const bool finished = state.raceState == 2 && state.raceTimeMs > 0;
  // Would this run actually take the tile? The site refuses a slower one
  // anyway, so offering it would be a button that exists to say no. An
  // untimed hold reads as 0, which any real time beats.
  const bool wouldTake = finished && (!hit.held || hit.holderTime <= 0 || state.raceTimeMs < hit.holderTime);

  if (hit.selfReported) {
    // This board checks nothing against TMX and says so on its own face, so
    // the time the game measured can go straight on it. Nowhere else.
    if (wouldTake && canClick) {
      const std::string label = "Take the tile with " + timeString(state.raceTimeMs);
      if (button(label.c_str(), Tone::Good)) pushCommand(Command::Kind::Check, hit.boardId, hit.idx, state.raceTimeMs);
      if (!config().autoSubmitSelfReported) ImGui::TextColored(kMuted, "Settings: do this automatically");
    } else if (finished && hit.held && !wouldTake) {
      ImGui::TextColored(kMuted, "Not faster than the tile.");
    } else if (!finished) {
      ImGui::TextColored(kMuted, "Finish the map to put your time on this tile.");
    }
    // Taking a tile with *no time on it* lives in one place only - the tile's
    // own detail, where it asks first. Offered here it sat a centimetre from
    // "take the tile with 13.91" and got pressed out of reflex.
  } else {
    if (finished) ImGui::TextColored(kMuted, "Upload the replay to TMX, then press \"I uploaded it\".");
    if (canClick && button("I uploaded it", Tone::Good)) pushCommand(Command::Kind::Check, hit.boardId, hit.idx);
  }

  ImGui::PopID();
  ImGui::PopID();
  ImGui::PopID();
}

// The run just finished, and the one thing worth doing straight after it:
// uploading the replay. It is what credits the map and what a bingo tile is
// checked against, and hunting the map down on the website afterwards is where
// people give up.
void drawRun(const State& state, const char* id, bool canClick) {
  if (state.raceState != 2 || state.raceTimeMs <= 0) return;
  ImGui::PushID(id);
  // Not "Finished" until something says so. The clock stops when the line is
  // crossed and when Escape is pressed, and congratulating somebody standing
  // at a checkpoint is how this feature lost its credibility the first time.
  const float top = ImGui::GetCursorPosY();
  ImGui::PushFont(g_fontBig);
  ImGui::TextColored(state.finishProved ? kOpen : kMuted, "%s", timeString(state.raceTimeMs).c_str());
  const float bigLine = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  ImGui::SameLine();
  // On the big number's baseline, not its top.
  ImGui::SetCursorPosY(top + bigLine - ImGui::GetTextLineHeight() - bigLine * 0.08f);
  ImGui::TextColored(kMuted, "%s", state.finishProved ? "finished" : "clock stopped - not confirmed as a finish");
  if (!state.map.uploadUrl.empty()) {
    if (canClick && button("Upload the replay to TMX", Tone::Primary)) {
      pushCommand(Command::Kind::OpenUrl, state.map.uploadUrl);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens this map's upload page in your browser.");
  }
  ImGui::PopID();
}

// A browser asking to be let in. At the top of both windows and not behind
// anything, because the person who needs to answer it is looking at the game,
// and a question they cannot find is a question that never gets answered.
void drawPairing(const char* id, bool canClick) {
  if (!bridge::status().pairing) return;
  ImGui::PushID(id);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(kMine.x, kMine.y, kMine.z, 0.14f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * g_scale);
  ImGui::BeginChild("pairing", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::TextColored(kMine, "A browser wants to connect");
  muted("Allow it to collect finished replays and upload them to TMX as you.");
  if (canClick) {
    if (button("Allow", Tone::Good)) bridge::approvePairing();
    // Apart, not side by side: the two answers to this question should never
    // be one slip of the mouse from each other.
    ImGui::SameLine(0.0f, 24.0f * g_scale);
    if (button("Not now")) bridge::refusePairing();
  } else {
    ImGui::TextColored(kMuted, "Press %s to answer.", keyName(config().toggleKey).c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PopID();
}

// The line that fades: the answer to the last thing the player pressed.
void drawToast(const std::string& text, double until) {
  if (text.empty()) return;
  const double left = until - nowSeconds();
  const float alpha = left <= 0 ? 0.35f : static_cast<float>(std::min(1.0, left / 1.0));
  ImGui::Spacing();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, width - 16.0f * g_scale);
  const float h = size.y + 10.0f * g_scale;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(p, ImVec2(p.x + width, p.y + h), col(ImVec4(0.18f, 0.19f, 0.24f, 1.0f), 0.95f * alpha),
                      6.0f * g_scale);
  draw->AddRectFilled(p, ImVec2(p.x + 3.0f * g_scale, p.y + h), col(kAccent, alpha), 2.0f);
  draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 10.0f * g_scale, p.y + 5.0f * g_scale),
                col(ImVec4(1, 1, 1, 1), alpha), text.c_str(), nullptr, width - 16.0f * g_scale);
  ImGui::Dummy(ImVec2(width, h));
}

// A tile index that still names a tile on this board, or -1.
const Tile* tileAt(const BoardView& board, int idx) {
  if (idx < 0) return nullptr;
  for (const Tile& tile : board.tiles) {
    if (tile.idx == idx) return &tile;
  }
  return nullptr;
}

// ------------------------------------------------------------------ the panels
//
// Two of them since 0.9.2: the map readout, and the bingo board in a window of
// its own. One panel meant a board big enough to show its screenshots pushed
// the map status off the bottom, and somebody playing for the project alone
// had a bingo-shaped panel. Each is placed and sized on its own, and the bingo
// one is simply absent without a board or a tile under the wheels.

/**
 * Where a panel goes: placed by us at start, after "put it back", and when the
 * screen size changes; after that the window owns its position, so a drag is
 * not fought by a SetNextWindowPos on the very next frame. Pulled back on
 * screen if the resolution shrank since it was saved - a panel parked off the
 * edge looks exactly like the mod being broken.
 */
void placeWindow(bool& place, float x, float y, float w, float h, ImVec2 fallbackPos, ImVec2 fallbackSize) {
  const ImGuiIO& io = ImGui::GetIO();
  if (place && io.DisplaySize.x > 0) {
    place = false;
    const float margin = 16.0f * g_scale;
    ImVec2 size = w > 80 && h > 80 ? ImVec2(w, h) : fallbackSize;
    size.x = std::min(size.x, io.DisplaySize.x - margin * 2.0f);
    size.y = std::min(size.y, io.DisplaySize.y - margin * 2.0f);
    ImVec2 position = x >= 0 && y >= 0 ? ImVec2(x, y) : fallbackPos;
    position.x = std::clamp(position.x, 0.0f, std::max(0.0f, io.DisplaySize.x - size.x));
    position.y = std::clamp(position.y, 0.0f, std::max(0.0f, io.DisplaySize.y - 60.0f * g_scale));
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f * g_scale, 100.0f * g_scale),
                                      ImVec2(1600.0f * g_scale, 2000.0f * g_scale));
  ImGui::SetNextWindowBgAlpha(config().overlayAlpha);
}

/** Remembered when a drag ends rather than every frame: this writes a file. */
void rememberWindow(float& x, float& y, float& w, float& h, bool canClick) {
  const ImVec2 position = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  if (canClick && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
      (position.x != x || position.y != y || size.x != w || size.y != h)) {
    x = position.x;
    y = position.y;
    w = size.x;
    h = size.y;
    config().save();
  }
}

// The scrollbar is always reserved, and thin. Without it a panel oscillates:
// content grows past the height, a scrollbar appears, the bar narrows the wrap
// width of every wrapped line, the text gets taller, and on the frame it is
// not needed the whole thing snaps back - which reads as the widget shaking.
//
// Click-through until it may take the mouse: while driving it is a readout,
// and a window that eats the mouse in a racing game is a bug, not a feature.
ImGuiWindowFlags panelFlags(bool canClick) {
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysVerticalScrollbar;
  if (!canClick) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
  return flags;
}

// A panel's title row: its name in its colour, a quieter line beside it, and
// on the right the connection dot and (when it takes the mouse) the way into
// the menu.
void drawPanelHeader(const State& state, const char* title, const ImVec4& colour, const std::string& subtitle,
                     bool canClick) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = fs * 1.15f;
  const float right = ImGui::GetWindowContentRegionMax().x;
  const float button = h;
  draw->AddText(g_fontTitle, h, p, col(colour), title);
  const float titleW = g_fontTitle->CalcTextSizeA(h, FLT_MAX, 0.0f, title).x;
  if (!subtitle.empty()) {
    const ImVec4 clip(p.x, p.y, p.x + right - button - fs * 1.6f, p.y + h);
    draw->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(p.x + titleW + fs * 0.4f, p.y + h - fs * 0.95f), col(kMuted),
                  subtitle.c_str(), nullptr, 0.0f, &clip);
  }
  ImGui::Dummy(ImVec2(titleW, h));

  ImGui::SameLine(right - button - fs * 1.2f);
  const ImVec2 dot = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(fs, h));
  const ImVec4 status = state.linked ? kOpen : state.linking ? kMine : kMuted;
  draw->AddCircleFilled(ImVec2(dot.x + fs * 0.5f, dot.y + h * 0.55f), fs * 0.26f, col(status));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", state.linked ? "Connected" : state.linking ? "Waiting for the code" : "Not connected");
  }
  if (canClick) {
    ImGui::SameLine(right - button);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("menu", ImVec2(button, h))) g_uiOpen = !g_uiOpen;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      draw->AddRectFilled(at, ImVec2(at.x + button, at.y + h), IM_COL32(255, 255, 255, 25), 4.0f * g_scale);
      ImGui::SetTooltip("%s the menu (%s)", g_uiOpen ? "Close" : "Open", keyName(config().toggleKey).c_str());
    }
    // Three bars: the menu.
    const ImU32 bars = hovered ? IM_COL32_WHITE : col(kMuted);
    for (int i = 0; i < 3; i++) {
      const float y = at.y + h * (0.3f + 0.2f * i);
      draw->AddLine(ImVec2(at.x + button * 0.22f, y), ImVec2(at.x + button * 0.78f, y), bars, 2.0f * g_scale);
    }
  }
  ImGui::Spacing();
}

// The map you are on, for the project: is it open, what is it worth, who else
// is on it, and the run just driven.
void drawPanelMap(const State& state, bool canClick) {
  if (!state.attached) {
    // Two different situations, and telling them apart is the whole point: in
    // the menus there is no map to check the addresses against, so calling that
    // "not recognised" accuses the build of something it has not done yet.
    if (!state.sawMap) {
      ImGui::TextColored(kMuted, "Waiting for a map.");
      muted("Load any map once and the mod confirms it can read this build.");
    } else {
      ImGui::TextColored(kWarn, "This TrackMania build is not recognised.");
      ImGui::TextColored(kMuted, "build %s", state.buildKey.c_str());
      muted("Nothing is being read or sent. Report that build id and it can be added.");
    }
    return;
  }
  if (!state.inRace) {
    ImGui::TextColored(kMuted, "In the menus.");
    return;
  }

  const MapStatus& map = state.map;
  const std::string& name = state.mapName.empty() ? map.name : state.mapName;
  if (config().panelImages && map.onExchange) {
    mapCard(map, name, ImGui::GetContentRegionAvail().x);
    ImGui::TextColored(verdictOf(map).colour, "%s", verdictOf(map).line.c_str());
  } else {
    ImGui::PushFont(g_fontTitle);
    ImGui::TextWrapped("%s", name.empty() ? "(unnamed map)" : name.c_str());
    ImGui::PopFont();
    if (!map.onExchange) {
      ImGui::TextColored(kMuted, "Not on an exchange.");
      return;
    }
    ImGui::TextColored(verdictOf(map).colour, "%s", verdictOf(map).line.c_str());
    ImGui::TextColored(kMuted, "%s #%d", map.exchange.c_str(), map.trackId);
  }
  if (!map.onExchange) return;

  if (map.claimed) {
    ImGui::TextColored(kMine, "Marked as yours on the site");
  } else if (!map.refused.empty() && map.open != 0) {
    // Not when the map is already finished: the line above has just said so.
    // The text comes from the site.
    ImGui::TextColored(kMuted, "%s", map.refused.c_str());
  } else if (!config().shareWhatIAmPlaying) {
    ImGui::TextColored(kMuted, "Sharing is off");
  }
  // Somebody else has this map marked. Not a warning - two people on one map is
  // allowed and always was - but it is the thing you would want to know before
  // spending the evening on it.
  for (const AlsoHere& other : state.alsoHere) {
    ImGui::TextColored(kWarn, "also here: %s", other.name.empty() ? "another player" : other.name.c_str());
  }

  if (state.raceState == 2 && state.raceTimeMs > 0) {
    ImGui::Spacing();
    drawRun(state, "panel-run", canClick);
  }

  // What happened to the replay after a finish, when the uploader is on.
  if (config().bridge) {
    const bridge::Status uploader = bridge::status();
    if (!uploader.lastResult.empty()) ImGui::TextColored(kMuted, "Replay: %s", uploader.lastResult.c_str());
  }
}

// The loaded map as a bingo tile: which board, who holds it, the time to beat,
// and the button that takes it. The top of the Bingo panel - while driving it
// is the one thing on it that changes.
void drawBingoHits(const State& state, bool canClick) {
  // A map is loaded but the race state never resolved: the offsets for this
  // build did not match. Everything that needs a finish is silently
  // impossible, so it is said once rather than left to look like the board
  // ignoring them.
  if (state.inRace && state.raceState < 0) {
    ImGui::TextColored(kWarn, "Cannot read this build's race state (%s)", raceStepName(state.raceStep));
    muted("Times cannot be filled in for you. Take tiles from the website, or report the build.");
  }
  bool first = true;
  for (const BoardHit& hit : state.hits) {
    if (!first) ImGui::Separator();
    first = false;
    drawHit(state, hit, "panel-hit", canClick);
  }
}

// What a board says about itself before its tiles: whose side you are on,
// how a tile is taken, and whether it is over.
void drawBoardNotes(const BoardView& board) {
  // A board past its end still loads - it is remembered by id - so it has to
  // say so, or it reads as a live board that ignores every finish.
  if (board.closed()) {
    ImGui::TextColored(kWarn, "Closed - ended %s", board.endsAt.substr(0, 10).c_str());
    muted("Pick a running board in the menu (%s, Bingo).", keyName(config().toggleKey).c_str());
  }
  if (board.myTeam > 0) {
    for (const TeamRow& team : board.teams) {
      if (team.team != board.myTeam) continue;
      ImGui::TextColored(fromU32(team.color, kMine), "You play for %s", team.name.c_str());
      break;
    }
  } else if (board.teamCount > 0) {
    ImGui::TextColored(kWarn, "Teams - join a side on the website");
  }
  if (board.gameOnly()) {
    muted("Plugin and replay only: your time comes from here, or from a replay file on the website. Nothing is typed.");
  } else if (board.selfReported()) {
    muted("No check: tiles are self-reported, TMX is not asked.");
  } else {
    muted("A tile is taken by a replay on TMX - drive, upload, then \"I uploaded it\".");
  }
}

void drawPanelBoard(const State& state, bool canClick) {
  const BoardView& board = state.board;
  const bool closed = board.closed();
  drawBoardNotes(board);
  // Everything below is faded on a closed board, and nothing on it can be
  // taken: the site refuses a capture there anyway, so a button would only
  // earn a refusal.
  if (closed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);

  const int clicked = tileGrid(state, board, ImGui::GetContentRegionAvail().x, canClick, g_panelTile);
  if (clicked >= 0) g_panelTile = clicked == g_panelTile ? -1 : clicked;

  ImGui::Spacing();
  drawStanding(board, 3);

  if (const Tile* tile = tileAt(board, g_panelTile)) {
    ImGui::Separator();
    drawTileDetail(state, board, *tile, "panel-tile", canClick && !closed);
  }
  if (closed) ImGui::PopStyleVar();
}

// Where the panels are, from the last frame: the menu opens on the other side,
// and the Bingo panel starts out under the map one.
ImVec2 g_panelPos(-1, -1);
ImVec2 g_panelSize(0, 0);

void drawBingoPanel(const State& state, bool canClick, float scale) {
  Config& c = config();
  static bool s_place = true;
  static ImVec2 s_display(0, 0);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  if (display.x != s_display.x || display.y != s_display.y || g_placePanel) {
    s_display = display;
    s_place = true;
  }
  const float width = 300.0f * g_scale * scale;
  const ImVec2 below(g_panelPos.x >= 0 ? g_panelPos.x : 16.0f * g_scale,
                     g_panelPos.x >= 0 ? g_panelPos.y + g_panelSize.y + 8.0f * g_scale : 400.0f * g_scale);
  placeWindow(s_place, c.bingoX, c.bingoY, c.bingoW, c.bingoH, below, ImVec2(width, 460.0f * g_scale * scale));

  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f * g_scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * g_scale, 10.0f * g_scale));
  if (ImGui::Begin("Bingo###tmx-bingo", nullptr, panelFlags(canClick))) {
    ImGui::SetWindowFontScale(scale);
    const BoardView& board = state.board;
    const std::string subtitle = board.loaded ? (board.title.empty() ? board.id : board.title) : std::string();
    drawPanelHeader(state, "Bingo", kMine, subtitle, canClick);
    if (!state.hits.empty() || (state.inRace && state.raceState < 0)) {
      drawBingoHits(state, canClick);
      if (board.loaded) ImGui::Separator();
    }
    if (board.loaded && !state.selectedBoard.empty()) drawPanelBoard(state, canClick);
    drawToast(state.bingoToast, state.bingoToastUntil);
    rememberWindow(c.bingoX, c.bingoY, c.bingoW, c.bingoH, canClick);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void drawPanel(const State& state) {
  ImGuiIO& io = ImGui::GetIO();
  Config& c = config();
  const float scale = std::clamp(c.overlayScale, 0.7f, 1.8f);
  const float margin = 16.0f * g_scale;
  const float width = 300.0f * g_scale * scale;
  const bool canClick = interactive();
  const bool showBoard = !state.selectedBoard.empty() && state.board.loaded;

  // The first run after the split: the old single panel's geometry goes to
  // the board, which was most of it, so that is where somebody expects it -
  // and this one starts beside it at a size that fits what is left.
  if (!state.selectedBoard.empty() && c.bingoW <= 0 && c.overlayW > 80 && c.overlayX >= 0) {
    c.bingoX = c.overlayX;
    c.bingoY = c.overlayY;
    c.bingoW = c.overlayW;
    c.bingoH = c.overlayH;
    c.overlayX = c.overlayX + c.overlayW + 8.0f;
    c.overlayW = width;
    c.overlayH = 0.0f;
    c.save();
    g_placePanel = true;
  }

  static ImVec2 s_display(0, 0);
  if (io.DisplaySize.x != s_display.x || io.DisplaySize.y != s_display.y) {
    s_display = io.DisplaySize;
    g_placePanel = true;
  }
  const bool placing = g_placePanel;
  const float height = (c.panelImages ? 400.0f : 230.0f) * g_scale * scale;
  placeWindow(g_placePanel, c.overlayX, c.overlayY, c.overlayW, c.overlayH,
              ImVec2(c.overlayCorner == 0 ? margin : io.DisplaySize.x - width - margin, margin + 80.0f * g_scale),
              ImVec2(width, height));

  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f * g_scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * g_scale, 10.0f * g_scale));
  if (ImGui::Begin("100% TMX###tmx-panel", nullptr, panelFlags(canClick))) {
    ImGui::SetWindowFontScale(scale);
    drawPanelHeader(state, "100% TMX", kAccentHi, std::string(), canClick);
    drawPairing("panel-pairing", canClick);

    if (state.linking) {
      // The code goes where the eyes are. The menu has it too, but somebody
      // who pressed Connect is looking at the panel.
      ImGui::TextColored(kMuted, "Type this code at %s", state.verifyUrl.c_str());
      ImGui::PushFont(g_fontBig);
      ImGui::TextColored(kMine, "%s", state.userCode.c_str());
      ImGui::PopFont();
      ImGui::Separator();
    } else if (!state.linked) {
      ImGui::TextColored(kMuted, "Not connected to 100tmx.com.");
      if (canClick && button("Connect", Tone::Primary)) {
        shared().write([](State& s) { s.linkError.clear(); });
        pushCommand(Command::Kind::Connect);
      }
      ImGui::Separator();
    }

    drawPanelMap(state, canClick);
    drawToast(state.toast, state.toastUntil);

    g_panelPos = ImGui::GetWindowPos();
    g_panelSize = ImGui::GetWindowSize();
    g_panelCentreX = g_panelPos.x + g_panelSize.x * 0.5f;
    rememberWindow(c.overlayX, c.overlayY, c.overlayW, c.overlayH, canClick);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);

  // Shown for a selected board, and for a map that is a tile on a board that
  // is not the one selected - that tile still wants its time.
  if (placing) g_placePanel = true;  // the Bingo panel is placed in the same pass
  if (showBoard || !state.hits.empty()) drawBingoPanel(state, canClick, scale);
  g_placePanel = false;
}

// ------------------------------------------------------------------ the menu

void drawPlayingPage(const State& state) {
  pageTitle("Now playing", "The map you are on, what it is worth to the project, and the boards it is a tile of.");
  if (!state.attached || !state.inRace) {
    Panel panel("This map");
    drawPanelMap(state, true);
    return;
  }
  const MapStatus& map = state.map;
  const std::string& name = state.mapName.empty() ? map.name : state.mapName;

  Columns columns(270.0f);
  {
    Panel panel("This map");
    if (map.onExchange) {
      mapCard(map, name, ImGui::GetContentRegionAvail().x);
      const Verdict verdict = verdictOf(map);
      ImGui::TextColored(verdict.colour, "%s", verdict.line.c_str());
      if (map.claimed) ImGui::TextColored(kMine, "Marked as yours on the site");
      else if (!map.refused.empty() && map.open != 0) ImGui::TextColored(kMuted, "%s", map.refused.c_str());
      else if (!config().shareWhatIAmPlaying) ImGui::TextColored(kMuted, "Sharing is off");
      for (const AlsoHere& other : state.alsoHere) {
        ImGui::TextColored(kWarn, "also here: %s", other.name.empty() ? "another player" : other.name.c_str());
      }
      ImGui::Spacing();
      ImGui::PushID("map-links");
      if (!map.url.empty() && button("Open on TMX")) pushCommand(Command::Kind::OpenUrl, map.url);
      if (!map.uploadUrl.empty()) {
        sameLineIfRoom("Upload page");
        if (button("Upload page")) pushCommand(Command::Kind::OpenUrl, map.uploadUrl);
      }
      ImGui::PopID();
    } else {
      ImGui::PushFont(g_fontTitle);
      ImGui::TextWrapped("%s", name.empty() ? "(unnamed map)" : name.c_str());
      ImGui::PopFont();
      ImGui::TextColored(kMuted, "Not on an exchange - nothing to credit here.");
    }
  }
  columns.next();
  {
    Panel panel("Your run");
    if (state.raceState == 2 && state.raceTimeMs > 0) {
      drawRun(state, "menu-run", true);
    } else if (state.raceState == 1) {
      ImGui::TextColored(kMuted, "Driving - %s", timeString(state.raceTimeMs).c_str());
    } else if (state.raceState == 0) {
      ImGui::TextColored(kMuted, "At the start line.");
    } else {
      ImGui::TextColored(kWarn, "Cannot read this build's race state (%s).", raceStepName(state.raceStep));
      muted("Times cannot be filled in for you. The Status page says more.");
    }
  }
  {
    Panel panel("Boards");
    if (state.hits.empty()) {
      muted(state.linked ? "This map is not a tile on any board you are in."
                         : "Connect this machine to see the boards this map is on.");
    }
    bool first = true;
    for (const BoardHit& hit : state.hits) {
      if (!first) ImGui::Separator();
      first = false;
      drawHit(state, hit, "menu-hit", true);
    }
  }
}

// A board in the picker: a little grid in its own shape - filled with the
// maps when it is the one open - its name, and what kind of board it is.
bool boardCard(const char* id, const std::string& title, const std::string& line, int size, bool active,
               const BoardView* view, float width) {
  const float pad = 8.0f * g_scale;
  const float art = std::floor(width * 0.42f);
  const float textH = ImGui::GetFontSize() * 2.0f + pad * 2.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 q(p.x + width, p.y + art + textH);
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("card", ImVec2(width, art + textH));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  const float rounding = 8.0f * g_scale;
  ImDrawList* draw = ImGui::GetWindowDrawList();

  draw->AddRectFilled(p, q, hovered ? IM_COL32(38, 40, 50, 255) : IM_COL32(26, 28, 35, 255), rounding);
  draw->AddRectFilledMultiColor(ImVec2(p.x, p.y + rounding), ImVec2(q.x, p.y + art), IM_COL32(44, 30, 34, 255),
                                IM_COL32(44, 30, 34, 255), IM_COL32(24, 26, 33, 255), IM_COL32(24, 26, 33, 255));
  draw->AddRectFilled(p, ImVec2(q.x, p.y + rounding + 1.0f), IM_COL32(44, 30, 34, 255), rounding,
                      ImDrawFlags_RoundCornersTop);

  if (size > 0) {
    const int n = std::clamp(size, 1, 12);
    const float side = art - pad * 2.0f;
    const float gap = std::max(1.0f, side * 0.025f);
    const float cell = (side - gap * (n - 1)) / n;
    const ImVec2 o(p.x + (width - side) * 0.5f, p.y + pad);
    for (int i = 0; i < n * n; i++) {
      const ImVec2 a(o.x + (i % n) * (cell + gap), o.y + (i / n) * (cell + gap));
      const ImVec2 b(a.x + cell, a.y + cell);
      const Tile* tile = view ? tileAt(*view, i) : nullptr;
      void* texture = tile ? textures::get(tile->imageUrl) : nullptr;
      if (texture) {
        draw->AddImage(texId(texture), a, b, ImVec2(0.21875f, 0.0f), ImVec2(0.78125f, 1.0f));
        if (tile->held) draw->AddRectFilled(a, b, col(fromU32(tile->holderColor, kTaken), 0.45f));
      } else {
        const bool held = tile && tile->held;
        draw->AddRectFilled(a, b, held ? col(fromU32(tile->holderColor, kTaken), 0.7f) : IM_COL32(255, 255, 255, 22),
                            2.0f);
      }
    }
  } else {
    // "None": the panel without a board.
    const char* none = "100% TMX only";
    const ImVec2 s = ImGui::CalcTextSize(none);
    draw->AddText(ImVec2(p.x + (width - s.x) * 0.5f, p.y + (art - s.y) * 0.5f), IM_COL32(255, 255, 255, 120), none);
  }

  if (active) {
    const char* label = "ACTIVE";
    tag(draw, ImVec2(p.x + pad, p.y + pad), label, col(kAccent), IM_COL32_WHITE, ImGui::GetFontSize() * 0.75f);
  }

  const ImVec4 clip(p.x + pad, p.y + art, q.x - pad, q.y - pad * 0.5f);
  draw->AddText(g_fontTitle, ImGui::GetFontSize() * 1.05f, ImVec2(p.x + pad, p.y + art + pad * 0.6f), IM_COL32_WHITE,
                title.c_str(), nullptr, 0.0f, &clip);
  draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.9f,
                ImVec2(p.x + pad, p.y + art + pad * 0.6f + ImGui::GetFontSize() * 1.15f), col(kMuted), line.c_str(),
                nullptr, 0.0f, &clip);

  if (active) {
    draw->AddRect(p, q, col(kAccent), rounding, 0, 2.0f * g_scale);
  } else if (hovered) {
    draw->AddRect(p, q, IM_COL32(255, 255, 255, 90), rounding, 0, 1.5f * g_scale);
  }
  return clicked;
}

void drawBingoPage(const State& state) {
  pageTitle("Bingo", "Pick the board the panel shows. Click a tile for its map, its holder and its buttons.");
  if (!state.linked) {
    Panel panel("Board");
    muted("Connect this machine first - the boards you are in come from your account.");
    if (button("Go to Connection", Tone::Primary)) config().menuPage = kConnection;
    return;
  }

  // The picker, as cards.
  if (chip("Refresh", false)) pushCommand(Command::Kind::RefreshBoards);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Asks the site again for the boards you are in.");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kMuted, "%s",
                     state.boards.empty() ? "You are not in any board that is still running - join one on the website."
                                          : "Your boards");
  ImGui::Spacing();

  struct Card {
    std::string id, title, line;
    int size;
  };
  std::vector<Card> cards;
  cards.push_back({"", "No board", "The panel shows the map only", 0});
  // The picker lists running boards only, so a board that ended while it was
  // selected would vanish from here and stay on the panel. It keeps its card,
  // saying it is over.
  const BoardView& shown = state.board;
  bool listed = false;
  for (const BoardSummary& board : state.boards) listed |= board.id == state.selectedBoard;
  if (!listed && shown.loaded && shown.id == state.selectedBoard && !shown.id.empty()) {
    cards.push_back({shown.id, shown.title.empty() ? shown.id : shown.title,
                     "closed " + shown.endsAt.substr(0, std::min<size_t>(10, shown.endsAt.size())),
                     shown.size > 0 ? shown.size : 5});
  }
  for (const BoardSummary& board : state.boards) {
    std::string line = board.kind;
    // Teams are worth knowing before the board is opened - and "no side yet"
    // is the one state that needs the website to fix.
    if (board.teams > 0) line += board.myTeam > 0 ? " - teams" : " - teams, pick a side on the site";
    if (board.endsAt.size() >= 10) line += " - ends " + board.endsAt.substr(0, 10);
    cards.push_back({board.id, board.title.empty() ? board.id : board.title, line, board.size > 0 ? board.size : 5});
  }
  const float spacing = 12.0f * g_scale;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int columns = std::max(1, static_cast<int>((avail + spacing) / (140.0f * g_scale + spacing)));
  const float width = std::floor((avail - spacing * (columns - 1)) / columns);
  for (size_t i = 0; i < cards.size(); i++) {
    if (i % columns != 0) ImGui::SameLine(0.0f, spacing);
    const Card& card = cards[i];
    const bool active = card.id == state.selectedBoard;
    const BoardView* view = active && state.board.loaded && state.board.id == card.id ? &state.board : nullptr;
    const std::string id = "board:" + card.id;
    if (boardCard(id.c_str(), card.title, card.line, card.size, active, view, width) && !active) {
      g_panelTile = -1;
      g_menuTile = -1;
      pushCommand(Command::Kind::SelectBoard, card.id);
    }
  }
  ImGui::Dummy(ImVec2(0.0f, spacing * 0.5f));

  const BoardView& board = state.board;
  if (state.selectedBoard.empty() || !board.loaded) {
    if (!state.selectedBoard.empty()) ImGui::TextColored(kMuted, "Loading the board...");
    return;
  }

  Columns layout(270.0f);
  {
    Panel panel("Board");
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted(board.title.empty() ? board.id.c_str() : board.title.c_str());
    ImGui::PopFont();
    drawBoardNotes(board);
    ImGui::Spacing();
    const int clicked = tileGrid(state, board, ImGui::GetContentRegionAvail().x, true, g_menuTile);
    if (clicked >= 0) g_menuTile = clicked;
  }
  layout.next();
  {
    Panel panel("Tile");
    if (const Tile* tile = tileAt(board, g_menuTile)) {
      drawTileDetail(state, board, *tile, "menu-tile", !board.closed());
    } else {
      muted("Click a tile on the board.");
    }
  }
  {
    Panel panel("Standing");
    drawStanding(board, 12);
  }
}

void drawConnectionPage(const State& state) {
  pageTitle("Connection", "This machine speaks for the account you approve it with on 100tmx.com.");
  Columns columns(270.0f);
  {
    Panel panel("This machine");
    if (state.linking) {
      ImGui::TextWrapped("Type this code at %s", state.verifyUrl.c_str());
      ImGui::PushFont(g_fontBig);
      ImGui::TextColored(kMine, "%s", state.userCode.c_str());
      ImGui::PopFont();
      muted("The browser should already be open on that page, signed in with Discord.");
      if (button("Copy the code")) {
        ImGui::SetClipboardText(state.userCode.c_str());
        toastHere("Code copied.");
      }
      ImGui::SameLine();
      if (button("Cancel")) {
        // Set here as well as sent: the worker notices it on its next tick
        // without having to get round to the command first.
        shared().write([](State& s) {
          s.linking = false;
          s.userCode.clear();
        });
        pushCommand(Command::Kind::CancelLink);
      }
    } else if (state.linked) {
      ImGui::TextColored(kOpen, "Connected.");
      muted("Disconnect here or on the website - either way the token stops working at once.");
      if (button("Disconnect", Tone::Danger)) ImGui::OpenPopup("Disconnect this machine?");
      if (confirm("Disconnect this machine?",
                  "Disconnect this machine from your account? Nothing more is sent, and you can connect again at any time.",
                  "Disconnect")) {
        pushCommand(Command::Kind::Disconnect);
      }
    } else {
      muted("Connecting shows a code here and opens the website, where you approve it while signed in with Discord.");
      if (button("Connect", Tone::Primary)) {
        shared().write([](State& s) { s.linkError.clear(); });
        pushCommand(Command::Kind::Connect);
      }
    }
    if (!state.linkError.empty()) {
      ImGui::Spacing();
      ImGui::TextColored(kWarn, "%s", state.linkError.c_str());
    }
  }
  columns.next();
  {
    Panel panel("Sharing");
    if (state.linked && state.askSharing) {
      // A notice, not a question - sharing is on by default. Still shown once
      // and in plain words, because a switch that sends something off the
      // machine should never be discovered rather than told.
      ImGui::TextColored(kMine, "Just connected - one thing to know:");
    }
    muted("While you are on a map, its id is sent to the site so it can tell you the map's status. With sharing on, "
          "the remaining list also shows you on that map, and drops you a few minutes after you stop. Nothing is "
          "sent while you are in the menus.");
    bool share = config().shareWhatIAmPlaying;
    if (switchRow("Share what I am playing", &share)) {
      config().shareWhatIAmPlaying = share;
      config().save();
      // Switching it off is not only about future requests: whatever is
      // standing now should go too.
      pushCommand(share ? Command::Kind::ReportNow : Command::Kind::ReleaseAll);
    }
    if (state.linked && state.askSharing) {
      if (button("Got it", Tone::Primary)) {
        shared().write([](State& s) { s.askSharing = false; });
        pushCommand(Command::Kind::ReportNow);
      }
    }
  }
  {
    // Beside the website link since 0.9.2: it is the other thing this
    // machine connects to.
    Panel panel("Replay upload");
    // The only switch here that opens a socket, so what that means is said
    // right under it.
    bool bridgeOn = config().bridge;
    if (switchRow("Upload my replays through the browser", &bridgeOn)) bridge::setEnabled(bridgeOn);
    muted("After every finish that beats your own record, the replay TrackMania saved is offered to the 100%% TMX "
          "browser extension, which uploads it to TMX signed in as you. Your TMX login never comes near this mod.");
    if (config().bridge) {
      const bridge::Status bridgeState = bridge::status();
      if (!bridgeState.running) {
        ImGui::TextColored(kWarn, "No free port - is a second TrackMania running?");
      } else if (bridgeState.paired) {
        ImGui::TextColored(kOpen, "Listening on 127.0.0.1:%d - the browser has been here", bridgeState.port);
      } else {
        ImGui::TextColored(kMuted, "Listening on 127.0.0.1:%d - waiting for the extension", bridgeState.port);
      }
      // The key itself is not printed: it is a secret from the other programs
      // on this machine, and a streamed screen is one of them.
      if (button("Copy the pairing key")) {
        ImGui::SetClipboardText(config().copy(config().bridgeKey).c_str());
        toastHere("Key copied - paste it into the extension.");
      }
      if (bridgeState.queued > 0) {
        ImGui::TextColored(kMine, "%d replay%s waiting for the browser", bridgeState.queued,
                           bridgeState.queued == 1 ? "" : "s");
      }
      if (!bridgeState.lastResult.empty()) ImGui::TextColored(kMuted, "Last: %s", bridgeState.lastResult.c_str());
      ImGui::TextColored(kMuted, "Needs autosaving on in TrackMania's replay settings.");

      // Two games, two folder names - Nations writes to Documents\TrackMania,
      // United to Documents\TmForever - and both are searched. This is for the
      // third case: a -userdir, a redirected Documents, a portable install.
      static char s_replayDir[512] = {0};
      static bool s_replayDirLoaded = false;
      if (!s_replayDirLoaded) {
        s_replayDirLoaded = true;
        strncpy_s(s_replayDir, sizeof(s_replayDir), config().copy(config().replayDir).c_str(), _TRUNCATE);
      }
      ImGui::TextColored(kMuted, "Replay folder (empty: the usual places)");
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##replaydir", "Documents\\TrackMania\\Tracks\\Replays\\Autosaves", s_replayDir,
                               sizeof(s_replayDir));
      // Kept in the field while typing and stored when it is left: the bridge
      // reads it from another thread, and half a path is no use to it.
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string dir = s_replayDir;
        config().edit([&](Config& c) { c.replayDir = dir; });
      }
    }
  }
}

void drawSettingsPage(const State& state) {
  pageTitle("Settings", "Everything here is saved as you change it.");
  Columns columns(270.0f);
  {
    Panel panel("Bingo");
    bool autoSubmit = config().autoSubmitSelfReported;
    if (switchRow("Put my finish straight onto self-reported boards", &autoSubmit)) {
      config().autoSubmitSelfReported = autoSubmit;
      config().save();
    }
    muted("Only on a board set to \"no check\" or \"plugin and replay only\", only when the run would take the tile, "
          "and only once the game itself reports the finish or has written the run's replay - a pause never "
          "counts. Every other board takes a tile by a replay on TMX, which the site goes and reads.");
  }
  columns.next();
  {
    Panel panel("Panel");
    bool overlayOn = config().overlay;
    if (switchRow("Show the panel while driving", &overlayOn)) {
      config().overlay = overlayOn;
      config().save();
    }
    bool images = config().panelImages;
    if (switchRow("Map pictures on the panel", &images, "Off: a compact text readout.")) {
      config().panelImages = images;
      config().save();
    }

    ImGui::TextColored(kMuted, "The panel takes the mouse");
    static const char* const kInputs[] = {"Except mid-run", "Only with the menu", "Always"};
    static const char* const kInputHints[] = {
        "Clickable everywhere but during a run - in the menus, at the start, on the results screen.",
        "Click-through unless this menu is open.", "Always clickable, even mid-run."};
    for (int i = 0; i < 3; i++) {
      if (i) sameLineIfRoom(kInputs[i]);
      ImGui::PushID(i);
      if (chip(kInputs[i], config().panelInput == i) && config().panelInput != i) {
        config().panelInput = i;
        config().save();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kInputHints[i]);
      ImGui::PopID();
    }

    ImGui::TextColored(kMuted, "Starts on the");
    ImGui::SameLine();
    if (chip("Left", config().overlayCorner == 0) && config().overlayCorner != 0) {
      config().overlayCorner = 0;
      config().save();
    }
    ImGui::SameLine();
    if (chip("Right", config().overlayCorner == 1) && config().overlayCorner != 1) {
      config().overlayCorner = 1;
      config().save();
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    float scale = config().overlayScale;
    if (ImGui::SliderFloat("Text size", &scale, 0.7f, 1.8f, "%.2f")) config().overlayScale = scale;
    if (ImGui::IsItemDeactivatedAfterEdit()) config().save();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    float alpha = config().overlayAlpha;
    if (ImGui::SliderFloat("Opacity", &alpha, 0.2f, 1.0f, "%.2f")) config().overlayAlpha = alpha;
    if (ImGui::IsItemDeactivatedAfterEdit()) config().save();

    if (button("Put the panels back")) {
      Config& c = config();
      c.overlayX = c.overlayY = -1.0f;
      c.overlayW = c.overlayH = 0.0f;
      c.bingoX = c.bingoY = -1.0f;
      c.bingoW = c.bingoH = 0.0f;
      c.save();
      g_placePanel = true;
    }
  }
  {
    Panel panel("Keys");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Open and close this menu");
    ImGui::SameLine();
    const std::string current = keyName(config().toggleKey);
    tag(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), current.c_str(), IM_COL32(52, 54, 66, 255),
        IM_COL32_WHITE, ImGui::GetFontSize());
    ImGui::Dummy(ImVec2(ImGui::CalcTextSize(current.c_str()).x + ImGui::GetFontSize(), ImGui::GetFrameHeight()));
    ImGui::SameLine();
    if (g_capturingKey) {
      ImGui::TextColored(kMine, "Press a key... (Esc: keep it)");
    } else if (button("Change")) {
      startKeyCapture();
    }
    if (const char* use = shadersUse(config().toggleKey); use && shadersPresent()) {
      ImGui::TextColored(kWarn, "TM Vibrant Shaders uses %s for %s too.", current.c_str(), use);
      muted("Both react to it. Pick a key nothing else uses.");
    }
  }
  (void)state;
}

void drawStatusPage(const State& state) {
  pageTitle("Status", "What is running, and what it can read. The lines worth copying into a bug report.");
  Columns columns(270.0f);
  {
    Panel panel("Versions");
    // Which DLL is actually running. The ModLoader keeps every version ever
    // installed and picks one itself, so "I just built that" and "that is what
    // the game loaded" are different claims - and a day was spent measuring
    // the older one.
    if (ImGui::BeginTable("versions", 2, ImGuiTableFlags_SizingFixedFit)) {
      auto row = [](const char* label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kMuted, "%s", label);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
      };
      row("Mod", TMX_VERSION_A);
      row("Game build", state.buildKey);
      row("Offsets", state.attached ? state.profile : "not recognised");
      row("Variant", state.variant.empty() ? "unknown" : state.variant);
      row("Map UID", state.uid);
      ImGui::EndTable();
    }
    // A build whose UID reads but whose race state does not is a build where
    // the walk to the local player takes a wrong turn, and this says which.
    if (state.raceState >= 0) {
      ImGui::TextColored(kMuted, "Race state %d, time %d ms", state.raceState, state.raceTimeMs);
      if (!state.finishProbe.empty()) muted("Player info: %s", state.finishProbe.c_str());
    } else if (state.inRace) {
      ImGui::TextColored(kWarn, "Race state unreadable - stops at %s", raceStepName(state.raceStep));
      muted("The map itself reads fine, so the build is close but not exact. Offsets can be corrected in config.ini "
            "under [offsets] without waiting for a release.");
    } else {
      ImGui::TextColored(kMuted, "Race state: no map loaded");
    }
  }
  {
    Panel panel("Last request");
    ImGui::TextWrapped("%s", state.lastCall.empty() ? "-" : state.lastCall.c_str());
    if (!state.lastError.empty()) ImGui::TextColored(kWarn, "%s", state.lastError.c_str());
  }
  columns.next();
  {
    // Where a build keeps its race clock is the one thing that cannot be
    // shipped known, so the mod works it out by watching: among the ints that
    // keep time with the wall clock, the race clock is the only one that
    // returns to zero when a run is restarted.
    Panel panel("Race clock");
    const game::CalibrationState& calib = state.calib;
    if (calib.haveChain) {
      if (calib.reading >= 0) {
        ImGui::TextColored(kOpen, "Reading %s right now", timeString(calib.reading).c_str());
      } else {
        ImGui::TextColored(kMuted, "Known, but nothing to read outside a race");
      }
      ImGui::TextColored(kMuted, "app -> %s", calib.chain.c_str());
      if (button("Learn it again")) ImGui::OpenPopup("Learn the clock again?");
      if (confirm("Learn the clock again?",
                  "Forget where this build keeps its race clock? It is learned again from your next laps, and times "
                  "are not filled in until it is.",
                  "Forget it")) {
        pushCommand(Command::Kind::ForgetCalibration);
      }
    } else if (state.inRace) {
      ImGui::TextColored(kWarn, "Learning it - %d clock%s found%s", calib.ticking, calib.ticking == 1 ? "" : "s",
                         calib.sawReset ? ", one has restarted" : "");
      muted("Drive a lap and press restart once. That is the whole calibration.");
    } else {
      ImGui::TextColored(kMuted, "Not learned yet - load a map");
    }

    // Only when the automatic route has had a chance and failed. It is a
    // fallback, not a step: anybody asked to type a time here has already
    // been let down once.
    if (!calib.haveChain && calib.ticking > 0 && state.inRace) {
      if (ImGui::TreeNode("It cannot find it - do it by hand")) {
        static char s_time[16] = "";
        static char s_address[32] = "";
        muted("Type the time the game showed after a finish, like 13.91.");
        ImGui::SetNextItemWidth(120.0f * g_scale);
        ImGui::InputText("##calibtime", s_time, sizeof(s_time), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (button("That was my time")) {
          std::string text = s_time;
          for (char& c : text) {
            if (c == ',') c = '.';
          }
          const double seconds = atof(text.c_str());
          const int ms = text.find('.') == std::string::npos && seconds > 1000 ? static_cast<int>(seconds)
                                                                               : static_cast<int>(seconds * 1000.0 + 0.5);
          if (ms > 0) pushCommand(Command::Kind::CalibrateTime, "", ms);
          s_time[0] = '\0';
        }
        if (calib.candidates > 0) {
          ImGui::TextColored(kMuted, "%u addresses still match - one more lap settles it",
                             static_cast<unsigned>(calib.candidates));
        }
        ImGui::SetNextItemWidth(160.0f * g_scale);
        ImGui::InputTextWithHint("##calibaddr", "0x0A1B2C3D", s_address, sizeof(s_address));
        ImGui::SameLine();
        if (button("Use this address")) {
          pushCommand(Command::Kind::CalibrateAddress, s_address);
          s_address[0] = '\0';
        }
        ImGui::TextColored(kMuted, "Or an address from Cheat Engine that holds the live time.");
        ImGui::TreePop();
      }
    }
    if (!calib.note.empty()) muted("%s", calib.note.c_str());
  }
  {
    Panel panel("Files");
    if (button("Open the 100TMX folder")) {
      // config.ini and log.txt both live there; the log is what a bug report
      // wants attached.
      wchar_t* raw = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw) {
        const std::wstring dir = std::wstring(raw) + L"\\100TMX";
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      if (raw) CoTaskMemFree(raw);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("config.ini and log.txt. In fullscreen the game may minimise.");
    muted("Attach log.txt to a bug report.");
  }
}

void drawAboutPage(const State& state) {
  pageTitle("About", "100% TMX " TMX_VERSION_A " - the project's overlay for TrackMania Forever.");
  (void)state;
  Columns columns(270.0f);
  {
    Panel panel("What this does");
    ImGui::Bullet();
    ImGui::TextWrapped("Shows whether the map you are on is still open for the 100%% TMX project, and what it is worth.");
    ImGui::Bullet();
    ImGui::TextWrapped("Puts your bingo board on screen, and takes tiles the way each board takes them.");
    ImGui::Bullet();
    ImGui::TextWrapped("Optionally hands finished replays to the browser extension for upload.");
    ImGui::Spacing();
    // The rule the whole project is built on, said where somebody can read it.
    ImGui::TextColored(kMine, "The mod never awards a finish.");
    muted("Credit comes from the first replay uploaded to TMX, the same as it always has.");
  }
  columns.next();
  {
    Panel panel("Links");
    const std::string base = config().baseUrl;
    if (button("100tmx.com")) pushCommand(Command::Kind::OpenUrl, base + "/");
    ImGui::SameLine();
    if (button("Bingo")) pushCommand(Command::Kind::OpenUrl, base + "/events/bingo");
    ImGui::SameLine();
    if (button("Linked machines")) pushCommand(Command::Kind::OpenUrl, base + "/link");
    muted("Links open in your browser. In fullscreen the game may minimise.");
  }
  {
    Panel panel("Credits");
    muted("Built with Dear ImGui and stb_image. Offsets after Twinkie (MIT). The menu's look follows TM Vibrant Shaders.");
  }
}

void drawMenuHeader(const State& state) {
  ImGui::PushFont(g_fontTitle);
  ImGui::TextColored(kAccentHi, "100%% TMX");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kMuted, "%s", TMX_VERSION_A);
  ImGui::SameLine(0.0f, 16.0f * g_scale);
  const char* status = state.linked ? "Connected" : state.linking ? "Waiting for the code" : "Not connected";
  if (chip(status, true, state.linked ? ImVec4(0.20f, 0.55f, 0.30f, 1.0f) : state.linking ? ImVec4(0.75f, 0.55f, 0.10f, 1.0f) : ImVec4(0.35f, 0.36f, 0.42f, 1.0f))) {
    config().menuPage = kConnection;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connection");

  // Right: the panel switch and close.
  const float h = ImGui::GetFrameHeight();
  const float right = ImGui::GetContentRegionMax().x;
  const char* panelLabel = "Panel";
  ImGui::SameLine(right - h - h * 1.8f - ImGui::CalcTextSize(panelLabel).x - 28.0f * g_scale);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(panelLabel);
  ImGui::SameLine();
  bool overlayOn = config().overlay;
  if (toggle("panel", &overlayOn)) {
    config().overlay = overlayOn;
    config().save();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the panel while driving");
  ImGui::SameLine(right - h);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.20f, 0.20f, 0.90f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.28f, 0.26f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  if (ImGui::Button("\xC3\x97", ImVec2(h, h))) g_uiOpen = false;  // U+00D7
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (%s)", keyName(config().toggleKey).c_str());
  ImGui::PopStyleColor(4);

  if (state.attached == false && state.sawMap) {
    ImGui::TextColored(kWarn, "This TrackMania build is not recognised (build %s).", state.buildKey.c_str());
  }
  ImGui::Spacing();
  drawPairing("menu-pairing", true);
}

void drawNavigation(const State& state) {
  static const char* const kPages[] = {"Now playing", "Bingo", "Connection", "Settings", "Status", "About"};
  static const char* const kHints[] = {"The map you are on", "Your boards, tile by tile", "Link this machine to your account",
                                       "How the mod behaves", "Versions, race clock, files", "What this is"};
  const float h = 38.0f * g_scale;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  for (int i = 0; i < kPageCount; i++) {
    const bool active = config().menuPage == i;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton(kPages[i], ImVec2(w, h)) && !active) {
      config().menuPage = i;
      config().save();
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetTooltip("%s", kHints[i]);
    if (active || hovered) {
      draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), active ? col(kAccent, 0.18f) : IM_COL32(255, 255, 255, 14),
                          6.0f * g_scale);
    }
    if (active) {
      draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.22f), ImVec2(p.x + 3.0f * g_scale, p.y + h * 0.78f), col(kAccent), 2.0f);
    }
    const float size = g_fontTitle->FontSize * 0.82f;
    draw->AddText(g_fontTitle, size, ImVec2(p.x + 14.0f * g_scale, p.y + (h - size) * 0.5f),
                  active ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text, 0.7f), kPages[i]);
    // A dot where something wants attention.
    const bool attention = (i == kConnection && (!state.linked || state.askSharing)) ||
                           (i == kStatus && state.inRace && state.raceState < 0) ||
                           (i == kSettings && shadersPresent() && shadersUse(config().toggleKey));
    if (attention) {
      draw->AddCircleFilled(ImVec2(p.x + w - 14.0f * g_scale, p.y + h * 0.5f), 4.0f * g_scale,
                            col(i == kConnection ? kAccentHi : kWarn));
    }
  }
  // Keys at the bottom of the bar.
  const float lines = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
  const float space = ImGui::GetContentRegionAvail().y - lines;
  if (space > 0.0f) ImGui::Dummy(ImVec2(0.0f, space));
  ImGui::TextColored(kMuted, "%s  this menu", keyName(config().toggleKey).c_str());
  ImGui::TextColored(kMuted, "Saved automatically");
}

void drawMenu(const State& state) {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float margin = 20.0f * g_scale;

  // Placed when it first opens and when the screen changes, on the side the
  // panel is not on; otherwise where it was left.
  static bool s_placed = false;
  static ImVec2 s_display(0, 0);
  if (!s_placed || display.x != s_display.x || display.y != s_display.y) {
    s_placed = true;
    s_display = display;
    const float maxW = std::max(320.0f, display.x - margin * 2.0f);
    const float w = std::clamp(display.x * 0.5f, std::min(780.0f * g_scale, maxW), std::min(1180.0f * g_scale, maxW));
    const float h = std::min(display.y - margin * 2.0f, 860.0f * g_scale);
    const bool panelOnLeft = g_panelCentreX < display.x * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(panelOnLeft ? display.x - margin - w : margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(560.0f * g_scale, display.x), std::min(420.0f * g_scale, display.y)),
                                      ImVec2(FLT_MAX, FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_scale, 14.0f * g_scale));
  const bool open = ImGui::Begin("100% TMX###tmx-menu", &g_uiOpen,
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  if (!open) {
    ImGui::End();
    return;
  }
  drawMenuHeader(state);

  ImGui::BeginChild("navigation", ImVec2(170.0f * g_scale, 0.0f));
  drawNavigation(state);
  ImGui::EndChild();
  ImGui::SameLine(0.0f, 14.0f * g_scale);

  ImGui::BeginChild("page", ImVec2(0.0f, 0.0f));
  ImGui::PushID(config().menuPage);
  switch (config().menuPage) {
    case kBingo: drawBingoPage(state); break;
    case kConnection: drawConnectionPage(state); break;
    case kSettings: drawSettingsPage(state); break;
    case kStatus: drawStatusPage(state); break;
    case kAbout: drawAboutPage(state); break;
    default: drawPlayingPage(state); break;
  }
  ImGui::PopID();
  ImGui::EndChild();
  ImGui::End();
}

// ------------------------------------------------------------------ setup

HWND gameWindow(IDirect3DDevice9* device) {
  D3DDEVICE_CREATION_PARAMETERS params{};
  if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow) return params.hFocusWindow;
  // A windowed device may have no focus window; the swap chain still knows
  // which window it presents into.
  IDirect3DSwapChain9* chain = nullptr;
  HWND window = nullptr;
  if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain) {
    D3DPRESENT_PARAMETERS present{};
    if (SUCCEEDED(chain->GetPresentParameters(&present))) window = present.hDeviceWindow;
    chain->Release();
  }
  return window;
}

void setupStyle() {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 8.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.PopupRounding = 6.0f;
  style.ScrollbarRounding = 6.0f;
  style.ChildRounding = 8.0f;
  style.WindowPadding = ImVec2(12.0f, 10.0f);
  style.FramePadding = ImVec2(8.0f, 4.0f);
  style.ItemSpacing = ImVec2(8.0f, 6.0f);
  style.WindowBorderSize = 1.0f;
  style.GrabMinSize = 9.0f;
  ImVec4* colors = style.Colors;
  colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
  colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
  colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.19f, 0.90f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.26f, 0.95f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.25f, 0.31f, 1.0f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.98f);
  colors[ImGuiCol_Button] = ImVec4(0.20f, 0.21f, 0.26f, 1.0f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.28f, 0.34f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.33f, 0.40f, 1.0f);
  colors[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.40f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.60f);
  colors[ImGuiCol_HeaderActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.80f);
  colors[ImGuiCol_SliderGrab] = kAccentHi;
  colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
  colors[ImGuiCol_CheckMark] = kAccentHi;
  colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);

  // Segoe UI (every Windows has it) reads much better than ImGui's pixel font,
  // and sizes grow with the screen.
  RECT client{};
  GetClientRect(g_window, &client);
  g_scale = std::clamp(static_cast<float>(client.bottom - client.top) / 1080.0f, 1.0f, 2.0f);
  style.ScaleAllSizes(g_scale);

  // Latin with its accents, Greek and Cyrillic: map and player names come in
  // all of them, and a name drawn as question marks is a name nobody knows.
  static ImVector<ImWchar> s_ranges;
  if (s_ranges.empty()) {
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar kExtra[] = {0x0100, 0x024F, 0x0370, 0x03FF, 0x2010, 0x205E, 0x2190, 0x21FF, 0};
    builder.AddRanges(kExtra);
    builder.BuildRanges(&s_ranges);
  }
  char fonts[MAX_PATH] = {};
  GetWindowsDirectoryA(fonts, MAX_PATH);
  const std::string regular = std::string(fonts) + "\\Fonts\\segoeui.ttf";
  const std::string semibold = std::string(fonts) + "\\Fonts\\seguisb.ttf";
  if (GetFileAttributesA(regular.c_str()) != INVALID_FILE_ATTRIBUTES) {
    const char* heavy = GetFileAttributesA(semibold.c_str()) != INVALID_FILE_ATTRIBUTES ? semibold.c_str() : regular.c_str();
    io.Fonts->AddFontFromFileTTF(regular.c_str(), 17.0f * g_scale, nullptr, s_ranges.Data);
    g_fontTitle = io.Fonts->AddFontFromFileTTF(heavy, 22.0f * g_scale, nullptr, s_ranges.Data);
    g_fontBig = io.Fonts->AddFontFromFileTTF(heavy, 32.0f * g_scale, nullptr, s_ranges.Data);
  }
  if (!g_fontTitle || !g_fontBig) {
    io.Fonts->Clear();
    ImFont* fallback = io.Fonts->AddFontDefault();
    g_fontTitle = fallback;
    g_fontBig = fallback;
  }
}

void setup(IDirect3DDevice9* device) {
  // The window first: without it there is nothing to take input from, and the
  // Win32 backend would be run every frame without ever having been set up.
  // Nothing is created until it is there, so trying again next frame is free.
  HWND window = gameWindow(device);
  if (!window) {
    log::once("overlay-window", "overlay: the device has no window yet - waiting");
    return;
  }
  g_window = window;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the mod keeps its own settings; ImGui keeps none
  io.LogFilename = nullptr;
  setupStyle();

  ImGui_ImplWin32_Init(g_window);
  ImGui_ImplDX9_Init(device);
  // Subclassing rather than a global hook: it is the game's own window, and
  // the original procedure is called for everything the panel does not eat.
  g_originalWndProc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndProc)));
  g_ready = true;
  log::line("overlay ready (window %p, subclassed %s, scale %.2f)", (void*)g_window, g_originalWndProc ? "yes" : "NO",
            g_scale);
}

bool gameHasFocus() {
  if (!g_window) return true;
  HWND foreground = GetForegroundWindow();
  return foreground == g_window || GetAncestor(foreground, GA_ROOTOWNER) == g_window;
}

}  // namespace

bool capturingInput() { return g_ready && g_uiOpen; }

void setMenuOpen(bool open) { g_uiOpen = open; }

void draw(IDirect3DDevice9* device) {
  if (!g_ready) setup(device);
  if (!g_ready) return;
  g_device = device;
  textures::beginFrame(device);

  // The key is read here rather than in the window procedure so it works even
  // when the game has swallowed it - but only while the game is the window in
  // front: F9 pressed in a browser is not meant for the game.
  pollKeyCapture();
  const bool down = !g_capturingKey && gameHasFocus() && (GetAsyncKeyState(config().toggleKey) & 0x8000) != 0;
  if (down && !g_toggleHeld) {
    g_uiOpen = !g_uiOpen;
    log::line("toggle key: window %s", g_uiOpen ? "open" : "closed");
  }
  if (!g_capturingKey) g_toggleHeld = down;

  State state = shared().read();
  g_inRace = state.inRace;
  g_raceState = state.raceState;
  g_raceStateTrusted = state.raceStateTrusted;
  g_raceClockMoving = state.raceClockMoving;
  g_raceDriving = state.raceDriving;
  g_readAt = state.readAt;

  // One line each time the panel starts or stops taking the mouse, with why.
  // "It would not let me click" is a report nobody can act on; this is.
  {
    static int s_last = -1;
    const int now = interactive() ? 1 : 0;
    if (now != s_last) {
      s_last = now;
      log::line("panel: %s (window %s, state %d, trusted %d, clock %s, driving %d, reading %.1fs old)",
                now ? "takes the mouse" : "click-through", g_uiOpen ? "open" : "closed", g_raceState,
                g_raceStateTrusted ? 1 : 0, g_raceClockMoving ? "moving" : "still", g_raceDriving ? 1 : 0,
                g_readAt > 0 ? nowSeconds() - g_readAt : -1.0);
    }
  }
  // A pick belongs to the board it was made on.
  if (state.selectedBoard != g_tileBoard) {
    g_tileBoard = state.selectedBoard;
    g_panelTile = -1;
    g_menuTile = -1;
  }
  if (config().menuPage < 0 || config().menuPage >= kPageCount) config().menuPage = kPlaying;

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  if (config().overlay || g_uiOpen) drawPanel(state);
  if (g_uiOpen) drawMenu(state);

  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
  publishMouseOwner();
}

void invalidate() {
  // The map pictures live in the managed pool and survive a reset by
  // themselves; ImGui's own buffers and font do not, and are made again on the
  // next frame.
  if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void shutdown() {
  if (!g_ready) return;
  g_ready = false;
  textures::releaseAll();
  if (g_window) {
    if (GetPropW(g_window, kMouseOwner) == selfHandle()) RemovePropW(g_window, kMouseOwner);
    // Only put the old procedure back if ours is still the one installed.
    // Another mod that subclassed after us sits on top, and writing over it
    // would cut it out of the chain - its own unhooking would then restore a
    // procedure that is no longer there. Left in place, ours passes
    // everything straight through now that g_ready is false.
    if (g_originalWndProc &&
        GetWindowLongPtrW(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(wndProc)) {
      SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
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

  if (g_ready) {
    const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    const bool key = message >= WM_KEYFIRST && message <= WM_KEYLAST;
    if (mouse && foreignOwnsMouse(window)) {
      // Another overlay has the pointer: its window is the one under it. Ours
      // must not light up or take the click, so ImGui is told the pointer left.
      if (message == WM_MOUSEMOVE) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
      return CallWindowProcW(g_originalWndProc, window, message, wparam, lparam);
    }

    // The game reads its keys itself and never turns a press into a
    // character, so a text field in the menu stayed empty. While one has the
    // focus, the character is made here - unless the game's own loop already
    // made it.
    if (message == WM_KEYDOWN && ImGui::GetIO().WantTextInput) {
      MSG queued;
      if (!PeekMessageW(&queued, window, WM_CHAR, WM_CHAR, PM_NOREMOVE)) {
        MSG press{window, message, wparam, lparam, 0, {}};
        TranslateMessage(&press);
      }
    }

    // Input is handed to ImGui on every message, not only while the menu is
    // open: the panel may take the mouse in the menus, and it can only do that
    // if ImGui hears the click.
    ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);

    // A release goes wherever its press went. Swallowing a release whose
    // press the game saw leaves a key stuck down in the game; passing on a
    // release whose press we took makes the game act on a click it never
    // saw start - a button under ours, pressed by a click on ours.
    int button = -1;
    switch (message) {
      case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: button = 0; break;
      case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: button = 1; break;
      case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: button = 2; break;
      case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4;
        break;
      default: break;
    }
    const bool buttonUp = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                          message == WM_XBUTTONUP;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    const unsigned vk = static_cast<unsigned>(wparam) & 0xFF;

    if (buttonUp && button >= 0) {
      const bool ours = g_swallowedButton[button];
      g_swallowedButton[button] = false;
      if (ours) return 1;
    } else if (keyUp) {
      const bool ours = g_swallowedKey[vk];
      g_swallowedKey[vk] = false;
      if (ours) return 1;
    } else if (interactive()) {
      // Swallowing is the part that stays conditional: only what the panel is
      // actually using, and only while it may take input at all - a driving
      // input must always reach the game, or somebody loses a run to it.
      const ImGuiIO& io = ImGui::GetIO();
      if (mouse && io.WantCaptureMouse) {
        if (button >= 0) g_swallowedButton[button] = true;
        return 1;
      }
      if (key && io.WantCaptureKeyboard) {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) g_swallowedKey[vk] = true;
        return 1;
      }
    }
  }
  return CallWindowProcW(g_originalWndProc, window, message, wparam, lparam);
}

}  // namespace overlay
}  // namespace tmx
