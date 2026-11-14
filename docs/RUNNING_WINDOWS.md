# Running Code Fairy PS4RT on Windows

Player guide for **MOBILE SUIT GUNDAM BATTLE OPERATION Code Fairy, CUSA28763 v01.03**.

**Bring your own game, update and volume keys.** You need the extracted game, the v01.03 update PKG
(only if your extraction is v01.00) and all three volume unlock-key PKGs. Nothing here supplies or bypasses
them. Releases contain no game data, firmware, PKGs or saves.

## Requirements

- Windows 10 version 1803 or newer, 64-bit.
- A GPU with Vulkan 1.3 drivers (recent NVIDIA, AMD or Intel drivers). FSR 4 needs newer GPUs;
  use FSR 3.1, FSR 1 or Off otherwise.
- About 74 GB free disk space for the extracted game, plus space for caches under `out\`.
- No Python, compiler or extra runtimes: the release bundles everything.

## 1. Get the package

1. Download the release from the **Releases** page. If it is split into parts, put every part in one folder and
   join them in `cmd`, as described in [WINDOWS-PORT.md](../WINDOWS-PORT.md). Check the size it states.
2. Extract it to a **writable folder with a short path**, for example `C:\CodeFairy\`. Avoid `Program Files`.
   Keep all files together.
3. Extract your own game PKG. This port reads the plaintext folder (it contains `eboot.bin`,
   `sce_sys\param.sfo`, `sce_module\`).
   The bundled `bin\ps4rt-pkg.exe` can do this; see [Update and volume-key setup](PLAYER_PKG_SETUP.md).

## 2. First launch

Double-click **`NoisyFairy.bat`**. In the launcher, press **Browse**, pick the extracted `CUSA28763` folder and press **Play**.
The **Options** tab holds the graphics settings (the same ones as the in-game menu), and a **Write log file** switch
(on by default) that saves everything the game prints to `out\CUSA28763\user\log`. The launcher returns when the game closes
and shows the exit code if it crashed.

The command line does the same job. Open a `cmd` window in the extracted folder and run:

```bat
Play-Game.bat --game "C:\path\to\extracted\CUSA28763"
```

Quote paths with spaces. The path is remembered in `out\CUSA28763\game-folder.txt`, so later sessions only need:

```bat
Play-Game.bat
```

Pass `--game` again if you move the game folder. Redirect output to keep a log:
`Play-Game.bat > log.txt 2>&1`.

If the game folder is v01.00, the setup window asks for your v01.03 update PKG and updates that folder **in place**.
Back it up first if you need the original. The three volume keys are installed through the setup window or
the launcher's **Install Volume Keys...** button (or the overlay's **Install DLC...**), then restart.

## 3. Menu, graphics and controls

Open the PS4RT overlay with **Insert**, or **hold L2 and click R3** on a gamepad. D-pad navigates, Cross selects,
Circle goes back, L1/R1 (or Q/E) switch tabs.

- **Internal resolution:** 1024x576 up to 4K, plus 21:9, 24:9 and 32:9 modes. Restart required.
- **Quality preset:** Max / Medium / Low (sun-shadow atlas size). Restart required.
- **Upscaler:** Off / FSR 1 / FSR 3.1 / FSR 4 v07 (live). **DLSS is not available on Windows builds.**
- **Frame rate:** original 60 FPS, or a 30 FPS cap. Set `PS4RT_FPS_LIMIT=60` (or 30) in the environment to force
  a limit if the game feels too fast; the default is paced to the display refresh.
- Sharpening, anisotropic filtering, FPS counter, achievements tab, **Install DLC...**.

Optional launcher overrides: `Play-Game.bat --upscaler fsr4 --output-resolution 4k`
(`off`, `fsr3`, `fsr4`; `1080`, `1440`, `4k`).

Default keyboard: WASD = left stick, arrows = right stick, Space = Cross, Left Shift = Circle,
E/Q = Square/Triangle, I/J/K/L = D-pad, Insert = overlay. Remap with:

```bat
set PS4RT_KEY_MAP=cross=Q,triangle=Space
Play-Game.bat
```

`PS4RT_PAD_MAP` remaps SDL gamepad buttons the same way.

## 4. Saving

Use the game's own save menu; **Restart Game** and **Exit Game** do not save for you.
Saves live in `out\CUSA28763\user\savedata\<user>\CUSA28763\`. Each time a save is closed the runtime also tries
to copy it to `out\CUSA28763\user\savedata-backups\`. A failed backup never blocks saving; it only prints
`save backup skipped ...` in the log.

## 5. Where files live

Everything the runtime generates is under `out\CUSA28763\` inside the package folder. See
[WINDOWS_DATA_LAYOUT.md](WINDOWS_DATA_LAYOUT.md). **Never share or commit `out\`.**

## 6. Troubleshooting

| Symptom | What to do |
| --- | --- |
| `STOP: reserving the PS4 address range ... error 487` | The runtime needs the 0x1000000000-0xfc00000000 range free. Launch with `Play-Game.bat` (it clears the high-entropy ASLR flag on bundled DLLs); do not run `bin\ps4rt.exe` from a copy with different DLLs. |
| Window closes immediately | Open the newest file in `out\CUSA28763\user\log` (or run `Play-Game.bat` from a `cmd` window and read the last lines). |
| Black screen at boot, no Bandai Namco logo | Boot takes a few seconds. If it stalls longer, close the game and report it on the Issues tab with the newest log; with logging on it contains a "Hang watch" section showing what every thread is doing. |
| `Fault: ...` report | Send the full console output (it includes registers and a stack excerpt). |
| Setup rejects the game | Confirm CUSA28763 / v01.03 and that `--game` is the extracted folder itself. |
| A volume stays locked | Install that volume's key PKG, then restart. |
| Game asks for T&C every start | Update to the current build; older builds could not release save mounts (fixed in 0.3). |
| Wrong visuals on older GPUs | Some PS4 shader features (barycentric interpolation, some formats) are missing on Pascal and older GPUs. Test 16:9 with FSR Off and report GPU/driver. |
| Hang on Chapter 1/3/5 | Known deadlock risk; see [WINDOWS-MUTEX-NOTES.md](WINDOWS-MUTEX-NOTES.md). Report the last console line. |

When reporting a problem include: the console log, Windows version, GPU/driver, chapter/mission, and whether the
visual issue reproduces at 16:9 with FSR Off.

## 7. Building from source

The Windows binaries are cross-compiled from Linux with clang and MinGW-w64. See
[windows-build/NOTES.md](windows-build/NOTES.md) and `windows-build/link.sh`.
