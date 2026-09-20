// The replay bridge: a door on 127.0.0.1 that the browser extension knocks on.
//
// TMX has no upload API. `POST /api/replays/upload` is authenticated by the
// site's own session cookie and guarded by an antiforgery token minted for a
// page on the exchange's origin - so the only thing that can upload for a
// player is something already running in their browser, signed in as them.
// That rules out this mod posting the replay itself, and it rules out asking
// for a TMX password, which is the part that matters: nothing here ever holds
// anybody's login.
//
// What the mod *can* do is hand the file over. After a finish on a map that is
// still open, the newest autosave is queued here; the 100% TMX browser
// extension long-polls for it, uploads it from TMX's own origin with the
// player's own session, and posts the outcome back so the overlay can say what
// happened. See docs/replay-upload-plan.md in the website repository.
//
// Off unless the player turns it on. While it is off no socket is opened at
// all - a listening port nobody asked for is not a feature.
#pragma once

#include <string>

namespace tmx {
namespace bridge {

/** Start listening, if the setting is on. Safe to call when already running. */
void start();

/** Stop and close the socket. Safe to call when not running. */
void stop();

/** Turn the setting on or off and start or stop accordingly. */
void setEnabled(bool on);

struct Status {
  bool running = false;
  int port = 0;
  /** A client has spoken to us at least once with the right key. */
  bool paired = false;
  /** Replays waiting to be collected. */
  int queued = 0;
  /** What happened to the last one, in plain words. Empty until something has. */
  std::string lastResult;
};

Status status();

/**
 * Offer the replay just driven, if one can be found.
 *
 * Called on the worker thread after a finish. Looks for an autosave written in
 * the last few seconds and, where it can, confirms the map UID appears in the
 * file before queueing it - the newest file is almost always the right one,
 * but "almost always" is not what should decide which replay is uploaded under
 * somebody's name.
 *
 * Returns false when nothing suitable was found, which is the ordinary case on
 * a machine with autosaves switched off.
 */
bool offerFinish(const std::string& site,
                 int trackId,
                 const std::string& mapName,
                 const std::string& uid,
                 int timeMs);

}  // namespace bridge
}  // namespace tmx
