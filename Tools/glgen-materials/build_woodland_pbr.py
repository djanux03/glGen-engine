#!/usr/bin/env python3
"""Build repeatable PBR maps from the generated woodland ground source tiles.

The source color images are 1254-square generated tiles.  This script gently
matches their opposite borders, then creates engine-ready 8K maps.  Height,
normal, and roughness are derived data rather than separately captured scans.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageEnhance, ImageFilter


ROOT = Path(__file__).resolve().parents[2]
ASSET_ROOT = ROOT / "assets" / "materials" / "woodland_ground_8k"
SOURCE_ROOT = ASSET_ROOT / "source_tiles"
TEXTURE_ROOT = ASSET_ROOT / "textures"
MASTER_ROOT = ASSET_ROOT / "masters_8k"
EDGE_BLEND = 0.12
RUNTIME_SIZE = 2048
MASTER_SIZE = 8192

MATERIALS = [
    {
        "key": "moss",
        "name": "Woodland Moss",
        "source": "moss_source.png",
        "roughness": 0.92,
        "roughness_span": 0.12,
        "normal_strength": 5.5,
        "height_strength": 1.3,
        "tiling": 4.5,
    },
    {
        "key": "forest_litter",
        "name": "Forest Leaf Litter",
        "source": "forest_litter_source.png",
        "roughness": 0.91,
        "roughness_span": 0.15,
        "normal_strength": 7.0,
        "height_strength": 1.6,
        "tiling": 4.0,
    },
    {
        "key": "gravel",
        "name": "Compacted Woodland Gravel",
        "source": "gravel_source.png",
        "roughness": 0.84,
        "roughness_span": 0.17,
        "normal_strength": 9.0,
        "height_strength": 1.9,
        "tiling": 3.5,
    },
    {
        "key": "bedrock",
        "name": "Exposed Forest Bedrock",
        "source": "bedrock_source.png",
        "roughness": 0.81,
        "roughness_span": 0.16,
        "normal_strength": 7.5,
        "height_strength": 1.7,
        "tiling": 4.5,
    },
    {
        "key": "wet_soil",
        "name": "Marsh Wet Soil",
        "source": "wet_soil_source.png",
        "roughness": 0.54,
        "roughness_span": 0.19,
        "normal_strength": 5.0,
        "height_strength": 1.1,
        "tiling": 3.5,
    },
]


def match_edges(image: Image.Image, fraction: float = EDGE_BLEND) -> Image.Image:
    """Cross-fade opposing edge bands so repeated tiles have no hard joins."""
    pixels = np.asarray(image.convert("RGB"), dtype=np.float32).copy()
    height, width, _ = pixels.shape
    blend_x = max(2, round(width * fraction))
    blend_y = max(2, round(height * fraction))

    def blend_axis(data: np.ndarray, axis: int, width_px: int) -> None:
        left = [slice(None)] * data.ndim
        right = [slice(None)] * data.ndim
        left[axis] = slice(0, width_px)
        right[axis] = slice(-1, -width_px - 1, -1)
        a = data[tuple(left)].copy()
        b = data[tuple(right)].copy()
        t = np.linspace(0.0, 1.0, width_px, dtype=np.float32)
        # Smoothstep avoids a visible change in slope where the original tile
        # resumes, while the first and last texels become identical.
        t = t * t * (3.0 - 2.0 * t)
        shape = [1] * data.ndim
        shape[axis] = width_px
        t = t.reshape(shape)
        common = (a + b) * 0.5
        data[tuple(left)] = common * (1.0 - t) + a * t
        data[tuple(right)] = common * (1.0 - t) + b * t

    blend_axis(pixels, 1, blend_x)
    blend_axis(pixels, 0, blend_y)
    return Image.fromarray(np.clip(pixels + 0.5, 0, 255).astype(np.uint8), "RGB")


def grayscale(image: Image.Image) -> Image.Image:
    return image.convert("L")


def as_float(image: Image.Image) -> np.ndarray:
    return np.asarray(image, dtype=np.float32) / 255.0


def build_height(base: Image.Image, strength: float) -> Image.Image:
    # Remove broad lighting and color drift: only small surface forms should
    # drive the normal map, or baked source shading turns into false relief.
    gray = grayscale(base)
    raw = as_float(gray)
    fine = raw - as_float(gray.filter(ImageFilter.GaussianBlur(2.0)))
    form = raw - as_float(gray.filter(ImageFilter.GaussianBlur(10.0)))
    height = np.clip(0.5 + strength * (0.28 * fine + 0.72 * form), 0.20, 0.80)
    return Image.fromarray(np.clip(height * 255.0 + 0.5, 0, 255).astype(np.uint8), "L")


def build_normal(height: Image.Image, strength: float) -> Image.Image:
    h = as_float(height)
    # Wrap derivatives match the tile's addressing mode at both borders.
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5 * strength
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5 * strength
    nx, ny, nz = -dx, -dy, np.ones_like(h)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    normal = np.stack((nx * inv, ny * inv, nz * inv), axis=-1)
    return Image.fromarray(np.clip(normal * 127.5 + 127.5 + 0.5, 0, 255).astype(np.uint8), "RGB")


def build_roughness(base: Image.Image, center: float, span: float) -> Image.Image:
    gray = grayscale(base)
    raw = as_float(gray)
    local = raw - as_float(gray.filter(ImageFilter.GaussianBlur(3.0)))
    # Fine crests and hollows vary the response without encoding lighting or
    # turning the wet-soil layer into an implausibly glossy puddle.
    value = center + np.clip(local * 1.65, -span * 0.55, span * 0.55)
    value += (0.5 - raw) * span * 0.35
    return Image.fromarray(np.clip(value * 255.0 + 0.5, 0, 255).astype(np.uint8), "L")


def upscale(image: Image.Image, size: int) -> Image.Image:
    return image.resize((size, size), Image.Resampling.LANCZOS)


def write_jpeg(image: Image.Image, path: Path, quality: int) -> None:
    image.save(path, "JPEG", quality=quality, subsampling=0, optimize=True, progressive=True)


def generate() -> None:
    TEXTURE_ROOT.mkdir(parents=True, exist_ok=True)
    MASTER_ROOT.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, object] = {
        "version": 1,
        "description": "Woodland ground PBR textures; runtime paths are 2K, optional master paths are 8K; paths are relative to the repository root.",
        "runtimeResolution": [RUNTIME_SIZE, RUNTIME_SIZE],
        "masterResolution": [MASTER_SIZE, MASTER_SIZE],
        "sourceResolution": [1254, 1254],
        "sourceDetail": "Built-in image generation at 1254x1254, edge-matched and Lanczos-upscaled. Height, normal, and roughness are derived from the generated color tiles.",
        "materials": {},
    }

    for item in MATERIALS:
        src_path = SOURCE_ROOT / item["source"]
        if not src_path.is_file():
            raise FileNotFoundError(f"Missing generated source tile: {src_path}")
        base = Image.open(src_path).convert("RGB")
        source_size = list(base.size)
        base = match_edges(base)
        base = ImageEnhance.Sharpness(base).enhance(1.12)
        height = build_height(base, item["height_strength"])
        normal = build_normal(height, item["normal_strength"])
        roughness = build_roughness(base, item["roughness"], item["roughness_span"])

        stem = f"woodland_{item['key']}"
        out = {
            "albedo": f"assets/materials/woodland_ground_8k/textures/{stem}_basecolor.jpg",
            "normal": f"assets/materials/woodland_ground_8k/textures/{stem}_normal.jpg",
            "roughness": f"assets/materials/woodland_ground_8k/textures/{stem}_roughness.jpg",
            "height": f"assets/materials/woodland_ground_8k/textures/{stem}_height.jpg",
            "tiling": item["tiling"],
        }
        master = {
            "albedo": f"assets/materials/woodland_ground_8k/masters_8k/{stem}_basecolor.jpg",
            "normal": f"assets/materials/woodland_ground_8k/masters_8k/{stem}_normal.jpg",
            "roughness": f"assets/materials/woodland_ground_8k/masters_8k/{stem}_roughness.jpg",
            "height": f"assets/materials/woodland_ground_8k/masters_8k/{stem}_height.jpg",
        }
        runtime_images = {
            "albedo": (base, 95),
            "normal": (normal, 97),
            "roughness": (roughness, 96),
            "height": (height, 96),
        }
        for map_name, (image, quality) in runtime_images.items():
            write_jpeg(upscale(image, RUNTIME_SIZE), ROOT / out[map_name], quality)
            write_jpeg(upscale(image, MASTER_SIZE), ROOT / master[map_name], quality)
        manifest["materials"][item["key"]] = {  # type: ignore[index]
            "name": item["name"],
            **out,
            "masters8k": master,
            "source": f"assets/materials/woodland_ground_8k/source_tiles/{item['source']}",
            "sourceResolution": source_size,
        }
        print(f"Built {item['name']} ({source_size[0]}x{source_size[1]} -> {RUNTIME_SIZE}x{RUNTIME_SIZE} runtime, {MASTER_SIZE}x{MASTER_SIZE} master)")

    (ASSET_ROOT / "materials.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    generate()
