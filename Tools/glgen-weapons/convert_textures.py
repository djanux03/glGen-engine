"""Convert the source Unreal texture conventions to glTF (requires Pillow)."""
from pathlib import Path
import sys
from PIL import Image, ImageOps

path = Path(sys.argv[1])
normal = Image.open(path / 'T_AK47_N.png').convert('RGB')
r, g, b = normal.split()
Image.merge('RGB', (r, ImageOps.invert(g), b)).save(path / 'normal_gl.png')
rough, metal, ao = Image.open(path / 'T_AK47_RMAO.png').convert('RGB').split()
Image.merge('RGB', (ao, rough, metal)).save(path / 'orm.png')
