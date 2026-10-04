# Weapon import tools

Runtime assets are already in `assets/weapons/ak47`; Blender is not required to
play. To rebuild the imported geometry from its licensed source:

```powershell
cmake -S Tools/glgen-weapons -B Build-weapon-import
cmake --build Build-weapon-import --config Release
Build-weapon-import/Release/weapon_import.exe assets/weapons/ak47/SKM_AK47.fbx assets/weapons/ak47
```

The converter overwrites `rig.json`. After converting, download and extract the
[CC0 arm archive](https://opengameart.org/content/fps-arms-rigged-only), then run
Blender in background mode to append the gripping poses:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' --background --python Tools/glgen-weapons/pose_arms.py -- 'path/to/FPS ARMS RIG 1 test anim.blend' assets/weapons/ak47
python Tools/glgen-weapons/convert_textures.py assets/weapons/ak47
```

Texture conversion needs Pillow. No texture image is synthesized. See the asset
directory's README for authors, licenses and the separate sound sources.

For a hidden, muted gameplay/rendering check after building the engine:
`python Tools/glgen-regress/rifle.py --sync-validation`.
