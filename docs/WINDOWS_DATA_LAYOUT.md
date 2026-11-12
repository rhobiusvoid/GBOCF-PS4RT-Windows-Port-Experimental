# Windows package and data layout

## Package folder

| Path | Contents |
| --- | --- |
| `Play-Game.bat` | Launcher entry point |
| `bin\` | `ps4rt.exe` (runtime), `ps4rt-setup.exe` (setup window), `ps4rt-pkg.exe` (PKG extractor), DLLs |
| `python\` | Bundled Python used by the launcher scripts |
| `scripts\` | Launcher and setup scripts |
| `gpu\assets\`, `fsr4_shaders\` | Overlay art and FSR 4 shader data |
| `docs\` | This documentation |

## Generated, private data: `out\CUSA28763\`

| Path | Purpose |
| --- | --- |
| `game-folder.txt` | Remembered game directory |
| `ps4rt.ini` | Graphics settings |
| `boot-linked.bin`, `content.bin` | Working data derived from your game |
| `user\savedata\<user>\CUSA28763\` | Game saves (`LAUNCHERSAVE`, `SYSSAVE`, `GAMESAVE`, each with a `.sce_sys` folder) |
| `user\savedata-backups\` | Snapshot copies made when a save is closed |
| `user\achievements\` | Local achievement state |
| `user\gpu\cache\` | Shader and pipeline caches |
| `player-content\dlc\` | Installed volume unlock keys (generic add-on folder name) |
| `player-content\update.json` | Update metadata |

Back up `out\CUSA28763\user\savedata\` to keep your progress. **Do not redistribute `out\`**: it contains
game-derived files and your saves.
