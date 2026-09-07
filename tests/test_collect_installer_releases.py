import io
import contextlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from tools.collect_installer_releases import main, select_releases, stable_version, unpack
from test_prepare_installer import package


def release(tag, **extra):
    return {"tag_name": tag, "draft": False, "prerelease": False,
            "assets": [{"name": f"bacnet-io-{tag}.tar.gz"}], **extra}


class CollectInstallerReleasesTests(unittest.TestCase):
    def test_new_release_stages_verified_candidate_and_creates_upload_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = package(root).rename(root / "v0.14.0")
            output, archive = root / "site", root / "upload.tar.gz"
            arguments = ["collect", "--repository", "owner/repo", "--tag", "v0.14.0",
                         "--candidate-root", str(root), "--output", str(output), "--new-asset", str(archive)]
            with patch("sys.argv", arguments), patch("tools.collect_installer_releases.gh", return_value=json.dumps([[release("v0.14.0", assets=[])]])), contextlib.redirect_stdout(io.StringIO()):
                main()
            self.assertEqual(json.loads((output / "catalog.json").read_text())["recommended"], "0.14.0")
            restored = unpack(archive, root / "restored", "v0.14.0")
            self.assertEqual((restored / "initial-flash.bin").read_bytes(), (candidate / "initial-flash.bin").read_bytes())

    def test_rerun_uses_original_asset_without_candidate_or_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = package(root)
            archive = root / "original.tar.gz"
            with tarfile.open(archive, "w:gz") as target:
                target.add(source, arcname="v0.14.0")
            def fake_gh(*arguments):
                if arguments[0] == "api":
                    return json.dumps([[release("v0.14.0")]])
                self.assertEqual(arguments[:3], ("release", "download", "v0.14.0"))
                destination = Path(arguments[arguments.index("--dir") + 1])
                shutil.copyfile(archive, destination / "bacnet-io-v0.14.0.tar.gz")
                return ""
            arguments = ["collect", "--repository", "owner/repo", "--tag", "v0.14.0",
                         "--candidate-root", str(root / "missing"), "--output", str(root / "site"),
                         "--new-asset", str(root / "replacement.tar.gz")]
            with patch("sys.argv", arguments), patch("tools.collect_installer_releases.gh", side_effect=fake_gh), contextlib.redirect_stdout(io.StringIO()):
                main()
            self.assertFalse((root / "replacement.tar.gz").exists())
            self.assertEqual((root / "site/0.14.0/initial-flash.bin").read_bytes(), (source / "initial-flash.bin").read_bytes())

    def test_only_approved_older_packages_are_kept(self):
        releases = [release("v0.14.2"), release("v0.14.1"), release("v0.14.0"),
                    release("v0.15.0"), release("v0.13.5"),
                    release("v0.14.3", draft=True), release("v0.14.4", prerelease=True)]
        self.assertEqual([r["tag_name"] for r in select_releases(releases, "v0.14.2")],
                         ["v0.14.2", "v0.14.1", "v0.14.0"])

    def test_unpublished_and_prerelease_cannot_be_recommended(self):
        for releases in [[], [release("v0.14.0", draft=True)], [release("v0.14.0", prerelease=True)]]:
            with self.assertRaisesRegex(ValueError, "published"):
                select_releases(releases, "v0.14.0")
        for tag in ["main", "v0.13.5", "v0.14.0-rc1", "../v0.14.0"]:
            with self.assertRaises(ValueError):
                stable_version(tag)

    def test_caps_history_and_skips_missing_assets(self):
        releases = [release(f"v0.14.{n}") for n in range(10)]
        releases[8]["assets"] = []
        self.assertEqual([r["tag_name"] for r in select_releases(releases, "v0.14.9")],
                         ["v0.14.9", "v0.14.7", "v0.14.6", "v0.14.5", "v0.14.4"])

    def test_extracts_verified_package_and_rejects_traversal_and_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = package(root)
            archive = root / "release.tar.gz"
            with tarfile.open(archive, "w:gz") as target:
                target.add(directory, arcname="v0.14.0")
            self.assertEqual(unpack(archive, root / "ok", "v0.14.0").name, "v0.14.0")
            for name, kind in [("../escape", tarfile.REGTYPE),
                               ("v0.14.0/link", tarfile.SYMTYPE),
                               ("v0.14.0/../../escape", tarfile.REGTYPE),
                               ("v0.14.1/wrong", tarfile.REGTYPE)]:
                with tarfile.open(archive, "w:gz") as target:
                    member = tarfile.TarInfo(name)
                    member.type = kind
                    member.linkname = "/tmp/escape"
                    target.addfile(member, io.BytesIO())
                with self.assertRaisesRegex(ValueError, "Unsafe"):
                    unpack(archive, root / "bad", "v0.14.0")
