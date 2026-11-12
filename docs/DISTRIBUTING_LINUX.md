# Code Fairy PS4RT: Linux source and binary distribution

Maintainer-facing release checklist for **CUSA28763 v01.03**. This is a game-specific runtime with GPL-derived upstream code and a shadPS4-derived Vulkan renderer. **No player release is published automatically by the build scripts.**

## 1. Release boundaries

- Publish **source code** and a **portable Linux x86-64 player runtime**, each with its matching SHA256 checksum.
- Never include original or prepared PS4 game code, game/update/**volume unlock-key** PKGs, `out/`, saves, cache directories, private research, internal agent instructions, credentials, logs or crash dumps.
- Retain `LICENSE`, source copyright/SPDX tags, `docs/UPSTREAM_CREDITS.md`, third-party notices, FSR 4 license/resources and the native PKG extractor's GPL source/credits.
- Review **`gpu/assets/overlay.png`**, **`gpu/assets/sounds/*.wav`** and README screenshots for redistribution permission. The assets have been customized for Code Fairy; they should not be described as generic generated placeholders without review.
- Verify no personal email addresses are exposed in the public repo's **Git commit author/committer metadata**. Sanitizing the working tree alone does not clean earlier commits.

For public publication from this private cleanup workspace, prefer an **independent, freshly reviewed Git history** based on an audited final tree rather than making the full development/cleanup history public.

## 2. Build/test from source

The developer checkout uses C11/C++23-capable tools, CMake/Ninja, Python 3, pkg-config, Vulkan, SDL3, FFmpeg, glslang, fmt, Boost, xxHash, robin-map, Zydis, X11, **Crypto++ and zlib** development dependencies.

~~~bash
PS4RT_LTO=OFF bash build.sh --test
bash run.sh --game "/absolute/path/to/CUSA28763"
~~~

Code Fairy requires its own extracted base game, **v01.03** (a matching update PKG if starting from v01.00) and all **three volume unlock-key PKGs** to test the full episodic campaign. If testing update setup against v01.00, use a disposable copy: the installer **updates that folder in place**. Development-generated artifacts under `out/` remain private.

## 3. Build the portable release

The checked toolchain uses a controlled **Ubuntu 22.04 / GCC 14 / glibc 2.35** environment with Podman, separate Crypto++ build support for native PKG handling, and a git-archive-based source filter.

Build prerequisite images as needed:

~~~bash
bash packaging/linux/build-images.sh
bash packaging/linux/build-pkg-image.sh
~~~

From a **reviewed, clean and committed** Git HEAD, run:

~~~bash
bash packaging/linux/build-release.sh
~~~

The builder copies only an allowlisted committed source snapshot, compiles and audits ELF dependencies and RUNPATHs, and writes the following **four local assets** to the release folder:

1. Linux player `.tar.gz`
2. Player archive `.sha256`
3. Source `.tar.gz`
4. Source archive `.sha256`

By default, build staging is in `~/Documents/PS4RT-Builds/<repository-name>/` and finished archives in `~/Documents/PS4RT-Releases/<repository-name>/`. The script refuses to overwrite existing assets and **does not push or publish** a GitHub Release. See [packaging/linux/README.md](../packaging/linux/README.md) for exact tooling and overrides.

## 4. Release verification

Before publishing, complete the following checks:

- **Source & history:** Scan the full public commit history and source archive for personal emails, secrets, private instructions, temp probes, game data, invalid attribution and accidental binary assets.
- **Build:** Verify the archive manifests, checksums, source revision, ELF architecture, bundled shared libraries, `$ORIGIN` RUNPATHs, and the maximum glibc version requirement.
- **First-launch:** Extract the completed player archive to a new directory; supply extracted **v01.00** CUSA28763 plus an owned matching **v01.03 update PKG**; confirm the in-place update, setup prompts and clean restart. Separately test already-v01.03 input. The update step can be skipped only when v01.03 is already applied.
- **Volume keys:** Test installation of **all three** PKGs through the existing **Install DLC...** flow, package identity checks, duplicates, restart and story volume unlocking. Skipping key installation must **not** bypass the game's episode locks. These are episodic license keys, **not optional add-on DLC**; `out/CUSA28763/player-content/dlc/` is just the generic installer storage path and must never be included in release archives.
- **Gameplay:** Smoke-test story boot, combat, movie playback, controller input, native language selection, game saves/load, local achievement unlocks, the overlay, 16:9 graphics, FSR Off/3.1/4 as supported, optional FSR 1, 30/60 FPS behavior and the tested ultrawide modes.
- **QA coverage:** Clearly state that developer gameplay testing reached **Chapter 9 of Volume 2** and that Volume 3 has not received a complete playthrough. Label **ultrawide** as experimental; do not present unrelated validated features as experimental.
- **Media/credits:** Audit game-themed overlay art, audio effects, screenshots, FSR4 shader/model assets, fonts and all copied third-party components for their respective redistribution requirements. Confirm FFmpeg's precise configured build/license terms.

Only publish the player and source assets after the exact extracted player passes checks. Do not rely solely on tests run in the original development worktree.

## 5. Platform and graphics caveats

The portable is for **native x86-64 Linux**, glibc 2.35+ and a working Vulkan loader/driver; it does not claim ARM64/FEX/Box64 compatibility. Fedora/RADV development testing does not certify SteamOS, NVIDIA, or other GPUs. FSR 4 v07 requires supported hardware Vulkan capabilities, and higher output resolutions may be temporally reconstructed rather than rendered as native scene geometry.

Code Fairy graphics options include 16:9, 21:9, 24:9 and 32:9; **ultrawide HUD layout is experimental**. Graphics quality shrinks the game's sun shadow atlas from 4096² to 2048²/1024². 30 FPS gameplay is supported as a portability option; movie playback may temporarily run at 60 FPS.

## 6. Credits, source and license

PS4RT includes adapted material from [gow3_pc](https://github.com/FelipePrado0/gow3_pc), [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), [shadPS4](https://github.com/shadps4-emu/shadPS4) and [AzaharPlus/shadPS4Plus](https://github.com/AzaharPlus/shadPS4Plus). The current repository's source license is **GPL-2.0-or-later**, while each included third-party component keeps its original license.

The FSR 4 **v07** assets have [their own license notice](../fsr4_shaders/LICENSE-FSR4-v07.txt) and a checksum/provenance manifest at [packaging/fsr4-v07-assets.json](../packaging/fsr4-v07-assets.json). The native PKG component also requires keeping its corresponding GPL source and dependency notices. The trophy metadata handling credits additional existing contributors; see [Upstream credits](UPSTREAM_CREDITS.md).

The project is independent of and not endorsed by the game's publishers, Sony or AMD. No copyright ownership or distribution rights in the PS4 game are implied by this software release.
