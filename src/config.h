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
  /**
   * How to reach the race clock on this build: "0x1F4/0x28/0x2BC".
   *
   * Everything else about reading the game is a table of offsets copied from
   * somebody else's executable. This one is *derived* - from an address the
   * player proved was the clock, either in Cheat Engine or by telling the mod
   * the time it had just shown them - and it is the only one that has ever been
   * checked against this machine rather than assumed about it.
   *
   * Empty means "not calibrated yet", and the old guesswork still runs.
   */
  std::string timeChain;

  /**
   * A one-shot: an address known to hold the clock, to derive `timeChain` from.
   *
   * Consumed and cleared - a heap address is true for one run of the game and
   * misleading afterwards, which is exactly the kind of stale fact that makes
   * this whole area so hard to debug.
   */
  std::string timeAddress;

  int toggleKey = 0x78;  // VK_F9

  /**
   * Let the browser extension collect finished replays and upload them.
   *
   * Off by default and opened by hand, because it is the one switch here that
   * makes the mod listen on a socket rather than only speak. What it serves is
   * a replay the player just drove, to a caller holding `bridgeKey`, on
   * loopback only - see bridge.cpp.
   */
  bool bridge = false;

  /** The pairing secret, generated the first time the bridge is switched on. */
  std::string bridgeKey;

  /**
   * Where TrackMania saved it, when it is not the usual place.
   *
   * Default is `Documents\TmForever\Tracks\Replays\Autosaves`. A player with a
   * redirected Documents folder or a `-userdir` of their own names it here
   * rather than going without.
   */
  std::string replayDir;

  std::string path() const;
  void load();
  void save() const;
};

Config& config();

}  // namespace tmx
