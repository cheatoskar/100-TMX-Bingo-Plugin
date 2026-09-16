// Map screenshots, from TMX to the screen.
//
// Three threads' worth of care in one small interface. The worker downloads the
// JPEG (never the render thread - that is the whole rule of this mod), decoding
// happens wherever the bytes land, and only the render thread may touch D3D9,
// so the finished pixels wait in a queue until the next frame picks them up.
//
// The cache is small and bounded on purpose: a 5x5 board is 25 images, and a
// mod that quietly grows its VRAM use while somebody plays is a mod that gets
// blamed for the stutter.
#pragma once

#include <string>

struct IDirect3DDevice9;

namespace tmx {
namespace textures {

/** Called from the worker: fetch this image if we do not have it yet. */
void request(int trackId, const std::string& url);

/**
 * The texture for a map, or nullptr while it is still coming.
 *
 * Render thread only. Uploads at most one pending image per frame, so a board
 * of twenty-five fills in over half a second instead of stalling one frame.
 */
void* get(IDirect3DDevice9* device, int trackId);

/** Before a device reset, and on shutdown: every texture belongs to that device. */
void releaseAll();

}  // namespace textures
}  // namespace tmx
