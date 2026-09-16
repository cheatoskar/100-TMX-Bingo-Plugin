// Getting a frame to draw in.
//
// Direct3D 9 dispatches through a vtable that every device of a class shares,
// so swapping entries in it is enough to be called on every frame - no inline
// patching, no trampoline allocation, no MinHook. The vtable is read from a
// throwaway device created on a hidden window, which is the standard trick
// (kiero does the same) and never touches the game's own device.
//
// There are **two** such classes in a modern d3d9.dll: the plain
// IDirect3DDevice9 and IDirect3DDevice9Ex, each with its own table. Which one a
// game draws through is not knowable from here, and patching only the plain one
// is exactly the bug that made this mod invisible - the hook reported success
// while the game rendered through the other table. So both are taken.
//
// Three entries per table: EndScene, where the overlay is drawn; Present, as a
// fallback for anything that reaches the screen without EndScene; and Reset,
// where the ImGui backend must let go of its device objects before the device
// is recreated - skip that one and alt-tabbing out of fullscreen is a crash.
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwctype>
#include <string>

#include "hook.h"
#include "log.h"
#include "overlay.h"

#pragma comment(lib, "d3d9.lib")

namespace tmx {
namespace hook {
namespace {

using EndSceneFn = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using PresentFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

// IDirect3DDevice9's vtable layout, unchanged since 2004. IDirect3DDevice9Ex
// inherits it, so the same indices hold there - its extra methods come after.
constexpr int kResetIndex = 16;
constexpr int kPresentIndex = 17;
constexpr int kEndSceneIndex = 42;

struct Patched {
  const char* name = "";
  void** vtable = nullptr;
  EndSceneFn endScene = nullptr;
  PresentFn present = nullptr;
  ResetFn reset = nullptr;
};

Patched g_tables[8];
int g_tableCount = 0;
bool g_drewOnce = false;

bool writePointer(void** slot, void* value) {
  DWORD previous = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) return false;
  *slot = value;
  VirtualProtect(slot, sizeof(void*), previous, &previous);
  return true;
}

Patched* tableFor(void** vtable) {
  for (int i = 0; i < g_tableCount; i++) {
    if (g_tables[i].vtable == vtable) return &g_tables[i];
  }
  return nullptr;
}

Patched* tableOf(IDirect3DDevice9* device) {
  return tableFor(*reinterpret_cast<void***>(device));
}

void drawOnce(IDirect3DDevice9* device, const char* where) {
  if (!g_drewOnce) {
    g_drewOnce = true;
    log::line("first %s from the game - the overlay is live", where);
  }
  // Everything the overlay does is bounded and local: it reads a copy of the
  // shared state and draws. It never waits on the network - that is the whole
  // reason the worker thread exists.
  overlay::draw(device);
}

HRESULT APIENTRY endSceneDetour(IDirect3DDevice9* device) {
  Patched* table = tableOf(device);
  if (!table) return S_OK;  // should not happen, and must not crash a game if it does
  drawOnce(device, "EndScene");
  return table->endScene(device);
}

// The belt to EndScene's braces: anything that reaches the screen has to
// Present, so a game whose EndScene we never see is still reachable here. It
// only draws while EndScene has not fired, so the two never both run in a frame.
HRESULT APIENTRY presentDetour(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window,
                               const RGNDATA* dirty) {
  Patched* table = tableOf(device);
  if (!table) return S_OK;
  if (!g_drewOnce) drawOnce(device, "Present");
  return table->present(device, src, dst, window, dirty);
}

HRESULT APIENTRY resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
  Patched* table = tableOf(device);
  overlay::invalidate();
  return table ? table->reset(device, params) : D3DERR_INVALIDCALL;
}

D3DPRESENT_PARAMETERS probeParams(HWND target) {
  D3DPRESENT_PARAMETERS params{};
  params.Windowed = TRUE;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = target;
  params.BackBufferFormat = D3DFMT_UNKNOWN;
  // A real size, not zero: a zero-sized backbuffer on an invisible window is
  // what made this fail outright with D3DERR_INVALIDCALL.
  params.BackBufferWidth = 16;
  params.BackBufferHeight = 16;
  return params;
}

// FPU_PRESERVE matters more than it looks: creating a device without it puts
// the whole process's FPU into single precision, and this is a game whose
// physics people measure in hundredths. The probe must leave no trace.
constexpr DWORD kProbeFlags =
    D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE;

// Creating the probe is the fragile step of the whole mod, and it fails for
// reasons that have nothing to do with the game: a driver that refuses a HAL
// device on an invisible window, a session with no display, an adapter in a
// mode it cannot match. So it is a list of attempts, each logged with its
// HRESULT - the last of which needs no display at all.
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
    D3DPRESENT_PARAMETERS params = probeParams(target);

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, attempt.type, target, kProbeFlags, &params, &device);
    if (SUCCEEDED(hr) && device) {
      log::line("probe device: %s worked", attempt.name);
      return device;
    }
    log::once(attempt.name, "probe device: %s failed (0x%08lx)", attempt.name, hr);
  }
  return nullptr;
}

