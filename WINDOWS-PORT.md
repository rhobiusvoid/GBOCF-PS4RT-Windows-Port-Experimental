# NoisyFairy: Windows port (0.4 Alpha, experimental)

Unofficial Windows port of https://github.com/HekamiAeon/GBO-Code-Fairy-PS4RT by HekamiAeon (GPL-2.0-or-later; upstream
history is not included here, this is a snapshot of upstream plus the port changes). All credit for the original runtime goes to Hekami.

- Cross-compiled on Linux with clang + MinGW-w64 (UCRT/msvcrt, libstdc++, `-femulated-tls`).
- New: `src/compat_win.[ch]`, `src/compat_win_mem.h` (POSIX shims, runtime-resolved VirtualAlloc2/MapViewOfFile3, setjmp/longjmp, 1 ms timer).
- Launcher scripts ported (`Play-Game.bat`, `scripts/*.py`).
- DLSS disabled on Windows (bridge header missing upstream); FSR 1/3.1/4 kept.
- See `docs/WINDOWS-MUTEX-NOTES.md` and `docs/RUNNING_WINDOWS.md` for the deadlock-risk review.
- 0.4 Alpha: fixes a boot crash on NVIDIA drivers that expose `VK_KHR_pipeline_binary` (seen on RTX 30/50 cards); the pipeline binary cache is now off by default on Windows (`PS4RT_PIPELINE_BINARIES=1` re-enables it).
- Prebuilt package: see Releases. Requires Windows 10 1803+, Vulkan 1.3 GPU. Bring your own game files and keys.

## Download and run
1. Get all four `NoisyFairy-0.4-Alpha.zip.partNN.bin` files from the Releases page into one folder.
2. Join them in cmd:
   `copy /b NoisyFairy-0.4-Alpha.zip.part00.bin+NoisyFairy-0.4-Alpha.zip.part01.bin+NoisyFairy-0.4-Alpha.zip.part02.bin+NoisyFairy-0.4-Alpha.zip.part03.bin NoisyFairy-0.4-Alpha.zip`
   The result must be exactly 89,373,605 bytes (SHA-256 d07ed7356227f9598fd04a0a45d8fedb9b86da49f5cf2779278d8006b15d1cc5).
3. Unzip, then run `Play-Game.bat --game "C:\path\to\extracted\CUSA28763"` from a cmd window.
4. Keep the v01.03 update PKG and the three volume-key PKGs as `.pkg` files; install them via the setup window or **Install DLC...** in the overlay (Insert key).

When reporting a problem include: last console line, Windows version, GPU/driver, CPU idle or busy if hung.
