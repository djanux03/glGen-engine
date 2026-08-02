#include "VkEditorSubsystem.h"

#include "../VkAppState.h"

bool VkEditorSubsystem::initialize() { return mState.vkEditor != nullptr; }

void VkEditorSubsystem::shutdown() {
  // VkEditor construction/teardown stays in main() for now.
}
