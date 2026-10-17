#include "textures.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "http.h"
#include "log.h"

namespace tmx {
namespace textures {
namespace {

// The biggest board is 10x10, plus the map you are on and a few to spare. A
// cache smaller than the board on screen evicts pictures that are drawn every
// frame, asks for them again, and downloads the same hundred images forever.
// At 288x162 that is about 20 MB, kept once by the card and once by Windows.
constexpr size_t kMaxTextures = 112;

// 16:9, and big enough for the larger places a picture is drawn - the map
// card and the tile detail - without looking soft. The old 160x90 was sized
// for a tooltip and read as a smudge anywhere larger.
constexpr int kWidth = 288;
constexpr int kHeight = 162;

// What a map screenshot can reasonably be. stb_image is not hardened against
// hostile input, and while TMX is not hostile, a bound costs nothing.
constexpr size_t kMaxBody = 8u << 20;
constexpr int kMaxSide = 4096;

// Two downloads at a time. One thread per image used to mean twenty-five
// requests against TMX at once whenever a board opened, which is somebody
// else's server being hit by a burst it did not ask for.
constexpr int kFetchers = 2;

// A picture that failed is asked for again after this long, rather than never:
// one network hiccup used to leave a tile blank for the rest of the session.
constexpr double kRetryAfter = 60.0;

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Decoded {
  std::string url;
  std::vector<uint32_t> bgra;  // kWidth x kHeight, ready to copy
};

enum class Want { Queued, Loading, Done, Failed };
struct Wanted {
  Want state = Want::Queued;
  double since = 0;
};

std::mutex g_lock;
std::deque<std::string> g_queue;          // waiting for a fetcher
std::deque<Decoded> g_ready;              // decoded, waiting for a frame
std::map<std::string, Wanted> g_wanted;   // everything asked for, and how it went
int g_fetchers = 0;

// Render thread only.
struct Live {
  IDirect3DTexture9* texture = nullptr;
  unsigned lastUsed = 0;
};
std::map<std::string, Live> g_cache;
IDirect3DDevice9* g_device = nullptr;
unsigned g_frame = 0;

// Down to kWidth x kHeight, cropped to 16:9 from the middle and averaged rather
// than point-sampled: TMX screenshots are not all the same shape, and nearest
// neighbour turned a 1080p picture into shimmering noise at this size.
std::vector<uint32_t> scale(const unsigned char* rgba, int width, int height) {
  int cropW = width;
  int cropH = height;
  if (static_cast<int64_t>(width) * 9 > static_cast<int64_t>(height) * 16) {
    cropW = static_cast<int>(static_cast<int64_t>(height) * 16 / 9);
  } else {
    cropH = static_cast<int>(static_cast<int64_t>(width) * 9 / 16);
  }
  cropW = std::max(cropW, 1);
  cropH = std::max(cropH, 1);
  const int x0 = (width - cropW) / 2;
  const int y0 = (height - cropH) / 2;

  std::vector<uint32_t> out(static_cast<size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; y++) {
    const int sy0 = y0 + static_cast<int>(static_cast<int64_t>(y) * cropH / kHeight);
    const int sy1 = std::max(sy0 + 1, y0 + static_cast<int>(static_cast<int64_t>(y + 1) * cropH / kHeight));
    for (int x = 0; x < kWidth; x++) {
      const int sx0 = x0 + static_cast<int>(static_cast<int64_t>(x) * cropW / kWidth);
      const int sx1 = std::max(sx0 + 1, x0 + static_cast<int>(static_cast<int64_t>(x + 1) * cropW / kWidth));
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int sy = sy0; sy < sy1 && sy < height; sy++) {
        const unsigned char* row = rgba + (static_cast<size_t>(sy) * width) * 4;
        for (int sx = sx0; sx < sx1 && sx < width; sx++) {
          const unsigned char* p = row + static_cast<size_t>(sx) * 4;
          r += p[0];
          g += p[1];
          b += p[2];
          n++;
        }
      }
      if (n == 0) n = 1;
      out[static_cast<size_t>(y) * kWidth + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
  }
  return out;
}

bool fetchOne(const std::string& url, Decoded* decoded) {
  // Qualified: this namespace has its own `get` and `request`, and the one
  // meant here is the HTTP one a level up.
  Response res = ::tmx::get(url);
  if (!res.ok || res.status != 200 || res.body.empty() || res.body.size() > kMaxBody) {
    log::once(("img" + url).c_str(), "map image %s: not available (HTTP %d, %u bytes)", url.c_str(), res.status,
              static_cast<unsigned>(res.body.size()));
    return false;
  }

  const auto* bytes = reinterpret_cast<const unsigned char*>(res.body.data());
  const int length = static_cast<int>(res.body.size());
  int width = 0;
  int height = 0;
  int channels = 0;
  if (!stbi_info_from_memory(bytes, length, &width, &height, &channels) || width <= 0 || height <= 0 ||
      width > kMaxSide || height > kMaxSide) {
    log::once(("dim" + url).c_str(), "map image %s: not a picture this can show (%dx%d)", url.c_str(), width, height);
    return false;
  }

  // Forced to four channels so a greyscale screenshot takes the same path.
  unsigned char* pixels = stbi_load_from_memory(bytes, length, &width, &height, &channels, 4);
  if (!pixels) {
    log::once(("dec" + url).c_str(), "map image %s: could not be decoded", url.c_str());
    return false;
  }
  decoded->url = url;
  decoded->bgra = scale(pixels, width, height);
  stbi_image_free(pixels);
  return true;
}

// A fetcher: takes URLs off the queue until there are none, then ends. Started
// on demand by request(), at most kFetchers of them at once.
void fetchLoop() {
  for (;;) {
    std::string url;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_queue.empty()) {
        g_fetchers--;
        return;
      }
      url = std::move(g_queue.front());
      g_queue.pop_front();
      g_wanted[url] = {Want::Loading, now()};
    }

    Decoded decoded;
    const bool ok = fetchOne(url, &decoded);

    std::lock_guard<std::mutex> guard(g_lock);
    if (!ok) {
      g_wanted[url] = {Want::Failed, now()};
    } else if (g_ready.size() < 16) {
      g_ready.push_back(std::move(decoded));
      g_wanted[url] = {Want::Done, now()};
    } else {
      // Nobody is drawing them (the panel is hidden, say). Forget it rather
      // than grow: the next request asks again.
      g_wanted.erase(url);
    }
  }
}

void releaseEmbedded();  // below, with the pictures built into the DLL

void release(Live& live) {
  if (live.texture) live.texture->Release();
  live.texture = nullptr;
}

IDirect3DTexture9* upload(IDirect3DDevice9* device, const Decoded& decoded) {
  // MANAGED, not DEFAULT: the runtime keeps a copy and restores it across a
  // device reset by itself. The DEFAULT-pool version had to be thrown away on
  // every alt-tab, and the board sat blank until the next refresh fetched all
  // twenty-five pictures again.
  IDirect3DTexture9* texture = nullptr;
  if (FAILED(device->CreateTexture(kWidth, kHeight, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) ||
      !texture) {
    return nullptr;
  }
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
    texture->Release();
    return nullptr;
  }
  for (int y = 0; y < kHeight; y++) {
    memcpy(static_cast<char*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y,
           &decoded.bgra[static_cast<size_t>(y) * kWidth], static_cast<size_t>(kWidth) * 4);
  }
  texture->UnlockRect(0);
  return texture;
}

// Least recently *drawn* goes first. Evicting by age of insertion dropped the
// pictures of the board on screen - the oldest ones - in favour of whatever
// had come in last.
void evictIfNeeded() {
  while (g_cache.size() > kMaxTextures) {
    auto oldest = g_cache.begin();
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
      if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
    }
    const std::string url = oldest->first;
    release(oldest->second);
    g_cache.erase(oldest);
    std::lock_guard<std::mutex> guard(g_lock);
    g_wanted.erase(url);
  }
}

}  // namespace

