// The one thread allowed to talk to the network.
#pragma once

namespace tmx {
namespace worker {

void start();

// Stop and wait. Never call this from DllMain - see dllmain.cpp.
void stop();

// Ask the loop to stop without waiting for it, for the process-exit path.
void signalStop();

}  // namespace worker
}  // namespace tmx
