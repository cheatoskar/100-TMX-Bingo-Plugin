// A log file, because a mod inside somebody else's game is otherwise a black
// box.
//
// `Documents\100TMX\log.txt`, appended, flushed on every line, and truncated
// when it grows past a megabyte. It is the difference between "nothing
// happens" and "the DLL was never loaded" / "the hook installed but the game
// never called EndScene" - three completely different bugs that look identical
// from the outside.
//
// Everything also goes to OutputDebugString, so DebugView shows it live.
#pragma once

namespace tmx {
namespace log {

void line(const char* format, ...);

/** Logs only when `key`'s message changes, for anything on a poll loop. */
void once(const char* key, const char* format, ...);

}  // namespace log
}  // namespace tmx
