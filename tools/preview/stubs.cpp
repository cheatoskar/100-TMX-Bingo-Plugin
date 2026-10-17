// What the overlay calls outside itself, standing in for the game.
//
// The previewer links the real overlay, textures and HTTP, and nothing that
// reads TrackMania, opens a socket or touches Documents\100TMX: the config and
// the log are stand-ins too, so a preview can never overwrite somebody's real
// config.ini - the one holding their machine token.
#include <cstdarg>
#include <cstdio>
#include <string>

#include "bridge.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "state.h"
#include "worker.h"

namespace tmx {

Shared& shared() {
  static Shared instance;
  return instance;
}

Config& config() {
  static Config instance;
  return instance;
}
std::string Config::path() const { return "(preview - nothing is saved)"; }
void Config::load() {}
void Config::save() const {}

namespace log {
void line(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  printf("\n");
}
void once(const char*, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  printf("\n");
}
}  // namespace log

namespace bridge {
bool g_previewPairing = false;
Status status() {
  Status out;
  out.running = true;
  out.port = 47615;
  out.paired = true;
  out.pairing = g_previewPairing;
  return out;
}
void approvePairing() {}
void refusePairing() {}
void setEnabled(bool) {}
}  // namespace bridge

namespace worker {
void releaseNow() {}
}  // namespace worker

}  // namespace tmx
