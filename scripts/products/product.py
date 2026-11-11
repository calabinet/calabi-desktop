#!/usr/bin/env python3
"""Prepare a pinned core plus one product; never publishes or signs anything."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PRODUCTS = ("desktop", "android")


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd or ROOT, text=True, encoding="utf-8").strip()


def config(product, root=ROOT):
    base = root / "apps" / ("client-" + product)
    version = (base / "VERSION").read_text(encoding="utf-8").strip()
    lock = json.loads((base / "core-source.json").read_text(encoding="utf-8"))
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("product VERSION must be MAJOR.MINOR.PATCH")
    if lock["repository"] != "https://github.com/calabinet/calabi.git":
        raise ValueError("core repository must be the authoritative public repository")
    if not re.fullmatch(r"[0-9a-f]{40}", lock["commit"]):
        raise ValueError("core commit must be a full SHA")
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", lock["version"]):
        raise ValueError("invalid core version")
    return version, lock


def copy_paths(source, dest, paths):
    files = run("git", "ls-files", "--", *paths, cwd=source).splitlines()
    if not files:
        raise ValueError("no tracked product source")
    for name in files:
        src = source / name
        if not src.is_file():
            continue
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)


def paths(product):
    common = ["README.md", "CHANGELOG.md", "apps/client-" + product, "scripts/products/product.py", "scripts/products/publish-release.py", "scripts/updatekit", "scripts/dev/build.ps1",
              "scripts/dev/build-cache.ps1", "scripts/dev/build-cache-policy.psd1", "scripts/dev/build-guard.sh"]
    if product == "android":
        return common + ["scripts/mobile"]
    return common + ["tools/rdp-p0", "tools/rdp-finder-menu", "scripts/build-desktop.ps1",
                     "scripts/build-desktop.sh", "scripts/gen-desktop-icons.ps1", "scripts/gen-desktop-icons.sh",
                     "scripts/bundle-macos-rdp.sh", "scripts/verify-macos-rdp-bundle.sh",
                     "scripts/package-macos-pkg.sh", "scripts/stage-release-installer.sh",
                     "scripts/macos-installer-resources", "scripts/gen-source-icon"]


def prepare(product, destination, override=None):
    version, lock = config(product)
    dest = destination.resolve()
    if dest.exists() and any(dest.iterdir()):
        raise ValueError("destination must be empty; existing directories are never removed")
    if dest == ROOT or ROOT in dest.parents:
        # Composed build trees may live under dist, never inside product source.
        if not (ROOT / "dist") in dest.parents:
            raise ValueError("use a separate directory or dist for a composed build")
    dest.mkdir(parents=True, exist_ok=True)
    if override:
        core = override.resolve()
        run("git", "clone", "--no-hardlinks", "--no-checkout", str(core), str(dest))
        commit = run("git", "rev-parse", "HEAD", cwd=core)
        run("git", "checkout", "--detach", commit, cwd=dest)
        # Local override explicitly includes tracked working-tree edits.
        copy_paths(core, dest, ["apps/client", "pkg", "VERSION", "go.work", "go.work.sum"])
        core_version = (core / "VERSION").read_text(encoding="utf-8").strip()
    else:
        run("git", "init", str(dest))
        run("git", "remote", "add", "origin", lock["repository"], cwd=dest)
        run("git", "fetch", "--depth=1", "origin", lock["commit"], cwd=dest)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=dest)
        commit = run("git", "rev-parse", "HEAD", cwd=dest)
        if commit != lock["commit"]:
            raise ValueError("fetched core commit does not match lock")
        core_version = (dest / "VERSION").read_text(encoding="utf-8").strip()
        if core_version != lock["version"]:
            raise ValueError("core VERSION does not match lock")
    copy_paths(ROOT, dest, paths(product))
    with (dest / "go.work").open("a", encoding="utf-8", newline="\n") as work:
        work.write("\nuse ./scripts/updatekit\n")
        if product == "desktop":
            work.write("use ./scripts/gen-source-icon\n")
    # Old core revisions may still contain other product sources. They are not used.
    actual = {"repository": lock["repository"], "version": core_version, "commit": commit}
    (dest / "apps" / ("client-" + product) / "core-source.json").write_text(
        json.dumps(actual, indent=2) + "\n", encoding="utf-8", newline="\n")
    origin = run("git", "remote", "get-url", "origin").removesuffix(".git").replace("git@github.com:", "https://github.com/")
    expected_origin = "https://github.com/calabinet/calabi-" + product
    # Never persist credentials or private remote addresses in build provenance.
    source_repository = expected_origin if origin == expected_origin else "development"
    manifest = {"product": product, "version": version, "core": actual,
                "source_commit": run("git", "rev-parse", "HEAD"), "local_override": bool(override),
                "source_repository": source_repository,
                "source_dirty": bool(run("git", "status", "--porcelain", "--", *paths(product)))}
    if product == "android":
        manifest["toolchain"] = json.loads((ROOT / "apps/client-android/mobile-toolchain.json").read_text(encoding="utf-8"))
    input_files = set(run("git", "ls-files", cwd=dest).splitlines())
    input_files.update(run("git", "ls-files", "--", *paths(product)).splitlines())
    manifest["inputs"] = {name: hashlib.sha256((dest / name).read_bytes()).hexdigest()
                          for name in sorted(input_files) if (dest / name).is_file()}
    (dest / "product-build.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({**{k: v for k, v in manifest.items() if k != "inputs"}, "input_files": len(manifest["inputs"])}, indent=2))


def manifest(product, directory):
    version, lock = config(product)
    source = json.loads((ROOT / "product-build.json").read_text(encoding="utf-8"))
    expected_repo = "https://github.com/calabinet/calabi-" + product
    source_repo = source.get("source_repository", "").removesuffix(".git").replace("git@github.com:", "https://github.com/")
    if source.get("source_dirty", True) or source_repo != expected_repo:
        raise ValueError("release metadata requires a clean public product checkout")
    if source["local_override"]:
        raise ValueError("local core overrides cannot produce release metadata")
    if source["product"] != product or source["version"] != version or source["core"] != lock:
        raise ValueError("build provenance differs from product configuration")
    if not source.get("inputs"):
        raise ValueError("build provenance has no source input hashes")
    for name, digest in source["inputs"].items():
        path = ROOT / name
        if Path(name).is_absolute() or ".." in Path(name).parts or not path.resolve().is_relative_to(ROOT):
            raise ValueError("source input path escaped build tree")
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError("source changed after preparation: " + name)
    allowed = {"desktop": {"calabi-desktop-windows-x64-setup.exe", "calabi-desktop-macos-universal.pkg"},
               "android": {"calabi-android.apk"}}[product]
    files = sorted(p for p in directory.iterdir() if p.name in allowed)
    if not files:
        raise ValueError("no product installers to stage")
    sums = "".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n" for p in files)
    (directory / "SHA256SUMS").write_text(sums, encoding="utf-8", newline="\n")
    source["artifacts"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    (directory / "build-manifest.json").write_text(json.dumps(source, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(sums, end="")


def archive_source(product, destination, edge_ca=None):
    version, core = config(product)
    record = json.loads((ROOT / "product-build.json").read_text(encoding="utf-8"))
    if record.get("local_override", True) or record.get("source_dirty", True) or record.get("source_repository") != "https://github.com/calabinet/calabi-" + product:
        raise ValueError("source archives require clean public product inputs")
    if record["product"] != product or record["version"] != version or record["core"] != core:
        raise ValueError("source archive configuration differs from preparation")
    if not record.get("inputs"):
        raise ValueError("missing source input hashes")
    for name, digest in record["inputs"].items():
        source = ROOT / name
        if Path(name).is_absolute() or ".." in Path(name).parts or not source.resolve().is_relative_to(ROOT):
            raise ValueError("archive source escaped prepared build")
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError("source changed after preparation: " + name)
    certificate = edge_ca.read_bytes() if edge_ca else None
    if certificate and (b"BEGIN CERTIFICATE" not in certificate or b"PRIVATE KEY" in certificate):
        raise ValueError("only a public PEM certificate may enter a source archive")
    if destination.suffix != ".zip" or destination.exists():
        raise ValueError("choose a new .zip destination")
    destination.parent.mkdir(parents=True, exist_ok=True)
    instructions = "# Prepared Calabi " + product + " source\n\nProduct " + version + "; core " + core["version"] + " at " + core["commit"] + ".\n\n"
    instructions += "This archive includes the fixed public core and product sources. It needs no Git checkout. Verify source identities with product-build.json.\n\n"
    if product == "desktop":
        prefix = 'EDGE_CA="$PWD/calabi-edge-ca.pem" ' if certificate else ''
        instructions += "On macOS, install Python 3, Go, Node.js, Rust/Tauri CLI and the native tools from README.md. Then run:\n\n```sh\n" + prefix + "bash scripts/build-desktop.sh --target universal-apple-darwin\nbash scripts/package-macos-pkg.sh\n```\n\n"
    else:
        instructions += "Install the pinned JDK, Android SDK/NDK, Go, gomobile and Gradle from README.md. Use scripts/mobile/build-release-android.ps1 for an officially signed upgrade; keep signing credentials outside the archive.\n\n"
    instructions += "These sources are for validation. Formal publication, platform signing and device upgrade acceptance are separate release steps. Build both desktop platforms from the same selected product source commit.\n"
    metadata = {"product": product, "version": version, "source_commit": record["source_commit"], "core": core,
                "public_edge_ca_sha256": hashlib.sha256(certificate).hexdigest() if certificate else None}
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for name in sorted(record["inputs"]):
            output.write(ROOT / name, name)
        output.write(ROOT / "product-build.json", "product-build.json")
        output.writestr("ARCHIVE-INSTRUCTIONS.md", instructions)
        output.writestr("SOURCE-ARCHIVE.json", json.dumps(metadata, indent=2) + "\n")
        if certificate:
            output.writestr("calabi-edge-ca.pem", certificate)
    print(json.dumps({**metadata, "archive": str(destination), "sha256": hashlib.sha256(destination.read_bytes()).hexdigest()}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["check", "prepare", "manifest", "core-commit", "archive"])
    parser.add_argument("product", choices=PRODUCTS)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--edge-ca", type=Path)
    parser.add_argument("--core-tree", type=Path, help="explicit development override; never releasable")
    args = parser.parse_args()
    if args.action == "archive":
        if not args.destination:
            parser.error("archive requires --destination")
        archive_source(args.product, args.destination.resolve(), args.edge_ca)
    elif args.action == "core-commit":
        provenance = ROOT / "product-build.json"
        if provenance.is_file():
            core = json.loads(provenance.read_text(encoding="utf-8"))["core"]
            if core["version"] != (ROOT / "VERSION").read_text(encoding="utf-8").strip() or not re.fullmatch(r"[0-9a-f]{40}", core["commit"]):
                raise ValueError("archive core provenance differs from VERSION")
            print(core["commit"])
        else:
            print(run("git", "rev-parse", "HEAD"))
    elif args.action == "check":
        print(json.dumps(dict(zip(["version", "core"], config(args.product))), indent=2))
    elif args.action == "prepare":
        if not args.destination:
            parser.error("prepare requires --destination")
        prepare(args.product, args.destination, args.core_tree)
    else:
        manifest(args.product, args.destination or ROOT / "dist" / ("release-" + args.product))


if __name__ == "__main__":
    main()
