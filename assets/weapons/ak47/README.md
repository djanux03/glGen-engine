# AK-47 asset credits

Downloaded 2026-10-01. All imported art and sound in this directory is CC0 1.0
Universal: https://creativecommons.org/publicdomain/zero/1.0/

* **Weapon:** Stein Games, [Free Classic Weapons Pack v1.1](https://stein-indie.itch.io/classic-weapons-pack).
  Original `SKM_AK47.fbx`, 2048px color, normal and roughness/metallic/AO textures.
  No generative AI was used by the asset author. Imported gun: 16,578 triangles.
* **Arms and skin:** para, [FPS arms (rigged only)](https://opengameart.org/content/fps-arms-rigged-only).
  Base mesh and photographic texture by the MakeHuman team. Archive:
  https://opengameart.org/sites/default/files/fps%20arms.7z
  Reposed for this rifle; 8,152 triangles across two forearms.
* **Shot reports:** [The Free Firearm Sound Library](https://opengameart.org/content/the-free-firearm-sound-library),
  Ben Jaszczak, Brian Nelson, Kevin Heras and Matthew Nanney. The three AK game
  reports are edited mixes of that library's CC0 recordings, obtained from
  https://github.com/yegors/hard-lines/tree/main/public/audio
  (`ak47-report-0.wav`, `ak47-report-1.wav`, `ak47-report-2.wav`). Their source
  layers and editing commands are recorded in that repository's
  `art/tools/make_ak47_audio.sh`. These are designed game effects, not recordings
  of this exact rifle.
* **Reload and charging foley:** SpringySpringo,
  [Gun reload sounds](https://opengameart.org/content/gun-reload-sounds).
  `assaultriflereload1_0.wav` and `shotguncock_0.wav`, renamed locally.

`texture_provenance.json` records the downloaded source and local SHA-256 hashes.
The original weapon license is preserved as `LICENSE.txt`.

## Conversion and runtime

`Tools/glgen-weapons/import.cpp` converts the weapon FBX to metre-scale glTF,
retaining the magazine, bolt, trigger and selector as independent rigid parts.
Source +Z is converted to engine -Z. DirectX normal-map green is inverted;
source R/G/B = roughness/metallic/AO is repacked into glTF AO/roughness/metallic.
`pose_arms.py` uses the author's IK rig in Blender to bake the gripping poses.
The original FBX and texture sources remain here; the arm source archive can be
downloaded again using the link above.

The game animates the shared gun and arm meshes without rebuilding geometry.
The forearm vertex stage anchors their bases to the camera during hand movement,
so reloads do not expose the cut ends of the imported first-person arm meshes.
Foreground depth keeps the rifle clear of nearby walls; world temporal history
does not accumulate weapon recoil. The assembled `ak47.gltf` is also available
as an editor world prop. It excludes first-person arms.

In the editor, use **Create > Player with AK-47**, then **Play**. LMB fires,
RMB aims, R reloads, B switches automatic/semi. Create > Shooting target adds
a damageable practice target. A magazine holds 30 rounds with 120 in reserve.
Shots use a game raycast with damage, impact marks and an impulse on dynamic
objects; there is no ballistic trajectory or penetration simulation.
