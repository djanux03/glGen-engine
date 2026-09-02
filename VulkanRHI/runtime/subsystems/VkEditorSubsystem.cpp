#include "VkEditorSubsystem.h"

#include "../VkAppState.h"
#include "../VkEditor.h"
#include "VulkanRenderer.h"

bool VkEditorSubsystem::initialize() {
  if (!mState.vkEditor)
    return false;
  // Saved graphics settings are DEFAULTS, so they have to land before
  // main()'s GLGEN_SMOKE_* pose block and GLGEN_SCRIPT override them. They
  // used to be read lazily on the first draw(), i.e. after both -- which
  // silently killed every headless sun override (GLGEN_SMOKE_DUSK/_NIGHT/
  // _SUNVIEW/_MOONVIEW set nothing else) and any `render.params{sunPitch=...}`
  // a script ran. See VkEditor::loadGraphicsSettingsOnce.
  if (mState.renderer)
    mState.vkEditor->loadGraphicsSettingsOnce(mState.assetDir,
                                              *mState.renderer);
  return true;
}

void VkEditorSubsystem::shutdown() {
  // VkEditor construction/teardown stays in main() for now.
}
