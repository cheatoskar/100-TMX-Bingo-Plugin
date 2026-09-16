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
#include "log.h"
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

bool g_drewOnce = false;

HRESULT APIENTRY endSceneDetour(IDirect3DDevice9* device) {
  if (!g_drewOnce) {
    g_drewOnce = true;
    log::line("first EndScene from the game - the overlay is live");
  }
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
//
// Creating one is the fragile step of the whole mod, and it fails for reasons
// that have nothing to do with the game: a driver that refuses a HAL device on
// an invisible window, a session with no display, an adapter in a mode the
// device cannot match. So it is not one attempt but a list of them, each logged
// with its HRESULT - and the last one, NULLREF, needs no display at all and
// exists precisely for tools that only want to read the vtable.
IDirect3DDevice9* makeProbeDevice(IDirect3D9* d3d, HWND window) {
  struct Attempt {
    const char* name;
    D3DDEVTYPE type;
    bool useDesktopWindow;
  };
  const Attempt attempts[] = {
      {"HAL", D3DDEVTYPE_HAL, false},
      {"HAL on the desktop window", D3DDEVTYPE_HAL, true},
      {"REF", D3DDEVTYPE_REF, false},
      {"NULLREF", D3DDEVTYPE_NULLREF, false},
  };

  for (const Attempt& attempt : attempts) {
    HWND target = attempt.useDesktopWindow ? GetDesktopWindow() : window;

    D3DPRESENT_PARAMETERS params{};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = target;
    params.BackBufferFormat = D3DFMT_UNKNOWN;
    params.BackBufferWidth = 16;
    params.BackBufferHeight = 16;

    // FPU_PRESERVE matters more here than it looks: creating a device without
    // it puts the whole process's FPU into single precision, and this is a game
    // whose physics people measure in hundredths. The probe must leave no trace.
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, attempt.type, target,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE,
                                   &params, &device);
    if (SUCCEEDED(hr) && device) {
      log::line("probe device: %s worked", attempt.name);
      return device;
    }
    log::once(attempt.name, "probe device: %s failed (0x%08lx)", attempt.name, hr);
  }
  return nullptr;
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
  if (!d3d) log::once("d3d", "Direct3DCreate9 returned nothing - no D3D9 in this process yet");
  if (d3d) {
    if (IDirect3DDevice9* device = makeProbeDevice(d3d, window)) {
      g_vtable = *reinterpret_cast<void***>(device);
      g_endScene = reinterpret_cast<EndSceneFn>(g_vtable[kEndSceneIndex]);
      g_reset = reinterpret_cast<ResetFn>(g_vtable[kResetIndex]);

      done = writePointer(&g_vtable[kEndSceneIndex], reinterpret_cast<void*>(&endSceneDetour)) &&
             writePointer(&g_vtable[kResetIndex], reinterpret_cast<void*>(&resetDetour));

      log::line("vtable %p: EndScene %p, Reset %p, patched %s", (void*)g_vtable, (void*)g_endScene,
                (void*)g_reset, done ? "yes" : "NO");
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
