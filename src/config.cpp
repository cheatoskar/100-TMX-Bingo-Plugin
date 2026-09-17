#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "game.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace tmx {
namespace {

std::string documentsDir() {
  wchar_t* raw = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw) {
    int size = WideCharToMultiByte(CP_UTF8, 0, raw, -1, nullptr, 0, nullptr, nullptr);
    out.resize(static_cast<size_t>(size > 0 ? size - 1 : 0));
    WideCharToMultiByte(CP_UTF8, 0, raw, -1, &out[0], size, nullptr, nullptr);
  }
  if (raw) CoTaskMemFree(raw);
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

// "0x972EB8" or "9908920" - the ini is where somebody adds their own build, so
// it takes the form the disassembler shows them.
uintptr_t asAddress(const std::string& v, uintptr_t fallback) {
  if (v.empty()) return fallback;
  return static_cast<uintptr_t>(strtoul(v.c_str(), nullptr, v.rfind("0x", 0) == 0 ? 16 : 10));
}

Config g_config;

}  // namespace

Config& config() { return g_config; }

std::string Config::path() const {
  std::string dir = documentsDir();
  if (dir.empty()) return "100TMX-config.ini";  // next to the exe: better than nowhere
  dir += "\\100TMX";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\config.ini";
}

void Config::load() {
  std::ifstream file(path());
  if (!file) return;

  std::string section;
  std::string line;
  game::Offsets custom;
  bool haveCustom = false;

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
      else if (key == "overlay") overlay = asBool(value, overlay);
      else if (key == "corner") overlayCorner = atoi(value.c_str());
      else if (key == "scale") overlayScale = static_cast<float>(atof(value.c_str()));
      else if (key == "alpha") overlayAlpha = static_cast<float>(atof(value.c_str()));
      else if (key == "w") overlayW = static_cast<float>(atof(value.c_str()));
      else if (key == "h") overlayH = static_cast<float>(atof(value.c_str()));
      else if (key == "input") panelInput = atoi(value.c_str());
      else if (key == "x") overlayX = static_cast<float>(atof(value.c_str()));
      else if (key == "y") overlayY = static_cast<float>(atof(value.c_str()));
      else if (key == "board") board = value;
      else if (key == "toggle_key") toggleKey = static_cast<int>(asAddress(value, static_cast<uintptr_t>(toggleKey)));
    } else if (section == "offsets") {
      // An escape hatch, not a normal thing to need: somebody on a build the
      // mod does not recognise can describe it here instead of waiting for a
      // release. Validated like every other profile before it is trusted.
      haveCustom = true;
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

void Config::save() const {
  std::ofstream file(path(), std::ios::trunc);
  if (!file) return;

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
       << "input = " << panelInput << "\n"
       << "board = " << board << "\n"
       << "toggle_key = " << toggleKey << "\n";
}

}  // namespace tmx
