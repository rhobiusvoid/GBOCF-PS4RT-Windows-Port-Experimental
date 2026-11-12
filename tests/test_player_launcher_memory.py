"""Private-game-free tests for PS4RT-Lab's reusable remembered-path player launcher."""
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import ps4rt_launch as launcher


class ProcessReplaced(Exception):
    pass


class RememberedGameTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.title_id = 'CUSA12345'
        self.game = self.root / 'game' / self.title_id
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(b'fixture')
        (self.game / 'eboot.bin').write_bytes(b'fixture')
        (self.root / 'bin').mkdir()
        (self.root / 'bin/ps4rt').write_bytes(b'fixture')
        self.saved = self.root / 'out' / self.title_id / 'game-folder.txt'
        (self.root / 'port_features.json').write_text(json.dumps({
            'schema_version': 1, 'title_id': self.title_id, 'display_name': 'Fixture Game',
            'supports_updates': False, 'supports_asset_dlc': False,
            'needs_native_pkg': False, 'base_version': '01.00',
            'target_version': None
        }))

    def launch(self, *args, prepare_fails=False, content_fails=False, title_id=None):
        with ExitStack() as stack:
            stack.enter_context(patch.object(launcher, 'ROOT', self.root))
            stack.enter_context(patch.object(launcher, 'load_sfo',
                                            return_value={'TITLE_ID': title_id or self.title_id}))
            if prepare_fails:
                stack.enter_context(patch.object(launcher, 'prepare',
                                                 side_effect=ValueError('prepare failed')))
            else:
                stack.enter_context(patch.object(launcher, 'prepare'))
            stack.enter_context(patch.object(launcher, 'link'))
            if content_fails:
                stack.enter_context(patch.object(launcher, 'prepare_content',
                                                 side_effect=RuntimeError('content failed')))
            else:
                stack.enter_context(patch.object(launcher, 'prepare_content'))
            stack.enter_context(patch.object(launcher.os, 'execve',
                                            side_effect=ProcessReplaced))
            return launcher.main(list(args))

    def test_no_initial_game_has_actionable_hint(self):
        with self.assertRaises(launcher.LauncherError) as raised:
            self.launch()
        self.assertIn('Play-Game.sh --game', str(raised.exception))
        self.assertIn('CUSA12345', str(raised.exception))
        self.assertIn('PLAYER_README.txt', str(raised.exception))
        self.assertFalse(self.saved.exists())

    def test_first_success_and_future_without_game(self):
        with self.assertRaises(ProcessReplaced):
            self.launch('--game', str(self.game))
        self.assertEqual(self.saved.read_text().strip(), str(self.game.resolve()))
        with self.assertRaises(ProcessReplaced):
            self.launch()

    def test_prepare_only_remembers_path(self):
        self.assertEqual(self.launch('--game', str(self.game), '--prepare-only'), 0)
        self.assertEqual(self.saved.read_text().strip(), str(self.game.resolve()))

    def test_missing_explicit_path_preserves_previous_location(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text(str(self.game) + '\n')
        with self.assertRaisesRegex(launcher.LauncherError, 'does not exist'):
            self.launch('--game', str(self.root / 'deleted'))
        self.assertEqual(self.saved.read_text(), str(self.game) + '\n')

    def test_moved_remembered_path_has_first_launch_hint(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text('/old/location/CUSA12345\n')
        with self.assertRaisesRegex(launcher.LauncherError, 'Play-Game.sh --game'):
            self.launch()

    def test_failed_prepare_or_content_preserves_previous_location(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text('/known-good/CUSA12345\n')
        for failure in ('prepare', 'content'):
            with self.subTest(failure=failure):
                with self.assertRaises((ValueError, RuntimeError)):
                    self.launch('--game', str(self.game),
                                prepare_fails=failure == 'prepare',
                                content_fails=failure == 'content')
                self.assertEqual(self.saved.read_text(), '/known-good/CUSA12345\n')

    def test_wrong_cusa_fails_without_changing_saved_path(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text('/known-good/CUSA12345\n')
        with self.assertRaises(SystemExit):
            self.launch('--game', str(self.game), title_id='CUSA99999')
        self.assertEqual(self.saved.read_text(), '/known-good/CUSA12345\n')

    def test_explicit_new_location_wins_and_updates_on_success(self):
        other = self.root / 'other' / 'CUSA12345'
        (other / 'sce_sys').mkdir(parents=True)
        (other / 'sce_sys/param.sfo').write_bytes(b'fixture')
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text(str(self.game) + '\n')
        with self.assertRaises(ProcessReplaced):
            self.launch('--game', str(other))
        self.assertEqual(self.saved.read_text(), str(other.resolve()) + '\n')

    def test_malformed_manifest_fails_closed(self):
        (self.root / 'port_features.json').write_text('{"title_id": "../other"}')
        with self.assertRaisesRegex(launcher.LauncherError, 'Invalid title_id'):
            self.launch('--game', str(self.game))
        self.assertFalse(self.saved.exists())

    def test_valid_title_with_invalid_capability_flag_fails_closed(self):
        (self.root / 'port_features.json').write_text(json.dumps({
            'title_id': self.title_id,
            'supports_updates': 'false',
            'supports_asset_dlc': False,
            'needs_native_pkg': False,
        }))
        with self.assertRaisesRegex(
                launcher.LauncherError, 'Invalid port feature flag: supports_updates'):
            self.launch('--game', str(self.game))
        self.assertFalse(self.saved.exists())

    def test_legacy_checkout_explicit_path_still_works(self):
        (self.root / 'port_features.json').unlink()
        with self.assertRaises(ProcessReplaced):
            self.launch('--game', str(self.game))
        self.assertEqual(self.saved.read_text(), str(self.game.resolve()) + '\n')


if __name__ == '__main__':
    unittest.main()
