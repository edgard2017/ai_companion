#!/usr/bin/env python3
"""Prepare a large eye that remains identical after a physical 180-degree rotation."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageChops


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    with Image.open(args.source) as image:
        image = image.convert("RGB")
        side = min(image.size)
        left = (image.width - side) // 2
        top = (image.height - side) // 2
        image = image.crop((left, top, left + side, top + side))
        image = image.resize((160, 160), Image.Resampling.LANCZOS)
        rotated = image.rotate(180)
        # Averaging makes every pixel pair exactly rotationally symmetric.
        image = ImageChops.add(image, rotated, scale=2.0)
        image.save(args.output)


if __name__ == "__main__":
    main()
