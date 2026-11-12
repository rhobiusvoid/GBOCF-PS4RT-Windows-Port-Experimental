# Windows port (experimental, untested on real Windows)

Unofficial Windows port of https://github.com/HekamiAeon/GBO-Code-Fairy-PS4RT (GPL-2.0-or-later; upstream
history is not included here, this is a snapshot of upstream plus the port changes).

- Cross-compiled on Linux with clang + MinGW-w64 (UCRT/msvcrt, libstdc++, `-femulated-tls`).
- New: `src/compat_win.[ch]`, `src/compat_win_mem.h` (POSIX shims, runtime-resolved VirtualAlloc2/MapViewOfFile3, setjmp/longjmp, 1 ms timer).
- Launcher scripts ported (`Play-Game.bat`, `scripts/*.py`).
- DLSS disabled on Windows (bridge header missing upstream); FSR 1/3.1/4 kept.
- See `docs/WINDOWS-MUTEX-NOTES.md` for the deadlock-risk review.
- Prebuilt package: see Releases. Requires Windows 10 1803+, Vulkan 1.3 GPU. Bring your own game files and keys.

## Download and run
1. Get all five `CodeFairy-PS4RT-Windows.zip.partNN.bin` files from the Releases page into one folder.
2. Join them in cmd:
   `copy /b CodeFairy-PS4RT-Windows.zip.part00.bin+CodeFairy-PS4RT-Windows.zip.part01.bin+CodeFairy-PS4RT-Windows.zip.part02.bin+CodeFairy-PS4RT-Windows.zip.part03.bin+CodeFairy-PS4RT-Windows.zip.part04.bin CodeFairy.zip`
   The result must be exactly 97,537,525 bytes.
3. Unzip, then run `Play-Game.bat --game "C:\path\to\extracted\CUSA28763"` from a cmd window.
4. Keep the v01.03 update PKG and the three volume-key PKGs as `.pkg` files; install them via the setup window or **Install DLC...** in the overlay (Insert key).

When reporting a problem include: last console line, Windows version, GPU/driver, CPU idle or busy if hung.
