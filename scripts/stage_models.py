#!/usr/bin/env python3
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import shutil
from pathlib import Path

try:
    import fcntl
except ImportError:
    fcntl = None


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


@contextlib.contextmanager
def stage_lock(destination_dir: Path):
    destination_dir.parent.mkdir(parents=True, exist_ok=True)
    lock_path = destination_dir.parent / ".stage_models.lock"
    with lock_path.open("w", encoding="utf-8") as lock_file:
        if fcntl is not None:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if fcntl is not None:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def clear_directory(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for child in directory.iterdir():
        try:
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child)
            else:
                child.unlink()
        except FileNotFoundError:
            continue


def copy_regular_models(models_dir: Path, destination_dir: Path) -> None:
    for file_path in sorted(models_dir.iterdir()):
        if not file_path.is_file():
            continue
        if file_path.name.endswith(".json") or ".part" in file_path.name:
            continue
        shutil.copy2(file_path, destination_dir / file_path.name)


def restore_from_manifest(models_dir: Path, destination_dir: Path, manifest_path: Path) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for model in manifest.get("models", []):
        output_path = destination_dir / model["file_name"]
        with output_path.open("wb") as merged:
            for part_name in model["parts"]:
                part_path = models_dir / part_name
                if not part_path.is_file():
                    raise FileNotFoundError(f"missing model part: {part_path}")
                with part_path.open("rb") as handle:
                    shutil.copyfileobj(handle, merged)

        expected_sha256 = model.get("sha256")
        if expected_sha256 and sha256_file(output_path) != expected_sha256:
            raise ValueError(f"sha256 mismatch for restored model: {output_path}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Copy and restore models into a build output directory."
    )
    parser.add_argument(
        "--models-dir",
        default="assets/models",
        help="Directory that contains model files or model chunks.",
    )
    parser.add_argument(
        "--destination",
        required=True,
        help="Directory where restored models should be written.",
    )
    parser.add_argument(
        "--manifest",
        default=DEFAULT_MANIFEST_NAME,
        help="Manifest file name written inside the models directory.",
    )
    args = parser.parse_args()

    models_dir = Path(args.models_dir).resolve()
    destination_dir = Path(args.destination).resolve()
    manifest_path = models_dir / args.manifest

    if not models_dir.is_dir():
        raise SystemExit(f"models directory does not exist: {models_dir}")

    with stage_lock(destination_dir):
        clear_directory(destination_dir)
        copy_regular_models(models_dir, destination_dir)

        if manifest_path.is_file():
            restore_from_manifest(models_dir, destination_dir, manifest_path)

    print(f"staged models to: {destination_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