// The Ex flavour, resolved dynamically because it does not exist on every
// d3d9.dll this game can be run against.
IDirect3DDevice9* makeProbeDeviceEx(HWND window) {
  HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
  if (!d3d9) return nullptr;

  auto create = reinterpret_cast<CreateExFn>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));
  if (!create) {
    log::once("ex", "no Direct3DCreate9Ex in this d3d9.dll");
    return nullptr;
  }

  IDirect3D9Ex* d3dEx = nullptr;
  HRESULT hr = create(D3D_SDK_VERSION, &d3dEx);
  if (FAILED(hr) || !d3dEx) {
    log::once("ex-create", "Direct3DCreate9Ex failed (0x%08lx)", hr);
    return nullptr;
  }

  D3DPRESENT_PARAMETERS params = probeParams(window);
  IDirect3DDevice9Ex* device = nullptr;
  hr = d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, kProbeFlags, &params, nullptr, &device);
  d3dEx->Release();

  if (FAILED(hr) || !device) {
    log::once("ex-device", "probe device: Ex failed (0x%08lx)", hr);
    return nullptr;
  }

  log::line("probe device: Ex worked");
  return device;
}

bool patchTable(const char* name, IDirect3DDevice9* device) {
  if (!device || g_tableCount >= 8) return false;

  void** vtable = *reinterpret_cast<void***>(device);
  if (tableFor(vtable)) {
    log::line("%s device shares an already patched vtable", name);
    return true;
  }

  Patched& table = g_tables[g_tableCount];
  table.name = name;
  table.vtable = vtable;
  table.endScene = reinterpret_cast<EndSceneFn>(vtable[kEndSceneIndex]);
  table.present = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);
  table.reset = reinterpret_cast<ResetFn>(vtable[kResetIndex]);

  // Counted before the writes: a detour can be entered the instant the first
  // pointer lands, and it finds its own table by vtable address.
  g_tableCount++;

  const bool done = writePointer(&vtable[kEndSceneIndex], reinterpret_cast<void*>(&endSceneDetour)) &&
                    writePointer(&vtable[kPresentIndex], reinterpret_cast<void*>(&presentDetour)) &&
                    writePointer(&vtable[kResetIndex], reinterpret_cast<void*>(&resetDetour));

  log::line("%s vtable %p: EndScene %p, Present %p, patched %s", name, (void*)vtable, (void*)table.endScene,
            (void*)table.present, done ? "yes" : "NO");
  if (!done) g_tableCount--;
  return done;
}

// ---------------------------------------------------------------------------
// Catching the game's own device.
//
// Patching a vtable read off a probe device only works when every device of
// that class shares one table. Here they do not: the addresses move on every
// run, so each device carries its own copy and the probe's patch is a patch on
// a table nobody else will ever use. Measured on this machine - three runs,
// three different vtable addresses, and not one frame through any of them.
//
// So the game's own objects have to be caught as they are made. Its import
// table is rewritten so that its call to Direct3DCreate9 comes here first; the
// factory it gets back is patched at CreateDevice; and the device that comes
// out of *that* is the one the game draws with, so its table is the one worth
// having.
//
// Import-table patching rather than an inline detour on purpose: no
// instruction-length disassembly, nothing executable rewritten, and if the
// import is not there the whole thing simply does not apply.
// ---------------------------------------------------------------------------

