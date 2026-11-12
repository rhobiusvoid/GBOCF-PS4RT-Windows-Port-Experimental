"""DLC folder title validation must match the player PKG-install path."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import player_setup as setup

TITLE = 'CUSA16429'
LABEL = 'NEWSAKURADLC0001'
CID = f'EP0177-{TITLE}_00-{LABEL}'

class AssetDlcSafetyTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.dlc = self.root / 'dlc'
        self.dlc.mkdir()
        self.payload = self.dlc / LABEL
        self.payload.mkdir()

    def installed(self, *, title=TITLE, category='ac', content_id=CID):
        data = {'TITLE_ID': title, 'CATEGORY': category,
                'CONTENT_ID': content_id}
        with patch.object(setup, 'DLC_ENABLED', True), \
             patch.object(setup, 'TITLE_ID', TITLE), \
             patch.object(setup, 'find_addons',
                          side_effect=[{LABEL: self.payload}, {}]), \
             patch.object(setup, 'load_sfo', return_value=data):
            return setup.installed_addons(self.root, explicit=self.dlc)

    def test_matching_asset_dlc_is_mounted(self):
        self.assertEqual(self.installed(), {LABEL: self.payload})

    def test_wrong_cusa_in_content_id_is_rejected(self):
        with self.assertRaisesRegex(setup.SetupError, 'does not match'):
            self.installed(content_id=CID.replace(TITLE, 'CUSA99999'))

    def test_inconsistent_sfo_title_is_rejected(self):
        with self.assertRaisesRegex(setup.SetupError, 'does not match'):
            self.installed(title='CUSA99999')

    def test_non_dlc_category_is_rejected(self):
        with self.assertRaisesRegex(setup.SetupError, 'not an asset'):
            self.installed(category='gp')

    def test_bad_label_content_id_pair_is_rejected(self):
        with self.assertRaisesRegex(setup.SetupError, 'does not match'):
            self.installed(content_id=CID[:-16] + 'ANOTHERDLC000001')

if __name__ == '__main__':
    unittest.main()
