#!/usr/bin/env python3
"""Collect the trusted build's ELF dependency closure for every Linux format.

glibc/ld.so belong to standalone executables only. PAM must keep the host's
libc, loader and libpam; its C++ runtime is statically linked at build time.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def run(*args, env=None):
    return subprocess.check_output(args, text=True, env=env, stderr=subprocess.STDOUT).strip()


def glibc_requirement(binary):
    versions = re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)\b", run("readelf", "--version-info", str(binary)))
    return max(versions, key=lambda value: tuple(map(int, value.split("."))), default="2.2.5")


def bundle(root):
    private = root / "usr/lib/smile2unlock"
    libc_dir = private / "glibc"
    libc_dir.mkdir()
    doc = root / "usr/share/doc/smile2unlock"
    license_dir = doc / "licenses/runtime"
    license_dir.mkdir(parents=True)
    executables = [root / "usr/bin/su_app"] + [
        root / "usr/libexec/smile2unlock" / name for name in ["su_authd", "su_deploy_helper"]]
    # Resolve staged project libraries, not an installed copy or Xmake cache
    # from the input's legacy DT_RPATH (which precedes LD_LIBRARY_PATH).
    for binary in executables:
        run("patchelf", "--set-rpath", str(private), str(binary))
    for library in private.glob("*.so*"):
        if library.is_file() and not library.is_symlink():
            run("patchelf", "--set-rpath", "$ORIGIN", str(library))
    pam, = root.glob("usr/**/security/pam_smile2unlock.so")
    needed = run("patchelf", "--print-needed", str(pam)).splitlines()
    if any(name.startswith(("libstdc++", "libgcc_s", "libc++")) for name in needed):
        raise RuntimeError("rebuild pam_smile2unlock with its static C++ runtime before packaging")

    # Keep driver-facing dispatchers and PAM owned by the target distribution.
    # Vendor drivers and their dlopen plugins cannot be transplanted safely.
    external = re.compile(r"^(?:lib(?:GL|EGL|GLX|GLdispatch|OpenGL|gbm|drm|vulkan|pam)[^/]*\.so)")
    glibc_names = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0",
                   "librt.so.1", "libresolv.so.2", "libutil.so.1", "libanl.so.1",
                   "libnss_files.so.2", "libnss_dns.so.2"}
    env = dict(os.environ, LD_LIBRARY_PATH=str(private), LD_PRELOAD="", LD_AUDIT="", LC_ALL="C")
    records = []
    resolved = {}
    host = set()
    loader = None
    for binary in executables + [path for path in private.glob("*.so*") if path.is_file()]:
        output = run("ldd", str(binary), env=env)
        if "not found" in output:
            raise RuntimeError(f"unresolved build dependency for {binary.name}: {output}")
        for line in output.splitlines():
            match = re.match(r"\s*(\S+) => (/.+?) \(0x[0-9a-f]+\)", line)
            if match:
                name, source = match.groups()
                if "/" in name:
                    if Path(name).name.startswith(("ld-linux", "ld64.so", "ld.so.")):
                        name = Path(name).name
                    else:
                        raise RuntimeError(f"non-relocatable absolute dependency: {name}")
                path = Path(source).resolve()
                if external.match(name):
                    host.add(name)
                    continue
                if name in resolved and resolved[name] != path:
                    raise RuntimeError(f"conflicting dependency sources for {name}")
                resolved[name] = path
            else:
                match = re.match(r"\s*(/\S+ld[^/]*\.so\S*) \(0x[0-9a-f]+\)", line)
                if match:
                    candidate = Path(match[1]).resolve()
                    if loader is not None and candidate != loader:
                        raise RuntimeError("build binaries use different ELF loaders")
                    loader = candidate
    if loader is None:
        raise RuntimeError("cannot locate the matching glibc ELF loader")
    resolved[loader.name] = loader
    # These compatibility/NSS libraries can be dlopened instead of DT_NEEDED.
    for name in glibc_names:
        path = loader.parent / name
        if path.is_file():
            resolved[name] = path.resolve()
    for name in ["libc.so.6", "libm.so.6"]:
        if name not in resolved:
            raise RuntimeError(f"matching glibc dependency missing: {name}")

    copied_licenses = set()

    def provenance(source):
        package = version = None
        if shutil.which("pacman"):
            result = subprocess.run(["pacman", "-Qqo", str(source)], text=True, capture_output=True)
            if result.returncode == 0:
                package = result.stdout.strip()
                version = run("pacman", "-Q", package).split(maxsplit=1)[1]
                directory = Path("/usr/share/licenses") / package
                if directory.is_dir() and package not in copied_licenses:
                    shutil.copytree(directory, license_dir / package, symlinks=False)
                    copied_licenses.add(package)
        elif shutil.which("dpkg-query"):
            result = subprocess.run(["dpkg-query", "-S", str(source)], text=True, capture_output=True)
            if result.returncode == 0:
                package = result.stdout.split(": ", 1)[0]
                version = run("dpkg-query", "-W", "-f=${Version}", package)
                copyright = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
                if copyright.is_file() and package not in copied_licenses:
                    shutil.copy2(copyright, license_dir / (package + ".copyright"))
                    copied_licenses.add(package)
        return {"package": package, "version": version}

    for name, source in sorted(resolved.items()):
        destination_dir = libc_dir if name in glibc_names or source == loader else private
        destination = destination_dir / name
        # Already staged project libraries retain their SONAME symlinks.
        if source.is_relative_to(private):
            continue
        shutil.copy2(source, destination)
        destination.chmod(0o755)
        records.append({"path": destination.relative_to(root).as_posix(),
                        "original_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        **provenance(source)})

    # Shared-license distributions keep these outside individual package dirs.
    for name in ["GPL-2.0-or-later", "GPL-3.0-or-later", "LGPL-2.1-or-later", "LGPL-3.0-or-later"]:
        for source in [Path("/usr/share/licenses/spdx") / (name + ".txt"),
                       Path("/usr/share/common-licenses") / {"GPL-2.0-or-later": "GPL-2",
                       "GPL-3.0-or-later": "GPL-3", "LGPL-2.1-or-later": "LGPL-2.1",
                       "LGPL-3.0-or-later": "LGPL-3"}[name]]:
            if source.is_file():
                shutil.copy2(source, license_dir / (name + ".txt"))
                break

    # This module uses only host libraries; never expose private libc to GDM.
    run("patchelf", "--remove-rpath", str(pam))
    loader_path = "/" + (libc_dir / loader.name).relative_to(root).as_posix()
    for binary in executables:
        relative = os.path.relpath(private, binary.parent)
        run("patchelf", "--set-interpreter", loader_path, str(binary))
        run("patchelf", "--set-rpath", f"$ORIGIN/{relative}/glibc:$ORIGIN/{relative}", str(binary))
    for library in private.glob("*.so*"):
        if library.is_file() and not library.is_symlink():
            run("patchelf", "--set-rpath", "$ORIGIN/glibc:$ORIGIN", str(library))
    manifest = {"schema": 1, "loader": loader_path, "libraries": records,
                "host_libraries": sorted(host | {"libpam.so.0"}),
                "pam_glibc_minimum": glibc_requirement(pam)}
    (doc / "runtime-libraries.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"bundled {len(records)} runtime libraries; PAM host glibc >= {manifest['pam_glibc_minimum']}")


def verify(root):
    manifest = json.loads((root / "usr/share/doc/smile2unlock/runtime-libraries.json").read_text())
    private = root / "usr/lib/smile2unlock"
    loader = (root / manifest["loader"].lstrip("/")).resolve()
    if not loader.is_relative_to(private / "glibc") or not loader.is_file():
        raise RuntimeError("invalid private runtime loader")
    for record in manifest["libraries"]:
        path = (root / record["path"]).resolve()
        if not path.is_relative_to(private) or not path.is_file():
            raise RuntimeError(f"bundled dependency missing: {record['path']}")
    pam, = root.glob("usr/**/security/pam_smile2unlock.so")
    if run("patchelf", "--print-rpath", str(pam)):
        raise RuntimeError("PAM must use only host libc/PAM")
    if any(name.startswith(("libstdc++", "libgcc_s", "libc++"))
           for name in run("patchelf", "--print-needed", str(pam)).splitlines()):
        raise RuntimeError("PAM C++ runtime must be linked statically")
    exports = {line.split()[-1] for line in run("nm", "-D", "--defined-only", str(pam)).splitlines()}
    if exports != {"pam_sm_authenticate", "pam_sm_setcred"}:
        raise RuntimeError("PAM must export only its two C entry points")
    if manifest["pam_glibc_minimum"] != glibc_requirement(pam):
        raise RuntimeError("incorrect PAM glibc requirement")
    env = dict(os.environ, LD_LIBRARY_PATH="", LD_PRELOAD="", LD_AUDIT="", LC_ALL="C")
    for relative in ["usr/bin/su_app", "usr/libexec/smile2unlock/su_authd",
                     "usr/libexec/smile2unlock/su_deploy_helper"]:
        binary = root / relative
        if run("patchelf", "--print-interpreter", str(binary)) != manifest["loader"]:
            raise RuntimeError(f"wrong ELF loader for {relative}")
        directory = os.path.relpath(private, binary.parent)
        expected = f"$ORIGIN/{directory}/glibc:$ORIGIN/{directory}"
        if run("patchelf", "--print-rpath", str(binary)) != expected:
            raise RuntimeError(f"wrong private RPATH for {relative}")
        output = run(str(loader), "--library-path", f"{private}/glibc:{private}", "--list", str(binary), env=env)
        for name, path in re.findall(r"\s*(\S+) => (/.*?) \(0x[0-9a-f]+\)", output):
            if name not in manifest["host_libraries"] and not Path(path).resolve().is_relative_to(private):
                raise RuntimeError(f"dependency escaped the private runtime: {name} => {path}")
        if "not found" in output:
            raise RuntimeError(f"unresolved private dependencies for {relative}")


if __name__ == "__main__":
    try:
        if sys.argv[1] == "--verify":
            verify(Path(sys.argv[2]).resolve())
        else:
            bundle(Path(sys.argv[1]).resolve())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"runtime bundling: {error}")
