// Map screenshots, from TMX to the screen.
//
// Three threads' worth of care in one small interface. A fetcher downloads the
// JPEG (never the render thread - that is the whole rule of this mod), decodes
// and scales it right there, and only the render thread may touch D3D9, so the
// finished pixels wait in a queue until the next frame picks them up.
//
// Keyed by the image's URL, not the track id: track ids are per exchange, and a
// TMNF #123 and a TMUF #123 are two different maps with two different pictures.
//
// The cache is small and bounded on purpose: a 5x5 board is 25 images, and a
// mod that quietly grows its VRAM use while somebody plays is a mod that gets
// blamed for the stutter.
#pragma once

#include <string>

struct IDirect3DDevice9;

namespace tmx {
namespace textures {

/** Any thread: fetch this image if we do not have it yet. */
void request(const std::string& url);

/**
 * Render thread, once per frame before any get(): uploads what has finished
 * downloading (two images at most, so a board fills in over a few frames
 * instead of stalling one) and notices a device the game has recreated.
 */
void beginFrame(IDirect3DDevice9* device);

/** Render thread: the texture for an image URL, or nullptr while it is still coming. */
void* get(const std::string& url);

/** What a picture is doing, so its placeholder can say so. */
enum class Status { None, Loading, Ready, Failed };
Status status(const std::string& url);

/** On shutdown: every texture belongs to the device. */
void releaseAll();

}  // namespace textures
}  // namespace tmx
