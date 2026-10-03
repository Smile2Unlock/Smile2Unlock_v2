#!/usr/bin/env python3
"""Generate OBS build inputs from a verified upstream Linux release archive."""
import argparse
import gzip
import hashlib
import io
import json
import re
import shutil
import tarfile
from datetime import datetime, timezone
from pathlib import Path
from xml.etree import ElementTree as ET

TEMPLATES = Path(__file__).resolve().parent


def digest(data, algorithm="sha256"):
    return hashlib.new(algorithm, data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("version must be a stable X.Y.Z release")
    archive = args.archive.read_bytes()
    if digest(archive) != args.sha256:
        raise SystemExit("upstream archive SHA256 mismatch")
    prefix = f"smile2unlock-{args.version}/"
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        runtime = json.load(tar.extractfile(prefix + "usr/share/doc/smile2unlock/runtime-libraries.json"))
        manifest = json.load(tar.extractfile(prefix + "usr/share/smile2unlock/release-info.json"))
    if manifest["version"] != args.version or manifest["build"] != {"platform": "linux", "arch": "x86_64"}:
        raise SystemExit("upstream version or architecture mismatch")
    release_date = datetime.fromisoformat(manifest["date"]).replace(tzinfo=timezone.utc)
    glibc = runtime["pam_glibc_minimum"]
    if not re.fullmatch(r"\d+\.\d+", glibc):
        raise SystemExit("invalid PAM glibc minimum")
    output = args.output.resolve()
    if output == TEMPLATES or output in TEMPLATES.parents:
        parser.error("output must not overwrite the templates")
    output.mkdir(parents=True, exist_ok=True)
    values = {
        "VERSION": args.version, "SHA256": args.sha256, "GLIBC": glibc,
        "HELPER_SHA256": digest((TEMPLATES / "prepare-payload.py").read_bytes()),
        "HOOK_SHA256": digest((TEMPLATES / "smile2unlock-remove.hook").read_bytes()),
    }

    def render(path):
        text = path.read_text()
        for name, value in values.items():
            text = text.replace(f"@{name}@", value)
        return text.encode()

    for name in ("smile2unlock.spec", "PKGBUILD", "smile2unlock.install"):
        (output / name).write_bytes(render(TEMPLATES / (name + ".in")))
    for name in ("prepare-payload.py", "smile2unlock-remove.hook"):
        shutil.copyfile(TEMPLATES / name, output / name)
    debian = {}
    for path in sorted((TEMPLATES / "debian").iterdir()):
        name = path.name.removesuffix(".in")
        debian[name] = render(path)
    debian["prepare-payload.py"] = (TEMPLATES / "prepare-payload.py").read_bytes()
    debian["source/format"] = b"3.0 (quilt)\n"
    debian["copyright"] = b"See usr/share/doc/smile2unlock/LICENSE and licenses/ in the upstream payload.\nThis package redistributes upstream release binaries and their complete license notices.\n"
    debian["changelog"] = (
        f"smile2unlock ({args.version}-1) unstable; urgency=medium\n\n"
        "  * Repackage checksum-verified upstream Linux release for OBS.\n\n"
        f" -- Smile2Unlock maintainers <ation_ciger@sgxi.cn>  {release_date.strftime('%a, %d %b %Y %H:%M:%S +0000')}\n"
    ).encode()
    stream = io.BytesIO()
    with gzip.GzipFile(fileobj=stream, mode="wb", mtime=0, filename="") as gz:
        with tarfile.open(fileobj=gz, mode="w") as tar:
            for name, data in sorted(debian.items()):
                info = tarfile.TarInfo("debian/" + name)
                info.size = len(data)
                info.mode = 0o755 if name in {"rules", "preinst", "postinst", "prerm", "postrm"} else 0o644
                tar.addfile(info, io.BytesIO(data))
    debian_archive = stream.getvalue()
    debian_name = f"smile2unlock_{args.version}-1.debian.tar.gz"
    original_name = f"smile2unlock_{args.version}.orig.tar.gz"
    (output / debian_name).write_bytes(debian_archive)
    files = [(original_name, archive), (debian_name, debian_archive)]
    dsc = (
        "Format: 3.0 (quilt)\nSource: smile2unlock\nBinary: smile2unlock\nArchitecture: amd64\n"
        f"Version: {args.version}-1\nMaintainer: Smile2Unlock maintainers <ation_ciger@sgxi.cn>\n"
        "Homepage: https://github.com/Smile2Unlock/Smile2Unlock_v2\n"
        "Standards-Version: 4.7.0\nBuild-Depends: debhelper-compat (= 13), python3\n"
    )
    for header, algorithm in [("Checksums-Sha1", "sha1"), ("Checksums-Sha256", "sha256"), ("Files", "md5")]:
        dsc += header + ":\n"
        for name, data in files:
            dsc += f" {digest(data, algorithm)} {len(data)} {name}\n"
    (output / "smile2unlock.dsc").write_text(dsc)
    services = ET.Element("services")
    download = ET.SubElement(services, "service", name="download_url")
    params = {
        "protocol": "https", "host": "github.com",
        "path": f"/Smile2Unlock/Smile2Unlock_v2/releases/download/v{args.version}/smile2unlock-{args.version}-linux-x86_64.tar.gz",
        "filename": original_name,
    }
    for name, value in params.items():
        ET.SubElement(download, "param", name=name).text = value
    verify = ET.SubElement(services, "service", name="verify_file")
    for name, value in {"file": "_service:download_url:" + original_name, "verifier": "sha256", "checksum": args.sha256}.items():
        ET.SubElement(verify, "param", name=name).text = value
    ET.indent(services)
    (output / "_service").write_text(ET.tostring(services, encoding="unicode") + "\n")
    print(f"Generated {args.version} build inputs in {output}; PAM requires glibc >= {glibc}")


if __name__ == "__main__":
    main()
