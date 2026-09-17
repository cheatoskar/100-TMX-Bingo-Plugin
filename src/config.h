// Everything the mod remembers between sessions.
//
// One ini in Documents\100TMX\config.ini, the way Twinkie keeps its own - a
// plain text file somebody can read, edit or delete, which matters when the
// thing it holds is a token that speaks for their account.
#pragma once

#include <string>

namespace tmx {

struct Config {
  // Where the site is. Overridable so the mod can be pointed at a dev server
  // without a rebuild.
  std::string baseUrl = "https://100tmx.com";

  // The bearer token from the device flow. Empty until a machine is linked.
  std::string token;

  /**
   * Share which map is loaded, so it shows on the remaining list.
   *
   * On by default now, at the project's request: the overlay is installed by
   * people playing the project's own boards, and a map nobody can see somebody
   * is on is the thing the remaining list exists to prevent. It is still one
   * switch, still in plain words in the settings window, and still the thing
   * that decides whether anything at all leaves the machine - the first-run
   * prompt now says it is on rather than asking to turn it on.
   */
  bool shareWhatIAmPlaying = true;

  bool overlay = true;
  int overlayCorner = 0;     // 0 left, 1 right - only the starting side
  // Where the panel was last dragged to. NaN-ish -1 means "never moved", in
  // which case the corner above decides.
  float overlayX = -1.0f;
  float overlayY = -1.0f;
  float overlayScale = 1.0f;
  /** Size the panel was last dragged to; 0 means "as big as its contents". */
  float overlayW = 0.0f;
  float overlayH = 0.0f;
  /**
   * When the panel takes the mouse.
   *
   * 0 automatic - it takes the mouse while the game is showing a cursor, which
   *   is to say in the menus, and never mid-race.
   * 1 only while the settings window is open.
   * 2 always.
   */
  int panelInput = 0;
  float overlayAlpha = 0.85f;

  // The board the panel is showing, remembered across sessions.
  std::string board;

  /**
   * Put a finish straight onto a self-reported board without asking.
   *
   * On by default, at the project's request. The reservation that kept it off
   * still stands and is worth keeping in view: an overlay that posts on
   * somebody's behalf is how a friendly board turns sour, so the switch stays,
   * it is named in plain words, and turning it off restores the button.
   *
   * It can only ever apply to a board whose setting is "no check" or "plugin
   * and replay only" - everywhere else a tile is taken by a replay on TMX and
   * the site goes and looks.
   */
  bool autoSubmitSelfReported = true;

  // VK_F9 by default: not bound by the game, and out of the way of the keys
  // people actually drive with.
  int toggleKey = 0x78;  // VK_F9

  std::string path() const;
  void load();
  void save() const;
};

Config& config();

}  // namespace tmx
