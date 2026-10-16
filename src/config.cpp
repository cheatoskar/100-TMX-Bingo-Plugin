#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "game.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace tmx {
namespace {

// Wide all the way to the file system. The path used to be turned into UTF-8
// and handed to the ANSI functions, which read those bytes in the system code
// page instead - so for a user called Jürgen the folder name came out as
// mojibake, the config was never written, and the machine had to be linked
// again on every start.
std::wstring documentsDirW() {
  wchar_t* raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw) out = raw;
  if (raw) CoTaskMemFree(raw);
  return out;
}

std::wstring configPathW() {
  std::wstring dir = documentsDirW();
  if (dir.empty()) return L"100TMX-config.ini";  // next to the exe: better than nowhere
  dir += L"\\100TMX";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\config.ini";
}

std::string utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], size, nullptr, nullptr);
  return out;
}

std::string trim(const std::string& in) {
  size_t a = in.find_first_not_of(" \t\r\n");
  size_t b = in.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : in.substr(a, b - a + 1);
}

bool asBool(const std::string& v, bool fallback) {
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return fallback;
}

// A number from a file somebody can edit by hand: anything that is not one, or
// is out of range, keeps the default instead of becoming 0 - a scale of 0 is a
// panel nobody can see, and that looks exactly like the mod being broken.
float asFloat(const std::string& v, float fallback, float lo, float hi) {
  char* end = nullptr;
  const double d = strtod(v.c_str(), &end);
  if (end == v.c_str() || !std::isfinite(d)) return fallback;
  return static_cast<float>(std::clamp(d, static_cast<double>(lo), static_cast<double>(hi)));
}

int asInt(const std::string& v, int fallback, int lo, int hi) {
  char* end = nullptr;
  const long n = strtol(v.c_str(), &end, 10);
  if (end == v.c_str()) return fallback;
  return static_cast<int>(std::clamp<long>(n, lo, hi));
}

// "0x972EB8" or "9908920" - the ini is where somebody adds their own build, so
// it takes the form the disassembler shows them.
uintptr_t asAddress(const std::string& v, uintptr_t fallback) {
  if (v.empty()) return fallback;
  return static_cast<uintptr_t>(strtoul(v.c_str(), nullptr, v.rfind("0x", 0) == 0 ? 16 : 10));
}

Config g_config;

}  // namespace

Config& config() { return g_config; }

std::string Config::path() const { return utf8(configPathW()); }

void Config::load() {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  std::ifstream file(configPathW().c_str());
  if (!file) return;

  std::string section;
  std::string line;
  game::Offsets custom;
  bool haveCustom = false;
  offsetLines.clear();

  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }

    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));

    if (section == "site") {
      if (key == "url" && !value.empty()) baseUrl = value;
      else if (key == "token") token = value;
    } else if (section == "mod") {
      if (key == "share") shareWhatIAmPlaying = asBool(value, shareWhatIAmPlaying);
      else if (key == "auto_submit") autoSubmitSelfReported = asBool(value, autoSubmitSelfReported);
      else if (key == "overlay") overlay = asBool(value, overlay);
      else if (key == "corner") overlayCorner = asInt(value, overlayCorner, 0, 1);
      else if (key == "scale") overlayScale = asFloat(value, overlayScale, 0.7f, 1.8f);
      else if (key == "alpha") overlayAlpha = asFloat(value, overlayAlpha, 0.2f, 1.0f);
      else if (key == "w") overlayW = asFloat(value, overlayW, 0.0f, 4000.0f);
      else if (key == "h") overlayH = asFloat(value, overlayH, 0.0f, 4000.0f);
      else if (key == "input") panelInput = asInt(value, panelInput, 0, 2);
      else if (key == "x") overlayX = asFloat(value, overlayX, -1.0f, 16000.0f);
      else if (key == "y") overlayY = asFloat(value, overlayY, -1.0f, 16000.0f);
      else if (key == "bingo_x") bingoX = asFloat(value, bingoX, -1.0f, 16000.0f);
      else if (key == "bingo_y") bingoY = asFloat(value, bingoY, -1.0f, 16000.0f);
      else if (key == "bingo_w") bingoW = asFloat(value, bingoW, 0.0f, 4000.0f);
      else if (key == "bingo_h") bingoH = asFloat(value, bingoH, 0.0f, 4000.0f);
      else if (key == "board") board = value;
      else if (key == "bridge") bridge = asBool(value, bridge);
      else if (key == "bridge_key") bridgeKey = value;
      else if (key == "replay_dir") replayDir = value;
      else if (key == "menu_page") menuPage = asInt(value, menuPage, 0, 16);
      else if (key == "panel_images") panelImages = asBool(value, panelImages);
      else if (key == "toggle_key") {
        // A virtual-key code. Mouse buttons and nonsense keep F9: a menu bound
        // to the left mouse button opens on every click in the game.
        const int vk = static_cast<int>(asAddress(value, static_cast<uintptr_t>(toggleKey)));
        if (vk >= 0x08 && vk <= 0xFE) toggleKey = vk;
      }
    } else if (section == "debug") {
      // Where the calibration lives. Its own section because it is the one
      // thing in here a player is ever told to type by hand.
      if (key == "time_chain") timeChain = value;
      else if (key == "time_address") timeAddress = value;
    } else if (section == "offsets") {
      // An escape hatch, not a normal thing to need: somebody on a build the
      // mod does not recognise can describe it here instead of waiting for a
      // release. Validated like every other profile before it is trusted.
      haveCustom = true;
      offsetLines.push_back(key + " = " + value);
      if (key == "name") custom.name = value;
      else if (key == "app") custom.app = asAddress(value, 0);
      else if (key == "get_id_name") custom.getIdName = asAddress(value, 0);
      else if (key == "challenge") custom.challenge = asAddress(value, 0);
      else if (key == "race") custom.race = asAddress(value, 0);
      else if (key == "challenge_uid") custom.challengeUid = asAddress(value, 0);
      else if (key == "challenge_name") custom.challengeName = asAddress(value, 0);
      else if (key == "race_player_info") custom.racePlayerInfo = asAddress(value, 0);
      else if (key == "player_info_player") custom.playerInfoPlayer = asAddress(value, 0);
      else if (key == "player_sub") custom.playerSub = asAddress(value, 0);
      else if (key == "player_state") custom.playerState = asAddress(value, 0);
      else if (key == "player_time") custom.playerTime = asAddress(value, 0);
    }
  }

  if (haveCustom && custom.app && custom.getIdName && custom.challenge) {
    if (custom.name.empty()) custom.name = "custom";
    game::addProfile(custom);
  }
}