void request(const std::string& url) {
  if (url.empty()) return;
  // Only the exchange's own https pictures. The URL comes from the site, and a
  // fetch is a fetch: there is no reason for this to ever go anywhere else.
  if (url.rfind("https://", 0) != 0) return;

  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_wanted.find(url);
  if (it != g_wanted.end()) {
    if (it->second.state != Want::Failed || now() - it->second.since < kRetryAfter) return;
  }
  g_wanted[url] = {Want::Queued, now()};
  g_queue.push_back(url);
  if (g_fetchers < kFetchers) {
    g_fetchers++;
    std::thread(fetchLoop).detach();
  }
}

void beginFrame(IDirect3DDevice9* device) {
  g_frame++;
  if (!device) return;
  if (device != g_device) {
    // A device the game made anew: the old textures belong to the old one.
    releaseEmbedded();
    for (auto& [url, live] : g_cache) release(live);
    g_cache.clear();
    g_device = device;
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto it = g_wanted.begin(); it != g_wanted.end();) {
      it = it->second.state == Want::Done ? g_wanted.erase(it) : std::next(it);
    }
  }

  for (int i = 0; i < 2; i++) {
    Decoded pending;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      if (g_ready.empty()) break;
      pending = std::move(g_ready.front());
      g_ready.pop_front();
    }
    if (g_cache.count(pending.url)) continue;
    if (IDirect3DTexture9* texture = upload(device, pending)) {
      g_cache[pending.url] = {texture, g_frame};
      evictIfNeeded();
    } else {
      std::lock_guard<std::mutex> guard(g_lock);
      g_wanted[pending.url] = {Want::Failed, now()};
    }
  }
}

