#include "scan.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "log.h"

namespace tmx {
namespace scan {
namespace {

/**
 * The readable parts of this process, asked once per search.
 *
 * Reading through SEH costs nothing when it succeeds, but a graph walk does
 * tens of millions of reads and most of the addresses it tries are not
 * mapped - and an access violation is expensive. Knowing the map up front
 * turns "try it and catch the fault" into a binary search.
 */
struct Region {
  uintptr_t base;
  uintptr_t end;
};

std::vector<Region> readableRegions() {
  std::vector<Region> out;
  SYSTEM_INFO si{};
  GetSystemInfo(&si);

  uintptr_t address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
  const uintptr_t limit = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

  MEMORY_BASIC_INFORMATION info{};
  while (address < limit && VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == sizeof(info)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t size = info.RegionSize;

    const bool readable = info.State == MEM_COMMIT &&
                          (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
                          (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;

    if (readable) out.push_back({base, base + size});
    address = base + size;
    if (size == 0) break;  // VirtualQuery should never say this; do not spin if it does
  }

  std::sort(out.begin(), out.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
  return out;
}

class Memory {
 public:
  Memory() : regions_(readableRegions()) {}

  /** Bytes readable from `address` onwards, 0 when it is not mapped at all. */
  size_t span(uintptr_t address) const {
    if (address < 0x10000) return 0;
    auto it = std::upper_bound(regions_.begin(), regions_.end(), address,
                               [](uintptr_t value, const Region& r) { return value < r.base; });
    if (it == regions_.begin()) return 0;
    --it;
    return address < it->end ? static_cast<size_t>(it->end - address) : 0;
  }

  bool readInt(uintptr_t address, int* out) const {
    if (span(address) < sizeof(int)) return false;
    memcpy(out, reinterpret_cast<const void*>(address), sizeof(int));
    return true;
  }

  bool readPtr(uintptr_t address, uintptr_t* out) const {
    if (span(address) < sizeof(uintptr_t)) return false;
    memcpy(out, reinterpret_cast<const void*>(address), sizeof(uintptr_t));
    return true;
  }

  const std::vector<Region>& regions() const { return regions_; }

 private:
  std::vector<Region> regions_;
};

/** A pointer worth following: mapped, aligned, and not obviously a small int. */
bool looksLikePointer(const Memory& memory, uintptr_t value) {
  return value >= 0x10000 && (value & 3) == 0 && memory.span(value) >= 0x40;
}

/** How far past an object's start a field can sit and still be called part of it. */
constexpr uintptr_t kFieldWindow = 0x1000;

/** How wide each object is searched for further pointers. */
constexpr uintptr_t kHopWindow = 0x400;

}  // namespace

std::string Chain::text() const {
  std::string out;
  for (uintptr_t hop : hops) {
    char buf[16];
    sprintf_s(buf, sizeof(buf), "0x%X", static_cast<unsigned>(hop));
    if (!out.empty()) out += "/";
    out += buf;
  }
  char tail[16];
  sprintf_s(tail, sizeof(tail), "0x%X", static_cast<unsigned>(delta));
  if (!out.empty()) out += "/";
  out += tail;
  return out;
}

Chain parse(const std::string& text) {
  Chain chain;
  if (text.empty()) return chain;

  std::vector<uintptr_t> parts;
  size_t at = 0;
  while (at < text.size()) {
    size_t next = text.find('/', at);
    if (next == std::string::npos) next = text.size();
    const std::string piece = text.substr(at, next - at);
    if (!piece.empty()) {
      const bool hex = piece.rfind("0x", 0) == 0 || piece.rfind("0X", 0) == 0;
      parts.push_back(static_cast<uintptr_t>(strtoul(piece.c_str(), nullptr, hex ? 16 : 10)));
    }
    at = next + 1;
  }

  // The last number is the field inside the final object; everything before it
  // is a pointer to follow. A single number is a field on the root itself.
  if (parts.empty()) return chain;
  chain.delta = parts.back();
  parts.pop_back();
  chain.hops = parts;
  return chain;
}

uintptr_t resolve(uintptr_t root, const Chain& chain) {
  if (!root) return 0;
  Memory memory;

  uintptr_t at = root;
  for (uintptr_t hop : chain.hops) {
    uintptr_t next = 0;
    if (!memory.readPtr(at + hop, &next) || !looksLikePointer(memory, next)) return 0;
    at = next;
  }
  return memory.span(at + chain.delta) >= sizeof(int) ? at + chain.delta : 0;
}

std::vector<Chain> findChains(uintptr_t root, uintptr_t target, int maxDepth, size_t maxVisited) {
  std::vector<Chain> found;
  if (!root || !target) return found;

  const Memory memory;
  if (!memory.span(target)) {
    log::line("scan: %p is not readable in this process - is the game still on that run?", (void*)target);
    return found;
  }

  struct Node {
    uintptr_t addr;
    int parent;
    uintptr_t offset;
    int depth;
  };

  std::vector<Node> nodes;
  std::unordered_set<uintptr_t> seen;
  std::deque<int> queue;

  nodes.push_back({root, -1, 0, 0});
  seen.insert(root);
  queue.push_back(0);

  const DWORD started = GetTickCount();

  while (!queue.empty() && nodes.size() < maxVisited) {
    const int index = queue.front();
    queue.pop_front();
    const Node node = nodes[index];

    // Is the answer inside this object? Objects overlap in a graph walk, so
    // this is checked on every node rather than only on leaves.
    if (target >= node.addr && target - node.addr < kFieldWindow) {
      Chain chain;
      chain.delta = target - node.addr;
      for (int walk = index; walk > 0; walk = nodes[walk].parent) chain.hops.push_back(nodes[walk].offset);
      std::reverse(chain.hops.begin(), chain.hops.end());
      found.push_back(chain);
      if (found.size() >= 6) break;
      continue;  // no point walking deeper through the object we were looking for
    }

    if (node.depth >= maxDepth) continue;

    const size_t reach = (std::min)(static_cast<size_t>(kHopWindow), memory.span(node.addr));
    for (uintptr_t offset = 0; offset + sizeof(uintptr_t) <= reach; offset += 4) {
      uintptr_t next = 0;
      memcpy(&next, reinterpret_cast<const void*>(node.addr + offset), sizeof(next));
      if (!looksLikePointer(memory, next)) continue;
      if (!seen.insert(next).second) continue;

      nodes.push_back({next, index, offset, node.depth + 1});
      queue.push_back(static_cast<int>(nodes.size()) - 1);
      if (nodes.size() >= maxVisited) break;
    }
  }

  log::line("scan: walked %zu objects in %lu ms, found %zu chain%s to %p", nodes.size(), GetTickCount() - started,
            found.size(), found.size() == 1 ? "" : "s", (void*)target);
  return found;
}

std::vector<uintptr_t> findValue(int value, size_t maxHits) {
  std::vector<uintptr_t> hits;
  const Memory memory;
  const DWORD started = GetTickCount();

  for (const Region& region : memory.regions()) {
    // Whole modules and mapped files are skipped: a race time lives on the
    // heap, and scanning the game's code for it is most of the work for none
    // of the answer.
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(region.base), &info, sizeof(info)) != sizeof(info)) continue;
    if (info.Type != MEM_PRIVATE) continue;

    for (uintptr_t at = region.base; at + sizeof(int) <= region.end; at += 4) {
      int here = 0;
      memcpy(&here, reinterpret_cast<const void*>(at), sizeof(here));
      if (here != value) continue;
      hits.push_back(at);
      if (hits.size() >= maxHits) {
        log::line("scan: stopped at %zu addresses holding %d", hits.size(), value);
        return hits;
      }
    }
  }

  log::line("scan: %zu addresses hold %d (%lu ms)", hits.size(), value, GetTickCount() - started);
  return hits;
}

std::vector<uintptr_t> keepHolding(const std::vector<uintptr_t>& candidates, int value) {
  std::vector<uintptr_t> kept;
  const Memory memory;
  for (uintptr_t address : candidates) {
    int here = 0;
    if (memory.readInt(address, &here) && here == value) kept.push_back(address);
  }
  return kept;
}

}  // namespace scan
}  // namespace tmx