namespace {

/*
 * Writing the file happens on a thread of its own, newest text wins.
 *
 * save() is called from the game's own render thread - letting go of a dragged
 * panel saves its position - and on the machine this was measured on, writing
 * one small file in Documents took anywhere from 2 to 50 seconds. On the render
 * thread that is the game frozen for as long. So save() only builds the text
 * and hands it over; whatever the disk costs is paid here, and several saves in
 * a row collapse into one write of the last.
 *
 * The text goes to a temp file that is then moved over the old one, so a crash
 * or a power cut mid-write leaves the previous file whole rather than a
 * truncated one - which used to cost the token, and with it the machine link.
 */
std::mutex g_saveLock;
std::condition_variable g_saveReady;
std::wstring g_savePath;
std::string g_saveText;
bool g_savePending = false;
bool g_saverStarted = false;

void saver() {
  for (;;) {
    std::wstring path;
    std::string text;
    {
      std::unique_lock<std::mutex> guard(g_saveLock);
      g_saveReady.wait(guard, [] { return g_savePending; });
      path = g_savePath;
      text = g_saveText;
      g_savePending = false;
    }
    const std::wstring temp = path + L".tmp";
    bool written = false;
    {
      std::ofstream file(temp.c_str(), std::ios::trunc | std::ios::binary);
      if (file) {
        file << text;
        file.flush();
        written = static_cast<bool>(file);
      }
    }
    if (!written || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(temp.c_str());
    }
  }
}

}  // namespace

void Config::save() const {
  std::ostringstream file;
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    file << "; 100% TMX - game mod.\n"
         << "; Delete this file to forget the machine link entirely.\n\n"
         << "[site]\n"
         << "url = " << baseUrl << "\n"
         << "token = " << token << "\n\n"
         << "[mod]\n"
         << "share = " << (shareWhatIAmPlaying ? "true" : "false") << "\n"
         << "auto_submit = " << (autoSubmitSelfReported ? "true" : "false") << "\n"
         << "overlay = " << (overlay ? "true" : "false") << "\n"
         << "corner = " << overlayCorner << "\n"
         << "scale = " << overlayScale << "\n"
         << "alpha = " << overlayAlpha << "\n"
         << "x = " << overlayX << "\n"
         << "y = " << overlayY << "\n"
         << "w = " << overlayW << "\n"
         << "h = " << overlayH << "\n"
         << "bingo_x = " << bingoX << "\n"
         << "bingo_y = " << bingoY << "\n"
         << "bingo_w = " << bingoW << "\n"
         << "bingo_h = " << bingoH << "\n"
         << "input = " << panelInput << "\n"
         << "board = " << board << "\n"
         << "toggle_key = " << toggleKey << "\n"
         << "menu_page = " << menuPage << "\n"
         << "panel_images = " << (panelImages ? "true" : "false") << "\n"
         << "bridge = " << (bridge ? "true" : "false") << "\n"
         << "bridge_key = " << bridgeKey << "\n"
         << "replay_dir = " << replayDir << "\n";

    // Written back so a calibration survives a restart - which is the whole
    // point of deriving a chain rather than keeping an address. `time_address`
    // is deliberately never written: it is consumed once, and by the next run
    // of the game it is a lie.
    if (!timeChain.empty()) {
      file << "\n[debug]\n"
           << "; How this build reaches the race clock, worked out from an address\n"
           << "; that was proved to hold it. Delete the line to calibrate again.\n"
           << "time_chain = " << timeChain << "\n";
    }

    if (!offsetLines.empty()) {
      file << "\n[offsets]\n";
      for (const std::string& line : offsetLines) file << line << "\n";
    }
  }

  {
    std::lock_guard<std::mutex> guard(g_saveLock);
    g_savePath = configPathW();
    g_saveText = file.str();
    g_savePending = true;
    if (!g_saverStarted) {
      g_saverStarted = true;
      std::thread(saver).detach();
    }
  }
  g_saveReady.notify_one();
}

}  // namespace tmx
