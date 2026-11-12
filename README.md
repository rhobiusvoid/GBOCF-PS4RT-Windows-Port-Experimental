# Mobile Suit Gundam Battle Operation Code Fairy (PS4): PS4RT for Linux

Game-specific **Windows port of a Linux x86-64 compatibility runtime** for **MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy** (PlayStation 4, **CUSA28763, v01.03**). Developed and gameplay-tested on Fedora Linux, then tested on Windows 10.

PS4RT runs prepared original PS4 x86-64 game code through Windows-hosted PS4 service compatibility layers and a **shadPS4-derived Vulkan renderer**. It is **not** a general PS4 emulator, a recreation of the game, or a substitute for owning the game.

<p align="center">
  <img src="docs/images/sample.png" alt="Code Fairy running in ultrawide on Linux with FSR 4 and a 60 FPS performance overlay" width="1100">
</p>

**Bring your own game and volume unlock keys.** To access Code Fairy's three episodic volumes, you need **(1) the extracted CUSA28763 game, (2) game version v01.03 in PKG format, and (3) all three volume unlock-key PKGs**. If your extracted game is already v01.03, you do not need to apply a separate update PKG. The volume keys are **not optional gameplay DLC**: they unlock the corresponding episodes already in the game, and the game will not progress past the title screen if you don't own Volume 1 at least. 

PS4RT neither supplies these files nor bypasses the game's volume locks. No PS4 firmware, user saves, decrypted executables or PlayStation Network authentication is provided. This project is independent of the game's rights holders, Sony and AMD.

## Download and play

This specific repo is a Windows port. Just download one of the releases, extract the following .7z file and run Play-Game.bat in a CMD window (or don't if you don't want to maintain logs), then follow the instructions to set the game up.

After successful setup, the path is remembered. For later sessions, simply run `Play-Game.bat`. Pass `--game` again if you move or replace the extracted game directory. Quote paths containing spaces. You can also pass `--dlc-pkg <path>` in order to install the Volume keys, in case you weren't able to install them during initial setup. This option is also available in the 'Insert' GUI.

### Required v01.03 game version and episodic volume keys

**Code Fairy v01.03 is required.**

- **Already have extracted v01.03:** PS4RT validates and prepares the game directly.
- **Have extracted v01.00:** The integrated setup asks for your own matching **CUSA28763 v01.03 update PKG** and applies the update before launching.
- **Important:** Applying that update **modifies the selected extracted CUSA28763 directory in place**. Back up the original v01.00 directory beforehand if you need to preserve it for another application. Your source PKG is not modified.
- **Three volume unlock-key PKGs are required to access all three volumes.** Code Fairy is episodic: these keys unlock existing story content; they are not conventional add-on DLC. After applying an update, PS4RT may offer a package picker. You can skip it to finish setup, but the volumes remain locked until their keys are installed. You can install the keys later using **Install DLC...** in the Graphics overlay by pressing 'Insert.'
- If setup is interrupted, rerun the launcher and supply the same update PKG to repair the installation.

See [Player update and volume-key setup](docs/PLAYER_PKG_SETUP.md) for detailed steps and command-line alternatives.

### Graphics and controls

Open the PS4RT menu with **Insert** on the keyboard, or **hold L2 and click R3** on a controller. Use the D-pad to navigate, Cross to select, and Circle to return. The menu separates **Graphics** from **Achievements**, with Code Fairy's compact two-tab layout. The Graphics page also provides FPS and game-related controls.

Available features include:

- **Internal resolutions & UltraWide Support:** 1024×576 (the low-end “540p” option), 720p, 1080p, 1440p, 2160p/4K, plus **21:9, 24:9 and 32:9 ultrawide** modes up to 5120×1440. Internal resolution changes require restarting.
- **Upscaling:** Off, **FSR 1**, **FSR 3.1**, or **FSR 4 v07** on capable hardware. FSR reconstructs the 3D scene before the HUD is composited; output resolution is selected separately, within the same aspect-ratio family. Upscaler selection is live.
- **Graphics quality:** **Max (original)**, **Medium**, and **Low**. Medium reduces the 4096² sun-shadow atlas to 2048²; Low uses 1024². Restart required.
- **Frame rate:** The game's original **60 FPS** behavior, with an optional **30 FPS cap** for weaker devices.
- **Anisotropic filtering** (game default, 2×, 4×, 8×, 16×), sharpening, display options, and an FPS counter.
- **Local achievements:** The game triggers trophy unlocks; the PS4RT overlay saves and displays progress, earned timestamps, icons and hidden entries. This is **not PSN trophy synchronization**, and previously completed objectives are not guaranteed to unlock retroactively.
- **Volume key installation:** Use the overlay's existing **Install DLC...** button for the three volume unlock-key PKGs. The button retains its generic name because the keys use the same PS4 add-on package mechanism; **no separate DLC is required**. Save and restart after installing keys so the game recognizes the unlocked volumes.

