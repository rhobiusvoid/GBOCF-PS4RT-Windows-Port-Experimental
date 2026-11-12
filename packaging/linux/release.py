#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sanitized source exporter and portable Linux release packager.

Use the SAME committed-source filter for container input and public archives.
Never recursively package a development directory or original PS4 files.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
ROOT_FILES = {'README.md', 'LICENSE', 'build.sh', 'run.sh', 'port_features.json', '.gitignore', '.gitattributes'}
ROOT_DIRS = {'src', 'gpu', 'scripts', 'third_party', 'tests', 'packaging',
             'fsr4_shaders', 'patches'}
PUBLIC_DOCS = {'docs/RUNNING_LINUX.md', 'docs/DISTRIBUTING_LINUX.md',
               'docs/UPSTREAM_CREDITS.md', 'docs/LINUX_BINARY_RELEASE.md',
               'docs/THIRD_PARTY_REVIEW.md', 'docs/PLAYER_PKG_SETUP.md',
                'docs/images/sample.png'}
PRIVATE_PARTS = {'.git', '.agents', '.codex', '.ghidra', 'agents', 'references',
                 'reference', 'original', 'analysis', 'savedata', 'saves', 'user',
                 'out', 'build', 'dist', 'cache', 'caches', 'captures',
                 'screenshots', 'frame-dump', 'cmakefiles', '__pycache__',
                 'templates', 'private', 'secrets', 'credentials', 'game-dumps'}
PRIVATE_NAMES = {'agents.md', 'claude.md', 'lab_readme.md', 'automated_porting.md',
                 'migration_notes.md', 'porting_log.md'}
PRIVATE_PREFIXES = ('porting_log', 'resume', 'next_session', 'handoff',
                    'continuity', 'private_', 'secret_', 'credentials', 'agent_notes')
PRIVATE_EXTS = {'.pkg', '.prx', '.sprx', '.self', '.elf', '.sfo', '.cpk', '.usm',
                '.g1t', '.g1m', '.at9', '.pfs', '.iso', '.o', '.obj', '.a', '.so',
                '.dll', '.exe', '.log', '.trace', '.dump', '.cache', '.pem',
                '.key', '.p12', '.pfx', '.pyc', '.sqlite', '.db', '.zip', '.tar',
                '.gz', '.7z', '.mp4', '.mkv', '.mov'}
APP_LIBS = ('libavcodec.so.62', 'libavformat.so.62', 'libavutil.so.60',
            'libfmt.so.11', 'libgcc_s.so.1', 'libSDL3.so.0', 'libstdc++.so.6',
            'libswresample.so.6', 'libswscale.so.9', 'libxxhash.so.0',
            'libz.so.1', 'libZydis.so.5.0')
BINS = {'bin/ps4rt': '$ORIGIN/../lib:$ORIGIN/gpu',
        'bin/gpu/libps4rtgpu.so': '$ORIGIN/../../lib'}
PLAYER = {'run.sh', 'scripts/ps4rt_launch.py', 'scripts/prepare.py',
          'scripts/link_modules.py', 'scripts/link_libc.py',
          'scripts/trophy_catalog.py', 'scripts/trophy_crypto.py',
          'scripts/content_profile.py'}
MEDIA = {'gpu/assets/overlay.png', 'gpu/assets/sounds/navigate.wav',
         'gpu/assets/sounds/confirm.wav', 'gpu/assets/sounds/cancel.wav',
         'gpu/assets/sounds/open.wav'}
# Six metadata manifests accompanying the reviewed FSR4 model assets.
FSR4_MANIFESTS = {
    f'fsr4_shaders/fsr4_model_v07_i8_{preset}_shader_manifest.json'
    for preset in ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs')
}
REQUIRED = PLAYER | {'build.sh', 'gpu/CMakeLists.txt', 'README.md', 'LICENSE'}


