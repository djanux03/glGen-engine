#include "VkWindowSubsystem.h"

#include "../VkAppState.h"

bool VkWindowSubsystem::initialize() { return mState.window != nullptr; }

void VkWindowSubsystem::shutdown() {
  // Window/renderer teardown stays in main() for now.
}
