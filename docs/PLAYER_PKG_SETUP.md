# Installing the Code Fairy v01.03 update and volume unlock keys

**Code Fairy PS4RT requires CUSA28763 v01.03 and uses three episodic volume unlock keys.** To access the full campaign, obtain **(1) the extracted game, (2) its matching v01.03 update if not already applied, and (3) all three volume unlock-key PKGs**. These keys unlock story content; they are **not optional DLC**. PS4RT neither bypasses the volume locks nor supplies any keys.

The portable runtime includes a native PKG parser and graphical setup utility. No separate PS4 emulator or extractor is required. The game, update and volume-key packages are **not** bundled or downloaded by PS4RT. The generic UI still says **Install DLC...** because the keys use the same PS4 add-on package format.

## First launch

1. Have your own extracted plaintext `CUSA28763` folder, containing `eboot.bin` and `sce_sys/param.sfo`.
2. Extract the PS4RT Linux player archive into a writable directory.
3. From the player directory, run:

~~~bash
bash Play-Game.sh --game "/absolute/path/to/CUSA28763"
~~~

4. **Already v01.03?** The launcher validates and starts the game without presenting a required update picker.
5. **v01.00?** The setup UI asks for **your own matching CUSA28763 v01.03 update PKG**. Select it, confirm installation, and allow extraction/verification to finish.
6. After a successful fresh update, PS4RT may offer its generic **DLC package chooser**. Select your owned **volume unlock-key PKGs** to unlock episodes. You can skip this *installation step* and continue launching, but the story volumes remain locked until their corresponding keys are installed. The launcher restarts into the updated game.

### Important: the update is in place

**The v01.03 update modifies the exact extracted `CUSA28763` folder selected with `--game`.** Files replaced by the patch are **not automatically backed up**, and the launcher avoids creating a second full-size game copy. If you need an untouched v01.00 extraction for another tool, **duplicate/back up the folder before updating**. The original update PKG and PS4RT saves are not modified by the update operation.

An interrupted update is detected and will prevent launching until repaired. Relaunch and select the **same v01.03 update**; do not attempt to boot the half-updated folder.

For scripted or headless setup, optional arguments are available:

~~~bash
bash Play-Game.sh --game "/absolute/path/to/CUSA28763" \
  --update-pkg "/absolute/path/to/your-v01.03-update.pkg" --no-dialogs
~~~

Alternatively, `--update-extracted` accepts a correctly extracted matching update folder. Supply **one** of `--update-pkg` or `--update-extracted`, not both. These options are for player-owned data and are not decryption-key providers.

## Installing the three volume unlock keys

1. Launch Code Fairy and save your progress.
2. Open the PS4RT overlay using **Insert**, or **L2 + R3 click**.
3. On the **Graphics** tab, select the bottom **Install DLC...** button and choose the owned, matching **CUSA28763 volume-key PKGs**. Install all three for access to the complete story.
4. Once installation finishes, **save and restart** using the overlay's **Restart to load DLC...** action.

Installed volume keys are stored under `out/CUSA28763/player-content/dlc/`, **a generic directory name inherited from PS4RT's add-on installer**. This does not mean Code Fairy has conventional DLC. The installer checks title/package identity, skips duplicates and validates content before staging. **Each volume remains locked without its key, and newly installed keys take effect on restart.** All three volume-key PKGs were checked through extraction, installation and rediscovery. Gameplay QA has reached **Chapter 9 (Volume 2)**; Volume 3 is not yet comprehensively tested.

For noninteractive installation, repeat the existing generic **`--dlc-pkg`** flag once per volume key (replace these example paths with your own filenames):

~~~bash
bash Play-Game.sh --game "/absolute/path/to/CUSA28763" \
  --dlc-pkg "/absolute/path/to/volume-1-key.pkg" \
  --dlc-pkg "/absolute/path/to/volume-2-key.pkg" \
  --dlc-pkg "/absolute/path/to/volume-3-key.pkg" --no-dialogs
~~~

## Troubleshooting

- **Update rejected:** Make sure the update is for **CUSA28763 v01.03** and matches your region/content. Other game versions are unsupported.
- **Unsupported base version:** This port accepts an already-updated v01.03 extraction or the supported v01.00 → v01.03 update process.
- **Interrupted update:** Reapply the exact matching v01.03 update. Do not delete the interrupted-update marker to bypass validation.
- **Volume still locked:** Check that the corresponding volume unlock-key PKG is installed, then restart. PS4RT does not bypass the game's lock or provide missing keys.
- **No graphical chooser:** Check your desktop/session; CLI options `--update-pkg`, `--dlc-pkg` and `--no-dialogs` can be used instead.
- **First launch fails:** Point `--game` to the actual extracted `CUSA28763` directory, not its parent, the PKG or `eboot.bin`.

See [Running on Linux](RUNNING_LINUX.md), [Main README](../README.md) and [Local data/release layout](LINUX_BINARY_RELEASE.md). The user's own game, update and volume-key packages must not be distributed with PS4RT.
