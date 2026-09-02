// The one place the built-in generator set is enumerated.
//
// Each generator lives in its own translation unit and exposes a single
// register function; this file calls them. Deliberately explicit rather than
// using static-initialization self-registration: static init order across
// translation units is unspecified, and a generator that registers itself
// before the registry's own storage is constructed is a genuinely awful bug to
// find. GeneratorRegistry::instance() calls this exactly once, after its
// storage exists.

#include "../GeneratorRegistry.h"

namespace gen {

void registerTreeGenerator();
void registerRockGenerator();
void registerGrassGenerator();
void registerKitbashGenerator();
void registerSculptGenerator();
void registerSweepGenerator();
void registerRevolveGenerator();
void registerPanelGenerator();
void registerCharacterGenerator();
void registerImportGenerator();

void registerBuiltinGenerators() {
  registerTreeGenerator();
  registerRockGenerator();
  registerGrassGenerator();
  registerKitbashGenerator();
  registerSculptGenerator();
  registerSweepGenerator();
  registerRevolveGenerator();
  registerPanelGenerator();
  registerCharacterGenerator();
  registerImportGenerator();
}

} // namespace gen
