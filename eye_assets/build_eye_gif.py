#!/usr/bin/env python3
"""Build synchronized 160x160 eye GIFs from a generated eye-pair image."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


SIZE = 160
FRAME_MS = 90
BG = (1, 8, 20)


def square_eye_crops(source: Image.Image) -> tuple[Image.Image, Image.Image]:
    """Split a horizontal pair and center-crop each half to a square."""
    width, height = source.size
    split = width // 2
    halves = (source.crop((0, 0, split, height)), source.crop((split, 0, width, height)))
    eyes: list[Image.Image] = []
    for half in halves:
        side = min(half.size)
        left = (half.width - side) // 2
        top = (half.height - side) // 2
        crop = half.crop((left, top, left + side, top + side))
        eyes.append(crop.resize((SIZE, SIZE), Image.Resampling.LANCZOS).convert("RGB"))
    return eyes[0], eyes[1]


def animated_frame(base: Image.Image, dx: int, dy: int, openness: float) -> Image.Image:
    canvas = Image.new("RGB", (SIZE, SIZE), BG)
    target_h = max(5, round(SIZE * openness))
    eye = base.resize((SIZE, target_h), Image.Resampling.LANCZOS)
    x = dx
    y = (SIZE - target_h) // 2 + dy
    canvas.paste(eye, (x, y))
    return canvas


def build_frames(base: Image.Image) -> list[Image.Image]:
    # A calm glance, pause, then one soft blink. The loop is 2.7 seconds.
    motion = (
        [(0, 0, 1.00)] * 6
        + [(-1, 0, 1.00), (-2, 0, 1.00), (-2, -1, 1.00)]
        + [(-2, -1, 1.00)] * 5
        + [(-1, 0, 1.00), (0, 0, 1.00)]
        + [(0, 0, 1.00)] * 6
        + [(0, 0, 0.70), (0, 0, 0.32), (0, 0, 0.07)]
        + [(0, 0, 0.32), (0, 0, 0.70), (0, 0, 1.00)]
        + [(0, 0, 1.00)] * 3
    )
    return [animated_frame(base, dx, dy, openness) for dx, dy, openness in motion]


def save_gif(frames: list[Image.Image], path: Path) -> None:
    frames[0].save(
        path,
        save_all=True,
        append_images=frames[1:],
        duration=FRAME_MS,
        loop=0,
        disposal=2,
        optimize=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    outputs = [
        args.output_dir / "left_eye_160.gif",
        args.output_dir / "right_eye_160.gif",
        args.output_dir / "pair_preview.gif",
        args.output_dir / "left_eye_still.png",
        args.output_dir / "right_eye_still.png",
    ]
    existing = [path for path in outputs if path.exists()]
    if existing:
        names = ", ".join(path.name for path in existing)
        raise SystemExit(f"Refusing to overwrite existing assets: {names}")

    with Image.open(args.source) as image:
        left, right = square_eye_crops(image.convert("RGB"))

    left.save(outputs[3])
    right.save(outputs[4])
    left_frames = build_frames(left)
    right_frames = build_frames(right)
    save_gif(left_frames, outputs[0])
    save_gif(right_frames, outputs[1])

    pair_frames: list[Image.Image] = []
    for left_frame, right_frame in zip(left_frames, right_frames):
        pair = Image.new("RGB", (SIZE * 2 + 16, SIZE), BG)
        pair.paste(left_frame, (0, 0))
        pair.paste(right_frame, (SIZE + 16, 0))
        pair_frames.append(pair)
    save_gif(pair_frames, outputs[2])


if __name__ == "__main__":
    main()
