# Reusable portable Linux build toolkit

This **developer-only** toolchain uses the Ubuntu 22.04, GCC 14, FFmpeg 8.0.3 (with PNG/zlib), SDL3 and glibc 2.35 combination validated by the separate Star Ocean 5 and Valkyria Revolution PS4RT builds.

The five Containerfiles and build-images.sh are derived from the tested toolchain.
The **generic build-runtime.sh and build-release.sh pipeline passed a clean
Star Ocean v1.1 rebuild, ELF audit, both archive/checksum audits, and an
extracted-player gameplay test on Fedora Linux in October 2026**. Individual new
ports still require their own compatibility and licensing review.

## Native PKG support for Code Fairy

Code Fairy uses the native PKG extractor to install the v01.03 game update
and its three episodic volume unlock-key packages. These keys are not
optional gameplay DLC, but use the same PS4 add-on package mechanism.
The overlay's existing **Install DLC...** action installs them; it does
not bypass the game's volume locks.

The build pipeline selects the Crypto++ image for this update/key-enabled
port, bundles `bin/ps4rt-pkg` and its audited Crypto++ dependency, and
includes the integrated setup UI. The generic packaging logic can also
handle ports with no native PKG requirement.

**Release check:** The development workflow has validated Code Fairy's
update and volume-key installation. The final portable archive still
requires an extracted-player first-launch, update, key-installation and
gameplay smoke test before v1.0 publication.

## One-time image creation

```bash
bash packaging/linux/build-images.sh
```

Image: `localhost/ps4rt-builder:linux`. Players do not need Podman or development packages.

## Compile a game repo from committed source

```bash
bash packaging/linux/build-runtime.sh
```

The script requires `build.sh` and `gpu/CMakeLists.txt`. It compiles a filtered `git archive HEAD` export inside an isolated temporary directory with `PS4RT_PORTABLE_BUILD=1`, `PS4RT_LTO=OFF` and `PS4RT_PGO=off`. The build contract outputs `out/ps4rt`, `out/gpu/libps4rtgpu.so`, and optionally `out/ps4rt-gpu-capabilities`. The sample Lab build.sh now has opt-in linker paths for the GCC14 container only; other builds stay unchanged.

The stage bundles 12 application shared libraries, uses `$ORIGIN`-relative RUNPATHs, audits glibc 2.35 max symbols and actual library resolution without `LD_LIBRARY_PATH`. It prints `BUNDLE=`, `BUILD_LOG=`, and `ELF_AUDIT=`.

**A staging bundle is not a complete playable or legally distributable release.** The compiler copies committed `scripts/`, `LICENSE`, `fsr4_shaders/` and the allowlisted `gpu/assets/overlay.png` / WAV placeholders when present. These files can be replaced in a game's Git history before its final build. Each independent title still supplies a reviewed player launcher, media permissions, source/runtime archive allowlist, third-party notices, SHA256 files, and gameplay smoke test. Neither original game content, generated linked game code, firmware, nor untracked files are copied.

The image may require a long one-time build and currently includes dependencies specific to the two tested ports. Other games may need new build-time components; don't silently assume the same 12 libraries suffice. This is a reproducible workflow from committed source, not a guarantee of bit-identical builds, SteamOS support or ARM64 compatibility. Keep Lab private files, AGENTS.md and source history out of public title repos.

## Predictable workspace and release folders

By default, the source project stays in `~/Documents/PS4RT-Games/<CUSAxxxxx>`.
The builder uses exactly **one** `~/Documents/PS4RT-Builds/<repository-name>`
location containing `source/`, `bundle/`, `build.log`, `elf-audit.txt`,
and `source-revision.txt`. It will reset generated source/bundle/logs **only**
if the directory has its own matching `.ps4rt-build-workspace` marker. It
refuses unrecognized directories or symlinked build folders. To choose another
temporary root set `PS4RT_LINUX_WORK_PARENT`. To name the project directory,
set `PS4RT_BUILD_ID` (letters/numbers/period/underscore/hyphen only).
These diagnostics stay local and are not pushed to GitHub.

For legacy per-game release scripts that produce archives in `out/community/`,
the optional collector moves already-reviewed archives into one folder:

```bash
bash packaging/linux/collect-release.sh
```

The collector verifies SHA256 companions before copying them into
`~/Documents/PS4RT-Releases/<repository-name>/`. It refuses to overwrite
different files with identical names. Set `PS4RT_RELEASE_ROOT` to change
the destination parent. **It neither builds nor uploads releases.**
The player archive must still be extracted and game-tested before publication.

## Reusable illustrated menu and replaceable media

The Code Fairy runtime loads its title-specific `gpu/assets/overlay.png`
and three WAV menu cues through `PS4RT_OVERLAY_BACKGROUND` and
`PS4RT_MENU_SOUND_DIR`. Artwork stays inside the translucent menu panel
with aspect-preserving cropping. The menu supports keyboard and gamepad
navigation and sound cues. Review redistribution rights for replaced
game-themed image/audio assets before release.
See the [asset guide](../../gpu/assets/README.md).

The optional sound regression test is invoked only when a port defines
`menu-sounds-test`; older repos without that target are still buildable.
The generic build and image-loading/audio paths were validated through the
Star Ocean v1.1 extracted-player smoke test. A new game still requires its
own runtime and gameplay validation.

## One-command Linux release (per-port smoke test required)

From a **new independent game's** committed source repository:

```bash
bash packaging/linux/build-release.sh
```

This runs the Ubuntu Podman builder, validates the resulting glibc 2.35 / x86-64 ELF
bundle, then creates **four** files under
`~/Documents/PS4RT-Releases/<repository-name>/`: one Linux player `.tar.gz`,
its `.sha256`, one sanitized source `.tar.gz`, and its `.sha256`.
Nothing is uploaded or published. It refuses to overwrite release assets.
The player command is `bash Play-Game.sh --game "/path/to/CUSAxxxxx"`.
Use `PS4RT_RELEASE_ROOT` to choose a different release parent. Existing
Star Ocean and Valkyria release scripts are unaffected.

**Private-file boundary:** `packaging/linux/release.py export-source` is
called by the builder **before** any Git files enter the temporary build
`source/` directory. It exports only whitelisted committed source, excluding
`AGENTS.md`, `CLAUDE.md`, `PORTING_LOG*`, continuity/handoff/resume notes,
Lab onboarding tools/templates, references, analysis, game dumps, savedata,
captures, logs, credentials and unknown media. The identical selection
rules drive the public source archive. The player archive has an even smaller
explicit file allowlist and does not recursively archive `source/` or `bundle/`.
A private development log remains local in the game workspace for the agent,
but new onboarded games do not commit it to Git history.

The generic pipeline passed the Star Ocean v1.1 Fedora build, archive,
privacy-fixture and extracted-player gameplay checks. For each new port,
inspect both archive manifests, run the automated checks and play-test the
extracted player before publication. Title-specific dependencies and licensed
media still require human review; this is not a publishing tool.
