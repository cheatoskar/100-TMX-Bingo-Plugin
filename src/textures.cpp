#include "textures.h"

#include <windows.h>
#include <d3d9.h>

#include <deque>
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

// A board is 25 tiles; this leaves room for the previous board's images to age
// out rather than being thrown away the moment somebody switches.
constexpr size_t kMaxTextures = 40;
constexpr int kWidth = 160;
constexpr int kHeight = 90;

struct Decoded {
  int trackId = 0;
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
};

std::mutex g_lock;
std::deque<Decoded> g_ready;                 // decoded, waiting for a frame
std::map<int, IDirect3DTexture9*> g_cache;   // live textures, render thread only
std::map<int, bool> g_wanted;                // asked for, so we ask only once
std::deque<int> g_order;                     // insertion order, for eviction

void fetch(int trackId, std::string url) {
  // Qualified: this namespace has its own `get` and `request`, and the one
  // meant here is the HTTP one a level up.
  Response res = ::tmx::get(url);
  if (!res.ok || res.status != 200 || res.body.empty()) {
    log::once(("img" + std::to_string(trackId)).c_str(), "map image %d: not available (HTTP %d)", trackId,
              res.status);
    return;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  // Forced to four channels: D3DFMT_A8R8G8B8 is what the texture below wants,
  // and a map screenshot that happens to be greyscale must not take a second
  // code path.
  unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(res.body.data()),
                                                static_cast<int>(res.body.size()), &width, &height, &channels, 4);
  if (!pixels || width <= 0 || height <= 0) {
    log::once(("dec" + std::to_string(trackId)).c_str(), "map image %d: could not be decoded", trackId);
    if (pixels) stbi_image_free(pixels);
    return;
  }

  Decoded decoded;
  decoded.trackId = trackId;
  decoded.width = width;
  decoded.height = height;
  decoded.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
  stbi_image_free(pixels);

  std::lock_guard<std::mutex> guard(g_lock);
  // Bounded like everything else here: if the render thread is not draining
  // these, the answer is to stop fetching, not to grow.
  if (g_ready.size() < 8) g_ready.push_back(std::move(decoded));
}

void evictIfNeeded() {
  while (g_order.size() > kMaxTextures) {
    const int oldest = g_order.front();
    g_order.pop_front();
    auto it = g_cache.find(oldest);
    if (it != g_cache.end()) {
      if (it->second) it->second->Release();
      g_cache.erase(it);
    }
    std::lock_guard<std::mutex> guard(g_lock);
    g_wanted.erase(oldest);
  }
}

}  // namespace

void request(int trackId, const std::string& url) {
  if (trackId <= 0 || url.empty()) return;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_wanted.count(trackId)) return;
    g_wanted[trackId] = true;
  }
  // One short-lived thread per image rather than a queue and a pool: a board is
  // twenty-five of these, once, and each one ends when its download does.
  std::thread(fetch, trackId, url).detach();
}

void* get(IDirect3DDevice9* device, int trackId) {
  if (!device) return nullptr;

  // One upload per frame. Creating a texture and copying a megabyte into it is
  // not free, and twenty-five at once is a visible hitch on the frame somebody
  // opened the panel.
  Decoded pending;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_ready.empty()) {
      pending = std::move(g_ready.front());
      g_ready.pop_front();
    }
  }

  if (pending.trackId != 0 && !g_cache.count(pending.trackId)) {
    IDirect3DTexture9* texture = nullptr;
    if (SUCCEEDED(device->CreateTexture(kWidth, kHeight, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                        &texture, nullptr)) &&
        texture) {
      D3DLOCKED_RECT locked{};
      if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD))) {
        // Nearest-neighbour down to a thumbnail: this is a 160x90 preview in a
        // tooltip, and pulling in a resampler for it would be the tail wagging
        // the dog.
        auto* out = static_cast<unsigned char*>(locked.pBits);
        for (int y = 0; y < kHeight; y++) {
          const int sy = pending.height * y / kHeight;
          for (int x = 0; x < kWidth; x++) {
            const int sx = pending.width * x / kWidth;
            const unsigned char* src = &pending.rgba[(static_cast<size_t>(sy) * pending.width + sx) * 4];
            unsigned char* dst = out + static_cast<size_t>(y) * locked.Pitch + static_cast<size_t>(x) * 4;
            dst[0] = src[2];  // B
            dst[1] = src[1];  // G
            dst[2] = src[0];  // R
            dst[3] = 255;
          }
        }
        texture->UnlockRect(0);
        g_cache[pending.trackId] = texture;
        g_order.push_back(pending.trackId);
        evictIfNeeded();
      } else {
        texture->Release();
      }
    }
  }

  auto it = g_cache.find(trackId);
  return it == g_cache.end() ? nullptr : static_cast<void*>(it->second);
}

void releaseAll() {
  for (auto& [id, texture] : g_cache) {
    if (texture) texture->Release();
  }
  g_cache.clear();
  g_order.clear();

  std::lock_guard<std::mutex> guard(g_lock);
  g_ready.clear();
  // Wanted is cleared too: after a device reset the images have to be uploaded
  // again, and "we already asked" would mean an empty board forever.
  g_wanted.clear();
}

}  // namespace textures
}  // namespace tmx
