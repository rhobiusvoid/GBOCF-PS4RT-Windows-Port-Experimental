# NoisyFairy: Windows port (0.5 Alpha, experimental)

Unofficial Windows port of https://github.com/HekamiAeon/GBO-Code-Fairy-PS4RT by HekamiAeon (GPL-2.0-or-later; upstream
history is not included here, this is a snapshot of upstream plus the port changes). All credit for the original runtime goes to Hekami.

- Cross-compiled on Linux with clang + MinGW-w64 (UCRT/msvcrt, libstdc++, `-femulated-tls`).
- New: `src/compat_win.[ch]`, `src/compat_win_mem.h` (POSIX shims, runtime-resolved VirtualAlloc2/MapViewOfFile3, setjmp/longjmp, 1 ms timer).
- Launcher scripts ported (`Play-Game.bat`, `scripts/*.py`).
- DLSS disabled on Windows (bridge header missing upstream); FSR 1/3.1/4 kept.
- See `docs/WINDOWS-MUTEX-NOTES.md` and `docs/RUNNING_WINDOWS.md` for the deadlock-risk review.
- 0.5 Alpha: new graphical launcher (`NoisyFairy.bat`), log files, working volume-key installer, pipeline binary cache back on for AMD/Intel (see `RELEASE-NOTES-0.5.md`).
- 0.4 Alpha: fixed a boot crash on NVIDIA drivers that expose `VK_KHR_pipeline_binary` (RTX 30/50 cards). The pipeline binary cache is off by default on NVIDIA only (`PS4RT_PIPELINE_BINARIES=1` forces it on, `0` off).
- Prebuilt package: see Releases. Requires Windows 10 1803+, Vulkan 1.3 GPU. Bring your own game files and keys.

## Download and run
1. Get all `NoisyFairy-0.5-Alpha.zip.partNN.bin` files from the Releases page into one folder.
2. Join them in cmd (adjust the list if there are more or fewer parts):
   `copy /b NoisyFairy-0.5-Alpha.zip.part00.bin+NoisyFairy-0.5-Alpha.zip.part01.bin+NoisyFairy-0.5-Alpha.zip.part02.bin+NoisyFairy-0.5-Alpha.zip.part03.bin NoisyFairy-0.5-Alpha.zip`
   The result must be exactly 93,015,186 bytes (SHA-256 899722f1aec541dd9042eb1e86acbe84f246f6d9c32bcde42f35d6cd3a328c39).
3. Unzip to a short writable path, then double-click `NoisyFairy.bat`, pick your extracted `CUSA28763` folder and press Play.
4. Keep the v01.03 update PKG and the three volume-key PKGs as `.pkg` files; install them via the setup window or **Install Volume Keys...** on the launcher.

The game should start within a few seconds. A black screen that never shows the Bandai Namco logo is a stall: close the game and report it on the Issues tab with the newest log from `out\CUSA28763\user\log`.

When reporting a problem include: that log file, Windows version, GPU/driver.
