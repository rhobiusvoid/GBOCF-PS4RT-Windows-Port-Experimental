"""Synthetic launcher flow matrix for update-only, DLC-only, both and neither."""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import ps4rt_launch as launcher


class PlayerRestart(Exception):
    pass


class GameBoot(Exception):
    pass


class PlayerModeTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.game = self.root / 'CUSA45678'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(b'fixture')
        (self.root / 'bin').mkdir()
        (self.root / 'bin/ps4rt').write_bytes(b'fixture')

    def launch(self, updates, dlc, *, version='01.01', extra=()):
        feature = {
            'schema_version': 1, 'title_id': 'CUSA45678',
            'display_name': 'Synthetic Fixture', 'base_version': '01.00',
            'target_version': '01.01' if updates else None,
            'supports_updates': updates, 'supports_asset_dlc': dlc,
            'needs_native_pkg': updates or dlc
        }
        (self.root / 'port_features.json').write_text(json.dumps(feature))
        actions = {'select': Mock(return_value=[]),
                   'install': Mock(return_value=[]),
                   'resolve': Mock(side_effect=lambda root, game, **kw: game),
                   'addons': Mock(return_value={})}
        setup = types.ModuleType('player_setup')
        setup.resolve_game = actions['resolve']
        setup.installed_addons = actions['addons']
        setup.install_dlc = actions['install']
        setup.add_dlc_dialog = Mock(return_value=0)
        setup.choose_setup = actions['select']
        setup.DLC_INSTALLED_EXIT = 10
        patcher = types.ModuleType('patch_assembly')
        patcher.incomplete_marker = lambda game: game / '.ps4rt-update-incomplete.json'
        with patch.dict(sys.modules, {'player_setup': setup, 'patch_assembly': patcher}), \
             patch.object(launcher, 'ROOT', self.root), \
             patch.object(launcher, 'load_sfo',
                          return_value={'TITLE_ID': 'CUSA45678', 'APP_VER': version}), \
             patch.object(launcher, 'prepare'), \
             patch.object(launcher, 'link'), \
             patch.object(launcher, 'prepare_content'), \
             patch.object(launcher.os, 'execv', side_effect=PlayerRestart), \
             patch.object(launcher.os, 'execve', side_effect=GameBoot):
            try:
                launcher.main(['--game', str(self.game), *extra])
            except PlayerRestart:
                status = 'restarted'
            except GameBoot:
                status = 'booted'
            except SystemExit:
                status = 'rejected'
        return status, actions

    def test_update_only_never_opens_dlc_picker(self):
        status, actions = self.launch(True, False, version='01.00')
        self.assertEqual(status, 'restarted')
        actions['resolve'].assert_called_once()
        actions['select'].assert_not_called()
        actions['install'].assert_not_called()

    def test_combined_update_offers_optional_dlc(self):
        status, actions = self.launch(True, True, version='01.00')
        self.assertEqual(status, 'restarted')
        actions['select'].assert_called_once_with(self.root, 'dlc')

    def test_no_prompt_when_game_is_already_updated(self):
        status, actions = self.launch(True, True, version='01.01')
        self.assertEqual(status, 'booted')
        actions['select'].assert_not_called()

    def test_dlc_only_does_not_call_game_update(self):
        status, actions = self.launch(False, True)
        self.assertEqual(status, 'booted')
        actions['resolve'].assert_not_called()
        actions['select'].assert_not_called()
        actions['addons'].assert_called_once()

    def test_game_only_skips_native_player_setup(self):
        status, actions = self.launch(False, False)
        self.assertEqual(status, 'booted')
        actions['resolve'].assert_not_called()
        actions['select'].assert_not_called()
        actions['addons'].assert_not_called()

    def test_game_only_rejects_dlc_cli(self):
        status, _ = self.launch(False, False, extra=('--dlc-pkg', 'example.pkg'))
        self.assertEqual(status, 'rejected')

    def test_dlc_only_rejects_update_cli(self):
        status, _ = self.launch(False, True, extra=('--update-pkg', 'example.pkg'))
        self.assertEqual(status, 'rejected')


if __name__ == '__main__':
    unittest.main()
