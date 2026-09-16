#pragma once

namespace tmx {
namespace hook {

/**
 * Rewrite the game's import of Direct3DCreate9 so its own factory - and then
 * its own device - comes through us. Must run before the game asks for it,
 * which is why it is separate from install() and called first.
 */
bool installImports();

bool install();
void remove();

/** True once a frame has actually been drawn through one of the patched tables. */
bool drewOnce();

}  // namespace hook
}  // namespace tmx