void* get(const std::string& url) {
  auto it = g_cache.find(url);
  if (it == g_cache.end()) return nullptr;
  it->second.lastUsed = g_frame;
  return it->second.texture;
}

Status status(const std::string& url) {
  if (url.empty()) return Status::None;
  if (g_cache.count(url)) return Status::Ready;
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_wanted.find(url);
  if (it == g_wanted.end()) return Status::None;
  return it->second.state == Want::Failed ? Status::Failed : Status::Loading;
}

namespace {

struct Embedded {
  bool decoded = false;
  int width = 0, height = 0;
  std::vector<uint32_t> bgra;
  IDirect3DTexture9* texture = nullptr;
};
std::map<int, Embedded> g_embedded;

void releaseEmbedded() {
  for (auto& [id, picture] : g_embedded) {
    if (picture.texture) picture.texture->Release();
    picture.texture = nullptr;
  }
}

HMODULE selfModule() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&selfModule), &module);
  return module;
}

bool decodeResource(int resourceId, Embedded& out) {
  HMODULE module = selfModule();
  HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));  // RT_RCDATA
  HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
  const void* bytes = loaded ? LockResource(loaded) : nullptr;
  const DWORD size = found ? SizeofResource(module, found) : 0;
  if (!bytes || size == 0) return false;
  int width = 0, height = 0, channels = 0;
  unsigned char* pixels = stbi_load_from_memory(static_cast<const unsigned char*>(bytes), static_cast<int>(size), &width,
                                                &height, &channels, 4);
  if (!pixels) return false;
  out.width = width;
  out.height = height;
  out.bgra.resize(static_cast<size_t>(width) * height);
  for (size_t i = 0; i < out.bgra.size(); i++) {
    const unsigned char* p = pixels + i * 4;
    out.bgra[i] = (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[0]) << 16) |
                  (static_cast<uint32_t>(p[1]) << 8) | p[2];
  }
  stbi_image_free(pixels);
  return true;
}

}  // namespace

void* embedded(int resourceId) {
  if (!g_device) return nullptr;
  Embedded& picture = g_embedded[resourceId];
  if (!picture.decoded) {
    picture.decoded = true;  // once: a missing resource is not looked up every frame
    if (!decodeResource(resourceId, picture)) log::line("textures: embedded picture %d missing", resourceId);
  }
  if (picture.texture || picture.bgra.empty()) return picture.texture;
  IDirect3DTexture9* texture = nullptr;
  if (FAILED(g_device->CreateTexture(static_cast<UINT>(picture.width), static_cast<UINT>(picture.height), 1, 0,
                                     D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) ||
      !texture) {
    picture.bgra.clear();
    return nullptr;
  }
  D3DLOCKED_RECT locked{};
  if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
    texture->Release();
    picture.bgra.clear();
    return nullptr;
  }
  for (int y = 0; y < picture.height; y++) {
    memcpy(static_cast<char*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y,
           &picture.bgra[static_cast<size_t>(y) * picture.width], static_cast<size_t>(picture.width) * 4);
  }
  texture->UnlockRect(0);
  picture.texture = texture;
  return texture;
}

void releaseAll() {
  releaseEmbedded();
  for (auto& [url, live] : g_cache) release(live);
  g_cache.clear();
  g_device = nullptr;

  std::lock_guard<std::mutex> guard(g_lock);
  g_ready.clear();
  g_queue.clear();
  g_wanted.clear();
}

}  // namespace textures
}  // namespace tmx
