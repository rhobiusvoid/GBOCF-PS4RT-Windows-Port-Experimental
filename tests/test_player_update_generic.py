"""Synthetic player-update safety tests; no proprietary PS4 game files."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import patch_assembly as updater

TITLE = "CUSA54321"
PRODUCT = "UP9999-CUSA54321_00-EXAMPLEPORT0001"
BASE = "01.03"
TARGET = "01.07"


def metadata(path):
    return json.loads(path.read_text())


def game(folder, version, category, product=PRODUCT):
    (folder / "sce_sys").mkdir(parents=True)
    (folder / "sce_sys/param.sfo").write_text(json.dumps({
        "TITLE_ID": TITLE, "APP_VER": version,
        "CATEGORY": category, "CONTENT_ID": product,
    }))
    (folder / "eboot.bin").write_bytes(("ELF " + version).encode())
    return folder


class PlayerUpdateTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.base = game(self.root / "base", BASE, "gd")
        self.update = game(self.root / "update", TARGET, "gp")
        (self.base / "asset.dat").write_text("old")
        (self.update / "asset.dat").write_text("new")
        (self.update / "extra.dat").write_text("added")

    def apply(self):
        return updater.apply_in_place(
            self.base, self.update, metadata,
            expected_id=TITLE, base_version=BASE, target_version=TARGET)

    def test_generic_version_and_content_install(self):
        result = self.apply()
        self.assertEqual(result["installation_mode"], "in-place")
        self.assertEqual((self.base / "asset.dat").read_text(), "new")
        self.assertEqual((self.base / "extra.dat").read_text(), "added")
        self.assertEqual(metadata(self.base / "sce_sys/param.sfo")["APP_VER"], TARGET)
        self.assertFalse(updater.incomplete_marker(self.base).exists())

    def test_mismatched_content_id_rejected_before_mutation(self):
        location = self.update / "sce_sys/param.sfo"
        info = metadata(location)
        info["CONTENT_ID"] = "UP9999-CUSA54321_00-ANOTHERPRODUCT00"
        location.write_text(json.dumps(info))
        with self.assertRaisesRegex(updater.PatchError, "CONTENT_ID mismatch"):
            self.apply()
        self.assertEqual((self.base / "asset.dat").read_text(), "old")
        self.assertFalse(updater.incomplete_marker(self.base).exists())

    def test_incorrect_update_version_is_rejected(self):
        location = self.update / "sce_sys/param.sfo"
        info = metadata(location)
        info["APP_VER"] = "01.08"
        location.write_text(json.dumps(info))
        with self.assertRaisesRegex(updater.PatchError, "matching v01.07"):
            self.apply()
        self.assertEqual((self.base / "asset.dat").read_text(), "old")

    def test_interruption_marker_and_exact_package_recovery(self):
        original = updater.os.replace
        count = [0]
        def interrupt(source, destination):
            if count[0] == 0:
                count[0] += 1
                raise OSError("simulated interruption")
            return original(source, destination)
        with patch.object(updater.os, "replace", side_effect=interrupt):
            with self.assertRaisesRegex(OSError, "simulated interruption"):
                self.apply()
        marker = updater.incomplete_marker(self.base)
        self.assertTrue(marker.is_file())
        self.assertEqual(json.loads(marker.read_text())["version"], TARGET)
        self.apply()
        self.assertFalse(marker.exists())
        self.assertEqual((self.base / "asset.dat").read_text(), "new")

    def test_modified_update_executable_cannot_resume(self):
        with patch.object(updater.os, "replace", side_effect=OSError("interrupted")):
            with self.assertRaises(OSError):
                self.apply()
        (self.update / "eboot.bin").write_bytes(b"changed update")
        with self.assertRaisesRegex(updater.PatchError, "another package"):
            self.apply()

    def test_symlinked_update_content_is_rejected(self):
        secret = self.root / "secret.txt"
        secret.write_text("do not access")
        (self.update / "link.dat").symlink_to(secret)
        with self.assertRaises(updater.PatchError):
            self.apply()
        self.assertFalse(updater.incomplete_marker(self.base).exists())


if __name__ == "__main__":
    unittest.main()
