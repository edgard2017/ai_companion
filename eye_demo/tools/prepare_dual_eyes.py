#!/usr/bin/env python3
"""Crop and enlarge a pair of anime eyes for 160x160 circular displays."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageOps


SIZE = 160
TARGET = 142


def enlarge(source: Path) -> Image.Image:
    with Image.open(source) as image:
        image = image.convert("RGB")
        background = Image.new("RGB", image.size, image.getpixel((0, 0)))
        difference = ImageChops.difference(image, background).convert("L")
        mask = difference.point(lambda value: 255 if value > 18 else 0)
        bbox = mask.getbbox()
        if bbox is None:
            raise SystemExit(f"Could not find eye artwork in {source}")

        # Keep a small antialiasing margin around the detected artwork.
        left, top, right, bottom = bbox
        left = max(0, left - 3)
        top = max(0, top - 3)
        right = min(image.width, right + 3)
        bottom = min(image.height, bottom + 3)
        eye = image.crop((left, top, right, bottom))

        scale = TARGET / max(eye.size)
        resized = eye.resize(
            (round(eye.width * scale), round(eye.height * scale)),
            Image.Resampling.LANCZOS,
        )
        canvas = Image.new("RGB", (SIZE, SIZE), image.getpixel((0, 0)))
        x = (SIZE - resized.width) // 2
        y = (SIZE - resized.height) // 2
        canvas.paste(resized, (x, y))
        return canvas


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("left_source", type=Path)
    parser.add_argument("right_source", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    # The official L/R GIF pairs use a horizontal mirror for the opposite eye,
    # including the pupil highlights and outer accents. Follow that convention
    # instead of preserving the generated image's shared lighting direction.
    left = enlarge(args.left_source)
    right = ImageOps.mirror(left)
    left.save(args.output_dir / "left_eye_160.png")
    right.save(args.output_dir / "right_eye_160.png")


if __name__ == "__main__":
    main()
