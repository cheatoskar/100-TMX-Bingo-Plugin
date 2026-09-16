// Entry point.
//
// The ModLoader injects this DLL; an ASI loader loads the same file under a
// different extension. Either way the only thing that happens on the loader's
// thread is starting one of our own - DllMain runs under the loader lock, where
// anything that touches another DLL can deadlock the game before it has drawn a
// frame.
#include <windows.h>

#include "config.h"
#include "hook.h"
#include "overlay.h"
#include "worker.h"

namespace {

DWORD WINAPI boot(LPVOID) {
  tmx::config().load();

  // The overlay comes up whether or not the site is reachable, and the worker
  // runs whether or not the overlay ever gets a frame: a player with no network
  // still gets a working game, and a player on a build we cannot read still
  // gets a window that says so.
  tmx::worker::start();
  tmx::hook::install();
  return 0;
}

}  // namespace

// Exported, not only used as the entry point.
//
// Every mod the TrackMania ModLoader ships - TMUnlimiter, TMInterface, the
// Competition Patch - exports `_DllMain@12`, and a mod that does not fails to
// start the game with a bare 0xc000007b box. The CRT still calls this through
// _DllMainCRTStartup as usual; the export is there so the loader can find it
// by name.
extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(module);
      HANDLE thread = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
      if (thread) CloseHandle(thread);
      break;
    }
    case DLL_PROCESS_DETACH:
      // Deliberately minimal, and deliberately not joining anything: DllMain is
      // under the loader lock, and waiting for a thread that might be inside
      // WinHTTP is how a game hangs on exit instead of closing.
      tmx::hook::remove();
      tmx::worker::signalStop();
      break;
    default:
      break;
  }
  return TRUE;
}
