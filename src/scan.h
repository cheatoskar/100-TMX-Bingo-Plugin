// Finding a pointer chain to an address we already know is right.
//
// Everything before this guessed: a table of offsets copied from another build,
// then a brute-force hunt for "an int that ticks like a clock". Both kept
// landing on the wrong number - the map clock instead of the race clock, or
// nothing at all - because neither had anything to check itself against.
//
// This has. Given one address that is known to hold the time (found in Cheat
// Engine, or found here by scanning for a time the player typed in), it walks
// the object graph from the game's own root and reports how to get there:
// `app + 0x1F4 -> +0x28 -> +0x2BC`. That chain is what survives a restart, and
// what can be written into the ini and shipped as a profile.
//
// It is a debugging tool that happens to live in the product, which is the
// point: the build that cannot be read is always somebody else's.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tmx {
namespace scan {

/** One way to reach an address: root -> +hops[0] -> +hops[1] ... + delta. */
struct Chain {
  std::vector<uintptr_t> hops;
  /** Offset inside the object the last hop lands on. */
  uintptr_t delta = 0;

  /** "0x1F4/0x28/0x2BC" - the form the ini takes and the log prints. */
  std::string text() const;
};

/**
 * Walk the object graph from `root` and find ways to reach `target`.
 *
 * Breadth-first, so the shortest chains come first, and bounded hard on both
 * axes - this runs on the worker thread while somebody is driving, and an
 * unbounded graph walk through a game's heap is a freeze, not a search.
 */
std::vector<Chain> findChains(uintptr_t root, uintptr_t target, int maxDepth = 4, size_t maxVisited = 60000);

/** Resolve a chain against the current process. 0 when any hop is stale. */
uintptr_t resolve(uintptr_t root, const Chain& chain);

/** Parse "0x1F4/0x28/0x2BC" back into a chain. Empty hops means "no chain". */
Chain parse(const std::string& text);

/**
 * Every readable address holding exactly this 32-bit value.
 *
 * For the calibration that needs no Cheat Engine: the player types the time the
 * game just showed, and the addresses holding it are where the clock lives.
 * Capped, because a small number like 1000 matches half the heap.
 */
std::vector<uintptr_t> findValue(int value, size_t maxHits = 4000);

/** How many of those are still holding it - used to narrow between two runs. */
std::vector<uintptr_t> keepHolding(const std::vector<uintptr_t>& candidates, int value);

}  // namespace scan
}  // namespace tmx
