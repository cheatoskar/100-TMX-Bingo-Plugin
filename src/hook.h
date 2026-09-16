#pragma once

namespace tmx {
namespace hook {

bool install();
void remove();

/** True once a frame has actually been drawn through one of the patched tables. */
bool drewOnce();

}  // namespace hook
}  // namespace tmx
