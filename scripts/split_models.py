#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path


DEFAULT_CHUNK_SIZE = 95 * 1024 * 1024
DEFAULT_MANIFEST_NAME = "split_manifest.json"


def sha256_file(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def split_file(file_path: Path, chunk_size: int, remove_original: bool) -> dict:
    file_size = file_path.stat().st_size
    total_parts = max(1, math.ceil(file_size / chunk_size))
    parts: list[str] = []

    with file_path.open("rb") as source:
        for index in range(total_parts):
            part_name = f"{file_path.name}.part{index:03d}"
            part_path = file_path.with_name(part_name)
            with part_path.open("wb") as target:
                remaining = chunk_size
                while remaining > 0:
                    block = source.read(min(1024 * 1024, remaining))
                    if not block:
                        break
                    target.write(block)
                    remaining -= len(block)
            parts.append(part_name)

    result = {
        "file_name": file_path.name,
        "size": file_size,
        "sha256": sha256_file(file_path),
        "parts": parts,
    }

    if remove_original:
        file_path.unlink()

    return result


def iter_model_files(models_dir: Path) -> list[Path]:
    return sorted(
        file_path
        for file_path in models_dir.iterdir()
        if file_path.is_file()
        and not file_path.name.endswith(".json")
        and ".part" not in file_path.name
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Split model files into git-friendly chunks."
    )
    parser.add_argument(
        "--models-dir",
        default="assets/models",
        help="Directory that contains model files.",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help="Maximum size of each output chunk in bytes.",
    )
    parser.add_argument(
        "--manifest",
        default=DEFAULT_MANIFEST_NAME,
        help="Manifest file name written inside the models directory.",
    )
    parser.add_argument(
        "--remove-originals",
        action="store_true",
        help="Delete the original model file after chunk files are created.",
    )
    args = parser.parse_args()

    models_dir = Path(args.models_dir).resolve()
    if not models_dir.is_dir():
        raise SystemExit(f"models directory does not exist: {models_dir}")

    manifest_path = models_dir / args.manifest
    manifest = {
        "chunk_size": args.chunk_size,
        "models": [],
    }

    for file_path in iter_model_files(models_dir):
        manifest["models"].append(
            split_file(file_path, args.chunk_size, args.remove_originals)
        )

    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"wrote manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
