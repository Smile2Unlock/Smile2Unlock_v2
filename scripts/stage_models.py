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


def sync_regular_file(src: Path, dst: Path) -> bool:
    """Copy regular file if missing or sha256 differs. Returns True if copied."""
    if dst.is_file() and sha256_file(dst) == sha256_file(src):
        return False
    shutil.copy2(src, dst)
    return True


def sync_merged_file(output_path: Path, parts: list[Path], expected_sha256: str | None) -> bool:
    """Merge parts into file if missing or sha256 differs. Returns True if merged."""
    if expected_sha256 and output_path.is_file():
        try:
            if sha256_file(output_path) == expected_sha256:
                return False
        except OSError:
            pass

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as merged:
        for part_path in parts:
            if not part_path.is_file():
                raise FileNotFoundError(f"missing model part: {part_path}")
            with part_path.open("rb") as handle:
                shutil.copyfileobj(handle, merged)

    if expected_sha256 and sha256_file(output_path) != expected_sha256:
        raise ValueError(f"sha256 mismatch for restored model: {output_path}")
    return True


def collect_regular_source_files(models_dir: Path) -> set[str]:
    """Names of non-part, non-json files in the source models dir."""
    names: set[str] = set()
    for entry in models_dir.iterdir():
        if not entry.is_file():
            continue
        if entry.name.endswith(".json") or ".part" in entry.name:
            continue
        names.add(entry.name)
    return names


def collect_manifest_targets(manifest_path: Path) -> set[str]:
    """File names that the manifest will produce."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    return {m["file_name"] for m in manifest.get("models", [])}


def prune_stale(destination_dir: Path, keep: set[str]) -> None:
    """Remove files in destination that are not in the keep set."""
    for entry in list(destination_dir.iterdir()):
        if entry.is_file() and entry.name not in keep:
            entry.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Incrementally sync model files, only copying when missing or changed."
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

    destination_dir.mkdir(parents=True, exist_ok=True)
    keep: set[str] = set()
    changed = False

    with stage_lock(destination_dir):
        # 1. Sync regular (non-split) files
        for src in sorted(models_dir.iterdir()):
            if not src.is_file():
                continue
            if src.name.endswith(".json") or ".part" in src.name:
                continue
            dst = destination_dir / src.name
            if sync_regular_file(src, dst):
                changed = True
            keep.add(src.name)

        # 2. Sync split-merged files from manifest
        if manifest_path.is_file():
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            for model in manifest.get("models", []):
                name = model["file_name"]
                parts = [models_dir / p for p in model["parts"]]
                dst = destination_dir / name
                if sync_merged_file(dst, parts, model.get("sha256")):
                    changed = True
                keep.add(name)

        # 3. Remove stale files
        before = {e.name for e in destination_dir.iterdir() if e.is_file()}
        stale = before - keep
        if stale:
            for name in sorted(stale):
                (destination_dir / name).unlink()
            changed = True

    if changed:
        print(f"staged models to: {destination_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
