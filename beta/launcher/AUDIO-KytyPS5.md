# Dashboard music and interface audio

Implemented inside the installed Avalonia KytyPS5 dashboard. The application uses
one in-process SDL2 playback device, routed through the system's normal
PipeWire/PulseAudio output (including HDMI/DisplayPort). No separate player,
background service, Electron runtime, emulator patch, GPU change or game-profile
change is involved.

## Music installed on this computer

All four exact compositions were obtained through free download links on their
official OverClocked ReMix pages:

| Menu theme | Arrangement and artist | Official source |
| --- | --- | --- |
| Ocarina of Time — Great Fairy's Fountain | Garden of Venus — RebeccaETripp | https://ocremix.org/remix/OCR03924 |
| Kingdom Hearts — Dearly Beloved | Dear Serenity — Tune in with Chewie, Kristin Naigus, HOUNDTRACK | https://ocremix.org/remix/OCR03886 |
| Metal Gear Solid — Sniper Wolf's Theme (Enclosure) | Liquid Metal — Vig | https://ocremix.org/remix/OCR00895 |
| Final Fantasy — Prelude | Hymn to the Crystals — RebeccaETripp | https://ocremix.org/remix/OCR03621 |

The files downloaded successfully from the `ocrmirror.org` mirror linked by each
source page. They are fan arrangements of the requested compositions, not generic
substitutes. The files' original MP3 names and ID3 tags are preserved. Artist and
site credits appear in the Audio section, music chooser and linked credits panel.
The applicable source terms are https://ocremix.org/info/Content_Policy.

The downloaded originals live in `~/.local/share/KytyPS5/audio/music/`, outside the
application package. `music-sources.json` in the parent folder records their
sources, original tags, durations, zero cost and SHA-256 hashes. This is the user's
local music installation; it is not a public music redistribution bundle.

`python3 scripts/fetch-dashboard-music.py` can fetch missing originals again from
the official mirrors. `./run-kytyps5.sh --prepare-music` prepares the local cache.
Neither downloads nor conversion run continuously. The GUI works offline once
the files are installed.

## Audio settings and import

Settings → Audio contains independent Background music and Interface sounds
toggles, separate 0–100% sliders, the five-entry Music theme chooser, Test sound,
Import local music, and Music sources & credits. Keyboard/DualSense navigation
uses the existing rows and left/right volume adjustments. Sliders also support
mouse clicking and dragging; keyboard control continues on the row selected by
the mouse. Test sound cycles through all eleven effects with left/right and
previews on selection.

First-use defaults are music enabled at 25%, effects enabled at 50%, and Great
Fairy's Fountain selected. Settings persist in the existing
`~/.config/KytyPS5/launcher.json` under `Audio`; unknown launcher fields survive.
Volume updates reach the mixer immediately. Slider persistence is debounced by
150 ms, with a final save on normal close. Zero gain produces digital silence for
that channel. Muting one channel does not change the other.

All four required theme entries remain available if their audio files go missing.
Choose the affected theme and use Import local music to supply a local copy. The
fifth entry imports arbitrary custom music. MP3, OGG, FLAC, WAV, AAC/M4A and Opus
are supported through the system FFmpeg decoder. An import is validated before
it replaces the current entry; failed imports leave the existing selection intact.

Existing optional game-title previews remain available under Appearance & library.
They use the same engine and music master gain. Explicitly selecting a dashboard
theme turns off the preview override. The preview volume remains available for
the older feature; it does not alter interface-sound gain.

## Playback and resource use

FFmpeg converts a track once into a hash-keyed, normalized 48 kHz stereo PCM cache,
then exits. Playback reads small buffered blocks from disk, retaining only a
short loop-head overlap in memory. It does not decode entire tracks into managed
arrays. Music uses a smooth one-second song crossfade and an approximately
1.2-second overlap at the loop seam. Only leading/trailing silence is trimmed;
pauses inside arrangements remain. Crossfades prevent clicks and gaps but are not
beat-matched musical edits.

Eleven Kenney Interface Sounds effects are preloaded. Their CC0 license and source
mapping are included in `src/KytyPS5.Launcher/AudioAssets/`. They cover navigation,
confirm, back, L1, R1, panel open/close, enable/disable, error and launch. The two
directional transitions have distinct pitch and stereo balance. Levels are
attenuated and gently filtered for a restrained dashboard sound.

Mouse, keyboard and controller share the same semantic event router. Nested
focus, click and settings events are combined, so a toggle or panel transition
does not also trigger a duplicate confirmation. Repeated navigation is throttled.
The software audio queue stays below 43 ms, plus device latency. The device pauses
when there is no audible work. Missing output devices produce a status message
and retry without stopping the GUI.

Game launch fades the complete dashboard mix for 450 ms, clears its queued audio,
and closes the playback device before starting the unchanged emulator command.
While a game is running, effects and music are both suspended. Exit/crash returns
to the dashboard and reopens the current default output with a 650 ms fade-in.

## Verification

`./run-kytyps5.sh --verify` covers the existing native command/profile checks plus
audio preferences, exact zero gain, independent 15%/65% mixing, crossfades, loop
seams, bounded voices, effect assets, event deduplication, import and SDL lifecycle.
Audio device checks use SDL's dummy driver.

The opt-in UI checks visit every settings row and test persistent sliders,
mouse-click and keyboard event paths, controller text entry, and a simulated game
handoff. The opt-in pointer fixture verifies actual X11 mouse dragging and
keyboard handoff using isolated launcher preferences.

`artifacts/audio-output-verification/result.json` records a real
PipeWire/PulseAudio monitor capture through a temporary test sink. It verifies
audible music/effects, crossfade/resume, exact digital silence for mute and the
game handoff, at most one output stream, and zero dashboard streams during the
simulated game. The temporary sink is removed after the test. The default output,
system volume, emulator audio, renderer and game profiles are not changed.

No PS5 title was booted during these audio tests. Runtime handoff was verified
with the existing process supervisor and a stub executable.