using CreateDeviceFn = HRESULT(APIENTRY*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                          IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT(APIENTRY*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                            D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using Create9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

constexpr int kCreateDeviceIndex = 16;
constexpr int kCreateDeviceExIndex = 20;

Create9Fn g_realCreate9 = nullptr;
Create9ExFn g_realCreate9Ex = nullptr;
CreateDeviceFn g_realCreateDevice = nullptr;
CreateDeviceExFn g_realCreateDeviceEx = nullptr;

HRESULT APIENTRY createDeviceDetour(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                    D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) {
  HRESULT hr = g_realCreateDevice(self, adapter, type, window, flags, params, out);
  if (SUCCEEDED(hr) && out && *out) {
    log::line("the game created its device - patching the table it actually uses");
    patchTable("game", *out);
  }
  return hr;
}

HRESULT APIENTRY createDeviceExDetour(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                      D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode,
                                      IDirect3DDevice9Ex** out) {
  HRESULT hr = g_realCreateDeviceEx(self, adapter, type, window, flags, params, mode, out);
  if (SUCCEEDED(hr) && out && *out) {
    log::line("the game created its Ex device - patching the table it actually uses");
    patchTable("game Ex", *out);
  }
  return hr;
}

void patchFactory(IDirect3D9* factory, bool isEx) {
  void** vtable = *reinterpret_cast<void***>(factory);

  if (!g_realCreateDevice) {
    g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[kCreateDeviceIndex]);
    writePointer(&vtable[kCreateDeviceIndex], reinterpret_cast<void*>(&createDeviceDetour));
    log::line("watching CreateDevice on the factory the game just made");
  }
  if (isEx && !g_realCreateDeviceEx) {
    g_realCreateDeviceEx = reinterpret_cast<CreateDeviceExFn>(vtable[kCreateDeviceExIndex]);
    writePointer(&vtable[kCreateDeviceExIndex], reinterpret_cast<void*>(&createDeviceExDetour));
    log::line("watching CreateDeviceEx on the factory the game just made");
  }
}

IDirect3D9* WINAPI create9Detour(UINT sdk) {
  IDirect3D9* factory = g_realCreate9 ? g_realCreate9(sdk) : nullptr;
  log::line("the game called Direct3DCreate9 (%s)", factory ? "ok" : "it failed");
  if (factory) patchFactory(factory, false);
  return factory;
}

HRESULT WINAPI create9ExDetour(UINT sdk, IDirect3D9Ex** out) {
  HRESULT hr = g_realCreate9Ex ? g_realCreate9Ex(sdk, out) : E_FAIL;
  log::line("the game called Direct3DCreate9Ex (0x%08lx)", hr);
  if (SUCCEEDED(hr) && out && *out) patchFactory(*out, true);
  return hr;
}

// Rewrite one imported function pointer in the host executable.
bool patchImport(const char* dll, const char* function, void* replacement, void** original) {
  HMODULE base = GetModuleHandleW(nullptr);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(base) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;

  auto import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<BYTE*>(base) + dir.VirtualAddress);
  for (; import->Name; import++) {
    const char* name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(base) + import->Name);
    if (_stricmp(name, dll) != 0) continue;

    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<BYTE*>(base) + import->OriginalFirstThunk);
    auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<BYTE*>(base) + import->FirstThunk);
    if (!import->OriginalFirstThunk) names = addresses;

    for (; names->u1.AddressOfData; names++, addresses++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto named = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<BYTE*>(base) + names->u1.AddressOfData);
      if (strcmp(named->Name, function) != 0) continue;

      *original = reinterpret_cast<void*>(addresses->u1.Function);
      if (!writePointer(reinterpret_cast<void**>(&addresses->u1.Function), replacement)) return false;
      log::line("import patched: %s!%s", dll, function);
      return true;
    }
  }
  return false;
}

}  // namespace

// Which graphics libraries are actually in this process.
//
// A wrapper d3d9.dll in the game folder, dgVoodoo, an ENB - any of them mean
// the game draws through a vtable that is not the one the system d3d9 hands
// out, and then a hook that reports success still never fires. Logged once, so
// that case is one line in the log rather than an evening of guessing.
void logGraphicsModules() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      std::wstring name(entry.szModule);
      for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
      if (name.find(L"d3d") != std::wstring::npos || name.find(L"ddraw") != std::wstring::npos ||
          name.find(L"opengl") != std::wstring::npos || name.find(L"dgvoodoo") != std::wstring::npos) {
        char path[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, path, MAX_PATH, nullptr, nullptr);
        log::once(path, "graphics module: %s", path);
      }
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
}

bool drewOnce() { return g_drewOnce; }

bool installImports() {
  static bool done = false;
  if (done) return true;

  bool any = false;
  any |= patchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&create9Detour),
                     reinterpret_cast<void**>(&g_realCreate9));
  any |= patchImport("d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<void*>(&create9ExDetour),
                     reinterpret_cast<void**>(&g_realCreate9Ex));
  if (!any) {
    log::once("imports", "the game does not import Direct3DCreate9 by name - falling back to the probe");
  }
  done = any;
  return any;
}

bool install() {
  if (g_tableCount > 0) return true;
  logGraphicsModules();

  installImports();  // no-op once it has taken

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"TmxProbeWindow";
  RegisterClassExW(&wc);  // harmless when an earlier attempt already registered it

  HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!window) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }

  if (IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION)) {
    if (IDirect3DDevice9* device = makeProbeDevice(d3d, window)) {
      patchTable("plain", device);
      device->Release();
    }
    d3d->Release();
  } else {
    log::once("d3d", "Direct3DCreate9 returned nothing - no D3D9 in this process yet");
  }

  if (IDirect3DDevice9* deviceEx = makeProbeDeviceEx(window)) {
    patchTable("Ex", deviceEx);
    deviceEx->Release();
  }

  DestroyWindow(window);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return g_tableCount > 0;
}

void remove() {
  for (int i = 0; i < g_tableCount; i++) {
    Patched& table = g_tables[i];
    if (table.endScene) writePointer(&table.vtable[kEndSceneIndex], reinterpret_cast<void*>(table.endScene));
    if (table.present) writePointer(&table.vtable[kPresentIndex], reinterpret_cast<void*>(table.present));
    if (table.reset) writePointer(&table.vtable[kResetIndex], reinterpret_cast<void*>(table.reset));
  }
  g_tableCount = 0;
}

}  // namespace hook
}  // namespace tmx
