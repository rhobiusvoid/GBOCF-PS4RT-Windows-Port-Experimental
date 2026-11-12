# Code Fairy PS4RT overlay assets

This Code Fairy port uses four **title-specific, replaceable** overlay resources:

~~~text
gpu/assets/overlay.png
gpu/assets/sounds/navigate.wav
gpu/assets/sounds/confirm.wav
gpu/assets/sounds/cancel.wav
~~~

The overlay draws the illustration inside its own translucent menu, and the menu sound player loads the three RIFF WAV cues. Missing/invalid audio has a built-in fallback. Files can be replaced without C++ changes; keep the filenames and supported limits (PNG no larger than 4096×4096, WAV no larger than 2 MiB each). `open.wav` is optional.

To provide your own assets at runtime without changing the repository:

~~~bash
PS4RT_OVERLAY_BACKGROUND="/absolute/path/to/menu.png" \
PS4RT_MENU_SOUND_DIR="/absolute/path/to/wavs" \
bash Play-Game.sh
~~~

**Licensing checkpoint:** These files are no longer necessarily the Lab's synthetic placeholders. Review the provenance and permission to redistribute *all* title-themed artwork, recorded menu audio, and README screenshots before producing a public release. This port does not claim rights to the underlying Gundam game or its soundtrack/sound effects.

The generic Lab template originally supplied procedurally generated placeholders; this project overrides those for its Code Fairy visual identity.
