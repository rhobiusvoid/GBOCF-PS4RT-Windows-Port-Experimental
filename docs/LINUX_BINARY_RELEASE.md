# Code Fairy PS4RT: Linux portable player and archive layout

This describes the intended **Linux x86-64 player release** for **Code Fairy CUSA28763 v01.03**. This cleanup repository contains the source prior to the first Code Fairy release; **do not assume a versioned player download is already published**.

## For players

When a release is published, use the project's GitHub **Releases** page to download:

- The **Linux player** `.tar.gz` archive and its `.sha256` checksum.
- The separately labeled **source** archive is for developers, not required to play.

Verify the checksum using the downloaded filename, then extract into a writable directory and run:

~~~bash
bash Play-Game.sh --game "/absolute/path/to/CUSA28763"
~~~

The player archive is built to bundle the `ps4rt` runtime, a setup UI, native PKG extractor, SDL3, FFmpeg, required Crypto++ runtime, FSR dependencies and title-specific menu assets. Players should not need CMake, compilers, an emulator or a separate PS4 PKG extraction tool. The planned ELF baseline is **glibc 2.35+**, native **x86-64/AMD64**, with a working host Vulkan GPU/driver.

**Code Fairy needs the extracted game, v01.03, and all three volume unlock-key PKGs to unlock the full episodic campaign.** If your game extraction is v01.00, setup requests the matching v01.03 update PKG and **modifies that game directory in place**. Back it up first if you need an unmodified copy. A game already at v01.03 needs no separate update PKG. The three **volume keys are not conventional DLC**; use the overlay's generic **Install DLC...** button to install them, then restart. PS4RT does not supply or bypass the keys. See [Update and volume-key guide](PLAYER_PKG_SETUP.md).

After successful setup the game path is remembered, so future starts can use `bash Play-Game.sh` alone. PS4RT does not provide the game, firmware, PKGs, executable or license material.

## Working files (private, not distributable)

Inside the extracted player directory, `out/CUSA28763/` stores generated and user-specific state:

| Location | Contents |
| --- | --- |
| `game-folder.txt` | Remembered original game-directory path |
| `boot-linked.bin` / `content.bin` | Generated from the player's game |
| `ps4rt.ini` | Port graphics/menu settings |
| `user/savedata/` | Game saves |
| `user/achievements/` | Local trophy/achievement state and catalog |
| `user/gpu/cache/` | Recompiled shaders and Vulkan pipeline caches |
| `player-content/dlc/` | User-installed volume unlock keys (generic DLC-format package directory) |
| `player-content/update.json` | Last successful update metadata |

**Do not distribute, commit, or attach this directory** to bug reports. It may contain proprietary game-derived files, volume unlock keys, private save data and caches. Developers should ask for relevant *log excerpts* and reproduction steps instead.

## Graphics and compatibility

The in-game menu can be opened with **Insert** or **L2 + R3 click**. It offers internal resolutions from low-end 1024×576 to 4K and 32:9 ultrawide, FSR Off/1/3.1/4 v07, graphics-quality presets (shadow atlas reduction), AF, performance display and 30/60 FPS play limits. Some choices require a restart. FSR4 requires suitable GPU capabilities, and ultrawide HUD layout remains experimental.

Hands-on gameplay QA has reached **Chapter 9 in Volume 2**, including completion of Volume 1. **Volume 3 remains unverified by a comprehensive playthrough**, and the complete hardware/distribution matrix is not tested. Do not interpret one machine's 60 FPS screenshot as a minimum hardware specification.

## For maintainers

The source tree includes `port_features.json` declaring update/DLC capabilities, the GPL-licensed native PKG parser, an isolated Ubuntu 22.04/glibc 2.35 portable builder, and a filtered archive packager. The builder produces a player and source archive with their own SHA256 companions. See [Distribution guide](DISTRIBUTING_LINUX.md) and [Packaging toolchain](../packaging/linux/README.md).

The output is not released automatically, and a successful automated build is not a substitute for game-version validation, player setup testing, asset/license checks or an extracted-portable gameplay smoke test.
