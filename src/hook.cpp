// Getting a frame to draw in.
//
// Direct3D 9 dispatches through a vtable that every device of that class
// shares, so swapping two entries in it is enough to be called on every frame -
// no inline patching, no trampoline allocation, no MinHook. The vtable itself
// is read from a throwaway device created on a hidden window, which is the
// standard trick (kiero does the same) and never touches the game's own device.
//
// Two entries are taken: EndScene, where the overlay is drawn, and Reset, where
// the ImGui backend has to let go of its device objects before the device is
// recreated - skip that one and alt-tabbing out of fullscreen is a crash.
#include <windows.h>
#include <d3d9.h>

#include "hook.h"
#include "overlay.h"

#pragma comment(lib, "d3d9.lib")

namespace tmx {
namespace hook {
namespace {

using EndSceneFn = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using ResetFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

// IDirect3DDevice9's vtable layout, which has not changed since 2004.
constexpr int kResetIndex = 16;
constexpr int kEndSceneIndex = 42;

void** g_vtable = nullptr;
EndSceneFn g_endScene = nullptr;
ResetFn g_reset = nullptr;

bool writePointer(void** slot, void* value) {
  DWORD previous = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) return false;
  *slot = value;
  VirtualProtect(slot, sizeof(void*), previous, &previous);
  return true;
}

HRESULT APIENTRY endSceneDetour(IDirect3DDevice9* device) {
  // Everything the overlay does is bounded and local: it reads a copy of the
  // shared state and draws. It never waits on the network - that is the whole
  // reason the worker thread exists.
  overlay::draw(device);
  return g_endScene(device);
}

HRESULT APIENTRY resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
  overlay::invalidate();
  return g_reset(device, params);
}

// A device that exists only to be asked where its methods live.
IDirect3DDevice9* makeProbeDevice(IDirect3D9* d3d, HWND window) {
  D3DPRESENT_PARAMETERS params{};
  params.Windowed = TRUE;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = window;
  params.BackBufferFormat = D3DFMT_UNKNOWN;

  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &params, &device);
  if (FAILED(hr)) {
    // Some drivers refuse a HAL device on a hidden window; a reference device
    // has the same vtable, which is all that is being read here.
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, window,
                           D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &params, &device);
  }
  return SUCCEEDED(hr) ? device : nullptr;
}

}  // namespace

bool install() {
  if (g_vtable) return true;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"TmxProbeWindow";
  if (!RegisterClassExW(&wc)) return false;

  HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!window) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }

  bool done = false;
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (d3d) {
    if (IDirect3DDevice9* device = makeProbeDevice(d3d, window)) {
      g_vtable = *reinterpret_cast<void***>(device);
      g_endScene = reinterpret_cast<EndSceneFn>(g_vtable[kEndSceneIndex]);
      g_reset = reinterpret_cast<ResetFn>(g_vtable[kResetIndex]);

      done = writePointer(&g_vtable[kEndSceneIndex], reinterpret_cast<void*>(&endSceneDetour)) &&
             writePointer(&g_vtable[kResetIndex], reinterpret_cast<void*>(&resetDetour));

      device->Release();
    }
    d3d->Release();
  }

  DestroyWindow(window);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);

  if (!done) g_vtable = nullptr;
  return done;
}

void remove() {
  if (!g_vtable) return;
  if (g_endScene) writePointer(&g_vtable[kEndSceneIndex], reinterpret_cast<void*>(g_endScene));
  if (g_reset) writePointer(&g_vtable[kResetIndex], reinterpret_cast<void*>(g_reset));
  g_vtable = nullptr;
}

}  // namespace hook
}  // namespace tmx
