#!/usr/bin/env python3
"""Verify a release tree and stage its payload for a distro's PAM directory."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("pam_directory")
    parser.add_argument("--remove-hook", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    destination = args.destination.resolve()
    pam = Path(args.pam_directory)
    if not pam.is_absolute() or ".." in pam.parts:
        parser.error("PAM directory must be an absolute path without '..'")
    manifest_path = Path("usr/share/smile2unlock/release-info.json")
    manifest = json.loads((source / manifest_path).read_text())
    if manifest["schema"] != 1 or manifest["build"] != {"platform": "linux", "arch": "x86_64"}:
        raise SystemExit("unsupported release manifest")
    paths = set()
    for entry in manifest["files"]:
        path = Path(entry["path"])
        if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != "usr":
            raise SystemExit(f"invalid release path: {path}")
        if path.as_posix() in paths:
            raise SystemExit(f"duplicate release path: {path}")
        paths.add(path.as_posix())
        file = source / path
        if not file.resolve().is_relative_to(source) or not file.is_file():
            raise SystemExit(f"missing or unsafe release file: {path}")
        if hashlib.sha256(file.read_bytes()).hexdigest() != entry["sha256"]:
            raise SystemExit(f"release checksum mismatch: {path}")
    actual = {p.relative_to(source).as_posix() for p in (source / "usr").rglob("*")
              if p.is_file() and p.relative_to(source) != manifest_path}
    if actual != paths:
        raise SystemExit("release manifest file set mismatch")
    shutil.copytree(source / "usr", destination / "usr", symlinks=True, dirs_exist_ok=True)
    old_path = Path("usr/lib/security/pam_smile2unlock.so")
    new_path = pam.relative_to("/") / old_path.name
    if old_path != new_path:
        (destination / new_path).parent.mkdir(parents=True, exist_ok=True)
        shutil.move(destination / old_path, destination / new_path)
        for entry in manifest["files"]:
            if entry["path"] == old_path.as_posix():
                entry["path"] = new_path.as_posix()
        (destination / old_path).parent.rmdir()
    if args.remove_hook:
        hook_path = Path("usr/share/libalpm/hooks/smile2unlock-remove.hook")
        (destination / hook_path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.remove_hook, destination / hook_path)
        (destination / hook_path).chmod(0o644)
        manifest["files"].append({"path": hook_path.as_posix(),
                                  "sha256": hashlib.sha256(args.remove_hook.read_bytes()).hexdigest()})
    (destination / manifest_path).write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
