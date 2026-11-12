"""Security regression tests for sanitized builder input and release archives."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'packaging/linux/release.py'
spec = importlib.util.spec_from_file_location('ps4rt_release', SCRIPT)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseFilterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ps4rt-release-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / 'game'
        self.repo.mkdir()
        subprocess.run(['git', 'init', '-q', '-b', 'main', str(self.repo)], check=True)
        safe = set(release.REQUIRED) | set(release.PUBLIC_DOCS) | {'port_features.json', 'src/example.c', 'gpu/assets/overlay.png',
                                       'gpu/assets/sounds/navigate.wav',
                                       'gpu/third_party/fsr-vulkan/generated/ffx-test/vk/shader.spv',
                                       'gpu/third_party/fonts/DejaVuSans.ttf',
                                       'fsr4_shaders/fsr4_model_v07_i8_quality_shader_manifest.json'}
        private = {'AGENTS.md', 'CLAUDE.md', 'LAB_README.md',
                   'docs/PORTING_LOG.md', 'docs/CUSA00001_RESUME.md',
                   'docs/AUTOMATED_PORTING.md', 'references/private_notes.txt',
                   'analysis/decompiled.txt', 'original/eboot.bin',
                   'tools/lab/start-port.py', 'scripts/secret_keys.pem',
                   'gpu/assets/private_art.png', 'out/build.log',
                   'tests/savedata/save.bin'}
        for name in safe | private:
            p = self.repo / name
            p.parent.mkdir(parents=True, exist_ok=True)
            if name == 'port_features.json':
                p.write_text(json.dumps({
                    'schema_version': 1, 'title_id': 'CUSA00001',
                    'display_name': 'Test', 'base_version': '01.00',
                    'target_version': None, 'supports_updates': False,
                    'supports_asset_dlc': False, 'needs_native_pkg': False
                }))
            else:
                p.write_bytes(('test-' + name).encode())
        subprocess.run(['git', '-C', str(self.repo), 'add', '-A'], check=True)
        subprocess.run(['git', '-C', str(self.repo), '-c', 'user.name=Test',
                        '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'fixture'], check=True)

    def test_tainted_git_commit_never_enters_builder_source(self):
        stage = self.root / 'stage'
        stage.mkdir()
        release.export_source(stage, self.repo)
        names = {str(p.relative_to(stage)) for p in stage.rglob('*') if p.is_file()}
        self.assertIn('port_features.json', names)
        self.assertIn('src/example.c', names)
        self.assertIn('gpu/assets/overlay.png', names)
        self.assertIn('gpu/assets/sounds/navigate.wav', names)
        self.assertIn('gpu/third_party/fsr-vulkan/generated/ffx-test/vk/shader.spv', names)
        self.assertIn('gpu/third_party/fonts/DejaVuSans.ttf', names)
        self.assertTrue(release.source_allowed('fsr4_shaders/LICENSE-FSR4-v07.txt'))
        self.assertIn('fsr4_shaders/fsr4_model_v07_i8_quality_shader_manifest.json', names)
        for path in names:
            self.assertTrue(release.source_allowed(path), path)
        self.assertNotIn('AGENTS.md', names)
        self.assertNotIn('docs/PORTING_LOG.md', names)
        self.assertNotIn('references/private_notes.txt', names)
        self.assertNotIn('tools/lab/start-port.py', names)
        self.assertFalse(release.source_allowed('tools/generate-overlay-assets.py'))
        self.assertFalse(release.source_allowed('tools/diagnostics/native/vk_gpu_profile.cpp'))

    def test_public_source_tar_uses_same_allowlist(self):
        rev, files = release.source_at_head(self.repo)
        target = self.root / 'source.tar.gz'
        release.write_archive(target, 'test-source', rev, 'source', files)
        with tarfile.open(target) as archive:
            names = [p.name.partition('/')[2] for p in archive.getmembers()]
        self.assertIn('port_features.json', names)
        self.assertIn('src/example.c', names)
        self.assertIn('docs/RUNNING_LINUX.md', names)
        self.assertIn('MANIFEST.json', names)
        self.assertNotIn('AGENTS.md', names)
        self.assertNotIn('docs/PORTING_LOG.md', names)
        self.assertNotIn('references/private_notes.txt', names)
        self.assertTrue(target.with_name(target.name + '.sha256').is_file())

    def test_runtime_is_explicit_subset_not_directory_copy(self):
        rev, files = release.source_at_head(self.repo)
        bundle = self.root / 'bundle'
        (bundle / 'bin').mkdir(parents=True)
        (bundle / 'bin/ps4rt').write_bytes(b'fake-fixture')
        items = release.runtime_entries(files, bundle, {'bin/ps4rt': 'RPATH'})
        self.assertIn('Play-Game.sh', items)
        self.assertIn('port_features.json', items)
        self.assertIn('scripts/ps4rt_launch.py', items)
        self.assertIn('gpu/assets/overlay.png', items)
        self.assertIn('fsr4_shaders/fsr4_model_v07_i8_quality_shader_manifest.json', items)
        self.assertNotIn('src/example.c', items)
        self.assertNotIn('AGENTS.md', items)
        self.assertNotIn('docs/PORTING_LOG.md', items)
        self.assertNotIn('docs/RUNNING_LINUX.md', items)
        self.assertTrue(all(not path.startswith(('docs/', 'source/')) for path in items))

    def test_code_fairy_assets_fail_closed(self):
        self.assertTrue(release.source_allowed('docs/PLAYER_PKG_SETUP.md'))
        self.assertTrue(release.source_allowed('docs/images/sample.png'))
        fake = {'port_features.json': (
            json.dumps({'title_id': 'CUSA28763'}).encode(), 0o644)}
        with self.assertRaisesRegex(SystemExit, 'release assets missing'):
            release.validate_code_fairy_assets(fake)

    def test_fresh_git_init_preserves_reviewed_binary_assets(self):
        """Mirror the one-commit release setup: git archive, init, add -A."""
        clean = self.root / 'clean-release'
        clean.mkdir()
        gitignore = SCRIPT.parents[2] / '.gitignore'
        (clean / '.gitignore').write_bytes(gitignore.read_bytes())

        wanted = {'gpu/assets/overlay.png', 'docs/images/sample.png'}
        presets = ('native', 'quality', 'balanced', 'performance',
                   'ultraperf', 'drs')
        stages = tuple(f'pass{i}' for i in range(1, 13)) + ('pre', 'post')
        for preset in presets:
            for stage in stages:
                wanted.add(
                    f'fsr4_shaders/fsr4_model_v07_i8_{preset}_4320_{stage}.spv')
        unwanted = {'gpu/assets/unreviewed.png',
                    'fsr4_shaders/unreviewed_shader.spv',
                    'game-dumps/private.pkg'}
        for name in wanted | unwanted:
            path = clean / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'test')
        subprocess.run(['git', 'init', '-q', '-b', 'release', str(clean)],
                       check=True)
        subprocess.run(['git', '-C', str(clean), 'add', '-A'], check=True)
        staged = {
            path.decode() for path in subprocess.check_output(
                ['git', '-C', str(clean), 'ls-files', '-z']
            ).split(bytes([0])) if path
        }
        self.assertTrue(wanted <= staged, sorted(wanted - staged))
        self.assertTrue(unwanted.isdisjoint(staged), sorted(unwanted & staged))
        self.assertEqual(len([p for p in staged if '_4320_' in p]), 84)

    def test_no_overwrite_and_no_unfiltered_destination(self):
        stage = self.root / 'stage'
        stage.mkdir()
        (stage / 'existing.txt').write_text('do not touch')
        with self.assertRaises(SystemExit):
            release.export_source(stage, self.repo)
        self.assertEqual((stage / 'existing.txt').read_text(), 'do not touch')

    def test_fs4_model_weights_and_auxiliary_shaders_survive(self):
        required = (
            'fsr4_shaders/fsr4_model_v07_i8_quality_pre_weights.bin',
            'fsr4_shaders/fsr4_model_v07_i8_quality_1080_pass1.spv',
            'fsr4_shaders/rcas.spv',
            'fsr4_shaders/spd_auto_exposure.spv',
            'fsr4_shaders/LICENSE-FSR4-v07.txt',
            'fsr4_shaders/fsr4_model_v07_i8_quality_shader_manifest.json',
        )
        for path in required:
            self.assertTrue(release.source_allowed(path), path)
        for path in ('fsr4_shaders/mystery.bin',
                     'fsr4_shaders/private_shader_manifest.json', 'src/secret.bin'):
            self.assertFalse(release.source_allowed(path), path)

    def test_feature_manifest_controls_native_bundle(self):
        base = {
            'schema_version': 1, 'title_id': 'CUSA00001',
            'display_name': 'Test', 'base_version': '01.00',
            'target_version': None, 'supports_updates': False,
            'supports_asset_dlc': False, 'needs_native_pkg': False
        }
        def source(**overrides):
            return {'port_features.json': ((json.dumps({**base, **overrides})).encode(), 0o644)}
        self.assertFalse(release.native_pkg_enabled(source()))
        self.assertTrue(release.native_pkg_enabled(source(
            supports_asset_dlc=True, needs_native_pkg=True)))
        self.assertTrue(release.native_pkg_enabled(source(
            supports_updates=True, needs_native_pkg=True, target_version='01.01')))
        for wrong in (source(needs_native_pkg=True),
                      source(supports_asset_dlc=True),
                      {'port_features.json': (b'bad-json', 0o644)}):
            with self.assertRaises(SystemExit):
                release.native_pkg_enabled(wrong)

    def test_native_pkg_source_filter_never_allows_player_pkgs(self):
        for name in ('scripts/ps4rt_pkg_native.cpp',
                     'scripts/build_pkg_native.sh',
                     'packaging/linux/Containerfile.pkg',
                     'third_party/ps4pkg/LICENSE',
                     'third_party/ps4pkg/core/file_format/pkg.cpp'):
            self.assertTrue(release.source_allowed(name), name)
        for name in ('original/update.pkg', 'scripts/private.pkg',
                     'packaging/linux/other.pkg',
                     'third_party/ps4pkg/private.pkg'):
            self.assertFalse(release.source_allowed(name), name)

    def test_private_names_are_blocked_independent_of_directory(self):
        for name in ('gpu/AGENTS.md', 'src/RESUME.md', 'scripts/credentials.txt',
                     'tools/lab/agent.py', 'docs/PORTING_LOG.md',
                     'gpu/assets/sounds/private.wav', 'fsr4_shaders/unreviewed.bin',
                     'gpu/third_party/unreviewed.spv'):
            self.assertFalse(release.source_allowed(name), name)


if __name__ == '__main__':
    unittest.main()
