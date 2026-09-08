#!/usr/bin/env python3
"""Collect verified, published GitHub release packages for the static installer.

Uses gh's existing authentication. Reuses an existing release asset without
overwriting it; a new candidate archive is uploaded separately by the workflow.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile
import zipfile

try:
    from .prepare_installer import stage, verify_package
except ImportError:
    from prepare_installer import stage, verify_package


def stable_version(tag: str) -> tuple[int, int, int]:
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        raise ValueError("Use a stable release tag such as v0.14.0")
    version = tuple(map(int, tag[1:].split(".")))
    if version < (0, 14, 0):
        raise ValueError("Browser setup requires v0.14.0 or newer")
    return version


def asset_name(tag: str) -> str:
    stable_version(tag)
    return f"bacnet-io-{tag}.tar.gz"


def select_releases(releases: list[dict], recommended: str) -> list[dict]:
    stable_version(recommended)
    eligible = []
    for release in releases:
        if release.get("draft") or release.get("prerelease"):
            continue
        try:
            stable_version(release["tag_name"])
        except (ValueError, KeyError):
            continue
        eligible.append(release)
    current = next((r for r in eligible if r["tag_name"] == recommended), None)
    if current is None:
        raise ValueError("Recommended tag must have a published, non-prerelease GitHub release")
    previous = [r for r in eligible if stable_version(r["tag_name"]) < stable_version(recommended)
                and any(a["name"] == asset_name(r["tag_name"]) for a in r.get("assets", []))]
    previous.sort(key=lambda r: stable_version(r["tag_name"]), reverse=True)
    return [current, *previous[:4]]


def unpack(archive: Path, destination: Path, tag: str) -> Path:
    stable_version(tag)
    with tarfile.open(archive, "r:gz") as source:
        members = source.getmembers()
        if len(members) > 1000 or sum(m.size for m in members) > 64 * 1024 * 1024:
            raise ValueError("Release archive is unexpectedly large")
        seen = set()
        for member in members:
            path = PurePosixPath(member.name)
            if (not path.parts or path.is_absolute() or ".." in path.parts
                    or path.parts[0] != tag or member.name in seen
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe release archive member")
            seen.add(member.name)
        source.extractall(destination, members=members, filter="data")
    directory = destination / tag
    if verify_package(directory)["version"] != tag[1:]:
        raise ValueError("Release archive version does not match its tag")
    return directory


def gh(*arguments: str) -> str:
    return subprocess.check_output(["gh", *arguments], text=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--candidate-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("installer/public/firmware"))
    parser.add_argument("--new-asset", type=Path, required=True)
    parser.add_argument("--new-zip", type=Path, help="create a missing desktop-friendly ZIP from the same verified package")
    args = parser.parse_args()
    stable_version(args.tag)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        raise ValueError("Invalid repository")
    if args.new_asset.exists():
        raise ValueError("New-asset output must not exist")
    if args.new_zip and args.new_zip.exists():
        raise ValueError("New-ZIP output must not exist")
    pages = json.loads(gh("api", "--paginate", "--slurp",
                          f"repos/{args.repository}/releases?per_page=100"))
    releases = select_releases([r for page in pages for r in page], args.tag)
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        directories = []
        for release in releases:
            tag = release["tag_name"]
            name = asset_name(tag)
            if any(a["name"] == name for a in release.get("assets", [])):
                gh("release", "download", tag, "--repo", args.repository,
                   "--pattern", name, "--dir", str(root))
                directories.append(unpack(root / name, root / "unpacked", tag))
            else:
                candidate = args.candidate_root / tag
                if verify_package(candidate)["version"] != tag[1:]:
                    raise ValueError("Candidate firmware version does not match the release tag")
                directories.append(candidate)
                args.new_asset.parent.mkdir(parents=True, exist_ok=True)
                with tarfile.open(args.new_asset, "w:gz") as archive:
                    archive.add(candidate, arcname=tag)
        catalog = stage(directories, args.output, args.tag[1:])
        if args.new_zip and not any(a["name"] == f"bacnet-io-{args.tag}.zip"
                                    for a in releases[0].get("assets", [])):
            args.new_zip.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(args.new_zip, "x", compression=zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(directories[0].rglob("*")):
                    if path.is_file():
                        archive.write(path, Path(args.tag) / path.relative_to(directories[0]))
        print(f"Verified {len(catalog['releases'])} published release(s)")


if __name__ == "__main__":
    main()