def fail(message):
    raise SystemExit('PS4RT release: ' + message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_name(name):
    p = PurePosixPath(name)
    if p.is_absolute() or not p.parts or '..' in p.parts:
        return False
    if any(part.lower() in PRIVATE_PARTS for part in p.parts):
        return False
    filename = p.name.lower()
    return not (filename in PRIVATE_NAMES or
                any(filename.startswith(v) for v in PRIVATE_PREFIXES) or
                filename.startswith(('.env', 'id_rsa', 'id_ed25519')))


def source_allowed(name):
    if not safe_name(name):
        return False
    p = PurePosixPath(name)
    parts = p.parts
    # FSR4 v07 requires model weight .bin files plus auxiliary SPIR-V passes.
    # Restrict this binary exception to these exact reviewed shader filenames.
    if parts[0] == 'fsr4_shaders' and len(parts) == 2:
        return (name in FSR4_MANIFESTS or
                (p.name.startswith('fsr4_model_v07_') and
                 p.suffix.lower() in ('.spv', '.bin')) or
                p.name in ('rcas.spv', 'spd_auto_exposure.spv') or
                p.name.lower() in ('readme.md', 'license', 'license-fsr4-v07.txt'))
    # Only the exact builder recipe gets an exception to the .pkg file ban.
    # Original game/update/DLC PKGs must never enter source archives.
    if name == 'packaging/linux/Containerfile.pkg':
        return True
    if p.suffix.lower() in PRIVATE_EXTS:
        return False
    if name in ROOT_FILES or name in PUBLIC_DOCS or name.startswith('docs/licenses/'):
        return True
    if len(parts) < 2 or parts[0] not in ROOT_DIRS:
        return False
    # No Lab automation, generators, or developer GPU tooling in public source.
    if parts[0] == 'tools':
        return False
    if parts[:2] == ('gpu', 'assets'):
        return name in MEDIA or name == 'gpu/assets/README.md'
    # CMake embeds this reviewed Bitstream/DejaVu licensed font into the GPU .so.
    if name == 'gpu/third_party/fonts/DejaVuSans.ttf':
        return True
    # Proven FSR 3 Vulkan shader assets have their own restricted location.
    if name.startswith('gpu/third_party/fsr-vulkan/generated/') and p.suffix == '.spv':
        return True
    if p.suffix.lower() in ('.bin', '.png', '.jpg', '.jpeg', '.webp',
                             '.wav', '.ttf', '.ico', '.spv'):
        return False
    return True


# This title's binary release is not usable without these external assets.
# Keep the contract local to Code Fairy; generic Lab ports have their own assets.
CODE_FAIRY_MEDIA = {'gpu/assets/overlay.png',
                    'gpu/assets/sounds/navigate.wav',
                    'gpu/assets/sounds/confirm.wav',
                    'gpu/assets/sounds/cancel.wav'}
CODE_FAIRY_FSR4_FILES = 273


def validate_code_fairy_assets(files, *, player=False):
    record = files.get('port_features.json')
    if record is None:
        return
    try:
        features = json.loads(record[0].decode('utf-8'))
    except (ValueError, UnicodeError, TypeError, IndexError) as error:
        fail('invalid port feature metadata: ' + str(error))
    if not isinstance(features, dict) or features.get('title_id') != 'CUSA28763':
        return

    required = set(CODE_FAIRY_MEDIA)
    if not player:
        required.update(('packaging/fsr4-v07-assets.json',
                         'docs/PLAYER_PKG_SETUP.md', 'docs/images/sample.png'))
    missing = required - files.keys()
    if missing:
        fail('Code Fairy release assets missing: ' + ', '.join(sorted(missing)))

    shaders = [name for name in files if name.startswith('fsr4_shaders/')]
    if len(shaders) != CODE_FAIRY_FSR4_FILES:
        fail(f'Code Fairy FSR4 payload incomplete: {len(shaders)}/'
             f'{CODE_FAIRY_FSR4_FILES} files')

    if player:
        return
    try:
        manifest = json.loads(files['packaging/fsr4-v07-assets.json'][0])
        entries = manifest['files']
        if not isinstance(entries, dict) or len(entries) < 100:
            fail('Code Fairy FSR4 checksum manifest is incomplete')
        for name, entry in entries.items():
            if not name.startswith('fsr4_shaders/') or name not in files:
                fail('Code Fairy FSR4 manifest references missing file: ' + name)
            data = files[name][0]
            if len(data) != entry['size'] or digest(data) != entry['sha256']:
                fail('Code Fairy FSR4 checksum mismatch: ' + name)
    except (ValueError, TypeError, KeyError, UnicodeError) as error:
        fail('invalid Code Fairy FSR4 checksum manifest: ' + str(error))


def source_at_head(repo=ROOT):
    revision = subprocess.check_output(
        ['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    # Scope Git itself to approved roots. Do NOT stream archived references,
    # private onboarding trees or history into Python just to discard them.
    tracked = subprocess.check_output(
        ['git', '-C', str(repo), 'ls-tree', '-r', '-z', '--name-only', 'HEAD'])
    paths = {item.decode() for item in tracked.split(b'\0') if item}
    candidates = ROOT_FILES | ROOT_DIRS | PUBLIC_DOCS | {'docs/licenses'}
    selected = [p for p in sorted(candidates)
                if p in paths or any(name.startswith(p + '/') for name in paths)]
    raw = subprocess.check_output(
        ['git', '-C', str(repo), 'archive', '--format=tar', 'HEAD', *selected])
    files = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for member in archive:
            if member.isdir() or not source_allowed(member.name):
                continue
            if not member.isfile():
                fail('allowed source contains symlink/submodule: ' + member.name)
            files[member.name] = (archive.extractfile(member).read(),
                                  0o755 if member.mode & 0o111 else 0o644)
    if not REQUIRED.issubset(files):
        fail('missing committed source: ' + ', '.join(sorted(REQUIRED - files.keys())))
    validate_code_fairy_assets(files)
    if native_pkg_enabled(files):
        natives = {'scripts/build_pkg_native.sh', 'scripts/ps4rt_pkg_native.cpp',
                   'scripts/build_setup.sh', 'scripts/ps4rt_setup_native.cpp',
                   'scripts/player_setup.py', 'packaging/linux/Containerfile.pkg',
                   'third_party/ps4pkg/LICENSE',
                   'third_party/ps4pkg/core/file_format/pkg.cpp'}
        if not natives.issubset(files):
            fail('missing committed native PKG sources: ' +
                 ', '.join(sorted(natives - files.keys())))
    if 'port_features.json' in files:
        cfg = json.loads(files['port_features.json'][0].decode('utf-8'))
        if cfg.get('supports_updates') and 'scripts/patch_assembly.py' not in files:
            fail('Missing player-side in-place update source')
    return revision, files


def export_source(destination, repo=ROOT):
    if destination.is_symlink() or not destination.is_dir() or any(destination.iterdir()):
        fail('source destination must be an empty real directory')
    rev, files = source_at_head(repo)
    for name, (data, mode) in sorted(files.items()):
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
    print('Filtered build source:', len(files), 'files at', rev)


def native_pkg_enabled(source):
    """Validate committed, public per-port capability data before packaging."""
    record = source.get('port_features.json')
    if record is None:
        return False  # legacy checkout without a generated feature manifest
    try:
        features = json.loads(record[0].decode('utf-8'))
        if not isinstance(features, dict):
            raise ValueError('expected object')
    except (UnicodeError, ValueError, TypeError, IndexError) as error:
        fail(f'invalid public port_features.json: {error}')
    keys = ('supports_updates', 'supports_asset_dlc', 'needs_native_pkg')
    if any(type(features.get(key)) is not bool for key in keys):
        fail('port_features.json must declare boolean feature flags')
    required = features['supports_updates'] or features['supports_asset_dlc']
    if features['needs_native_pkg'] != required:
        fail('port_features.json native PKG capability does not match update/DLC flags')
    return required


def verify_bundle(bundle, native_pkg=False):
    if bundle.is_symlink() or not bundle.is_dir():
        fail('missing or linked build bundle')
    expected = dict(BINS)
    bin_paths = set(BINS)
    extractor = bundle / 'bin/ps4rt-pkg'
    crypto = []
    if native_pkg:
        expected['bin/ps4rt-pkg'] = '$ORIGIN/../lib'
        expected['bin/ps4rt-setup'] = '$ORIGIN/../lib'
        bin_paths.update(('bin/ps4rt-pkg', 'bin/ps4rt-setup'))
        crypto = [p.name for p in (bundle / 'lib').iterdir()
                  if re.fullmatch(r'libcrypto(?:\+\+|pp)\.so\.\d+', p.name)]
        if len(crypto) != 1:
            fail('expected exactly one bundled Crypto++ shared library')
        needed = subprocess.check_output(
            ['readelf', '-d', str(extractor)], text=True)
        if crypto[0] not in needed:
            fail('Crypto++ SONAME does not match the native PKG extractor')
        expected['lib/' + crypto[0]] = '$ORIGIN'
    elif extractor.exists() or (bundle / 'bin/ps4rt-setup').exists() or any(
            re.fullmatch(r'libcrypto(?:\+\+|pp)\.so\.\d+', p.name)
            for p in (bundle / 'lib').iterdir()):
        fail('game-only release unexpectedly contains a native PKG binary/library')
    optional = 'bin/ps4rt-gpu-capabilities'
    if (bundle / optional).is_file():
        expected[optional] = '$ORIGIN/../lib'
    expected.update({'lib/' + name: '$ORIGIN' for name in APP_LIBS})
    for name, rpath in expected.items():
        p = bundle / name
        if p.is_symlink() or not p.is_file():
            fail('missing/symlinked ELF: ' + name)
        header = p.open('rb').read(20)
        if len(header) < 20 or header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 62:
            fail('not an x86-64 ELF: ' + name)
        actual = subprocess.check_output(['patchelf', '--print-rpath', str(p)], text=True).strip()
        if actual != rpath:
            fail('unexpected RUNPATH: ' + name)
        versions = subprocess.check_output(['readelf', '-W', '--version-info', str(p)], text=True)
        requirements = [tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_(\d+(?:\.\d+)+)', versions)]
        if requirements and max(requirements) > (2, 35):
            fail('glibc requirement >2.35: ' + name)
    env = os.environ.copy()
    env.pop('LD_LIBRARY_PATH', None)
    env.pop('LD_PRELOAD', None)
    for name in bin_paths | ({optional} if optional in expected else set()):
        result = subprocess.run(['ldd', str(bundle / name)], capture_output=True, text=True, env=env)
        if result.returncode or 'not found' in result.stdout + result.stderr:
            fail('host dependency check failed: ' + name + '\n' + result.stdout + result.stderr)
        for line in result.stdout.splitlines():
            match = re.match(r'\s*(\S+)\s+=>\s+(/\S+)\s+\(', line)
            if match and match.group(1) in (*APP_LIBS, *crypto):
                if Path(match.group(2)).resolve() != (bundle / 'lib' / match.group(1)).resolve():
                    fail('dependency escaped bundle: ' + line)
    return expected


def runtime_entries(source, bundle, audited):
    result = {p: source[p] for p in PLAYER}
    if native_pkg_enabled(source):
        for name in ('scripts/player_setup.py', 'third_party/ps4pkg/LICENSE',
                     'third_party/ps4pkg/README.md'):
            if name not in source:
                fail('missing player setup dependency: ' + name)
            result[name] = source[name]
        font = 'gpu/third_party/fonts/DejaVuSans.ttf'
        if font in source:
            result[font] = source[font]
        cfg = json.loads(source['port_features.json'][0].decode('utf-8'))
        if cfg['supports_updates']:
            result['scripts/patch_assembly.py'] = source['scripts/patch_assembly.py']
    if 'port_features.json' in source:
        result['port_features.json'] = source['port_features.json']
    result.update({p: data for p, data in source.items()
                   if p in MEDIA or p.startswith('fsr4_shaders/') or
                   (p.startswith('gpu/third_party/fsr-vulkan/generated/') and p.endswith('.spv')) or
                   p in ('LICENSE', 'gpu/third_party/fonts/LICENSE-DejaVu.txt')})
    for name in audited:
        result[name] = ((bundle / name).read_bytes(),
                        0o755 if name.startswith('bin/') else 0o644)
    launcher = (b'#!/usr/bin/env bash\nset -euo pipefail\n'
                b'cd -- "$(dirname -- "$0")"\nexec bash ./run.sh "$@"\n')
    result['Play-Game.sh'] = (launcher, 0o755)
    result['PLAYER_README.txt'] = (
        b'PS4RT Linux x86-64 (glibc 2.35+)\n'
        b'First launch: bash Play-Game.sh --game "/absolute/path/to/CUSAxxxxx"\n'
        b'Later launches: bash Play-Game.sh (the game folder is remembered).\n'
        b'Use your own legally obtained extracted PS4 data.\n'
        b'For instructions, visit the project repository or release page.\n'
        b'No game dumps, user saves or private developer documents included.\n', 0o644)
    validate_code_fairy_assets(result, player=True)
    return result


def write_archive(target, stem, revision, kind, files):
    if target.exists() or target.is_symlink() or target.with_name(target.name + '.part').exists():
        fail('refusing existing archive: ' + str(target))
    payload = dict(files)
    payload['MANIFEST.json'] = ((json.dumps({
        'revision': revision, 'kind': kind,
        'files': {n: digest(data) for n, (data, _) in sorted(files.items())}
    }, indent=2, sort_keys=True) + '\n').encode(), 0o644)
    part = target.with_name(target.name + '.part')
    with tarfile.open(part, 'w:gz') as archive:
        for name, (data, mode) in sorted(payload.items()):
            if not safe_name(name):
                fail('forbidden archive entry: ' + name)
            member = tarfile.TarInfo(stem + '/' + name)
            member.uid = member.gid = 0
            member.uname = member.gname = ''
            member.mode = mode
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    part.rename(target)
    target.with_name(target.name + '.sha256').write_text(
        digest(target.read_bytes()) + '  ' + target.name + '\n')
    print('Wrote archive and SHA256:', target)


def package(output, project, bundle, repo=ROOT):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,79}', project):
        fail('invalid project name')
    if subprocess.run(['git', '-C', str(repo), 'diff', '--quiet', 'HEAD']).returncode:
        fail('commit tracked source changes before release')
    revision, source = source_at_head(repo)
    audited = verify_bundle(bundle, native_pkg=native_pkg_enabled(source))
    runtime = runtime_entries(source, bundle, audited)
    output.mkdir(parents=True, exist_ok=True)
    if output.is_symlink():
        fail('symlinked release directory')
    prefix = project + '-' + revision[:7]
    targets = [output / (prefix + '-source.tar.gz'),
               output / (prefix + '-linux-x86_64.tar.gz')]
    for path in targets:
        if (path.exists() or path.is_symlink() or
            path.with_name(path.name + '.sha256').exists() or
            path.with_name(path.name + '.part').exists()):
            fail('release asset exists; refusing overwrite: ' + str(path))
    write_archive(targets[0], prefix + '-source', revision, 'source', source)
    write_archive(targets[1], prefix + '-linux-x86_64', revision, 'linux-x86_64', runtime)
    print('Four release assets created, not published. Inspect and play-test before release.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    export = sub.add_parser('export-source')
    export.add_argument('--destination', type=Path, required=True)
    pack = sub.add_parser('package')
    pack.add_argument('--bundle-from', type=Path, required=True)
    pack.add_argument('--output', type=Path, required=True)
    pack.add_argument('--name', required=True)
    args = parser.parse_args()
    if args.action == 'export-source':
        export_source(args.destination)
    else:
        package(args.output, args.name, args.bundle_from)


if __name__ == '__main__':
    main()
