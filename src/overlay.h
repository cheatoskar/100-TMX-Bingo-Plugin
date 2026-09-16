// What you see while you drive.
#pragma once

#include <windows.h>

struct IDirect3DDevice9;

namespace tmx {
namespace overlay {

// Called from inside the game's own EndScene, on the render thread.
void draw(IDirect3DDevice9* device);

// Around the device being reset (alt-tab, resolution change): the ImGui DX9
// backend holds device objects and must let go of them first.
void invalidate();
void shutdown();

// True while the panel is taking input, so the game does not also act on it.
bool capturingInput();

// The window procedure the game's window is subclassed with.
LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

}  // namespace overlay
}  // namespace tmx
