#!/usr/bin/env python3
"""Prepare and launch the Code Fairy PS4RT compatibility runtime on Linux.

Game-specific PS4 compatibility software; not a general-purpose emulator.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from prepare import load_sfo, prepare
from link_modules import link
from content_profile import prepare as prepare_content

ROOT = Path(__file__).resolve().parent.parent
WINDOWS = os.name == 'nt'
EXE_SUFFIX = '.exe' if WINDOWS else ''


def _exec(program, argv, env=None):
    """Replace this process with program (POSIX), or run it to completion and exit with its
    status (Windows has no exec: os.exec* there starts a detached child and returns at once,
    which would end the launcher window while the game is still starting)."""
    if not WINDOWS:
        if env is None:
            os.execv(program, argv)
        os.execve(program, argv, env)
    sys.stdout.flush()
    code = subprocess.call(argv, env=env)
    if code:
        print(f'ps4rt.exe exited with code {code} (0x{code & 0xffffffff:08X})', file=sys.stderr)
    sys.exit(code)


def _join_command(argv):
    return subprocess.list2cmdline(argv) if WINDOWS else shlex.join(argv)
TITLE_ID_PATTERN = re.compile(r'CUSA\d{5}\Z')



class LauncherError(ValueError):
    """A player-facing setup error with actionable first-launch instructions."""



# Internal resolutions saved by the in-game menu (gpu/shim/ps4rt_graphics.h Resolutions).
# 'native' is the game's own 1920x1080 and needs no patch. Above 1080 lines the runtime also
# makes the game's display buffer that size (high-res display, PS4RT_HIRES_DISPLAY=0 turns it off).
# '540p' renders at 1024x576: at 960x540 (height not a multiple of 8) the game leaves three
# scene targets without a format and every particle effect (explosions, dust, sparks) vanishes.
RENDER_RESOLUTIONS = {
    '720p': '1280x720', '540p': '1024x576', '1440p': '2560x1440', '2160p': '3840x2160',
    '2560x1080': '2560x1080', '3440x1440': '3440x1440', '3840x1440': '3840x1440',
    '3840x1080': '3840x1080', '5120x1440': '5120x1440',
}

# Graphics quality presets saved by the in-game menu (gpu/shim/ps4rt_graphics.h Quality): the
# sun shadow map size the runtime patches in (src/runtime_quality.c). 'max' is the game's own 4096.
SHADOW_MAP_SIZES = {'medium': '2048', 'low': '1024'}

def _achievements(title_id: str, game: Path, user_dir: Path, env: dict) -> None:
    from trophy_catalog import prepare_catalog, trophy_language
    env.pop('PS4RT_TROPHY_CATALOG', None)
    env['PS4RT_ACHIEVEMENTS_DIR'] = str(user_dir / 'achievements')
    try:
        language = int(env.get('PS4RT_LANGUAGE', '1'))
        language = trophy_language(title_id, user_dir, language)
        env['PS4RT_TROPHY_LANGUAGE'] = str(language)
        catalog = prepare_catalog(game, user_dir, language)
        if catalog is not None:
            env['PS4RT_TROPHY_CATALOG'] = str(catalog)
    except (OSError, ValueError, KeyError, UnicodeError) as error:
        # A missing or damaged trophy archive must not prevent gameplay.
        print(f'Achievements unavailable: {error}', flush=True)


def _port_title_id() -> str | None:
    """Find the per-port identity without scanning other players' save directories.

    The public manifest is generated during onboarding. Older source checkouts
    may not have it yet and can still boot with an explicit --game argument.
    """
    path = ROOT / 'port_features.json'
    if not path.is_file():
        return None
    try:
        meta = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, ValueError) as error:
        raise LauncherError(f'Invalid port_features.json: {error}') from error
    if not isinstance(meta, dict):
        raise LauncherError('port_features.json must contain an object')
    title_id = meta.get('title_id')
    if not isinstance(title_id, str) or not TITLE_ID_PATTERN.fullmatch(title_id):
        raise LauncherError('Invalid title_id in port_features.json')
    return title_id


def _capabilities() -> dict:
    path = ROOT / 'port_features.json'
    if not path.is_file():
        return {'supports_updates': False, 'supports_asset_dlc': False,
                'needs_native_pkg': False, 'target_version': None}
    metadata = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(metadata, dict):
        raise LauncherError('Invalid port feature manifest')
    for flag in ('supports_updates', 'supports_asset_dlc', 'needs_native_pkg'):
        if type(metadata.get(flag)) is not bool:
            raise LauncherError('Invalid port feature flag: ' + flag)
    if metadata['needs_native_pkg'] != (
            metadata['supports_updates'] or metadata['supports_asset_dlc']):
        raise LauncherError('Inconsistent native PKG capabilities')
    return metadata


def _restart_after_setup(args, game: Path, *, forward_dlc: bool = False) -> None:
    """Re-execute the launcher after changing installed files, before game startup."""
    argv = [sys.executable, str(Path(__file__).resolve()), '--game', str(game),
            '--no-dialogs']
    if args.dlc_dir is not None:
        argv += ['--dlc-dir', str(args.dlc_dir)]
    if forward_dlc:
        for pkg in args.dlc_pkg:
            argv += ['--dlc-pkg', str(pkg)]
    if args.pkg_tool:
        argv += ['--pkg-tool', args.pkg_tool]
    if args.upscaler is not None:
        argv += ['--upscaler', args.upscaler]
    if args.output_resolution is not None:
        argv += ['--output-resolution', args.output_resolution]
    print('PS4RT: installation complete; restarting clean player launcher.', flush=True)
    _exec(sys.executable, argv)


def _first_launch_hint(title_id: str | None) -> str:
    label = title_id if title_id else 'CUSAxxxxx'
    launcher = ('Play-Game.bat --game "C:\\path\\to\\' + label + '"' if WINDOWS
                else f'bash Play-Game.sh --game "/absolute/path/to/{label}"')
    return ('First launch:\n'
            f'  {launcher}\n\n'
            'For setup instructions, see PLAYER_README.txt.')


def _select_game_folder(explicit: Path | None, expected_title: str | None) -> Path:
    """Use an explicit path or the validated location saved by this one port."""
    hint = _first_launch_hint(expected_title)
    if explicit is not None:
        selected = explicit
    else:
        if expected_title is None:
            raise LauncherError('Game folder not configured.\n\n' + hint)
        remembered = ROOT / 'out' / expected_title / 'game-folder.txt'
        try:
            saved = remembered.read_text(encoding='utf-8').strip()
        except (OSError, UnicodeError):
            saved = ''
        if not saved:
            raise LauncherError('Game folder not configured.\n\n' + hint)
        selected = Path(saved)
    try:
        game = selected.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise LauncherError(f'Game folder does not exist: {selected}.\n\n{hint}') from error
    if not game.is_dir():
        raise LauncherError(f'Expected an extracted PS4 game directory: {game}.\n\n{hint}')
    return game


def _remember_game_folder(destination: Path, game: Path) -> None:
    """Atomically persist only after all requested preparation steps pass."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                     prefix='.game-folder-', dir=destination.parent,
                                     delete=False) as stream:
        tmp = Path(stream.name)
        stream.write(str(game) + '\n')
    try:
        tmp.replace(destination)
    finally:
        tmp.unlink(missing_ok=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path,
                        help='Your extracted plaintext CUSAxxxxx directory (remembered after setup)')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--update-pkg', type=Path,
                        help='Player-owned update PKG, for ports requiring an update')
    parser.add_argument('--update-extracted', type=Path,
                        help='Already extracted matching game update')
    parser.add_argument('--dlc-dir', type=Path, help='Optional extracted asset-DLC folder')
    parser.add_argument('--dlc-pkg', type=Path, action='append', default=[],
                        help='Player-owned asset DLC PKG (repeat for multiple)')
    parser.add_argument('--install-dlc', action='store_true',
                        help='Open DLC installer from the in-game overlay')
    parser.add_argument('--no-dialogs', action='store_true',
                        help='Avoid interactive setup dialogs (scripting/headless)')
    parser.add_argument('--pkg-tool', help='Developer-only PKG extractor override')
    parser.add_argument('--upscaler', choices=('off', 'fsr3', 'fsr4'),
                        help='Override the saved graphics setting for this launch')
    parser.add_argument('--output-resolution', choices=('1080', '1440', '4k'))
    args = parser.parse_args(argv)
    # Validate the port identity before feature flags. A malformed title ID
    # must fail closed with the specific error that names the unsafe field.
    expected_title = _port_title_id()
    flags = _capabilities()
    updates_enabled = flags['supports_updates']
    dlc_enabled = flags['supports_asset_dlc']
    if args.update_pkg and args.update_extracted:
        parser.error('Choose an update PKG or extracted update, not both')
    if (args.update_pkg or args.update_extracted) and not updates_enabled:
        parser.error('This port has no update installer')
    if (args.dlc_dir or args.dlc_pkg or args.install_dlc) and not dlc_enabled:
        parser.error('This port has no asset DLC installer')
    game = _select_game_folder(args.game, expected_title)
    title_id = str(load_sfo(game / 'sce_sys/param.sfo').get('TITLE_ID', ''))
    if not TITLE_ID_PATTERN.fullmatch(title_id):
        parser.error('Expected a PS4 game with CUSAxxxxx TITLE_ID in param.sfo')
    if expected_title is not None and title_id != expected_title:
        parser.error(f'This port requires {expected_title} game data, not {title_id}')
    # The generated port uses Sakura's player installer only for declared features.
    addons = {}
    if flags['needs_native_pkg']:
        from player_setup import (resolve_game, installed_addons, install_dlc,
                                  add_dlc_dialog, choose_setup, DLC_INSTALLED_EXIT)
        if args.install_dlc:
            if args.dlc_pkg:
                added = install_dlc(ROOT, args.dlc_pkg, pkg_tool=args.pkg_tool,
                                    show_window=not args.no_dialogs)
                return DLC_INSTALLED_EXIT if added else 0
            return add_dlc_dialog(ROOT, pkg_tool=args.pkg_tool)
    # Portable archives use bin/ps4rt; developer builds retain out/ps4rt.
    portable = ROOT / ('bin/ps4rt' + EXE_SUFFIX)
    runtime = portable if portable.is_file() else ROOT / ('out/ps4rt' + EXE_SUFFIX)
    if not args.prepare_only and not runtime.is_file():
        parser.error('Runtime missing: build with bash build.sh or use a portable release')
    if updates_enabled:
        from patch_assembly import incomplete_marker
        details = load_sfo(game / 'sce_sys/param.sfo')
        current = str(details.get('APP_VER', '')).strip()
        was_base = current == flags.get('base_version')
        marker = incomplete_marker(game)
        needs_update = (current != flags.get('target_version')
                        or marker.exists() or marker.is_symlink())
        game = resolve_game(ROOT, game, update_pkg=args.update_pkg,
                            update_extracted=args.update_extracted,
                            pkg_tool=args.pkg_tool,
                            dialogs=not args.no_dialogs and not args.prepare_only)
        if needs_update and not args.prepare_only:
            # Optional DLC is offered only when BOTH features were enabled.
            if dlc_enabled and was_base and not args.no_dialogs and not args.dlc_pkg:
                selected = choose_setup(ROOT, 'dlc')
                if selected:
                    install_dlc(ROOT, selected, pkg_tool=args.pkg_tool)
            _restart_after_setup(args, game, forward_dlc=True)
            raise RuntimeError('Restart unexpectedly returned')
    if dlc_enabled:
        if args.dlc_pkg:
            added = install_dlc(ROOT, args.dlc_pkg, pkg_tool=args.pkg_tool,
                                show_window=not args.no_dialogs and not args.prepare_only)
            if added and not args.prepare_only:
                _restart_after_setup(args, game)
                raise RuntimeError('Restart unexpectedly returned after DLC')
        addons = installed_addons(ROOT, args.dlc_dir)
    out = ROOT / 'out' / title_id
    prepare(game, out)
    link(game, out)
    if dlc_enabled:
        prepare_content(game, out, 'full', addons)
    else:
        prepare_content(game, out, 'full')
    # Do not overwrite a working saved path when extraction or preparation fails.
    _remember_game_folder(out / 'game-folder.txt', game)
    if args.prepare_only:
        return 0
    user_dir = out / 'user'
    gpu_user = user_dir / 'gpu'
    gpu_user.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # Do not leak compatibility overrides from the caller's shell into a new title.
    for key in ('PS4RT_TRANSITION_SNAPSHOT', 'PS4RT_BC6_FLOAT',
                'PS4RT_WQM_HELPERS', 'PS4RT_ASYNC_SHADERS',
                'PS4RT_OBJECT_MOTION', 'PS4RT_FSR1', 'PS4RT_PARTIAL_HEIGHT_VIEWS',
                'PS4RT_UPSCALER', 'PS4RT_OUTPUT_RES', 'PS4RT_RENDER_RES',
                'PS4RT_UPSCALE_PRESET', 'PS4RT_UPSCALE_BEFORE_CS',
                'PS4RT_UI_TRIGGER_VS', 'PS4RT_PRESET_FILE', 'PS4RT_RCAS',
                'PS4RT_FSR_SHARPNESS', 'PS4RT_JITTER'):
        env.pop(key, None)
    # Start with the known working runtime behavior; future ports adapt it in-source.
    # Do not force ASYNC_SHADERS on every launch; the menu saves this option.
    env.update(PS4RT_BC6_FLOAT='unsigned', PS4RT_TRANSITION_SNAPSHOT='1',
               PS4RT_OBJECT_MOTION='0',
               PS4RT_FSR1='0', PS4RT_PARTIAL_HEIGHT_VIEWS='0')
    env['PS4RT_GPU_USER_DIR'] = str(gpu_user)
    env['PS4RT_CONFIG'] = str(out / 'ps4rt.ini')
    _achievements(title_id, game, user_dir, env)
    # Internal resolution saved by the in-game menu (applied on restart); an explicit
    # PS4RT_CF_RESOLUTION from the caller wins. A value this launcher took from the ini on
    # an earlier launch (inherited through "Apply and restart") is not the caller's: drop it
    # so the newly saved choice applies.
    if env.pop('PS4RT_CF_RESOLUTION_FROM_INI', None):
        env.pop('PS4RT_CF_RESOLUTION', None)
    if 'PS4RT_CF_RESOLUTION' not in env:
        try:
            for line in (out / 'ps4rt.ini').read_text().splitlines():
                key, _, value = line.partition('=')
                if key.strip() == 'render_resolution':
                    size = RENDER_RESOLUTIONS.get(value.strip().lower())
                    if size:
                        env['PS4RT_CF_RESOLUTION'] = size
                        env['PS4RT_CF_RESOLUTION_FROM_INI'] = '1'
        except OSError:
            pass
    # Graphics quality preset (shadow map size), same rules as the internal resolution.
    if env.pop('PS4RT_CF_SHADOW_MAP_FROM_INI', None):
        env.pop('PS4RT_CF_SHADOW_MAP', None)
    if 'PS4RT_CF_SHADOW_MAP' not in env:
        try:
            for line in (out / 'ps4rt.ini').read_text().splitlines():
                key, _, value = line.partition('=')
                if key.strip() == 'graphics_quality':
                    size = SHADOW_MAP_SIZES.get(value.strip().lower())
                    if size:
                        env['PS4RT_CF_SHADOW_MAP'] = size
                        env['PS4RT_CF_SHADOW_MAP_FROM_INI'] = '1'
        except OSError:
            pass
    if dlc_enabled:
        env['PS4RT_DLC_INSTALL_SCRIPT'] = str(Path(__file__).resolve())
        env['PS4RT_ORIGINAL_GAME_DIR'] = str(game)
    # Restart through the player launcher, preserving the selected game and options.
    # The raw runtime cannot reconstruct --game once Python execs it.
    restart = [sys.executable, str(Path(__file__).resolve()), '--game', str(game)]
    if flags['needs_native_pkg']:
        restart.append('--no-dialogs')
    if args.dlc_dir is not None:
        restart += ['--dlc-dir', str(args.dlc_dir)]
    if args.upscaler is not None:
        restart += ['--upscaler', args.upscaler]
    if args.output_resolution is not None:
        restart += ['--output-resolution', args.output_resolution]
    env['PS4RT_RESTART_COMMAND'] = _join_command(restart)
    # Stable asset defaults: game-specific PNG/WAV replacements need no C++ edits.
    if not env.get('PS4RT_OVERLAY_BACKGROUND'):
        art = ROOT / 'gpu/assets/overlay.png'
        if art.is_file():
            env['PS4RT_OVERLAY_BACKGROUND'] = str(art)
    if not env.get('PS4RT_MENU_SOUND_DIR'):
        sounds = ROOT / 'gpu/assets/sounds'
        if sounds.is_dir():
            env['PS4RT_MENU_SOUND_DIR'] = str(sounds)
    if args.upscaler is not None:
        env['PS4RT_UPSCALER'] = args.upscaler
    if args.output_resolution is not None:
        env['PS4RT_OUTPUT_RES'] = {
            '1080': '1920x1080', '1440': '2560x1440', '4k': '3840x2160'
        }[args.output_resolution]
    print(f'PS4RT Code Fairy: {title_id}', flush=True)
    command = [str(runtime), str(out / 'boot-linked.bin'), '--app0', str(game),
               '--user', str(user_dir), '--content-profile', str(out / 'content.bin'),
               '--timeout', '0']
    if dlc_enabled:
        for label, directory in sorted(addons.items()):
            command += ['--addcont', f'{label}={directory}']
    _exec(str(runtime), command, env)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        sys.exit(f'PS4RT: {error}')
