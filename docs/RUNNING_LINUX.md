# Running Code Fairy PS4RT on Linux

This is the player guide for **MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy, CUSA28763 v01.03**, using the PS4RT Linux x86-64 runtime.

**Bring your own game, v01.03 update (if needed) and three volume unlock-key PKGs.** The three keys enable Code Fairy's episodic volumes; they are not conventional DLC and PS4RT does not bypass the locks. Player releases and source code do not include game data, PS4 firmware, update/key PKGs or saves. The first official player release has not yet been published from this cleanup repository.

## 1. Obtain and start the portable

1. When published, download the **Linux x86-64 player** `.tar.gz` archive and matching SHA256 from the project's Releases page. The separate source archive is for developers.
2. Extract the player archive into a **writable** directory. Keep all files together.
3. Have an extracted plaintext `CUSA28763` folder containing `eboot.bin`, `sce_sys/param.sfo`, `sce_module/` and game data.
4. In a terminal opened inside the extracted player directory, launch:

~~~bash
bash Play-Game.sh --game "/absolute/path/to/CUSA28763"
~~~

For subsequent sessions after a successful first launch:

~~~bash
bash Play-Game.sh
~~~

The launcher remembers the validated directory at `out/CUSA28763/game-folder.txt`. If you move the game, pass `--game` again. You may use spaces in paths when quoted. A missing game path prints a setup hint pointing to `PLAYER_README.txt`.

**Version and volume keys:** Extracted `v01.03` starts directly; `v01.00` requires a matching `v01.03` update PKG through the included installer. The installer **updates the extracted game's files in place**; back up the original folder if you want to keep it. **All three volume keys are needed to unlock the full episodic campaign, regardless of whether the game was already updated.** Install them using the generic **Install DLC...** button on the Graphics tab; then restart. See [Player update and volume-key setup](PLAYER_PKG_SETUP.md).

## 2. Menu and graphics

Open the PS4RT overlay with **Insert**, or **L2 + R3 click** on a gamepad. Navigate via D-pad, confirm with Cross, and back out with Circle. Use L1/R1 (or Q/E) to switch menu tabs when shown.

The **Graphics** screen exposes:

- **Internal resolution:** 1024×576 (labelled low-end 540p), 1280×720, 1920×1080, 2560×1440, 3840×2160, plus 2560×1080, 3440×1440, 3840×1440, 3840×1080 and 5120×1440. Changing the internal resolution requires a **restart**. 1024×576 is used instead of 960×540 to preserve particle effects.
- **Quality preset:** Max retains the 4096×4096 sun-shadow atlas; Medium lowers it to 2048×2048 and Low to 1024×1024. **Restart required.**
- **FSR:** Off / FSR 1 / FSR 3.1 / FSR 4 v07, depending on available hardware. FSR 1 is lighter and does not require temporal history. FSR 3.1 and 4 perform temporal reconstruction. Select an **upscale output** separately, within the same aspect-ratio family.
- **Sharpening** and **anisotropic filtering**. AF changes require restarting. Upscaler and output choices apply live.
- **Frame rate limit:** 60 FPS original or optional 30 FPS for lower-power devices. The game simulation must stay at the correct speed; do not force a higher present rate externally.
- **Display mode, FPS/performance overlay, and other game options.**

**Ultrawide is experimental.** The 21:9, 24:9 and 32:9 scene can be rendered wider, but some HUD markers, dialogue bubbles or camera-dependent elements may not position perfectly. Try 16:9 when investigating UI defects.

The game has its **own language selector**; a duplicate PS4RT language toggle is unnecessary.

Optional launcher overrides for the advertised command-line subset:

~~~bash
bash Play-Game.sh --upscaler fsr4 --output-resolution 4k
~~~

Only `off`, `fsr3` and `fsr4` are accepted for `--upscaler`, and `1080`, `1440` and `4k` for `--output-resolution`. Choose FSR 1, the internal resolution, and additional options in the overlay. Unsupported FSR 4 devices can use FSR 3.1 or a lighter mode.

When gameplay is capped at 30 FPS, the runtime can temporarily allow 60 FPS during movies for correct playback.

## 3. Volume unlock keys, achievements and saving

- **Volume keys (required for the full story):** Obtain **all three** CUSA28763 volume unlock-key PKGs. Code Fairy's episodes are gated by these keys; the port **does not disable the locks**. The packages use the same installation path as standard PS4 DLC, so select **Install DLC...** at the bottom of the **Graphics** tab, install your keys, then **save and restart**. The game has no separate conventional DLC to install. The three volume-key packages were exercised during development.
- **Achievements:** The overlay's **Achievements** tab presents local unlocks, trophy icons, progress, hidden achievements and dates. They are stored locally, **not synced to PSN**. Existing game saves are not necessarily scanned to unlock historical trophies automatically.
- **Saves:** Use the game's own save UI. **Restart Game** and **Exit Game** do not save your story progress for you.

Default keyboard controls include WASD for the left stick, arrow keys for the right stick, Space for Cross, Left Shift for Circle, E/Q for Square/Triangle, I/J/K/L for D-pad and Insert for the overlay. You can remap buttons at launch:

~~~bash
PS4RT_KEY_MAP="cross=Q,triangle=Space" bash Play-Game.sh
~~~

`PS4RT_PAD_MAP` similarly remaps SDL gamepad buttons. These do not remap stick axes or the overlay shortcut.

## 4. Where files live

The extracted portable and source checkout use `out/CUSA28763/`:

| Path | Purpose |
| --- | --- |
| `game-folder.txt` | Remembered CUSA28763 directory |
| `ps4rt.ini` | Saved graphics preferences |
| `boot-linked.bin` and `content.bin` | Locally generated game-derived working data |
| `user/savedata/` | In-game save files |
| `user/achievements/` | Locally earned achievement state and prepared trophy catalog |
| `user/gpu/cache/` | GPU shader and pipeline caches |
| `player-content/dlc/` | Three volume unlock-key PKGs, stored in the generic add-on directory |
| `player-content/update.json` | Local update-install metadata, when present |

**Do not redistribute `out/`**, including installed volume keys, or original PS4 game/update data. The release builder excludes them.

## 5. Requirements and troubleshooting

- Native **x86-64/AMD64 Linux**, **glibc 2.35 or newer**, Python 3 and a working Vulkan graphics/desktop stack.
- The portable is intended to carry SDL3, FFmpeg and native PKG/extractor dependencies. Do not add arbitrary system libraries or set `LD_LIBRARY_PATH` unless troubleshooting an independently verified problem.
- Developer gameplay QA has reached **Chapter 9 in Volume 2**, including completion of Volume 1. **Volume 3 has not been comprehensively gameplay-tested**. Steam Deck/SteamOS, ARM64 translators and broad GPU compatibility are not guaranteed.
- If setup rejects the game, confirm **CUSA28763** and **v01.03**, and that `--game` points to the correct extracted directory.
- If a v01.00 update was interrupted, rerun it with the **same valid v01.03 PKG**. Do not remove partial-update markers manually.
- If a volume remains locked, check that its matching unlock-key PKG is installed and restart the game.
- If the screen or HUD is wrong, test **16:9 + FSR Off** and record your exact resolution/upscaler/graphics-quality options.

For building from source:

~~~bash
PS4RT_LTO=OFF bash build.sh --test
bash run.sh --game "/absolute/path/to/CUSA28763"
~~~

Read [Distribution and maintainer instructions](DISTRIBUTING_LINUX.md) for the controlled portable builder, and [Upstream credits](UPSTREAM_CREDITS.md) for software licenses.
