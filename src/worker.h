// The one thread allowed to talk to the network.
#pragma once

namespace tmx {
namespace worker {

void start();

// Stop and wait. Never call this from DllMain - see dllmain.cpp.
void stop();

// Ask the loop to stop without waiting for it, for the process-exit path.
void signalStop();

/**
 * Drop every mark, now, on the calling thread.
 *
 * For the window closing: the worker thread may be asleep or mid-request, and
 * the process is about to go away, so this sends the release itself with a
 * short timeout rather than handing it to somebody who may never run again.
 */
void releaseNow();

}  // namespace worker
}  // namespace tmx