The in-game menu also offers **FSR 1** and the finer-grained internal/ultrawide choices; not every menu option has a launcher flag. See [Running on Linux](docs/RUNNING_LINUX.md) for details.

## Current compatibility status

**Playable and tested through Chapter 9 (Volume 2) on a Radeon 9070XT and Ryzen 9700X (using the original Linux build, by Hakemi).** Gameplay QA has covered the tutorial, story missions through Chapter 9, combat and bosses, cutscenes, menus, saving/loading and completion of Volume 1. **Volume 3 has not been fully gameplay-tested.** The displayed 60 FPS/FSR 4 screenshot documents a particular tested configuration, not a performance guarantee for other GPUs.

**Windows 10 was able to go up to Chapter 2 before a trophy crash that was fixed in 0.2.** The rest of the game should be playable from start to finish with no worries.

Known limitations and notes:

- **Ultrawide is experimental.** Some camera-driven HUD elements, may appear misplaced or flicker. Reproduce at 16:9 before reporting a widescreen-specific regression. Cutscenes and pre-recorded videos remain at 16:9 aspect ratio as they were designed for that size and can look distorted otherwise. 
- **FSR 4 v07** needs compatible Vulkan GPU features; use FSR 3.1, FSR 1 or Off where unsupported.
- **30 FPS mode** is intended for lower-power hardware. The runtime may lift the limiter for 60 FPS video playback to avoid the videos playing at half speed and during dialogue as they are less intensive.
- FPS depends heavily on GPU power, internal resolution, graphics-quality preset and shader/cache state. **Steam Deck/SteamOS performance is not yet guaranteed.**
- Version **v01.03 / CUSA28763** is the tested and required title combination. Other title IDs or executable versions are unsupported.

Please report reproducible defects on the **published project's GitHub Issues page** with your game version, mission/chapter, GPU/driver, resolution, graphics preset, upscaler and reproduction steps. For visual problems, also include whether they reproduce in 16:9 with FSR Off.

## Build from source (developers)

Players should use a portable release, not a developer build. For Fedora/source builds install a C/C++ toolchain, Python 3, CMake, Ninja, pkg-config and development dependencies for Vulkan, SDL3, FFmpeg, glslang, Boost, fmt, xxHash, Zydis, X11, **Crypto++ and zlib** (for native PKG handling).

~~~bash
PS4RT_LTO=OFF bash build.sh --test
bash run.sh --game "/absolute/path/to/CUSA28763"
~~~

For a portable Ubuntu 22.04 / glibc 2.35 release build, follow [Portable build tools](packaging/linux/README.md) and [Distribution guide](docs/DISTRIBUTING_LINUX.md). **No release is published automatically** by those scripts.

Local generated state resides under `out/CUSA28763/`, including prepared game code, saves, local trophies, caches and installed volume keys. **Never publish or commit `out/`.** See [Linux binary and data layout](docs/LINUX_BINARY_RELEASE.md).

## Documentation

- [Running on Linux](docs/RUNNING_LINUX.md)
- [Linux binary release and local data](docs/LINUX_BINARY_RELEASE.md)
- [Player update and volume-key setup](docs/PLAYER_PKG_SETUP.md)
- [Developer distribution and release checklist](docs/DISTRIBUTING_LINUX.md)
- [Upstream source credits and licensing](docs/UPSTREAM_CREDITS.md)

## Credits and licensing

PS4RT adapts open-source compatibility runtimes and renderer work from **gow3_pc**, **bloodborne_pc**, **shadPS4** and related contributors. The integrated PKG extractor is adapted from **AzaharPlus/shadPS4Plus**. Local trophy handling also incorporates work credited in `scripts/trophy_crypto.py`. REA and Ghidra were used for analysis, not bundled as runtime dependencies.

Runtime source is **GPL-2.0-or-later**; third-party code, fonts, shaders and media retain their individual terms. Refer to [Upstream credits and notices](docs/UPSTREAM_CREDITS.md), [LICENSE](LICENSE) and [FSR 4 asset notice](fsr4_shaders/LICENSE-FSR4-v07.txt). No copyright or trademark ownership of the underlying game is claimed.
