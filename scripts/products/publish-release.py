#!/usr/bin/env python3
"""Verify and preview one product release. Publishing requires --publish and an existing matching tag."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import zipfile
from pathlib import Path

from product import ROOT, config, run

PUBKEY = "+Un1Ssse7lflEeEPsr382+j7WduOrY8K7f4xZPMZ5fs="


def release(product, directory, publish=False):
    version, core = config(product)
    manifest_file = directory / "build-manifest.json"
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    repo = "calabinet/calabi-" + product
    origin = manifest.get("source_repository", "").removesuffix(".git").replace("git@github.com:", "https://github.com/")
    if manifest.get("product") != product or manifest.get("version") != version or manifest.get("core") != core:
        raise ValueError("manifest differs from product version/core lock")
    if manifest.get("local_override", True) or manifest.get("source_dirty", True) or origin != "https://github.com/" + repo:
        raise ValueError("release needs a clean public product build")
    expected = {"desktop": {"calabi-desktop-windows-x64-setup.exe", "calabi-desktop-macos-universal.pkg"},
                "android": {"calabi-android.apk"}}[product]
    if set(manifest["artifacts"]) != expected:
        raise ValueError("release requires the complete product installer set")
    sums = {}
    for line in (directory / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        digest, name = line.split()
        if name in sums:
            raise ValueError("duplicate checksum entry")
        sums[name] = digest
    if sums != manifest["artifacts"]:
        raise ValueError("checksums differ from signed provenance")
    for name, digest in sums.items():
        if Path(name).name != name or hashlib.sha256((directory / name).read_bytes()).hexdigest() != digest:
            raise ValueError("installer digest mismatch: " + name)
    if product == "android":
        sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
        if not sdk:
            raise ValueError("set ANDROID_HOME to verify the APK before release")
        tools = sorted((Path(sdk) / "build-tools").iterdir(), key=lambda p: tuple(int(n) for n in p.name.split(".") if n.isdigit()))[-1]
        apk = directory / "calabi-android.apk"
        signer = tools / ("apksigner.bat" if os.name == "nt" else "apksigner")
        certs = subprocess.check_output([str(signer), "verify", "--print-certs", str(apk)], text=True, encoding="utf-8", errors="replace")
        pinned = (ROOT / "scripts/mobile/android-release-cert.sha256").read_text(encoding="utf-8").strip().replace(":", "").lower()
        digests = [v.lower() for v in re.findall(r"certificate SHA-256 digest:\s*([0-9a-fA-F]+)", certs)]
        if digests != [pinned]:
            raise ValueError("APK signer differs from the established release certificate")
        aapt = tools / ("aapt2.exe" if os.name == "nt" else "aapt2")
        package = subprocess.check_output([str(aapt), "dump", "badging", str(apk)], text=True, encoding="utf-8", errors="replace").splitlines()[0]
        code = sum(int(v) * scale for v, scale in zip(version.split("."), [1000000, 1000, 1]))
        if "name='net.calabi.app'" not in package or f"versionName='{version}'" not in package or f"versionCode='{code}'" not in package:
            raise ValueError("APK identity or version differs from product release metadata")
        with zipfile.ZipFile(apk) as archive:
            for abi in ["arm64-v8a", "armeabi-v7a"]:
                library = archive.read("lib/" + abi + "/libgojni.so")
                if core["commit"].encode() not in library or core["version"].encode() not in library:
                    raise ValueError("APK core differs from the pinned source: " + abi)
    signature = directory / "build-manifest.json.sig"
    if not signature.is_file():
        raise ValueError("sign build-manifest.json with updatekit sign-file first")
    cmd = ["go", "run", "./scripts/updatekit", "verify-file", "--file", str(manifest_file),
           "--sig", str(signature), "--pubkey", PUBKEY]
    if os.name == "nt":
        cmd = ["pwsh", "-NoProfile", "-File", str(ROOT / "scripts/dev/build.ps1")] + cmd
    subprocess.run(cmd, cwd=ROOT, check=True)
    if product == "desktop":
        descriptor = directory / "latest.json"
        update = json.loads(descriptor.read_text(encoding="utf-8"))
        expected_platforms = {"windows-x86_64": "calabi-desktop-windows-x64-setup.exe", "darwin-universal": "calabi-desktop-macos-universal.pkg"}
        if update["version"] != version or set(update["platforms"]) != set(expected_platforms):
            raise ValueError("desktop update descriptor must contain only both desktop platforms at the product version")
        for platform, name in expected_platforms.items():
            entry = update["platforms"][platform]
            if entry["sha256"] != sums[name] or not entry.get("signature"):
                raise ValueError("desktop update artifact differs from release provenance")
        verify_update = list(cmd)
        verify_update[verify_update.index("--file")+1] = str(descriptor)
        verify_update[verify_update.index("--sig")+1] = str(directory / "latest.json.sig")
        subprocess.run(verify_update, cwd=ROOT, check=True)
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    section = re.search(r"^## " + re.escape(version) + r"[^\n]*\n(.*?)(?=^## |\Z)", changelog, re.M | re.S)
    if not section:
        raise ValueError("missing product changelog")
    notes = directory / "release-notes.md"
    text = section[1].strip() + "\n\nBundled core: " + core["version"] + " (`" + core["commit"] + "`).\n"
    if product == "android":
        certificate = (ROOT / "scripts/mobile/android-release-cert.sha256").read_text(encoding="utf-8").strip()
        if not re.fullmatch(r"[0-9A-Fa-f:]{64,95}", certificate):
            raise ValueError("invalid pinned Android signer fingerprint")
        text += "\nAndroid signing certificate SHA-256: `" + certificate + "`.\n"
        text += "Verify with `apksigner verify --print-certs calabi-android.apk`.\n"
    notes.write_text(text, encoding="utf-8")
    attachments = [str(directory / n) for n in sorted(expected)] + [str(directory / "SHA256SUMS"), str(manifest_file), str(signature)]
    print(json.dumps({"repository": repo, "tag": "v"+version, "source_commit": manifest["source_commit"], "attachments": attachments}, indent=2))
    if publish:
        tag = run("gh", "api", "repos/"+repo+"/git/ref/tags/v"+version, "--jq", ".object.sha")
        kind = run("gh", "api", "repos/"+repo+"/git/ref/tags/v"+version, "--jq", ".object.type")
        if kind == "tag":
            tag = run("gh", "api", "repos/"+repo+"/git/tags/"+tag, "--jq", ".object.sha")
        if tag != manifest["source_commit"]:
            raise ValueError("public release tag does not match the built product source")
        subprocess.run(["gh", "release", "create", "v"+version, "--repo", repo, "--verify-tag",
                        "--title", "Calabi "+product+" "+version, "--notes-file", str(notes)] + attachments, cwd=ROOT, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("product", choices=["desktop", "android"])
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--publish", action="store_true")
    args = parser.parse_args()
    release(args.product, (args.directory or ROOT / "dist" / ("release-"+args.product)).resolve(), args.publish)
