# Menulody

Menulody is a background music pak for NextUI. It plays MP3 and WAV files while you browse the menu, pauses when you launch games or other paks, and resumes when you return.

## Install

1. Copy `Menulody.pak` to `Tools/<platform>/` on your SD card.
2. Launch Menulody once from the Tools menu.
3. Add music to your library folder, or point Menulody at additional folders from Settings.

Supported platforms:

- `tg5040`
- `tg5050`
- `my355`

Supported audio formats:

- `.mp3`
- `.wav`

## How Playback Works

Menulody now has two playback modes:

- `Single Song Loop`: choose a song in `Library` and press `A`. Menulody switches to that one track and loops it until you later choose a playlist source.
- `Playlist Source`: choose `All Songs` or a saved playlist in `Playlists`. Menulody then plays from that source using shuffle and repeat settings.

Notes:

- Saved playlists are source selectors. They are not separate in-app music players anymore.
- `Preview` is temporary. It does not change the active source.
- If a preview is already playing, pressing `Y` again stops it.

## Controls

Main screens follow Apostrophe / NextUI footer conventions:

- `B`: back or cancel
- `A`: confirm / select
- `START`: save on screens that stage changes first

Important screens:

- `Library`
  - `A`: start single-song loop mode for the selected track
  - `Y`: preview / stop preview
- `Now Playing`
  - `L2`: previous track
  - `R2`: next track
  - `X`: shuffle
  - `Y`: repeat
  - `A`: play / pause
- `Playlists`
  - `A`: activate `All Songs` or the selected saved playlist
  - `X`: delete saved playlist
- `Settings`
  - changes are staged locally
  - `START`: save
  - `B`: discard staged changes

## Settings

Settings are stored in shared userdata:

- `.../.userdata/shared/Menulody/settings.json`

You can configure:

- music folders
- shuffle
- repeat
- volume
- pause on pak launch
- auto-start daemon at boot
- now-playing overlay duration

Music folders are scanned recursively.

Default music folder:

- `/mnt/SDCARD/Music`

Saved playlists are stored here:

- `.../.userdata/shared/Menulody/playlists/`

## Overlay

The now-playing pill appears:

- on first playback after boot
- when the actual song changes

It does not reappear when the same song loops in repeat-one mode.

## Auto Pause / Resume

When Menulody is installed and run once, it installs launch hooks in userdata so it can:

- pause before ROM launches
- optionally pause before pak launches
- resume when you return to NextUI

If `Auto-Start Daemon` is enabled, Menulody also installs a boot hook so the daemon starts automatically.

## Troubleshooting

If no songs appear:

- confirm your files are `.mp3` or `.wav`
- confirm the music folders in Settings are correct
- save Settings after changing folders so Menulody can rescan

If music does not resume:

- open Menulody once to ensure hooks are installed
- confirm the daemon is running with `menulody --status`
- try disabling and re-enabling `Auto-Start Daemon`

If playlists look empty:

- some playlist entries may point to files that no longer exist
- re-create the playlist from the current library
